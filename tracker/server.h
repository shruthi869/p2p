// server.h - the tracker's network side: accepts client connections and turns
// each request into a TrackerState operation or query.
//
// Threading model: one accept thread + one thread per connected client.
// At most a handful of clients connect to a tracker, and each request is a
// few map lookups, so thread-per-connection is the simplest correct design
// (epoll would be the choice for thousands of connections).
#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

    // Bind + listen, then start the accept thread. False (with err) on failure.
    bool start(const Endpoint& ep, std::string& err);

    // Graceful shutdown: stop accepting, wake every connection thread, join
    // them all, close every socket. Safe to call more than once.
    void stop();

    uint16_t port() const { return port_; }

private:
    struct Conn {
        int               fd = -1;
        std::thread       thread;
        std::atomic<bool> done{false};  // thread finished; ready to be joined
    };
    // Per-connection login state (lives on the connection thread's stack).
    struct ClientSession {
        std::string user;        // empty = not logged in
        std::string session_id;
    };

    void    accept_loop();
    void    serve(Conn* conn);
    Message handle(ClientSession& cs, const Message& req);
    void    reap_finished_locked();
    std::string new_session_id();
    void    log(const std::string& line) const;

    TrackerState&     state_;
    const int         tracker_no_;
    const bool        verbose_;
    int               listen_fd_ = -1;
    uint16_t          port_ = 0;
    std::thread       accept_thread_;

    // A socket is closed ONLY after its thread has been joined. If the thread
    // closed it itself, stop() could shutdown() an fd number the kernel had
    // already handed to someone else.
    std::mutex                         conns_mu_;
    std::list<std::unique_ptr<Conn>>   conns_;
    bool                               stopping_ = false;  // guarded by conns_mu_

    std::atomic<unsigned long> session_counter_{0};
    const long long            start_ms_;
};

}  // namespace p2p
