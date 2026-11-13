// tests/test_protocol.cpp - unit tests for framing and socket helpers.
// No test framework: a CHECK macro counts failures, main() returns non-zero
// if any check failed. Run with `make test` (and `make asan/tsan/valgrind`).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "net.h"
#include "protocol.h"
#include "test_util.h"

using namespace p2p;

// A connected pair of stream sockets in one process: fd[0] <-> fd[1].
struct SockPair {
    int fd[2] = {-1, -1};
    SockPair() {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) std::abort();
    }
    ~SockPair() { close_end(0); close_end(1); }
    void close_end(int i) {
        if (fd[i] >= 0) ::close(fd[i]);
        fd[i] = -1;
    }
};

static std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + 7) % 251);
    return s;
}

static std::string raw_header(char m0, char m1, uint8_t type, uint8_t status, uint32_t len) {
    std::string h(kHeaderSize, '\0');
    h[0] = m0; h[1] = m1; h[2] = static_cast<char>(type); h[3] = static_cast<char>(status);
    h[4] = static_cast<char>(len >> 24); h[5] = static_cast<char>(len >> 16);
    h[6] = static_cast<char>(len >> 8);  h[7] = static_cast<char>(len);
    return h;
}

// ---------------------------------------------------------------- tests

static void test_small_roundtrip() {
    SockPair sp;
    Message out{MsgType::LOGIN, Status::OK, pack_fields({"alice", "secret"})};
    CHECK(send_msg(sp.fd[0], out) == IoResult::OK);
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::OK);
    CHECK(in.type == MsgType::LOGIN && in.status == Status::OK && in.payload == out.payload);
}

static void test_empty_payload_and_reply_type() {
    SockPair sp;
    Message out{reply_type(MsgType::PING), Status::NOT_OWNER, ""};
    CHECK(send_msg(sp.fd[0], out) == IoResult::OK);
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::OK);
    CHECK(is_reply(in.type) && !is_reply(MsgType::PING));
    CHECK(in.type == reply_type(MsgType::PING) && in.status == Status::NOT_OWNER && in.payload.empty());
}

// Bigger than the socket buffer: send() must be called many times (partial
// sends), and the receiver must loop (partial receives). Needs two threads,
// otherwise the sender blocks forever on a full buffer.
static void test_large_payload(size_t size) {
    SockPair sp;
    Message out{MsgType::GET_PIECE, Status::OK, pattern(size)};
    IoResult sent = IoResult::ERROR;
    std::thread writer([&] { sent = send_msg(sp.fd[0], out); });
    Message in;
    IoResult got = recv_msg(sp.fd[1], in);
    writer.join();
    CHECK(sent == IoResult::OK);
    CHECK(got == IoResult::OK);
    CHECK(in.payload.size() == size && in.payload == out.payload);
}

// The frame trickles in one byte at a time: header split across recv()s.
static void test_fragmented_frame() {
    SockPair sp;
    Message out{MsgType::UPLOAD_FILE, Status::OK, pattern(3000)};
    const std::string frame = encode_message(out);
    std::thread writer([&] {
        for (size_t i = 0; i < frame.size();) {
            size_t chunk = i < 40 ? 1 : 997;  // header byte-by-byte, then odd-sized chunks
            if (chunk > frame.size() - i) chunk = frame.size() - i;
            send_all(sp.fd[0], frame.data() + i, chunk);
            i += chunk;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::OK);
    writer.join();
    CHECK(in.type == MsgType::UPLOAD_FILE && in.payload == out.payload);
}

// Three frames glued together in one write must come out as three messages.
static void test_back_to_back() {
    SockPair sp;
    std::string glued = encode_message({MsgType::PING, Status::OK, "one"}) +
                        encode_message({MsgType::LOGOUT, Status::OK, ""}) +
                        encode_message({MsgType::JOIN_GROUP, Status::OK, "g1"});
    CHECK(send_all(sp.fd[0], glued.data(), glued.size()) == IoResult::OK);
    Message a, b, c;
    CHECK(recv_msg(sp.fd[1], a) == IoResult::OK && a.type == MsgType::PING && a.payload == "one");
    CHECK(recv_msg(sp.fd[1], b) == IoResult::OK && b.type == MsgType::LOGOUT && b.payload.empty());
    CHECK(recv_msg(sp.fd[1], c) == IoResult::OK && c.type == MsgType::JOIN_GROUP && c.payload == "g1");
}

static void test_clean_close() {
    SockPair sp;
    sp.close_end(0);
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::CLOSED);
}

static void test_truncated_header() {
    SockPair sp;
    CHECK(send_all(sp.fd[0], "P2x", 3) == IoResult::OK);
    sp.close_end(0);
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::TRUNCATED);
}

