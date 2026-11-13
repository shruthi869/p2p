// tracker_link.cpp - connect to whichever tracker is up, fail over on errors.
#include "tracker_link.h"

#include <cstdio>
#include <unistd.h>

namespace p2p {

namespace {
constexpr int kConnectTimeoutMs = 1000;
}

TrackerLink::TrackerLink(std::vector<Endpoint> trackers, int io_timeout_ms, bool announce)
    : trackers_(std::move(trackers)), io_timeout_ms_(io_timeout_ms), announce_(announce) {}

TrackerLink::~TrackerLink() {
    std::lock_guard<std::mutex> lock(mu_);
    disconnect_locked();
}

void TrackerLink::disconnect_locked() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

int TrackerLink::connected_tracker() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fd_ >= 0 ? static_cast<int>(current_) + 1 : 0;
}

void TrackerLink::remember_login(const std::string& user, const std::string& password,
                                 const std::string& peer_addr) {
    std::lock_guard<std::mutex> lock(mu_);
    have_login_ = true;
    user_ = user;
    password_ = password;
    peer_addr_ = peer_addr;
}

void TrackerLink::forget_login() {
    std::lock_guard<std::mutex> lock(mu_);
    have_login_ = false;
    user_.clear();
    password_.clear();
    peer_addr_.clear();
}

bool TrackerLink::ensure_connected(std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    return fd_ >= 0 || connect_locked(err);
}

// One round trip. false = this connection is unusable (dead, hung past the
// timeout, or answered with something that is not the reply we asked for).
bool TrackerLink::exchange_locked(const Message& req, Message& reply) {
    if (send_msg(fd_, req) != IoResult::OK) return false;
    if (recv_msg(fd_, reply) != IoResult::OK) return false;
    return reply.type == reply_type(req.type);
}

// Try every tracker, starting with the current one. On success, if we were
// logged in, log in again on the new connection (a tracker session belongs
// to a TCP connection, so a new connection starts logged out).
bool TrackerLink::connect_locked(std::string& err) {
    const size_t n = trackers_.size();
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = (current_ + i) % n;
        int fd = connect_tcp(trackers_[idx], kConnectTimeoutMs);
        if (fd < 0) continue;
        set_io_timeout(fd, io_timeout_ms_);
        fd_ = fd;
        current_ = idx;

        if (have_login_) {
            Message rep;
            Message login{MsgType::LOGIN, Status::OK, pack_fields({user_, password_, peer_addr_})};
            if (!exchange_locked(login, rep)) {  // this tracker died right away: try the next
                disconnect_locked();
                continue;
            }
            if (rep.status != Status::OK)
                std::fprintf(stderr, "warning: re-login on tracker %zu failed: %s\n", idx + 1,
                             status_text(rep.status));
        }
        const int now = static_cast<int>(idx) + 1;
        if (announce_ && now != last_announced_) {
            std::printf("[connected to tracker %d at %s]\n", now, trackers_[idx].str().c_str());
            std::fflush(stdout);
        }
        last_announced_ = now;
        return true;
    }
    err = "no tracker reachable";
    return false;
}

bool TrackerLink::call(MsgType type, const std::vector<std::string>& fields, Message& reply,
                       std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    const Message req{type, Status::OK, pack_fields(fields)};
    // Attempt 1 on the current tracker; if it fails, attempt 2 starts from
    // the OTHER tracker. Two attempts = each tracker gets one chance.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (fd_ < 0 && !connect_locked(err)) return false;
        if (exchange_locked(req, reply)) return true;
        disconnect_locked();
        current_ = (current_ + 1) % trackers_.size();
    }
    err = "lost connection to tracker";
    return false;
}

}  // namespace p2p
