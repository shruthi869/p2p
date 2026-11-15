// tracker/main.cpp - entry point: ./tracker tracker_info.txt tracker_no
//
// Starts the server, then waits on the console for "quit". Ctrl-C / SIGTERM
// also shut down gracefully (all threads joined, all sockets closed), which
// keeps valgrind/ASan reports clean and lets the port be reused at once.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/signalfd.h>
#include <unistd.h>
#include <vector>

#include "net.h"
#include "server.h"
#include "state.h"
#include "sync.h"

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s tracker_info.txt tracker_no\n", argv[0]);
        return 1;
    }
    std::vector<p2p::Endpoint> trackers;
    std::string err;
    if (!p2p::read_tracker_info(argv[1], trackers, err)) {
        std::fprintf(stderr, "tracker: %s\n", err.c_str());
        return 1;
    }
    const std::string no_str = argv[2];
    if (no_str != "1" && no_str != "2") {
        std::fprintf(stderr, "tracker: tracker_no must be 1 or 2\n");
        return 1;
    }
    const int no = no_str == "1" ? 1 : 2;

    // Block SIGINT/SIGTERM BEFORE starting any thread (threads inherit the
    // mask), then receive them through a signalfd we can poll() next to stdin.
    // No async signal handler = no async-signal-safety worries.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    int sig_fd = signalfd(-1, &sigs, SFD_CLOEXEC);
    if (sig_fd < 0) {
        std::fprintf(stderr, "tracker: signalfd: %s\n", std::strerror(errno));
        return 1;
    }
    p2p::ignore_sigpipe();

    p2p::TrackerState state;
    p2p::SyncLink sync(state, no, trackers[no == 1 ? 1 : 0], /*verbose=*/true);  // pushes to the OTHER tracker
    p2p::TrackerServer server(state, no, /*verbose=*/true, &sync);
    if (!server.start(trackers[no - 1], err)) {
        std::fprintf(stderr, "tracker: %s\n", err.c_str());
        ::close(sig_fd);
        return 1;
    }
    std::printf("[tracker %d] listening on %s  (commands: quit, status, sync off, sync on)\n", no,
                trackers[no - 1].str().c_str());
    std::fflush(stdout);
    sync.start();

    pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {sig_fd, POLLIN, 0}};
    std::string pending;
    bool quit = false;
    while (!quit) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            signalfd_siginfo si{};
            if (::read(sig_fd, &si, sizeof si) == static_cast<ssize_t>(sizeof si))
                std::printf("[tracker %d] got %s, shutting down\n", no, strsignal(static_cast<int>(si.ssi_signo)));
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[256];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) {
                fds[0].fd = -1;  // stdin closed (e.g. run in background): keep serving, wait for a signal
                continue;
            }
            pending.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string cmd = trim(pending.substr(0, nl));
                pending.erase(0, nl + 1);
                if (cmd == "quit") {
                    quit = true;
                    break;
                }
                if (cmd == "sync off") {  // simulate a network partition between the trackers
                    sync.set_enabled(false);
                    server.set_sync_accept(false);
                    std::printf("[tracker %d] sync disabled (simulated partition)\n", no);
                } else if (cmd == "sync on") {
                    server.set_sync_accept(true);
                    sync.set_enabled(true);
                    std::printf("[tracker %d] sync enabled\n", no);
                } else if (cmd == "status") {
                    const auto c = state.counts();
                    std::printf("[tracker %d] status: %zu users, %zu online, %zu groups, %zu files; sync %s\n", no,
                                c.users, c.sessions, c.groups, c.files,
                                !sync.enabled() ? "off" : sync.connected() ? "connected" : "waiting for peer");
                } else if (!cmd.empty()) {
                    std::printf("[tracker %d] unknown command '%s' (try: quit, status, sync off, sync on)\n", no,
                                cmd.c_str());
                }
                std::fflush(stdout);
            }
        }
    }

    server.stop();  // no more client changes...
    sync.stop();    // ...then stop pushing
    ::close(sig_fd);
    std::printf("[tracker %d] stopped\n", no);
    return 0;
}