static void test_truncated_payload() {
    SockPair sp;
    std::string partial = raw_header('P', '2', 0x01, 0, 100) + "only ten b";
    CHECK(send_all(sp.fd[0], partial.data(), partial.size()) == IoResult::OK);
    sp.close_end(0);
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::TRUNCATED);
}

static void test_bad_magic() {
    SockPair sp;
    std::string h = raw_header('G', 'E', 0x01, 0, 0);  // e.g. someone sent "GET / HTTP"
    CHECK(send_all(sp.fd[0], h.data(), h.size()) == IoResult::OK);
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::BAD_HEADER);
}

static void test_too_large() {
    SockPair sp;
    std::string h = raw_header('P', '2', 0x01, 0, kMaxPayload + 1);
    CHECK(send_all(sp.fd[0], h.data(), h.size()) == IoResult::OK);
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::TOO_LARGE);
    CHECK(in.payload.empty());  // never allocated

    Message huge{MsgType::PING, Status::OK, std::string(kMaxPayload + 1, 'x')};
    CHECK(send_msg(sp.fd[0], huge) == IoResult::TOO_LARGE);
}

static void test_recv_timeout() {
    SockPair sp;
    CHECK(set_io_timeout(sp.fd[1], 200));
    auto t0 = std::chrono::steady_clock::now();
    Message in;
    CHECK(recv_msg(sp.fd[1], in) == IoResult::TIMEOUT);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(ms >= 150 && ms < 2000);
}

// main() does NOT call ignore_sigpipe(), so this proves MSG_NOSIGNAL alone
// keeps us alive when writing to a dead peer.
static void test_send_to_closed_peer() {
    SockPair sp;
    sp.close_end(1);
    CHECK(send_msg(sp.fd[0], {MsgType::PING, Status::OK, "hello?"}) == IoResult::CLOSED);
}

static void test_fields() {
    CHECK(pack_fields({}) == "");
    CHECK(split_fields("").empty());
    CHECK(pack_fields({"a", "", "b c"}) == "a\n\nb c");
    auto f = split_fields("a\n\nb c");
    CHECK(f.size() == 3 && f[0] == "a" && f[1].empty() && f[2] == "b c");
    auto g = split_fields(pack_fields({"x", ""}));
    CHECK(g.size() == 2 && g[0] == "x" && g[1].empty());
}

static void test_parse_endpoint() {
    Endpoint ep;
    CHECK(parse_endpoint("127.0.0.1:6001", ep) && ep.ip == "127.0.0.1" && ep.port == 6001);
    CHECK(ep.str() == "127.0.0.1:6001");
    CHECK(!parse_endpoint("127.0.0.1", ep));
    CHECK(!parse_endpoint(":6001", ep));
    CHECK(!parse_endpoint("localhost:6001", ep));
    CHECK(!parse_endpoint("1.2.3.4:0", ep));
    CHECK(!parse_endpoint("1.2.3.4:65536", ep));
    CHECK(!parse_endpoint("1.2.3.4:12a", ep));
    CHECK(!parse_endpoint("1.2.3.4:", ep));
    CHECK(!parse_endpoint("999.1.1.1:80", ep));
}

