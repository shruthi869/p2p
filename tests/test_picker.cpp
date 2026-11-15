// tests/test_picker.cpp - PiecePicker on its own: rarest-first, only pieces
// the peer has, never the same piece twice, WAIT vs NONE, random ties,
// blocking pick woken up, cancel, and many threads at once.
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#include "piece_picker.h"
#include "test_util.h"

using namespace p2p;
using Map = std::vector<bool>;

static void test_rarest_first() {
    PiecePicker p(4, 1);
    const Map a = {true, true, true, true};   // peer A has everything
    const Map b = {true, true, false, false}; // peer B has pieces 0 and 1
    p.add_peer(a);
    p.add_peer(b);  // availability: 0->2, 1->2, 2->1, 3->1
    size_t i = 99, j = 99;
    CHECK(p.try_pick(a, i) == PiecePicker::Try::PICKED && (i == 2 || i == 3));  // rarest first
    CHECK(p.try_pick(a, j) == PiecePicker::Try::PICKED && (j == 2 || j == 3) && j != i);
    size_t k = 99;
    CHECK(p.try_pick(a, k) == PiecePicker::Try::PICKED && (k == 0 || k == 1));  // then the common ones
}

static void test_only_pieces_peer_has() {
    PiecePicker p(4, 2);
    const Map b = {false, true, false, true};
    p.add_peer(b);
    std::set<size_t> got;
    size_t i;
    while (p.try_pick(b, i) == PiecePicker::Try::PICKED) got.insert(i);
    CHECK(got == (std::set<size_t>{1, 3}));
}

static void test_no_duplicates_and_done() {
    PiecePicker p(5, 3);
    const Map a(5, true);
    p.add_peer(a);
    std::set<size_t> got;
    size_t i;
    for (int n = 0; n < 5; ++n) {
        CHECK(p.try_pick(a, i) == PiecePicker::Try::PICKED);
        got.insert(i);
    }
    CHECK(got.size() == 5);                                // 5 different pieces
    CHECK(p.try_pick(a, i) == PiecePicker::Try::WAIT);     // all in progress: maybe one fails
    for (size_t x : got) p.done(x);
    CHECK(p.all_done() && p.done_count() == 5);
    CHECK(p.try_pick(a, i) == PiecePicker::Try::NONE);
}

static void test_failed_piece_comes_back() {
    PiecePicker p(1, 4);
    const Map a = {true};
    p.add_peer(a);
    size_t i;
    CHECK(p.try_pick(a, i) == PiecePicker::Try::PICKED && i == 0);
    CHECK(p.try_pick(a, i) == PiecePicker::Try::WAIT);
    p.failed(0);  // e.g. bad SHA-1 from that peer
    CHECK(p.try_pick(a, i) == PiecePicker::Try::PICKED && i == 0);
}

// B only has pieces that A is busy with: B must WAIT (A might fail), and
// once A finishes them B gets NONE (nothing left that B can provide).
static void test_wait_then_none() {
    PiecePicker p(3, 5);
    const Map a = {true, true, true}, b = {true, true, false};
    p.add_peer(a);
    p.add_peer(b);
    size_t i;
    std::set<size_t> a_got;
    // A takes piece 2 (rarest), then 0 and 1.
    for (int n = 0; n < 3; ++n) {
        CHECK(p.try_pick(a, i) == PiecePicker::Try::PICKED);
        a_got.insert(i);
    }
    CHECK(p.try_pick(b, i) == PiecePicker::Try::WAIT);
    p.done(0);
    p.done(1);
    CHECK(p.try_pick(b, i) == PiecePicker::Try::NONE);  // only piece 2 is left and B doesn't have it
}

// With equal availability, the first pick must be spread over all pieces.
static void test_random_ties() {
    std::vector<int> count(10, 0);
    const Map a(10, true);
    for (uint32_t seed = 0; seed < 2000; ++seed) {
        PiecePicker p(10, seed);
        p.add_peer(a);
        size_t i;
        if (p.try_pick(a, i) == PiecePicker::Try::PICKED) ++count[i];
    }
    for (int c : count) CHECK(c > 100);  // expected ~200 each
}

// A worker blocked in pick() wakes up when another worker gives a piece back.
static void test_blocking_pick_wakes() {
    PiecePicker p(1, 6);
    const Map a = {true};
    p.add_peer(a);
    size_t first;
    CHECK(p.try_pick(a, first) == PiecePicker::Try::PICKED);
    std::atomic<bool> returned{false};
    size_t got = 99;
    bool ok = false;
    std::thread t([&] {
        ok = p.pick(a, got);
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!returned);  // still waiting
    p.failed(first);
    t.join();
    CHECK(ok && got == 0);
}

static void test_cancel_wakes() {
    PiecePicker p(1, 7);
    const Map a = {true};
    p.add_peer(a);
    size_t i;
    p.try_pick(a, i);
    bool ok = true;
    std::thread t([&] { ok = p.pick(a, i); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    p.cancel();
    t.join();
    CHECK(!ok);
}

// 8 workers drain 2000 pieces: every piece done exactly once (run under TSan too).
static void test_many_threads() {
    const size_t n = 2000;
    PiecePicker p(n, 8);
    const Map a(n, true);
    std::vector<std::atomic<int>> times(n);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            p.add_peer(a);
            size_t i;
            while (p.pick(a, i)) {
                ++times[i];
                p.done(i);
            }
            p.remove_peer(a);
        });
    }
    for (auto& th : threads) th.join();
    bool exactly_once = true;
    for (auto& x : times) exactly_once = exactly_once && x == 1;
    CHECK(exactly_once);
    CHECK(p.all_done());
}

int main() {
    static const TestCase tests[] = {
        {"rarest piece first", test_rarest_first},
        {"only pieces the peer has", test_only_pieces_peer_has},
        {"no duplicates; done -> NONE", test_no_duplicates_and_done},
        {"failed piece comes back", test_failed_piece_comes_back},
        {"WAIT while in progress, then NONE", test_wait_then_none},
        {"random tie-breaking", test_random_ties},
        {"blocking pick wakes on failed()", test_blocking_pick_wakes},
        {"cancel wakes waiters", test_cancel_wakes},
        {"8 threads, 2000 pieces, each exactly once", test_many_threads},
    };
    return run_tests(tests);
}
