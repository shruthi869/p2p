// server.h - the tracker's network side: turns each client request into a
// TrackerState operation or query.
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

namespace p2p {

class TrackerServer {
public:
    TrackerServer(TrackerState& state, int tracker_no, bool verbose);
    ~TrackerServer();
    TrackerServer(const TrackerServer&) = delete;
    TrackerServer& operator=(const TrackerServer&) = delete;

    bool     start(const Endpoint& ep, std::string& err) { return server_.start(ep, err); }
    void     stop() { server_.stop(); }
    uint16_t port() const { return server_.port(); }

private:
    // Per-connection login state (lives on the connection thread's stack).
    struct ClientSession {
        std::string user;        // empty = not logged in
        std::string session_id;
    };

    void        serve(int fd);  // runs on the connection's thread
    Message     handle(ClientSession& cs, const Message& req);
    std::string new_session_id();
    void        log(const std::string& line) const;

    TrackerState&              state_;
    const int                  tracker_no_;
    const bool                 verbose_;
    std::atomic<unsigned long> session_counter_{0};
    const long long            start_ms_;
    ConnServer                 server_;  // last: destroyed (= stopped) first
};

}  // namespace p2p