static std::string write_temp(const std::string& content) {
    char path[] = "/tmp/p2p_tracker_info_XXXXXX";
    int fd = ::mkstemp(path);
    if (fd < 0) std::abort();
    if (::write(fd, content.data(), content.size()) != static_cast<ssize_t>(content.size())) std::abort();
    ::close(fd);
    return path;
}

static void test_tracker_info() {
    std::vector<Endpoint> t;
    std::string err;
    std::string ok = write_temp("# trackers\r\n127.0.0.1 6001\r\n\r\n  127.0.0.1   6002\n");
    CHECK(read_tracker_info(ok, t, err));
    CHECK(t.size() == 2 && t[0].port == 6001 && t[1].port == 6002 && t[1].ip == "127.0.0.1");
    ::unlink(ok.c_str());

    std::string one = write_temp("127.0.0.1 6001\n");
    CHECK(!read_tracker_info(one, t, err) && err.find("exactly 2") != std::string::npos);
    ::unlink(one.c_str());

    std::string extra = write_temp("127.0.0.1 6001 junk\n127.0.0.1 6002\n");
    CHECK(!read_tracker_info(extra, t, err));
    ::unlink(extra.c_str());

    CHECK(!read_tracker_info("/nonexistent/tracker_info.txt", t, err));
}

// Real TCP over loopback: listen, connect, request/reply.
static void test_tcp_loopback() {
    int lfd = listen_tcp({"127.0.0.1", 0});  // port 0 = let the kernel pick a free port
    CHECK(lfd >= 0);
    if (lfd < 0) return;
    uint16_t port = local_port(lfd);
    CHECK(port != 0);

    std::thread server([lfd] {
        int c = ::accept(lfd, nullptr, nullptr);
        if (c < 0) return;
        Message req;
        if (recv_msg(c, req) == IoResult::OK)
            send_msg(c, {reply_type(req.type), Status::OK, "pong:" + req.payload});
        ::close(c);
    });

    int cfd = connect_tcp({"127.0.0.1", port}, 1000);
    CHECK(cfd >= 0);
    if (cfd >= 0) {
        CHECK(set_io_timeout(cfd, 2000));
        CHECK(send_msg(cfd, {MsgType::PING, Status::OK, "hi"}) == IoResult::OK);
        Message rep;
        CHECK(recv_msg(cfd, rep) == IoResult::OK);
        CHECK(rep.type == reply_type(MsgType::PING) && rep.payload == "pong:hi");
        CHECK(recv_msg(cfd, rep) == IoResult::CLOSED);  // server closed after replying
        ::close(cfd);
    }
    server.join();
    ::close(lfd);
}

static void test_connect_refused() {
    int lfd = listen_tcp({"127.0.0.1", 0});
    uint16_t port = local_port(lfd);
    ::close(lfd);  // now nothing listens on that port
    auto t0 = std::chrono::steady_clock::now();
    int fd = connect_tcp({"127.0.0.1", port}, 1000);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(fd < 0 && errno == ECONNREFUSED);
    CHECK(ms < 500);  // refused immediately, not after the timeout
    if (fd >= 0) ::close(fd);
}

int main() {
    static const TestCase tests[] = {
        {"small roundtrip", test_small_roundtrip},
        {"empty payload + reply type", test_empty_payload_and_reply_type},
        {"512 KiB payload (partial send/recv)", [] { test_large_payload(512 * 1024); }},
        {"max payload (4 MiB)", [] { test_large_payload(kMaxPayload); }},
        {"fragmented frame", test_fragmented_frame},
        {"back-to-back frames", test_back_to_back},
        {"clean close", test_clean_close},
        {"truncated header", test_truncated_header},
        {"truncated payload", test_truncated_payload},
        {"bad magic", test_bad_magic},
        {"oversized length", test_too_large},
        {"recv timeout", test_recv_timeout},
        {"send to closed peer (no SIGPIPE)", test_send_to_closed_peer},
        {"field packing", test_fields},
        {"parse endpoint", test_parse_endpoint},
        {"tracker_info.txt", test_tracker_info},
        {"tcp loopback", test_tcp_loopback},
        {"connect refused", test_connect_refused},
    };
    return run_tests(tests);
}
