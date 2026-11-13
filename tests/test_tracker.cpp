// tests/test_tracker.cpp - tracker rules (TrackerState) and a real tracker
// server running in-process on a kernel-chosen port.
#include <atomic>
#include <chrono>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "file_meta.h"
#include "net.h"
#include "protocol.h"
#include "server.h"
#include "sha1.h"
#include "state.h"
#include "test_util.h"

using namespace p2p;
using Fields = std::vector<std::string>;

// ------------------------------------------------------------ state tests

static Status op(TrackerState& st, MsgType t, Fields args) { return st.apply({t, std::move(args)}); }
static std::string D(const std::string& u, const std::string& p) { return password_digest(u, p); }

static void add_user(TrackerState& st, const std::string& u) {
    CHECK(op(st, MsgType::CREATE_USER, {u, D(u, "pw")}) == Status::OK);
}

static void test_validation() {
    CHECK(valid_id("alice") && valid_id("A_b-c.9") && valid_id(std::string(64, 'x')));
    CHECK(!valid_id("") && !valid_id(std::string(65, 'x')) && !valid_id("a b") && !valid_id("a/b"));
    CHECK(valid_password("p@ss!") && !valid_password("") && !valid_password("a b"));
}

static void test_users() {
    TrackerState st;
    add_user(st, "alice");
    CHECK(op(st, MsgType::CREATE_USER, {"alice", D("alice", "x")}) == Status::USER_EXISTS);
    CHECK(op(st, MsgType::CREATE_USER, {"bad id", "d"}) == Status::BAD_REQUEST);
    CHECK(op(st, MsgType::CREATE_USER, {"onlyone"}) == Status::BAD_REQUEST);
    CHECK(st.user_exists("alice") && !st.user_exists("bob"));
}

static void test_login_rules() {
    TrackerState st;
    add_user(st, "alice");
    CHECK(op(st, MsgType::LOGIN, {"alice", D("alice", "wrong"), "127.0.0.1:7001", "s1"}) == Status::AUTH_FAILED);
    CHECK(op(st, MsgType::LOGIN, {"ghost", D("ghost", "pw"), "127.0.0.1:7001", "s1"}) == Status::AUTH_FAILED);
    CHECK(op(st, MsgType::LOGIN, {"alice", D("alice", "pw"), "127.0.0.1:7001", "s1"}) == Status::OK);
    CHECK(st.is_online("alice"));
    // a second client (different address) cannot steal the session
    CHECK(op(st, MsgType::LOGIN, {"alice", D("alice", "pw"), "127.0.0.1:7002", "s2"}) == Status::ALREADY_LOGGED_IN);
    // the same client reconnecting (e.g. after tracker failover) may log in again
    CHECK(op(st, MsgType::LOGIN, {"alice", D("alice", "pw"), "127.0.0.1:7001", "s3"}) == Status::OK);
    // the OLD session's late disconnect must not log out the new one
    CHECK(op(st, MsgType::LOGOUT, {"alice", "s1"}) == Status::NOT_LOGGED_IN);
    CHECK(st.is_online("alice"));
    CHECK(op(st, MsgType::LOGOUT, {"alice", "s3"}) == Status::OK);
    CHECK(!st.is_online("alice"));
}

