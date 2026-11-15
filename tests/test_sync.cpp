// tests/test_sync.cpp - tracker-to-tracker sync.
//   Part A: merge rules on plain TrackerState objects (deterministic).
//   Part B: two real in-process trackers with SyncLinks: live changes,
//           restart catch-up, partition + heal, client failover.
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "file_meta.h"
#include "net.h"
#include "server.h"
#include "sha1.h"
#include "shared_files.h"
#include "state.h"
#include "sync.h"
#include "test_util.h"
#include "tracker_link.h"

using namespace p2p;
using Fields = std::vector<std::string>;

static std::string D(const std::string& u, const std::string& p) { return password_digest(u, p); }
static Status op(TrackerState& st, MsgType t, Fields a) { return st.apply({t, std::move(a)}); }

static Fields upload(const std::string& actor, const std::string& g, const std::string& name, const std::string& tag) {
    return {actor, g, name, "100", sha1_hex(tag), sha1_hex(tag + "0")};  // 100 bytes = 1 piece
}

// ===================================================== Part A: merge rules

// A synced pair, built the way live sync builds it: each session is created
// on the tracker its client is connected to (alice on 1, bob on 2), and the
// trackers exchange snapshots.
static void base_pair(TrackerState& t1, TrackerState& t2) {
    op(t1, MsgType::CREATE_USER, {"alice", D("alice", "pw")});
    op(t1, MsgType::CREATE_USER, {"bob", D("bob", "pw")});
    op(t1, MsgType::LOGIN, {"alice", D("alice", "pw"), "127.0.0.1:7001", "1-100-1"});
    op(t1, MsgType::CREATE_GROUP, {"alice", "g"});
    op(t1, MsgType::UPLOAD_FILE, upload("alice", "g", "a.txt", "A"));
    t2.merge(t1.snapshot(), 1, 2);
    op(t2, MsgType::LOGIN, {"bob", D("bob", "pw"), "127.0.0.1:7002", "2-100-1"});
    t1.merge(t2.snapshot(), 2, 1);
}

static void test_base_pair_in_sync() {
    TrackerState t1, t2;
    base_pair(t1, t2);
    CHECK(t1.snapshot() == t2.snapshot());
    CHECK(t1.is_online("bob") && t2.is_online("alice"));
}

// Tracker 2 crashes (its clients' connections die with it) and restarts with
// an empty state; one snapshot from tracker 1 makes it identical again.
static void test_restart_catch_up() {
    TrackerState t1, t2;
    base_pair(t1, t2);
    CHECK(t1.drop_sessions_owned_by(2) == 1);  // tracker 1 notices tracker 2 is gone: bob offline
    TrackerState fresh2;                       // tracker 2, restarted
    CHECK(fresh2.merge(t1.snapshot(), 1, 2));
    CHECK(t1.merge(fresh2.snapshot(), 2, 1));  // tracker 1 merges the (empty) snapshot too: no harm
    CHECK(fresh2.snapshot() == t1.snapshot());
    CHECK(fresh2.is_online("alice") && !fresh2.is_online("bob") && fresh2.group_owner("g") == "alice");
    FileDetails d;
    CHECK(fresh2.file_info("alice", "g", "a.txt", d) == Status::OK && d.seeder_addrs == Fields{"127.0.0.1:7001"});
    CHECK(!fresh2.merge("garbage\tnot a snapshot", 1, 2));  // malformed: rejected, nothing changed
    CHECK(fresh2.snapshot() == t1.snapshot());
}

