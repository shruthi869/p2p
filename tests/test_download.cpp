// tests/test_download.cpp - peer server + downloader against a real
// in-process tracker and real in-process peers (each with its own peer
// server, tracker link and download manager, exactly like a client process).
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "downloads.h"
#include "file_meta.h"
#include "net.h"
#include "peer_server.h"
#include "server.h"
#include "sha1.h"
#include "shared_files.h"
#include "state.h"
#include "test_util.h"
#include "tracker_link.h"

using namespace p2p;

static uint16_t dead_port() {
    int fd = listen_tcp({"127.0.0.1", 0});
    uint16_t p = local_port(fd);
    ::close(fd);
    return p;
}

struct TrackerFixture {
    TrackerState  state;
    TrackerServer server{state, 1, false};
    Endpoint      ep;
    TrackerFixture() {
        std::string err;
        server.start({"127.0.0.1", 0}, err);
        ep = {"127.0.0.1", server.port()};
    }
};

static Endpoint start_peer_server(PeerServer& s) {
    std::string err;
    s.start({"127.0.0.1", 0}, err);
    return {"127.0.0.1", s.port()};
}

// One simulated client.
struct Peer {
    SharedFiles     shared;
    PeerServer      server;
    Endpoint        ep;
    TrackerLink     link;
    DownloadManager dm;

    explicit Peer(const Endpoint& tracker)
        : server(shared), ep(start_peer_server(server)), link({tracker, {"127.0.0.1", dead_port()}}, 5000, false),
          dm(link, shared, ep, false) {}

    Status call(MsgType t, const std::vector<std::string>& f) {
        Message rep;
        std::string err;
        return link.call(t, f, rep, err) ? rep.status : Status::INTERNAL_ERROR;
    }
    void login(const std::string& user) {
        call(MsgType::CREATE_USER, {user, "pw"});
        CHECK(call(MsgType::LOGIN, {user, "pw", ep.str()}) == Status::OK);
        link.remember_login(user, "pw", ep.str());
    }
    bool share(const std::string& group, const std::string& path) {
        FileMeta m;
        std::string err;
        if (!hash_file(path, m, err)) return false;
        if (call(MsgType::UPLOAD_FILE, {group, m.name, std::to_string(m.size), m.file_hash,
                                        join_hashes(m.piece_hashes)}) != Status::OK)
            return false;
        shared.add({group, path, m});
        return true;
    }
    // Wait until the download of `name` is no longer running.
    DownloadStatus wait(const std::string& name) {
        for (int i = 0; i < 400; ++i) {
            for (const auto& d : dm.snapshot())
                if (d.name == name && d.state != DownloadStatus::RUNNING) return d;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        DownloadStatus timeout;
        timeout.error = "timed out";
        return timeout;
    }
};

// A scratch directory under /tmp. At the end: files first, then
// subdirectories, then the directory itself (rmdir only works on empty dirs).
struct TempDir {
    std::string path;
    std::vector<std::string> files, dirs;
    TempDir() {
        char tmpl[] = "/tmp/p2p_dl_XXXXXX";
        if (!::mkdtemp(tmpl)) std::abort();
        path = tmpl;
    }
    ~TempDir() {
        for (const auto& f : files) ::unlink(f.c_str());
        for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) ::rmdir(d->c_str());
        ::rmdir(path.c_str());
    }
    std::string sub(const std::string& name) {  // a subdirectory
        std::string p = path + "/" + name;
        ::mkdir(p.c_str(), 0755);
        dirs.push_back(p);
        return p;
    }
    std::string write(const std::string& name, const std::string& content) {
        std::string p = path + "/" + name;
        int fd = ::open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0 || !pwrite_full(fd, content.data(), content.size(), 0)) std::abort();
        ::close(fd);
        files.push_back(p);
        return p;
    }
    void track(const std::string& p) { files.push_back(p); }
};

static std::string random_bytes(size_t n, unsigned seed) {
    std::string s(n, '\0');
    unsigned x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        s[i] = static_cast<char>(x >> 16);
    }
    return s;
}

static std::string read_all(const std::string& path) {
    std::string out;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return "<missing>";
    char buf[65536];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return out;
}

static bool exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

// alice creates group g and shares files; the other peers join g.
static void make_group(Peer& owner, std::vector<Peer*> members) {
    CHECK(owner.call(MsgType::CREATE_GROUP, {"g"}) == Status::OK);
    for (Peer* m : members) {
        m->call(MsgType::JOIN_GROUP, {"g"});
    }
}
static void accept_all(Peer& owner, const std::vector<std::string>& users) {
    for (const auto& u : users) CHECK(owner.call(MsgType::ACCEPT_REQUEST, {"g", u}) == Status::OK);
}

// ------------------------------------------------------------------ tests

