// sync.cpp - the push side of tracker-to-tracker sync.
#include "sync.h"

#include <chrono>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

namespace p2p {

std::string encode_op(const Op& op) {
    std::vector<std::string> f = {std::to_string(static_cast<int>(op.type))};
    f.insert(f.end(), op.args.begin(), op.args.end());
    return pack_fields(f);
}

bool decode_op(const std::string& payload, Op& op) {
    std::vector<std::string> f = split_fields(payload);
    uint64_t t = 0;
    if (f.empty() || f[0].empty() || f[0].size() > 3) return false;
    for (char c : f[0])
        if (c < '0' || c > '9') return false;
    t = std::stoul(f[0]);
    op.type = static_cast<MsgType>(t);
    op.args.assign(f.begin() + 1, f.end());
    return true;
}

SyncLink::SyncLink(TrackerState& state, int my_no, const Endpoint& peer, bool verbose)
    : state_(state), my_no_(my_no), peer_no_(my_no == 1 ? 2 : 1), peer_(peer), verbose_(verbose) {}

SyncLink::~SyncLink() { stop(); }

void SyncLink::start() { thread_ = std::thread(&SyncLink::loop, this); }

void SyncLink::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return;
        stopping_ = true;
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);  // wake a blocked send/recv
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

Status SyncLink::apply_and_forward(const Op& op) {
    std::lock_guard<std::mutex> lock(mu_);
    Status s = state_.apply(op);
    if (s == Status::OK && connected_) {
        queue_.push_back(op);
        cv_.notify_all();
    }
    return s;
}

void SyncLink::set_enabled(bool on) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        enabled_ = on;
        if (!on && fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);  // cut the link now
    }
    cv_.notify_all();
}

bool SyncLink::enabled() const {
    std::lock_guard<std::mutex> lock(mu_);
    return enabled_;
}

bool SyncLink::connected() const {
    std::lock_guard<std::mutex> lock(mu_);
    return connected_;
}

bool SyncLink::exchange(int fd, const Message& req) {
    Message rep;
    return send_msg(fd, req) == IoResult::OK && recv_msg(fd, rep) == IoResult::OK &&
           rep.type == reply_type(req.type) && rep.status == Status::OK;
}

void SyncLink::loop() {
    bool was_connected = false;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stopping_ || enabled_; });
            if (stopping_) return;
        }
        // 1. Connect + say who we are.
        int fd = connect_tcp(peer_, 1000);
        if (fd >= 0) {
            set_io_timeout(fd, 5000);
            if (!exchange(fd, {MsgType::SYNC_HELLO, Status::OK, std::to_string(my_no_)})) {
                ::close(fd);  // peer refused (e.g. its sync is off) or died
                fd = -1;
            }
        }
        if (fd < 0) {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(kRetryMs), [&] { return stopping_; });
            continue;
        }

        // 2. Snapshot, taken atomically with "start queueing ops".
        std::string snap;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopping_ || !enabled_) {
                ::close(fd);
                continue;
            }
            snap = state_.snapshot();
            queue_.clear();
            connected_ = true;
            fd_ = fd;
        }
        bool ok = exchange(fd, {MsgType::SYNC_SNAPSHOT, Status::OK, snap});
        if (ok) {
            const auto c = state_.counts();
            log("sync: connected to tracker " + std::to_string(peer_no_) + ", sent snapshot (" +
                std::to_string(c.users) + " users, " + std::to_string(c.groups) + " groups, " +
                std::to_string(c.files) + " files)");
            was_connected = true;
        }

        // 3./4. Stream ops in order; heartbeat when idle.
        while (ok) {
            Message req;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(kHeartbeatMs),
                             [&] { return stopping_ || !enabled_ || !queue_.empty(); });
                if (stopping_ || !enabled_) break;
                if (queue_.empty()) {
                    req = {MsgType::PING, Status::OK, ""};
                } else {
                    req = {MsgType::SYNC_OP, Status::OK, encode_op(queue_.front())};
                    queue_.pop_front();
                }
            }
            ok = exchange(fd, req);
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            connected_ = false;
            queue_.clear();  // the next snapshot will carry these changes
            fd_ = -1;
        }
        ::close(fd);
        if (was_connected) {
            log("sync: link to tracker " + std::to_string(peer_no_) + " is down");
            was_connected = false;
        }
    }
}

void SyncLink::log(const std::string& line) const {
    if (!verbose_) return;
    std::printf("[tracker %d] %s\n", my_no_, line.c_str());
    std::fflush(stdout);
}

}  // namespace p2p