// Both trackers change during a partition, then exchange snapshots: they
// must end up IDENTICAL, with conflicts resolved the same way on both.
static void test_partition_converges() {
    TrackerState t1, t2;
    base_pair(t1, t2);

    // --- partition: tracker 1's side ---
    op(t1, MsgType::CREATE_USER, {"xavier", D("xavier", "pw")});
    op(t1, MsgType::CREATE_GROUP, {"alice", "g_one"});
    op(t1, MsgType::JOIN_GROUP, {"xavier", "g"});
    op(t1, MsgType::ACCEPT_REQUEST, {"alice", "g", "xavier"});
    op(t1, MsgType::CREATE_USER, {"zed", D("zed", "pw_from_1")});   // conflict: same id, other password
    op(t1, MsgType::CREATE_GROUP, {"alice", "same"});              // conflict: same name, other owner
    // --- partition: tracker 2's side ---
    op(t2, MsgType::CREATE_USER, {"yara", D("yara", "pw")});
    op(t2, MsgType::CREATE_GROUP, {"bob", "g_two"});
    op(t2, MsgType::JOIN_GROUP, {"yara", "g"});
    op(t2, MsgType::ACCEPT_REQUEST, {"alice", "g", "yara"});
    op(t2, MsgType::CREATE_USER, {"zed", D("zed", "pw_from_2")});
    op(t2, MsgType::CREATE_GROUP, {"bob", "same"});
    op(t2, MsgType::JOIN_GROUP, {"bob", "g"});                     // pending request made on tracker 2
    op(t2, MsgType::UPLOAD_FILE, upload("bob", "g_two", "b.txt", "B"));

    // --- heal: each merges the other's snapshot ---
    const std::string s1 = t1.snapshot(), s2 = t2.snapshot();
    CHECK(t1.merge(s2, 2, 1));
    CHECK(t2.merge(s1, 1, 2));
    CHECK(t1.snapshot() == t2.snapshot());  // converged

    CHECK(t1.user_exists("xavier") && t1.user_exists("yara"));
    CHECK(t1.group_owner("g_one") == "alice" && t1.group_owner("g_two") == "bob");
    CHECK(t1.group_members("g") == (Fields{"alice", "xavier", "yara"}));  // tracker 1's order first
    CHECK(t1.group_owner("same") == "alice");                              // conflict -> tracker 1 wins
    // zed's password: tracker 1's version won on BOTH trackers.
    CHECK(op(t2, MsgType::LOGIN, {"zed", D("zed", "pw_from_2"), "127.0.0.1:7009", "2-1-9"}) == Status::AUTH_FAILED);
    CHECK(op(t2, MsgType::LOGIN, {"zed", D("zed", "pw_from_1"), "127.0.0.1:7009", "2-1-9"}) == Status::OK);
    Fields pending;
    CHECK(t1.list_requests("alice", "g", pending) == Status::OK && pending == Fields{"bob"});
    FileDetails d;
    CHECK(t1.file_info("bob", "g_two", "b.txt", d) == Status::OK && d.seeder_addrs == Fields{"127.0.0.1:7002"});
}

// Each tracker is the authority for its OWN sessions: if bob logged out on
// tracker 2 during the partition, tracker 1's stale copy must go.
static void test_session_authority() {
    TrackerState t1, t2;
    base_pair(t1, t2);
    op(t2, MsgType::JOIN_GROUP, {"bob", "g"});
    op(t2, MsgType::ACCEPT_REQUEST, {"alice", "g", "bob"});
    CHECK(op(t2, MsgType::UPLOAD_FILE, upload("bob", "g", "a.txt", "A")) == Status::OK);  // bob also seeds a.txt
    t1.merge(t2.snapshot(), 2, 1);
    FileDetails d;
    CHECK(t1.file_info("alice", "g", "a.txt", d) == Status::OK && d.seeder_addrs.size() == 2);

    op(t2, MsgType::LOGOUT, {"bob", "2-100-1"});  // bob logs out on tracker 2 (t1 doesn't hear: partition)
    CHECK(t1.is_online("bob"));
    t1.merge(t2.snapshot(), 2, 1);
    CHECK(!t1.is_online("bob"));  // tracker 2 is authoritative for bob's session
    CHECK(t1.file_info("alice", "g", "a.txt", d) == Status::OK && d.seeder_addrs == Fields{"127.0.0.1:7001"});
}