static void test_group_rules() {
    TrackerState st;
    for (auto u : {"alice", "bob", "carol"}) add_user(st, u);
    CHECK(op(st, MsgType::CREATE_GROUP, {"alice", "g1"}) == Status::OK);
    CHECK(op(st, MsgType::CREATE_GROUP, {"bob", "g1"}) == Status::GROUP_EXISTS);
    CHECK(op(st, MsgType::CREATE_GROUP, {"ghost", "g2"}) == Status::NO_SUCH_USER);
    CHECK(op(st, MsgType::CREATE_GROUP, {"alice", "bad name"}) == Status::BAD_REQUEST);
    CHECK(st.group_owner("g1") == "alice");

    CHECK(op(st, MsgType::JOIN_GROUP, {"bob", "g1"}) == Status::OK);
    CHECK(op(st, MsgType::JOIN_GROUP, {"bob", "g1"}) == Status::REQUEST_PENDING);
    CHECK(op(st, MsgType::JOIN_GROUP, {"alice", "g1"}) == Status::ALREADY_MEMBER);
    CHECK(op(st, MsgType::JOIN_GROUP, {"bob", "nope"}) == Status::NO_SUCH_GROUP);

    Fields pending;
    CHECK(st.list_requests("bob", "g1", pending) == Status::NOT_OWNER);
    CHECK(st.list_requests("alice", "g1", pending) == Status::OK && pending == Fields{"bob"});
    CHECK(st.list_requests("alice", "nope", pending) == Status::NO_SUCH_GROUP);

    CHECK(op(st, MsgType::ACCEPT_REQUEST, {"bob", "g1", "bob"}) == Status::NOT_OWNER);
    CHECK(op(st, MsgType::ACCEPT_REQUEST, {"alice", "g1", "carol"}) == Status::NO_SUCH_REQUEST);
    CHECK(op(st, MsgType::ACCEPT_REQUEST, {"alice", "g1", "bob"}) == Status::OK);
    CHECK(st.group_members("g1") == (Fields{"alice", "bob"}));
    CHECK(op(st, MsgType::JOIN_GROUP, {"bob", "g1"}) == Status::ALREADY_MEMBER);

    CHECK(op(st, MsgType::LEAVE_GROUP, {"carol", "g1"}) == Status::NOT_MEMBER);
    CHECK(op(st, MsgType::LEAVE_GROUP, {"bob", "nope"}) == Status::NO_SUCH_GROUP);
    CHECK(op(st, MsgType::LEAVE_GROUP, {"bob", "g1"}) == Status::OK);
    CHECK(st.group_members("g1") == Fields{"alice"});
}

// Owner leaves -> oldest remaining member becomes owner; last one out deletes the group.
static void test_owner_handover() {
    TrackerState st;
    for (auto u : {"alice", "bob", "carol", "dave"}) add_user(st, u);
    op(st, MsgType::CREATE_GROUP, {"alice", "g"});
    for (auto u : {"bob", "carol"}) {
        op(st, MsgType::JOIN_GROUP, {u, "g"});
        op(st, MsgType::ACCEPT_REQUEST, {"alice", "g", u});
    }
    op(st, MsgType::JOIN_GROUP, {"dave", "g"});  // left pending
    CHECK(op(st, MsgType::LEAVE_GROUP, {"alice", "g"}) == Status::OK);
    CHECK(st.group_owner("g") == "bob");
    CHECK(op(st, MsgType::LEAVE_GROUP, {"bob", "g"}) == Status::OK);
    CHECK(st.group_owner("g") == "carol");
    Fields pending;
    CHECK(st.list_requests("carol", "g", pending) == Status::OK && pending == Fields{"dave"});
    CHECK(op(st, MsgType::LEAVE_GROUP, {"carol", "g"}) == Status::OK);
    CHECK(st.group_owner("g").empty() && st.list_groups().empty());
    CHECK(op(st, MsgType::JOIN_GROUP, {"dave", "g"}) == Status::NO_SUCH_GROUP);
}

static void test_list_groups_sorted() {
    TrackerState st;
    add_user(st, "alice");
    for (auto g : {"zeta", "alpha", "mid"}) op(st, MsgType::CREATE_GROUP, {"alice", g});
    CHECK(st.list_groups() == (Fields{"alpha", "mid", "zeta"}));
}

// Upload op args for a fake file of `size` bytes whose content is "tagged" by `tag`.
static Fields upload_args(const std::string& actor, const std::string& group, const std::string& name,
                          uint64_t size, const std::string& tag) {
    std::vector<std::string> pieces;
    for (uint64_t i = 0; i < piece_count(size); ++i) pieces.push_back(sha1_hex(tag + std::to_string(i)));
    return {actor, group, name, std::to_string(size), sha1_hex(tag), join_hashes(pieces)};
}

static void test_password_digest_is_sha1() {
    CHECK(password_digest("alice", "pw") == sha1_hex("alice:pw"));
    CHECK(password_digest("alice", "pw") != password_digest("bob", "pw"));  // user id acts as salt
}