static void test_basic_download() {
    TrackerFixture t;
    TempDir dir;
    Peer alice(t.ep), bob(t.ep);
    alice.login("alice");
    bob.login("bob");
    make_group(alice, {&bob});
    accept_all(alice, {"bob"});

    const std::string content = random_bytes(kPieceSize * 2 + 12345, 1);  // 3 pieces
    const std::string src = dir.write("data.bin", content);
    CHECK(alice.share("g", src));

    const std::string out_dir = dir.sub("bob_dl");
    std::string err;
    CHECK(bob.dm.start("g", "data.bin", out_dir, err));
    DownloadStatus d = bob.wait("data.bin");
    dir.track(out_dir + "/data.bin");
    CHECK(d.state == DownloadStatus::COMPLETED);
    CHECK(d.pieces_done == 3 && d.pieces_total == 3 && d.bad_pieces == 0);
    CHECK(read_all(out_dir + "/data.bin") == content);
    CHECK(!exists(out_dir + "/data.bin.part"));

    // The downloader is now a seeder too.
    FileDetails info;
    CHECK(t.state.file_info("alice", "g", "data.bin", info) == Status::OK);
    CHECK(info.seeder_addrs.size() == 2);
    SharedFile sf;
    CHECK(bob.shared.find("g", "data.bin", sf) && sf.path == out_dir + "/data.bin");

    // Errors that are caught before any thread starts.
    CHECK(!bob.dm.start("g", "data.bin", out_dir, err) && err.find("already exists") != std::string::npos);
    CHECK(!bob.dm.start("g", "nope.bin", out_dir, err) && err == "no such file in this group");
    CHECK(!bob.dm.start("g", "data.bin", src, err) && err.find("not a directory") != std::string::npos);
    const std::string alice_dir = dir.sub("alice_dl");
    // alice -> only other seeder is bob now; stop bob sharing first to leave alice alone.
    CHECK(bob.call(MsgType::STOP_SHARE, {"g", "data.bin"}) == Status::OK);
    CHECK(!alice.dm.start("g", "data.bin", alice_dir, err) && err.find("no other peer") != std::string::npos);
}

