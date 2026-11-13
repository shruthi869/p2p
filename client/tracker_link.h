// tracker_link.h - the client's connection to "the tracker system".
//
// The client knows both trackers. It talks to one at a time; if that one dies
// or hangs (I/O error or timeout), the link switches to the other tracker,
// logs in again with the remembered credentials, and retries the request
// once. Callers just call call() and never see which tracker answered.
//
// Thread-safe: one request/reply at a time on the connection (a mutex), so
// download threads (later parts) can share the link with the command loop.
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "net.h"
#include "protocol.h"

namespace p2p {

class TrackerLink {
public:
    explicit TrackerLink(std::vector<Endpoint> trackers, int io_timeout_ms = 5000,
                         bool announce = true);
    ~TrackerLink();
    TrackerLink(const TrackerLink&) = delete;
    TrackerLink& operator=(const TrackerLink&) = delete;

    // One request -> one reply. Returns false (with err) only if NO tracker
    // could answer; tracker-level errors come back as reply.status.
    bool call(MsgType type, const std::vector<std::string>& fields, Message& reply, std::string& err);

    // Connect now if not connected (used at startup to report status early).
    bool ensure_connected(std::string& err);

    // Credentials kept in memory so a failover can log in again transparently.
    void remember_login(const std::string& user, const std::string& password, const std::string& peer_addr);
    void forget_login();

    int connected_tracker() const;  // 1 or 2; 0 = not connected

private:
    bool connect_locked(std::string& err);
    bool exchange_locked(const Message& req, Message& reply);
    void disconnect_locked();

    mutable std::mutex    mu_;
    std::vector<Endpoint> trackers_;
    const int             io_timeout_ms_;
    const bool            announce_;           // print "connected to tracker N" on changes
    int                   fd_ = -1;
    size_t                current_ = 0;        // index of the tracker to use / try first
    int                   last_announced_ = 0; // last tracker we told the user about
    bool                  have_login_ = false;
    std::string           user_, password_, peer_addr_;
};

}  // namespace p2p