static void test_file_rules() {
    TrackerState st;
    for (auto u : {"alice", "bob", "carol"}) add_user(st, u);
    op(st, MsgType::LOGIN, {"alice", D("alice", "pw"), "127.0.0.1:7001", "s1"});
    op(st, MsgType::LOGIN, {"bob", D("bob", "pw"), "127.0.0.1:7002", "s2"});
    op(st, MsgType::CREATE_GROUP, {"alice", "g"});
    op(st, MsgType::JOIN_GROUP, {"bob", "g"});
    op(st, MsgType::ACCEPT_REQUEST, {"alice", "g", "bob"});

    const uint64_t size = kPieceSize * 2 + 5;  // 3 pieces
    CHECK(op(st, MsgType::UPLOAD_FILE, upload_args("carol", "g", "f.bin", size, "A")) == Status::NOT_MEMBER);
    CHECK(op(st, MsgType::UPLOAD_FILE, upload_args("alice", "nope", "f.bin", size, "A")) == Status::NO_SUCH_GROUP);
    Fields wrong_count = upload_args("alice", "g", "f.bin", size, "A");
    wrong_count[3] = std::to_string(kPieceSize);  // 1 piece declared, 3 hashes sent
    CHECK(op(st, MsgType::UPLOAD_FILE, wrong_count) == Status::BAD_REQUEST);
    Fields bad_name = upload_args("alice", "g", "../etc/passwd", size, "A");
    CHECK(op(st, MsgType::UPLOAD_FILE, bad_name) == Status::BAD_REQUEST);
    Fields zero = upload_args("alice", "g", "z.bin", 0, "A");
    CHECK(op(st, MsgType::UPLOAD_FILE, zero) == Status::BAD_REQUEST);

    CHECK(op(st, MsgType::UPLOAD_FILE, upload_args("alice", "g", "f.bin", size, "A")) == Status::OK);
    CHECK(op(st, MsgType::UPLOAD_FILE, upload_args("alice", "g", "f.bin", size, "A")) == Status::OK);  // idempotent
    CHECK(op(st, MsgType::UPLOAD_FILE, upload_args("bob", "g", "f.bin", size, "B")) == Status::FILE_CONFLICT);
    CHECK(op(st, MsgType::UPLOAD_FILE, upload_args("bob", "g", "f.bin", size, "A")) == Status::OK);  // 2nd seeder

    std::vector<FileSummary> files;
    CHECK(st.list_files("carol", "g", files) == Status::NOT_MEMBER);
    CHECK(st.list_files("bob", "g", files) == Status::OK && files.size() == 1);
    CHECK(files[0].name == "f.bin" && files[0].size == size && files[0].seeders == 2);

    FileDetails d;
    CHECK(st.file_info("bob", "g", "missing", d) == Status::NO_SUCH_FILE);
    CHECK(st.file_info("bob", "g", "f.bin", d) == Status::OK);
    CHECK(d.size == size && d.file_hash == sha1_hex("A") && d.piece_hashes.size() == 3 * kHashHexLen);
    CHECK(d.seeder_addrs == (Fields{"127.0.0.1:7001", "127.0.0.1:7002"}));

    // Logout removes the user as a seeder everywhere ("stop sharing").
    CHECK(op(st, MsgType::LOGOUT, {"alice", "s1"}) == Status::OK);
    CHECK(st.file_info("bob", "g", "f.bin", d) == Status::OK && d.seeder_addrs == Fields{"127.0.0.1:7002"});

    CHECK(op(st, MsgType::STOP_SHARE, {"alice", "g", "f.bin"}) == Status::NOT_SHARING);
    CHECK(op(st, MsgType::STOP_SHARE, {"bob", "g", "nope"}) == Status::NO_SUCH_FILE);
    CHECK(op(st, MsgType::STOP_SHARE, {"bob", "g", "f.bin"}) == Status::OK);
    CHECK(st.list_files("bob", "g", files) == Status::OK && files.empty());  // last seeder gone -> file gone

    // Leaving the group drops your shares in it.
    CHECK(op(st, MsgType::UPLOAD_FILE, upload_args("bob", "g", "h.bin", 10, "H")) == Status::OK);
    CHECK(op(st, MsgType::LEAVE_GROUP, {"bob", "g"}) == Status::OK);
    CHECK(st.list_files("alice", "g", files) == Status::OK && files.empty());
}

// ------------------------------------------------------------ server tests

// A minimal blocking client for talking to the server in tests.
struct TestClient {
    int fd = -1;
    explicit TestClient(uint16_t port) {
        fd = connect_tcp({"127.0.0.1", port}, 1000);
        if (fd >= 0) set_io_timeout(fd, 5000);
    }
    ~TestClient() { close(); }
    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    // Send a request, return the reply (status INTERNAL_ERROR if the I/O failed).
    Message call(MsgType t, const Fields& f = {}) {
        Message rep;
        rep.status = Status::INTERNAL_ERROR;
        if (send_msg(fd, {t, Status::OK, pack_fields(f)}) != IoResult::OK) return rep;
        if (recv_msg(fd, rep) != IoResult::OK) rep.status = Status::INTERNAL_ERROR;
        return rep;
    }
    Status st(MsgType t, const Fields& f = {}) { return call(t, f).status; }
};

