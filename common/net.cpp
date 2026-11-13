// net.cpp - socket helpers and tracker_info.txt parsing.
#include "net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace p2p {

namespace {

bool fill_addr(const Endpoint& ep, sockaddr_in& addr) {
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep.port);
    if (inet_pton(AF_INET, ep.ip.c_str(), &addr.sin_addr) != 1) {
        errno = EINVAL;
        return false;
    }
    return true;
}

// Close fd but keep the errno that explains why we are bailing out.
int fail_close(int fd) {
    int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
}

}  // namespace

bool parse_port(const std::string& text, uint16_t& out) {
    if (text.empty() || text.size() > 5) return false;
    unsigned long v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned long>(c - '0');
    }
    if (v == 0 || v > 65535) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

bool parse_endpoint(const std::string& text, Endpoint& out) {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    Endpoint ep;
    ep.ip = text.substr(0, colon);
    in_addr tmp{};
    if (inet_pton(AF_INET, ep.ip.c_str(), &tmp) != 1) return false;
    if (!parse_port(text.substr(colon + 1), ep.port)) return false;
    out = ep;
    return true;
}

bool read_tracker_info(const std::string& path, std::vector<Endpoint>& out, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::vector<Endpoint> result;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ss(line);
        std::string ip, port, extra;
        if (!(ss >> ip) || ip[0] == '#') continue;  // blank or comment
        Endpoint ep;
        ep.ip = ip;
        in_addr tmp{};
        if (!(ss >> port) || (ss >> extra) || inet_pton(AF_INET, ip.c_str(), &tmp) != 1 ||
            !parse_port(port, ep.port)) {
            err = path + ":" + std::to_string(lineno) + ": expected '<IPv4> <port>'";
            return false;
        }
        result.push_back(ep);
    }
    if (result.size() != 2) {
        err = path + ": expected exactly 2 trackers, found " + std::to_string(result.size());
        return false;
    }
    out = result;
    return true;
}

int listen_tcp(const Endpoint& ep, int backlog) {
    sockaddr_in addr{};
    if (!fill_addr(ep, addr)) return -1;
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) return fail_close(fd);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) return fail_close(fd);
    if (::listen(fd, backlog) < 0) return fail_close(fd);
    return fd;
}

// Non-blocking connect + poll() is the standard way to put a time limit on
// connect(); a plain blocking connect can hang for over a minute.
int connect_tcp(const Endpoint& ep, int timeout_ms) {
    sockaddr_in addr{};
    if (!fill_addr(ep, addr)) return -1;
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return fail_close(fd);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        if (errno != EINPROGRESS) return fail_close(fd);
        pollfd pfd{fd, POLLOUT, 0};
        int rc;
        do {
            rc = ::poll(&pfd, 1, timeout_ms);
        } while (rc < 0 && errno == EINTR);
        if (rc == 0) {
            errno = ETIMEDOUT;
            return fail_close(fd);
        }
        if (rc < 0) return fail_close(fd);
        int soerr = 0;
        socklen_t len = sizeof soerr;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) return fail_close(fd);
        if (soerr != 0) {
            errno = soerr;  // e.g. ECONNREFUSED
            return fail_close(fd);
        }
    }
    if (::fcntl(fd, F_SETFL, flags) < 0) return fail_close(fd);  // back to blocking
    return fd;
}

bool set_io_timeout(int fd, int timeout_ms) {
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
           ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

uint16_t local_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

void ignore_sigpipe() {
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGPIPE, &sa, nullptr);
}

}  // namespace p2p
