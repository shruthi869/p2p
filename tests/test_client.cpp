// tests/test_client.cpp - the client's TrackerLink against real in-process
// trackers: picking a live tracker, failover with automatic re-login,
// recovery after both were down, a hung tracker, and concurrent callers.
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

#include "commands.h"
#include "net.h"
#include "server.h"
#include "state.h"
#include "test_util.h"
#include "tracker_link.h"

using namespace p2p;

// A tracker we can stop and restart on the same port.
struct Tracker {
    int                            no;
    TrackerState                   state;
    std::unique_ptr<TrackerServer> server;
    uint16_t                       port = 0;

    explicit Tracker(int n) : no(n) {}
    bool start(uint16_t want_port = 0) {
        server = std::make_unique<TrackerServer>(state, no, false);
        std::string err;
        if (!server->start({"127.0.0.1", want_port}, err)) return false;
        port = server->port();
        return true;
    }
    void stop() { server.reset(); }  // destructor = graceful stop
    Endpoint ep() const { return {"127.0.0.1", port}; }
    void add_user(const std::string& u, const std::string& pw) {
        state.apply({MsgType::CREATE_USER, {u, password_digest(u, pw)}});
    }
};

// A port nothing listens on.
static uint16_t dead_port() {
    int fd = listen_tcp({"127.0.0.1", 0});
    uint16_t p = local_port(fd);
    ::close(fd);
    return p;
}

static Status call_status(TrackerLink& link, MsgType t, const std::vector<std::string>& f = {}) {
    Message rep;
    std::string err;
    if (!link.call(t, f, rep, err)) return Status::INTERNAL_ERROR;
    return rep.status;
}

static void test_tokenize() {
    CHECK(tokenize("  login   alice\tpw  ") == (std::vector<std::string>{"login", "alice", "pw"}));
    CHECK(tokenize("   ").empty());
}

static void test_skips_dead_first_tracker() {
    Tracker t2(2);
    CHECK(t2.start());
    TrackerLink link({{"127.0.0.1", dead_port()}, t2.ep()}, 2000, false);
    CHECK(call_status(link, MsgType::PING) == Status::OK);
    CHECK(link.connected_tracker() == 2);
}

// The key failover test: logged in on tracker 1, tracker 1 dies, the next
// command transparently reconnects to tracker 2, logs in again, and succeeds.
static void test_failover_relogin() {
    Tracker t1(1), t2(2);
    CHECK(t1.start() && t2.start());
    // Part 7 will sync users between trackers; until then, create on both.
    t1.add_user("alice", "pw");
    t2.add_user("alice", "pw");

    TrackerLink link({t1.ep(), t2.ep()}, 2000, false);
    CHECK(call_status(link, MsgType::LOGIN, {"alice", "pw", "127.0.0.1:7001"}) == Status::OK);
    link.remember_login("alice", "pw", "127.0.0.1:7001");
    CHECK(link.connected_tracker() == 1 && t1.state.is_online("alice"));

    t1.stop();
    CHECK(call_status(link, MsgType::LIST_GROUPS) == Status::OK);  // would be NOT_LOGGED_IN without re-login
    CHECK(link.connected_tracker() == 2);
    CHECK(t2.state.is_online("alice"));
}

static void test_all_down_then_recover() {
    Tracker t1(1);
    CHECK(t1.start());
    const uint16_t p1 = t1.port;
    TrackerLink link({t1.ep(), {"127.0.0.1", dead_port()}}, 2000, false);
    CHECK(call_status(link, MsgType::PING) == Status::OK);

    t1.stop();
    Message rep;
    std::string err;
    CHECK(!link.call(MsgType::PING, {}, rep, err));
    CHECK(err == "no tracker reachable");

    CHECK(t1.start(p1));  // tracker comes back on the same port
    CHECK(call_status(link, MsgType::PING) == Status::OK);
    CHECK(link.connected_tracker() == 1);
}

// A tracker that accepts the TCP connection but never answers (e.g. frozen
// with SIGSTOP). Only the recv timeout saves us from waiting forever.
static void test_hung_tracker_times_out() {
    int hung = listen_tcp({"127.0.0.1", 0});  // kernel completes the handshake; nobody ever reads
    Tracker t2(2);
    CHECK(t2.start());
    TrackerLink link({{"127.0.0.1", local_port(hung)}, t2.ep()}, 300, false);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(call_status(link, MsgType::PING) == Status::OK);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(link.connected_tracker() == 2);
    CHECK(ms >= 250 && ms < 3000);
    ::close(hung);
}

// Several threads share one link (download threads will do this later).
static void test_concurrent_callers() {
    Tracker t1(1);
    CHECK(t1.start());
    TrackerLink link({t1.ep(), {"127.0.0.1", dead_port()}}, 2000, false);
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (int j = 0; j < 50; ++j)
                if (call_status(link, MsgType::PING) == Status::OK) ++ok;
        });
    for (auto& th : threads) th.join();
    CHECK(ok == 200);
}

int main() {
    ignore_sigpipe();
    static const TestCase tests[] = {
        {"tokenize", test_tokenize},
        {"skips a dead first tracker", test_skips_dead_first_tracker},
        {"failover + automatic re-login", test_failover_relogin},
        {"both trackers down, then recovery", test_all_down_then_recover},
        {"hung tracker -> timeout -> failover", test_hung_tracker_times_out},
        {"4 threads share one link", test_concurrent_callers},
    };
    return run_tests(tests);
}