struct RunningTracker {
    TrackerState  state;
    TrackerServer server{state, 1, /*verbose=*/false};
    bool          ok = false;
    RunningTracker() {
        std::string err;
        ok = server.start({"127.0.0.1", 0}, err);
    }
};

static void test_server_flow() {
    RunningTracker t;
    CHECK(t.ok);
    TestClient a(t.server.port()), b(t.server.port());
    CHECK(a.fd >= 0 && b.fd >= 0);

    Message pong = a.call(MsgType::PING);
    CHECK(pong.type == reply_type(MsgType::PING) && pong.status == Status::OK);
    CHECK(a.st(MsgType::LIST_GROUPS) == Status::NOT_LOGGED_IN);
    CHECK(a.st(MsgType::CREATE_GROUP, {"g1"}) == Status::NOT_LOGGED_IN);

    CHECK(a.st(MsgType::CREATE_USER, {"alice", "pw1"}) == Status::OK);
    CHECK(a.st(MsgType::CREATE_USER, {"bob", "pw2"}) == Status::OK);
    CHECK(a.st(MsgType::CREATE_USER, {"bad/id", "pw"}) == Status::BAD_REQUEST);
    CHECK(a.st(MsgType::CREATE_USER, {"onlyname"}) == Status::BAD_REQUEST);

    CHECK(a.st(MsgType::LOGIN, {"alice", "nope", "127.0.0.1:7001"}) == Status::AUTH_FAILED);
    CHECK(a.st(MsgType::LOGIN, {"alice", "pw1", "not-an-address"}) == Status::BAD_REQUEST);
    CHECK(a.st(MsgType::LOGIN, {"alice", "pw1", "127.0.0.1:7001"}) == Status::OK);
    CHECK(a.st(MsgType::LOGIN, {"alice", "pw1", "127.0.0.1:7001"}) == Status::ALREADY_LOGGED_IN);
    CHECK(a.st(MsgType::CREATE_GROUP, {"g1"}) == Status::OK);

    CHECK(b.st(MsgType::LOGIN, {"alice", "pw1", "127.0.0.1:7002"}) == Status::ALREADY_LOGGED_IN);
    CHECK(b.st(MsgType::LOGIN, {"bob", "pw2", "127.0.0.1:7002"}) == Status::OK);
    CHECK(b.st(MsgType::JOIN_GROUP, {"g1"}) == Status::OK);
    CHECK(b.st(MsgType::LIST_REQUESTS, {"g1"}) == Status::NOT_OWNER);

    Message req = a.call(MsgType::LIST_REQUESTS, {"g1"});
    CHECK(req.status == Status::OK && split_fields(req.payload) == Fields{"bob"});
    CHECK(a.st(MsgType::ACCEPT_REQUEST, {"g1", "bob"}) == Status::OK);
    Message groups = b.call(MsgType::LIST_GROUPS);
    CHECK(groups.status == Status::OK && split_fields(groups.payload) == Fields{"g1"});

    // Files over the wire: upload by alice, list + file info by bob.
    Fields up = upload_args("x", "g1", "doc.txt", 1000, "D");
    up.erase(up.begin());  // the client doesn't send the actor; the tracker knows who you are
    CHECK(a.st(MsgType::UPLOAD_FILE, up) == Status::OK);
    Message lf = b.call(MsgType::LIST_FILES, {"g1"});
    CHECK(lf.status == Status::OK && split_fields(lf.payload) == Fields{"doc.txt\t1000\t1"});
    Message fi = b.call(MsgType::FILE_INFO, {"g1", "doc.txt"});
    Fields info = split_fields(fi.payload);
    CHECK(fi.status == Status::OK && info.size() == 4 && info[0] == "1000" && info[1] == sha1_hex("D") &&
          info[3] == "127.0.0.1:7001");
    CHECK(b.st(MsgType::STOP_SHARE, {"g1", "doc.txt"}) == Status::NOT_SHARING);
    CHECK(a.st(MsgType::STOP_SHARE, {"g1", "doc.txt"}) == Status::OK);

    CHECK(b.st(MsgType::LEAVE_GROUP, {"g1"}) == Status::OK);
    CHECK(a.st(MsgType::LOGOUT) == Status::OK);
    CHECK(a.st(MsgType::LOGOUT) == Status::NOT_LOGGED_IN);
    CHECK(a.st(MsgType::LIST_GROUPS) == Status::NOT_LOGGED_IN);
    CHECK(!t.state.is_online("alice") && t.state.is_online("bob"));
}