// The other tracker died: drop the sessions it owned and their seeder entries.
static void test_drop_sessions_owned_by() {
    TrackerState t1, t2;
    base_pair(t1, t2);
    op(t1, MsgType::CREATE_GROUP, {"bob", "gb"});
    op(t1, MsgType::UPLOAD_FILE, upload("bob", "gb", "only_bob.txt", "OB"));
    CHECK(t1.drop_sessions_owned_by(2) == 1);
    CHECK(!t1.is_online("bob") && t1.is_online("alice"));
    std::vector<FileSummary> files;
    CHECK(t1.list_files("bob", "gb", files) == Status::OK && files.empty());  // bob was its only seeder
    CHECK(t1.list_files("alice", "g", files) == Status::OK && files.size() == 1);
    CHECK(session_owner("1-5-6") == 1 && session_owner("2-5-6") == 2 && session_owner("s1") == 0);
}

static void test_op_encoding() {
    Op in{MsgType::UPLOAD_FILE, {"a", "g", "f", "10", "", "x"}};
    Op out;
    CHECK(decode_op(encode_op(in), out) && out.type == in.type && out.args == in.args);
    CHECK(!decode_op("", out) && !decode_op("abc\nx", out));
}

// ==================================================== Part B: live trackers

static uint16_t free_port() {
    int fd = listen_tcp({"127.0.0.1", 0});
    uint16_t p = local_port(fd);
    ::close(fd);
    return p;
}

struct SyncedTracker {
    int                            no;
    TrackerState                   state;
    std::unique_ptr<SyncLink>      sync;
    std::unique_ptr<TrackerServer> server;
    Endpoint                       ep;
    bool                           ok = false;

    SyncedTracker(int n, uint16_t my_port, uint16_t peer_port) : no(n) {
        sync = std::make_unique<SyncLink>(state, no, Endpoint{"127.0.0.1", peer_port}, false);
        server = std::make_unique<TrackerServer>(state, no, false, sync.get());
        std::string err;
        ok = server->start({"127.0.0.1", my_port}, err);
        ep = {"127.0.0.1", my_port};
        sync->start();
    }
    ~SyncedTracker() {
        server.reset();  // no more client changes (it uses sync)...
        sync.reset();    // ...then stop pushing
    }
    void partition(bool off) {
        sync->set_enabled(!off);
        server->set_sync_accept(!off);
    }
};

