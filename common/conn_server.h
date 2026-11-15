// conn_server.h - a reusable TCP server: one accept thread + one thread per
// connection + graceful shutdown. Used by the tracker (client requests) and
// by every client's peer server (piece requests), so the tricky lifecycle
// code exists, and is tested, in exactly one place.
//
// The handler runs on the connection's own thread and owns the conversation
// (it loops on recv/send). When it returns, the connection is shut down.
#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "net.h"

namespace p2p {

class ConnServer {
public:
    using Handler = std::function<void(int fd)>;

    explicit ConnServer(Handler handler);
    ~ConnServer();
    ConnServer(const ConnServer&) = delete;
    ConnServer& operator=(const ConnServer&) = delete;

    // Bind + listen, then start the accept thread. False (with err) on failure.
    bool start(const Endpoint& ep, std::string& err);

    // Stop accepting, wake every connection thread (shutdown() makes blocked
    // accept()/recv() return), join them all, close every socket.
    // Safe to call more than once.
    void stop();

    uint16_t port() const { return port_; }

private:
    struct Conn {
        int               fd = -1;
        std::thread       thread;
        std::atomic<bool> done{false};  // handler returned; ready to be joined
    };

    void accept_loop();
    void reap_finished_locked();

    Handler     handler_;
    int         listen_fd_ = -1;
    uint16_t    port_ = 0;
    std::thread accept_thread_;

    // A socket is closed ONLY after its thread has been joined. If the thread
    // closed it itself, stop() could shutdown() an fd number the kernel had
    // already handed to someone else.
    std::mutex                       conns_mu_;
    std::list<std::unique_ptr<Conn>> conns_;
    bool                             stopping_ = false;  // guarded by conns_mu_
};

}  // namespace p2p