// Closing the connection without "logout" must end the session.
static void test_auto_logout_on_disconnect() {
    RunningTracker t;
    TestClient a(t.server.port());
    a.st(MsgType::CREATE_USER, {"carol", "pw"});
    CHECK(a.st(MsgType::LOGIN, {"carol", "pw", "127.0.0.1:7001"}) == Status::OK);
    a.close();  // simulate a crash: no logout message

    // The server notices asynchronously; retry for up to 2 s.
    TestClient b(t.server.port());
    Status s = Status::INTERNAL_ERROR;
    for (int i = 0; i < 100; ++i) {
        s = b.st(MsgType::LOGIN, {"carol", "pw", "127.0.0.1:7009"});  // different address
        if (s == Status::OK) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(s == Status::OK);
}

static void test_bad_clients() {
    RunningTracker t;
    // Garbage instead of a frame: the server drops the connection.
    TestClient junk(t.server.port());
    CHECK(send_all(junk.fd, "GET / HTTP/1.1\r\n", 16) == IoResult::OK);
    Message m;
    CHECK(recv_msg(junk.fd, m) == IoResult::CLOSED);

    // Unknown message type: error reply, connection stays usable.
    TestClient c(t.server.port());
    CHECK(c.st(static_cast<MsgType>(0x7F)) == Status::UNKNOWN_TYPE);
    CHECK(c.st(MsgType::PING) == Status::OK);
}

// Many clients at once. Run under ThreadSanitizer this proves the shared
// state has no data races; the contested group proves check+insert is atomic.
static void test_concurrent_clients() {
    RunningTracker t;
    const int kThreads = 8, kPerThread = 20;
    std::atomic<int> created{0}, contested_wins{0}, errors{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            TestClient c(t.server.port());
            std::string me = "user" + std::to_string(i);
            if (c.st(MsgType::CREATE_USER, {me, "pw"}) != Status::OK ||
                c.st(MsgType::LOGIN, {me, "pw", "127.0.0.1:" + std::to_string(7100 + i)}) != Status::OK) {
                ++errors;
                return;
            }
            for (int j = 0; j < kPerThread; ++j) {
                if (c.st(MsgType::CREATE_GROUP, {me + "_g" + std::to_string(j)}) == Status::OK) ++created;
                else ++errors;
            }
            Status s = c.st(MsgType::CREATE_GROUP, {"contested"});
            if (s == Status::OK) ++contested_wins;
            else if (s != Status::GROUP_EXISTS) ++errors;
        });
    }
    for (auto& th : threads) th.join();
    CHECK(errors == 0);
    CHECK(created == kThreads * kPerThread);
    CHECK(contested_wins == 1);  // exactly one creator, never two
    CHECK(t.state.list_groups().size() == static_cast<size_t>(kThreads * kPerThread + 1));
}

// stop() must finish promptly even with idle clients connected, and every
// client must see its connection closed.
static void test_stop_with_connected_clients() {
    auto t = std::make_unique<RunningTracker>();
    TestClient a(t->server.port()), b(t->server.port()), c(t->server.port());
    a.st(MsgType::CREATE_USER, {"dan", "pw"});
    CHECK(a.st(MsgType::LOGIN, {"dan", "pw", "127.0.0.1:7001"}) == Status::OK);
    CHECK(b.st(MsgType::PING) == Status::OK);  // make sure b is accepted

    auto t0 = std::chrono::steady_clock::now();
    t->server.stop();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);
    Message m;
    CHECK(recv_msg(a.fd, m) == IoResult::CLOSED);
    CHECK(recv_msg(b.fd, m) == IoResult::CLOSED);
    CHECK(!t->state.is_online("dan"));  // connection ended -> session ended
    t.reset();  // destructor calls stop() again: must be harmless
}

int main() {
    ignore_sigpipe();
    static const TestCase tests[] = {
        {"id/password validation", test_validation},
        {"users", test_users},
        {"login rules", test_login_rules},
        {"group rules", test_group_rules},
        {"owner handover", test_owner_handover},
        {"list_groups sorted", test_list_groups_sorted},
        {"password digest = salted sha1", test_password_digest_is_sha1},
        {"file sharing rules", test_file_rules},
        {"server: full user/group flow", test_server_flow},
        {"server: auto logout on disconnect", test_auto_logout_on_disconnect},
        {"server: garbage + unknown type", test_bad_clients},
        {"server: 8 concurrent clients", test_concurrent_clients},
        {"server: stop with clients connected", test_stop_with_connected_clients},
    };
    return run_tests(tests);
}
