// sync.h - keeps the two trackers' states the same.
//
// Each tracker runs one SyncLink: a thread that connects to the OTHER
// tracker (as a client of its normal port) and pushes this tracker's
// changes. So there are two connections, one per direction, and each
// tracker only ever pushes its OWN changes (no echo loops).
//
// On every (re)connect:
//   1. SYNC_HELLO(my_no)                    "I am tracker N"
//   2. SYNC_SNAPSHOT(full state)            the receiver MERGES it (see TrackerState::merge)
//   3. SYNC_OP(op) ... for every change applied here from now on, in order,
//      each acknowledged before the next (keeps order, gives backpressure)
//   4. PING every kHeartbeatMs while idle   so the receiver can tell
//      "quiet" from "dead or frozen"
// Because both trackers do this, a restarted tracker (empty state) gets the
// other's full state, and after a partition both sides get each other's
// changes. Changes made while the link is down are not queued: the
// snapshot on reconnect carries them.
//
// Ordering rule: applying an op locally and queueing it for the peer happen
// under one mutex, and so does "take snapshot + start queueing". An op is
// therefore either inside the snapshot or in the queue after it, never
// both and never neither.
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "net.h"
#include "protocol.h"
#include "state.h"

namespace p2p {

class SyncLink {
public:
    static constexpr int kHeartbeatMs = 1000;      // PING the peer this often when idle
    static constexpr int kPeerSilenceMs = 3500;    // receiver drops a peer link silent this long
    static constexpr int kRetryMs = 500;           // reconnect attempts while the peer is down

    SyncLink(TrackerState& state, int my_no, const Endpoint& peer, bool verbose);
    ~SyncLink();
    SyncLink(const SyncLink&) = delete;
    SyncLink& operator=(const SyncLink&) = delete;

    void start();
    void stop();

    // Apply a change made by one of OUR clients and, if it succeeded and the
    // peer is connected, queue it for the peer - atomically (see above).
    Status apply_and_forward(const Op& op);

    // Fault injection for testing partitions ("sync off" / "sync on").
    void set_enabled(bool on);
    bool enabled() const;
    bool connected() const;

private:
    void loop();
    bool exchange(int fd, const Message& req);  // send + wait for OK reply
    void log(const std::string& line) const;

    TrackerState&   state_;
    const int       my_no_;
    const int       peer_no_;
    const Endpoint  peer_;
    const bool      verbose_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::deque<Op>          queue_;
    bool                    connected_ = false;  // snapshot sent; ops are being forwarded
    bool                    enabled_ = true;
    bool                    stopping_ = false;
    int                     fd_ = -1;            // current connection (for stop() to wake it)
    std::thread             thread_;
};

// Encode/decode an Op for SYNC_OP: fields = [type number, args...].
std::string encode_op(const Op& op);
bool        decode_op(const std::string& payload, Op& op);

}  // namespace p2p
