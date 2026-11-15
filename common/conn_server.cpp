// conn_server.cpp - accept loop, per-connection threads, graceful stop.
#include "conn_server.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace p2p {

ConnServer::ConnServer(Handler handler) : handler_(std::move(handler)) {}

ConnServer::~ConnServer() { stop(); }

bool ConnServer::start(const Endpoint& ep, std::string& err) {
    listen_fd_ = listen_tcp(ep);
    if (listen_fd_ < 0) {
        err = "cannot listen on " + ep.str() + ": " + std::strerror(errno);
        return false;
    }
    port_ = local_port(listen_fd_);
    accept_thread_ = std::thread(&ConnServer::accept_loop, this);
    return true;
}

void ConnServer::accept_loop() {
    while (true) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            {
                std::lock_guard<std::mutex> lock(conns_mu_);
                if (stopping_) break;  // stop() shut the listening socket down
            }
            // e.g. EMFILE (out of fds): back off instead of spinning at 100% CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        std::lock_guard<std::mutex> lock(conns_mu_);
        reap_finished_locked();
        if (stopping_) {
            ::close(fd);
            break;
        }
        conns_.push_back(std::make_unique<Conn>());
        Conn* conn = conns_.back().get();
        conn->fd = fd;
        conn->thread = std::thread([this, conn] {
            handler_(conn->fd);
            // Tell the peer we are done NOW (it sees EOF immediately); the fd
            // itself is closed later, by whoever joins this thread.
            ::shutdown(conn->fd, SHUT_RDWR);
            conn->done = true;
        });
    }
}

// Join and free connection threads that have already finished, so a
// long-running server does not pile up dead thread objects.
void ConnServer::reap_finished_locked() {
    for (auto it = conns_.begin(); it != conns_.end();) {
        if ((*it)->done) {
            (*it)->thread.join();
            ::close((*it)->fd);
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

void ConnServer::stop() {
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    // 1. Wake the accept thread: shutdown() makes a blocked accept() return.
    if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
    if (accept_thread_.joinable()) accept_thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;

    // 2. Wake every connection thread: shutdown() makes its blocked recv()
    //    return 0, so its handler leaves its loop. Then join, then close.
    std::lock_guard<std::mutex> lock(conns_mu_);
    for (auto& c : conns_) ::shutdown(c->fd, SHUT_RDWR);
    for (auto& c : conns_) {
        c->thread.join();
        ::close(c->fd);
    }
    conns_.clear();
}

}  // namespace p2p
