// server.h - the tracker's network side: turns each client request into a
// TrackerState operation or query, and accepts the other tracker's sync
// connection (SYNC_HELLO / SYNC_SNAPSHOT / SYNC_OP, see sync.h).
//
// Threading model (provided by ConnServer): one accept thread + one thread
// per connected client. At most a handful of clients connect to a tracker,
// and each request is a few map lookups, so thread-per-connection is the
// simplest correct design (epoll would be the choice for thousands).
#pragma once

#include <atomic>
#include <string>

#include "conn_server.h"
#include "net.h"
#include "protocol.h"
#include "state.h"
#include "sync.h"

namespace p2p {

class TrackerServer {
public:
    // `sync` may be null (single tracker, e.g. in unit tests): changes are
    // then applied locally only.
    TrackerServer(TrackerState& state, int tracker_no, bool verbose, SyncLink* sync = nullptr);
    ~TrackerServer();
    TrackerServer(const TrackerServer&) = delete;
    TrackerServer& operator=(const TrackerServer&) = delete;

    bool     start(const Endpoint& ep, std::string& err) { return server_.start(ep, err); }
    void     stop() { server_.stop(); }
    uint16_t port() const { return server_.port(); }

    // "sync off": refuse (and cut) the other tracker's incoming sync link.
    void set_sync_accept(bool on) { sync_accept_ = on; }

private:
    // Per-connection state (lives on the connection thread's stack).
    struct ClientSession {
        int         fd = -1;
        std::string user;          // empty = not logged in
        std::string session_id;
        int         peer_tracker = 0;  // != 0: this connection is the other tracker's sync link
        bool        drop = false;      // close the connection after this reply
    };

    void        serve(int fd);  // runs on the connection's thread
    Message     handle(ClientSession& cs, const Message& req);
    Message     handle_sync(ClientSession& cs, const Message& req);
    Status      apply_local(const Op& op);
    std::string new_session_id();
    void        log(const std::string& line) const;

    TrackerState&              state_;
    const int                  tracker_no_;
    const bool                 verbose_;
    SyncLink*                  sync_;
    std::atomic<bool>          sync_accept_{true};
    std::atomic<unsigned long> session_counter_{0};
    const long long            start_ms_;
    ConnServer                 server_;  // last: destroyed (= stopped) first
};

}  // namespace p2p