static bool wait_until(const std::function<bool()>& pred, int ms = 10000) {
    for (int waited = 0; waited < ms; waited += 20) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

static Status call(TrackerLink& link, MsgType t, const Fields& f) {
    Message rep;
    std::string err;
    return link.call(t, f, rep, err) ? rep.status : Status::INTERNAL_ERROR;
}

// A change on either tracker shows up on the other.
static void test_live_sync_both_ways() {
    const uint16_t p1 = free_port(), p2 = free_port();
    SyncedTracker t1(1, p1, p2), t2(2, p2, p1);
    CHECK(t1.ok && t2.ok);
    CHECK(wait_until([&] { return t1.sync->connected() && t2.sync->connected(); }));

    TrackerLink a({t1.ep, t2.ep}, 3000, false);  // a client of tracker 1
    TrackerLink b({t2.ep, t1.ep}, 3000, false);  // a client of tracker 2
    CHECK(call(a, MsgType::CREATE_USER, {"alice", "pw"}) == Status::OK);
    CHECK(call(a, MsgType::LOGIN, {"alice", "pw", "127.0.0.1:7001"}) == Status::OK);
    CHECK(call(a, MsgType::CREATE_GROUP, {"g1"}) == Status::OK);
    Fields up = upload("x", "g1", "f.txt", "F");
    up.erase(up.begin());
    CHECK(call(a, MsgType::UPLOAD_FILE, up) == Status::OK);

    CHECK(wait_until([&] { return t2.state.snapshot() == t1.state.snapshot(); }));
    CHECK(t2.state.group_owner("g1") == "alice" && t2.state.is_online("alice"));

    // Same password works on tracker 2; tracker 2's own client changes reach tracker 1.
    CHECK(call(b, MsgType::LOGIN, {"alice", "pw", "127.0.0.1:7999"}) == Status::ALREADY_LOGGED_IN);  // one login
    CHECK(call(b, MsgType::CREATE_USER, {"bob", "pw"}) == Status::OK);
    CHECK(call(b, MsgType::LOGIN, {"bob", "pw", "127.0.0.1:7002"}) == Status::OK);
    CHECK(call(b, MsgType::JOIN_GROUP, {"g1"}) == Status::OK);
    // Replication is asynchronous (eventually consistent): the request made
    // on tracker 2 reaches tracker 1 a moment later. Wait for it, as a real
    // owner would only see it once it is listed.
    CHECK(wait_until([&] {
        Fields p;
        return t1.state.list_requests("alice", "g1", p) == Status::OK && p == Fields{"bob"};
    }));
    CHECK(call(a, MsgType::ACCEPT_REQUEST, {"g1", "bob"}) == Status::OK);  // accepted on tracker 1
    CHECK(wait_until([&] { return t1.state.snapshot() == t2.state.snapshot(); }));
    CHECK(t2.state.group_members("g1") == (Fields{"alice", "bob"}));
    Message rep;
    std::string err;
    CHECK(b.call(MsgType::LIST_FILES, {"g1"}, rep, err) && rep.status == Status::OK &&
          split_fields(rep.payload) == Fields{"f.txt\t100\t1"});
}

// Tracker 2 dies; changes continue on tracker 1; tracker 2 restarts EMPTY
// and catches up from tracker 1's snapshot.
static void test_restart_catches_up_live() {
    const uint16_t p1 = free_port(), p2 = free_port();
    SyncedTracker t1(1, p1, p2);
    auto t2 = std::make_unique<SyncedTracker>(2, p2, p1);
    TrackerLink a({t1.ep, t2->ep}, 3000, false);
    CHECK(call(a, MsgType::CREATE_USER, {"alice", "pw"}) == Status::OK);
    CHECK(wait_until([&] { return t2->state.user_exists("alice"); }));

    t2.reset();  // tracker 2 goes down
    CHECK(call(a, MsgType::CREATE_USER, {"carl", "pw"}) == Status::OK);  // only tracker 1 sees this
    CHECK(call(a, MsgType::LOGIN, {"carl", "pw", "127.0.0.1:7003"}) == Status::OK);
    CHECK(call(a, MsgType::CREATE_GROUP, {"while_down"}) == Status::OK);

    t2 = std::make_unique<SyncedTracker>(2, p2, p1);  // back, with an empty state
    CHECK(t2->ok);
    CHECK(wait_until([&] { return t2->state.snapshot() == t1.state.snapshot(); }));
    CHECK(t2->state.user_exists("carl") && t2->state.group_owner("while_down") == "carl");
}

// "sync off" on tracker 1 = partition. Both sides change; after "sync on"
// both have everything and are identical.
static void test_partition_and_heal_live() {
    const uint16_t p1 = free_port(), p2 = free_port();
    SyncedTracker t1(1, p1, p2), t2(2, p2, p1);
    CHECK(wait_until([&] { return t1.sync->connected() && t2.sync->connected(); }));
    TrackerLink a({t1.ep, t2.ep}, 3000, false), b({t2.ep, t1.ep}, 3000, false);
    call(a, MsgType::CREATE_USER, {"alice", "pw"});
    call(a, MsgType::LOGIN, {"alice", "pw", "127.0.0.1:7001"});
    CHECK(wait_until([&] { return t2.state.is_online("alice"); }));

    t1.partition(true);
    // Each side notices the link is gone and drops the other side's sessions.
    CHECK(wait_until([&] { return !t1.sync->connected() && !t2.sync->connected(); }));
    CHECK(wait_until([&] { return !t2.state.is_online("alice"); }));

    CHECK(call(a, MsgType::CREATE_GROUP, {"left_side"}) == Status::OK);
    CHECK(call(b, MsgType::CREATE_USER, {"bob", "pw"}) == Status::OK);
    CHECK(call(b, MsgType::LOGIN, {"bob", "pw", "127.0.0.1:7002"}) == Status::OK);
    CHECK(call(b, MsgType::CREATE_GROUP, {"right_side"}) == Status::OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(t1.state.group_owner("right_side").empty() && t2.state.group_owner("left_side").empty());

    t1.partition(false);
    CHECK(wait_until([&] { return t1.state.snapshot() == t2.state.snapshot(); }));
    CHECK(t1.state.group_owner("right_side") == "bob" && t2.state.group_owner("left_side") == "alice");
    CHECK(t1.state.is_online("bob") && t2.state.is_online("alice"));
}

// A client seeding a file is on tracker 1; tracker 1 dies. Its next command
// fails over to tracker 2, logs in again and re-announces its files, so it
// is listed as a seeder on tracker 2 again.
static void test_client_failover_reannounce() {
    const uint16_t p1 = free_port(), p2 = free_port();
    auto t1 = std::make_unique<SyncedTracker>(1, p1, p2);
    SyncedTracker t2(2, p2, p1);
    CHECK(wait_until([&] { return t1->sync->connected() && t2.sync->connected(); }));

    SharedFiles shared;
    TrackerLink link({t1->ep, t2.ep}, 3000, false);
    link.set_reannounce([&shared] {
        std::vector<Fields> out;
        for (const auto& f : shared.all())
            out.push_back({f.group, f.meta.name, std::to_string(f.meta.size), f.meta.file_hash,
                           join_hashes(f.meta.piece_hashes)});
        return out;
    });
    call(link, MsgType::CREATE_USER, {"alice", "pw"});
    CHECK(call(link, MsgType::LOGIN, {"alice", "pw", "127.0.0.1:7001"}) == Status::OK);
    link.remember_login("alice", "pw", "127.0.0.1:7001");
    call(link, MsgType::CREATE_GROUP, {"g"});
    FileMeta m;
    m.name = "f.txt";
    m.size = 100;
    m.file_hash = sha1_hex("F");
    m.piece_hashes = {sha1_hex("F0")};
    CHECK(call(link, MsgType::UPLOAD_FILE, {"g", m.name, "100", m.file_hash, join_hashes(m.piece_hashes)}) ==
          Status::OK);
    shared.add({"g", "/tmp/whatever", m, {}});
    FileDetails d;
    CHECK(wait_until([&] { return t2.state.file_info("alice", "g", "f.txt", d) == Status::OK; }));

    t1.reset();  // tracker 1 crashes
    // Tracker 2 sees the link drop and forgets tracker 1's sessions (alice's) -> file unlisted.
    CHECK(wait_until([&] { return !t2.state.is_online("alice"); }));
    std::vector<FileSummary> files;
    CHECK(t2.state.list_files("alice", "g", files) == Status::OK && files.empty());

    // The client's next command: failover + re-login + re-announce, transparently.
    CHECK(call(link, MsgType::LIST_GROUPS, {}) == Status::OK);
    CHECK(link.connected_tracker() == 2);
    CHECK(t2.state.is_online("alice"));
    CHECK(t2.state.file_info("alice", "g", "f.txt", d) == Status::OK && d.seeder_addrs == Fields{"127.0.0.1:7001"});
}

int main() {
    ignore_sigpipe();
    static const TestCase tests[] = {
        {"snapshot exchange builds an identical pair", test_base_pair_in_sync},
        {"restart: empty tracker catches up from one snapshot", test_restart_catch_up},
        {"partition: both sides change, merge converges", test_partition_converges},
        {"each tracker is authoritative for its own sessions", test_session_authority},
        {"drop sessions of an unreachable tracker", test_drop_sessions_owned_by},
        {"op encoding", test_op_encoding},
        {"live: changes flow both ways", test_live_sync_both_ways},
        {"live: restarted tracker catches up", test_restart_catches_up_live},
        {"live: partition (sync off) then heal", test_partition_and_heal_live},
        {"live: client failover re-announces its files", test_client_failover_reannounce},
    };
    return run_tests(tests);
}
