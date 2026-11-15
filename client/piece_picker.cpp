// piece_picker.cpp - rarest-first piece selection with random tie-breaks.
#include "piece_picker.h"

namespace p2p {

PiecePicker::PiecePicker(size_t pieces, uint32_t seed) : state_(pieces, NEEDED), avail_(pieces, 0), rng_(seed) {}

void PiecePicker::add_peer(const std::vector<bool>& has) {
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < has.size() && i < avail_.size(); ++i)
        if (has[i]) ++avail_[i];
}

void PiecePicker::remove_peer(const std::vector<bool>& has) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (size_t i = 0; i < has.size() && i < avail_.size(); ++i)
            if (has[i] && avail_[i] > 0) --avail_[i];
    }
    cv_.notify_all();  // a waiting worker may now be the only one left for some piece
}

// Scan all pieces once: among NEEDED pieces this peer has, take the one with
// the lowest availability. Ties: "reservoir sampling" - the k-th tie replaces
// the current choice with probability 1/k, which makes every tied piece
// equally likely without building a list. O(pieces) per pick: 2048 pieces
// for 1 GB, trivial next to fetching 512 KiB.
PiecePicker::Try PiecePicker::try_pick_locked(const std::vector<bool>& has, size_t& index) {
    if (cancelled_ || done_ == state_.size()) return Try::NONE;
    int best_avail = 0;
    size_t ties = 0;
    bool waiting_possible = false;
    for (size_t i = 0; i < state_.size() && i < has.size(); ++i) {
        if (!has[i]) continue;
        if (state_[i] == IN_PROGRESS) waiting_possible = true;
        if (state_[i] != NEEDED) continue;
        if (ties == 0 || avail_[i] < best_avail) {
            best_avail = avail_[i];
            index = i;
            ties = 1;
        } else if (avail_[i] == best_avail) {
            ++ties;
            if (std::uniform_int_distribution<size_t>(1, ties)(rng_) == 1) index = i;
        }
    }
    if (ties > 0) {
        state_[index] = IN_PROGRESS;
        return Try::PICKED;
    }
    return waiting_possible ? Try::WAIT : Try::NONE;
}

PiecePicker::Try PiecePicker::try_pick(const std::vector<bool>& has, size_t& index) {
    std::lock_guard<std::mutex> lock(mu_);
    return try_pick_locked(has, index);
}

bool PiecePicker::pick(const std::vector<bool>& has, size_t& index) {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        Try r = try_pick_locked(has, index);
        if (r == Try::PICKED) return true;
        if (r == Try::NONE) return false;
        cv_.wait(lock);  // woken by done()/failed()/cancel()/remove_peer()
    }
}

void PiecePicker::done(size_t index) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_[index] != DONE) {
            state_[index] = DONE;
            ++done_;
        }
    }
    cv_.notify_all();
}

void PiecePicker::failed(size_t index) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_[index] == IN_PROGRESS) state_[index] = NEEDED;
    }
    cv_.notify_all();
}

void PiecePicker::cancel() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled_ = true;
    }
    cv_.notify_all();
}

bool PiecePicker::all_done() const {
    std::lock_guard<std::mutex> lock(mu_);
    return done_ == state_.size();
}

size_t PiecePicker::done_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return done_;
}

}  // namespace p2p
