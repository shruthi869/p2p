// piece_picker.h - decides which piece each download worker fetches next.
//
// One worker thread per peer. Each worker repeatedly asks: "give me a piece
// that MY peer has and nobody has fetched yet". The picker answers with the
// RAREST such piece (fewest peers have it), breaking ties at random.
//
// Why rarest-first: a piece that only one peer has is at risk - if that peer
// leaves, nobody can finish. Fetching rare pieces first (and then seeding
// them ourselves) spreads them quickly. Random tie-breaking stops all
// downloaders from requesting the same piece at the same moment.
//
// Every piece is in one state: NEEDED -> IN_PROGRESS -> DONE. A failed fetch
// (bad hash, dead peer) puts it back to NEEDED so another worker takes it.
// A piece is never handed to two workers at once: no duplicate downloads.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <vector>

namespace p2p {

class PiecePicker {
public:
    enum class Try {
        PICKED,  // here is a piece for you
        WAIT,    // nothing free now, but a piece your peer has is in progress
                 // elsewhere and might fail and come back: wait
        NONE,    // nothing more this peer can contribute (or all done / cancelled)
    };

    PiecePicker(size_t pieces, uint32_t seed);

    // A peer's piece map joins / leaves the availability counts.
    void add_peer(const std::vector<bool>& has);
    void remove_peer(const std::vector<bool>& has);

    Try  try_pick(const std::vector<bool>& has, size_t& index);  // never blocks
    bool pick(const std::vector<bool>& has, size_t& index);      // blocks while WAIT; false = NONE

    void done(size_t index);    // verified and written
    void failed(size_t index);  // give it back: someone else should fetch it
    void cancel();              // wake everyone; all picks return NONE from now on

    bool   all_done() const;
    size_t done_count() const;

private:
    enum State : uint8_t { NEEDED, IN_PROGRESS, DONE };
    Try try_pick_locked(const std::vector<bool>& has, size_t& index);

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::vector<State>      state_;
    std::vector<int>        avail_;  // how many connected peers have each piece
    size_t                  done_ = 0;
    bool                    cancelled_ = false;
    std::mt19937            rng_;
};

}  // namespace p2p