// Peer A's copy is corrupted on disk AFTER it was shared. The downloader
// must detect the bad piece by its SHA-1 and fetch it from peer B instead.
static void test_corrupted_piece_refetched() {
    TrackerFixture t;
    TempDir dir;
    Peer a(t.ep), b(t.ep), c(t.ep);
    a.login("a");
    b.login("b");
    c.login("c");
    make_group(a, {&b, &c});
    accept_all(a, {"b", "c"});

    const std::string content = random_bytes(kPieceSize * 3 + 7, 2);  // 4 pieces
    const std::string pa = dir.write("a_copy.bin", content);
    CHECK(a.share("g", pa));
    // b shares the same content under the same name (different directory)
    const std::string bdir = dir.sub("b");
    const std::string pb = bdir + "/a_copy.bin";
    {
        int fd = ::open(pb.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        pwrite_full(fd, content.data(), content.size(), 0);
        ::close(fd);
        dir.track(pb);
    }
    CHECK(b.share("g", pb));

    // Corrupt piece 1 of a's copy (a is the first seeder, so it is asked first).
    int fd = ::open(pa.c_str(), O_WRONLY);
    CHECK(pwrite_full(fd, "GARBAGE!", 8, static_cast<off_t>(kPieceSize + 100)));
    ::close(fd);

    const std::string out = dir.sub("c_dl");
    std::string err;
    CHECK(c.dm.start("g", "a_copy.bin", out, err));
    DownloadStatus d = c.wait("a_copy.bin");
    dir.track(out + "/a_copy.bin");
    CHECK(d.state == DownloadStatus::COMPLETED);
    CHECK(d.bad_pieces == 1);
    CHECK(read_all(out + "/a_copy.bin") == content);
}

// Every seeder has bad data -> FAILED, and no partial file is left behind.
static void test_all_seeders_bad() {
    TrackerFixture t;
    TempDir dir;
    Peer a(t.ep), c(t.ep);
    a.login("a");
    c.login("c");
    make_group(a, {&c});
    accept_all(a, {"c"});
    const std::string content = random_bytes(kPieceSize + 1, 3);
    const std::string pa = dir.write("f.bin", content);
    CHECK(a.share("g", pa));
    int fd = ::open(pa.c_str(), O_WRONLY);
    pwrite_full(fd, "X", 1, 0);
    ::close(fd);

    const std::string out = dir.sub("c_dl");
    std::string err;
    CHECK(c.dm.start("g", "f.bin", out, err));
    DownloadStatus d = c.wait("f.bin");
    CHECK(d.state == DownloadStatus::FAILED);
    CHECK(d.error.find("no working peer") != std::string::npos);
    CHECK(!exists(out + "/f.bin") && !exists(out + "/f.bin.part"));
}

// The first listed seeder has crashed (its peer server is gone but the
// tracker hasn't noticed yet): the downloader moves on to the next seeder.
static void test_dead_seeder_skipped() {
    TrackerFixture t;
    TempDir dir;
    auto a = std::make_unique<Peer>(t.ep);
    Peer b(t.ep), c(t.ep);
    a->login("a");
    b.login("b");
    c.login("c");
    make_group(*a, {&b, &c});
    accept_all(*a, {"b", "c"});
    const std::string content = random_bytes(kPieceSize * 2, 4);
    const std::string pa = dir.write("x.bin", content);
    CHECK(a->share("g", pa));
    const std::string bdir = dir.sub("b");
    const std::string pb = bdir + "/x.bin";
    {
        int fd = ::open(pb.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        pwrite_full(fd, content.data(), content.size(), 0);
        ::close(fd);
        dir.track(pb);
    }
    CHECK(b.share("g", pb));
    a->server.stop();  // a's peer server dies; its tracker session is still alive

    const std::string out = dir.sub("c_dl");
    std::string err;
    CHECK(c.dm.start("g", "x.bin", out, err));
    DownloadStatus d = c.wait("x.bin");
    dir.track(out + "/x.bin");
    CHECK(d.state == DownloadStatus::COMPLETED && d.bad_pieces == 0);
    CHECK(read_all(out + "/x.bin") == content);
}

// Talk to a peer server directly with hostile / wrong requests.
static void test_peer_server_requests() {
    SharedFiles shared;
    PeerServer server(shared);
    const Endpoint ep = start_peer_server(server);
    TempDir dir;
    const std::string content = random_bytes(kPieceSize + 10, 5);
    const std::string p = dir.write("s.bin", content);
    FileMeta m;
    std::string err;
    CHECK(hash_file(p, m, err));
    shared.add({"g", p, m});

    int fd = connect_tcp(ep, 1000);
    CHECK(fd >= 0);
    set_io_timeout(fd, 5000);
    auto ask = [&](const std::vector<std::string>& f, Message& rep) {
        return send_msg(fd, {MsgType::GET_PIECE, Status::OK, pack_fields(f)}) == IoResult::OK &&
               recv_msg(fd, rep) == IoResult::OK;
    };
    Message rep;
    CHECK(ask({"g", "s.bin", "1"}, rep) && rep.status == Status::OK && rep.payload == content.substr(kPieceSize));
    CHECK(ask({"g", "s.bin", "0"}, rep) && rep.status == Status::OK && sha1_hex(rep.payload) == m.piece_hashes[0]);
    CHECK(ask({"g", "s.bin", "2"}, rep) && rep.status == Status::NO_SUCH_PIECE);
    CHECK(ask({"g", "other.bin", "0"}, rep) && rep.status == Status::NO_SUCH_FILE);
    CHECK(ask({"h", "s.bin", "0"}, rep) && rep.status == Status::NO_SUCH_FILE);          // wrong group
    CHECK(ask({"g", "../../etc/passwd", "0"}, rep) && rep.status == Status::NO_SUCH_FILE);  // no paths
    CHECK(ask({"g", "s.bin"}, rep) && rep.status == Status::BAD_REQUEST);
    CHECK(ask({"g", "s.bin", "-1"}, rep) && rep.status == Status::BAD_REQUEST);
    CHECK(send_msg(fd, {MsgType::PING, Status::OK, ""}) == IoResult::OK && recv_msg(fd, rep) == IoResult::OK &&
          rep.status == Status::UNKNOWN_TYPE);
    ::close(fd);
}

// One client downloads two different files at the same time.
static void test_two_concurrent_downloads() {
    TrackerFixture t;
    TempDir dir;
    Peer a(t.ep), c(t.ep);
    a.login("a");
    c.login("c");
    make_group(a, {&c});
    accept_all(a, {"c"});
    const std::string c1 = random_bytes(kPieceSize * 4 + 3, 6), c2 = random_bytes(kPieceSize * 3 + 9, 7);
    CHECK(a.share("g", dir.write("one.bin", c1)));
    CHECK(a.share("g", dir.write("two.bin", c2)));
    const std::string out = dir.sub("c_dl");
    std::string err;
    CHECK(c.dm.start("g", "one.bin", out, err));
    CHECK(c.dm.start("g", "two.bin", out, err));
    CHECK(c.wait("one.bin").state == DownloadStatus::COMPLETED);
    CHECK(c.wait("two.bin").state == DownloadStatus::COMPLETED);
    dir.track(out + "/one.bin");
    dir.track(out + "/two.bin");
    CHECK(read_all(out + "/one.bin") == c1 && read_all(out + "/two.bin") == c2);
}

int main() {
    ignore_sigpipe();
    static const TestCase tests[] = {
        {"download from one peer + error cases", test_basic_download},
        {"corrupted piece re-fetched from another peer", test_corrupted_piece_refetched},
        {"all seeders bad -> failed, no partial file", test_all_seeders_bad},
        {"dead seeder skipped", test_dead_seeder_skipped},
        {"peer server: wrong/hostile requests", test_peer_server_requests},
        {"two downloads at once", test_two_concurrent_downloads},
    };
    return run_tests(tests);
}
