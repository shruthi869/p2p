// downloads.cpp - parallel multi-peer downloads: coordinator + one worker
// thread per peer, rarest-first piece choice, per-piece SHA-1, atomic finish.
#include "downloads.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

#include "sha1.h"

namespace p2p {

namespace {

constexpr int kPeerConnectTimeoutMs = 2000;
constexpr int kPeerIoTimeoutMs = 10000;  // a peer silent this long is treated as dead

// FILE_INFO reply: size, file_sha1, piece_sha1s, peer addresses...
bool parse_file_info(const Message& rep, const std::string& name, const Endpoint& self, FileMeta& meta,
                     std::vector<Endpoint>& peers) {
    const std::vector<std::string> f = split_fields(rep.payload);
    FileMeta m;
    if (f.size() < 3 || !parse_u64(f[0], m.size) || !is_hex_hash(f[1]) || !split_hashes(f[2], m.piece_hashes) ||
        m.piece_hashes.size() != piece_count(m.size))
        return false;
    m.name = name;
    m.file_hash = f[1];
    peers.clear();
    for (size_t i = 3; i < f.size(); ++i) {
        Endpoint ep;
        if (parse_endpoint(f[i], ep) && ep.str() != self.str()) peers.push_back(ep);  // never ourselves
    }
    meta = std::move(m);
    return true;
}

}  // namespace

DownloadManager::DownloadManager(TrackerLink& link, SharedFiles& shared, const Endpoint& self, bool announce)
    : link_(link), shared_(shared), self_(self), announce_(announce) {}

DownloadManager::~DownloadManager() {
    cancel_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& j : jobs_) j->picker->cancel();  // wake workers waiting for a piece
    }
    for (auto& j : jobs_)
        if (j->thread.joinable()) j->thread.join();
}

bool DownloadManager::start(const std::string& group, const std::string& name, const std::string& dest_dir,
                            std::string& err) {
    // 1. Metadata + peers from the tracker.
    Message rep;
    if (!link_.call(MsgType::FILE_INFO, {group, name}, rep, err)) return false;
    if (rep.status != Status::OK) {
        err = status_text(rep.status);
        return false;
    }
    auto job = std::make_unique<Job>();
    if (!parse_file_info(rep, name, self_, job->meta, job->peers)) {
        err = "tracker sent malformed file info";
        return false;
    }
    if (job->peers.empty()) {
        err = "no other peer is sharing this file right now";
        return false;
    }
    // 2. Destination checks.
    std::string dir = dest_dir;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        err = "destination " + dest_dir + " is not a directory";
        return false;
    }
    const std::string final_path = dir + "/" + name;
    if (::stat(final_path.c_str(), &st) == 0) {
        err = final_path + " already exists";
        return false;
    }
    if (shared_.contains(group, name)) {  // our share entry would be overwritten
        err = "you are already sharing " + name + " in group " + group;
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& j : jobs_)
        if (j->st.dest_path == final_path && j->st.state == DownloadStatus::RUNNING) {
            err = "already downloading to " + final_path;
            return false;
        }
    job->st.group = group;
    job->st.name = name;
    job->st.dest_path = final_path;
    job->st.pieces_total = job->meta.piece_hashes.size();
    job->picker = std::make_unique<PiecePicker>(job->meta.piece_hashes.size(), std::random_device{}());
    Job* raw = job.get();
    jobs_.push_back(std::move(job));
    raw->thread = std::thread(&DownloadManager::run, this, raw);
    return true;
}

// Tell the tracker we share this file (partially at first). Once is enough;
// UPLOAD_FILE with the same hash just adds us as a seeder.
void DownloadManager::announce(Job* job) {
    if (job->announced.exchange(true)) return;
    Message rep;
    std::string err;
    const FileMeta& m = job->meta;
    link_.call(MsgType::UPLOAD_FILE,
               {job->st.group, m.name, std::to_string(m.size), m.file_hash, join_hashes(m.piece_hashes)}, rep, err);
}

bool DownloadManager::refresh_peers(Job* job) {
    Message rep;
    std::string err;
    FileMeta meta;
    std::vector<Endpoint> peers;
    if (!link_.call(MsgType::FILE_INFO, {job->st.group, job->st.name}, rep, err) || rep.status != Status::OK ||
        !parse_file_info(rep, job->st.name, self_, meta, peers) || meta.file_hash != job->meta.file_hash)
        return false;  // file gone, or replaced by different content
    job->peers = peers;
    return true;
}

// One worker = one peer = one TCP connection, running until the picker has
// nothing more this peer can give us.
void DownloadManager::worker(Job* job, const Endpoint& peer, int out_fd) {
    const FileMeta& meta = job->meta;
    const size_t n = meta.piece_hashes.size();
    int fd = connect_tcp(peer, kPeerConnectTimeoutMs);
    if (fd < 0) return;  // peer gone (crashed / logged out): others will cover
    set_io_timeout(fd, kPeerIoTimeoutMs);

    // Which pieces does this peer have? ('1'/'0' per piece)
    Message rep;
    const Message map_req{MsgType::GET_PIECE_MAP, Status::OK, pack_fields({job->st.group, meta.name})};
    if (send_msg(fd, map_req) != IoResult::OK || recv_msg(fd, rep) != IoResult::OK ||
        rep.type != reply_type(MsgType::GET_PIECE_MAP) || rep.status != Status::OK || rep.payload.size() != n) {
        ::close(fd);
        return;
    }
    std::vector<bool> has(n);
    for (size_t i = 0; i < n; ++i) has[i] = rep.payload[i] == '1';

    ++job->active;
    job->picker->add_peer(has);
    size_t i = 0;
    while (!cancel_ && job->picker->pick(has, i)) {
        const size_t len = static_cast<size_t>(piece_length(meta.size, i));
        const Message req{MsgType::GET_PIECE, Status::OK, pack_fields({job->st.group, meta.name, std::to_string(i)})};
        const bool got = send_msg(fd, req) == IoResult::OK && recv_msg(fd, rep) == IoResult::OK &&
                         rep.type == reply_type(MsgType::GET_PIECE) && rep.status == Status::OK;
        if (got && rep.payload.size() == len && sha1_hex(rep.payload) == meta.piece_hashes[i]) {
            // pwrite at the piece's own offset: no shared file position, so
            // workers write the same file concurrently without a lock.
            if (!pwrite_full(out_fd, rep.payload.data(), len, static_cast<off_t>(i * kPieceSize))) {
                std::lock_guard<std::mutex> lock(mu_);
                job->fatal = std::string("write error: ") + std::strerror(errno);
                job->picker->failed(i);
                job->picker->cancel();
                break;
            }
            shared_.mark_piece(job->st.group, meta.name, i);  // we can serve it from now on
            job->picker->done(i);
            ++job->done;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ++job->st.per_peer[peer.str()];
            }
            announce(job);
            continue;
        }
        // Wrong data, error reply, or no reply: give the piece back to the picker.
        job->picker->failed(i);
        if (got) {  // the peer answered with bytes that fail the SHA-1: never trust it again
            ++job->bad;
            std::lock_guard<std::mutex> lock(mu_);
            job->bad_peers.insert(peer.str());
        }
        break;
    }
    job->picker->remove_peer(has);
    --job->active;
    ::close(fd);
}

void DownloadManager::run(Job* job) {
    const std::string final_path = job->st.dest_path;  // fixed before the thread started
    const std::string part_path = final_path + ".part";
    const std::string group = job->st.group, name = job->st.name;
    const FileMeta& meta = job->meta;
    const size_t n = meta.piece_hashes.size();

    int out = ::open(part_path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (out < 0) {
        finish(job, false, "cannot create " + part_path + ": " + std::strerror(errno));
        return;
    }
    // Full size up front: a sparse file, so pieces can be written at any
    // offset in any order.
    if (::ftruncate(out, static_cast<off_t>(meta.size)) != 0) {
        ::close(out);
        ::unlink(part_path.c_str());
        finish(job, false, std::string("cannot size file: ") + std::strerror(errno));
        return;
    }
    shared_.add({group, part_path, meta, std::vector<bool>(n, false)});  // partial seeding

    auto fail = [&](const std::string& why) {
        ::close(out);
        ::unlink(part_path.c_str());  // never leave a partial file behind
        shared_.remove(group, name);
        if (job->announced) {         // we told the tracker we share it: take that back
            Message rep;
            std::string err;
            link_.call(MsgType::STOP_SHARE, {group, name}, rep, err);
        }
        finish(job, false, why);
    };

    std::mt19937 rng(std::random_device{}());
    for (int round = 0; round < kMaxRounds && !cancel_ && !job->picker->all_done(); ++round) {
        if (round > 0 && !refresh_peers(job)) break;
        std::vector<Endpoint> peers;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!job->fatal.empty()) break;
            for (const auto& p : job->peers)
                if (!job->bad_peers.count(p.str())) peers.push_back(p);
        }
        if (peers.empty()) break;
        std::shuffle(peers.begin(), peers.end(), rng);  // spread load across seeders
        if (peers.size() > kMaxWorkers) peers.resize(kMaxWorkers);

        std::vector<std::thread> workers;
        for (const auto& p : peers) workers.emplace_back(&DownloadManager::worker, this, job, p, out);
        for (auto& w : workers) w.join();
    }

    if (!job->picker->all_done()) {
        std::string why;
        {
            std::lock_guard<std::mutex> lock(mu_);
            why = job->fatal;
        }
        if (why.empty() && cancel_) why = "cancelled";
        if (why.empty())
            why = "no working peer for " + std::to_string(n - job->picker->done_count()) + " piece(s)";
        return fail(why);
    }

    // End-to-end check: every piece matched its hash, but verify the whole
    // file too (catches e.g. an offset bug or a bad disk write).
    std::string whole;
    if (!hash_fd(out, meta.size, whole) || whole != meta.file_hash) return fail("whole-file SHA-1 mismatch");
    ::close(out);
    out = -1;

    // Give it its real name. link() makes the final name appear atomically
    // with the complete, verified content while the .part path stays valid
    // for any peer reading it right now; then we switch the share over and
    // drop the .part name. (rename() is the fallback, e.g. no hard links.)
    if (::link(part_path.c_str(), final_path.c_str()) == 0) {
        shared_.complete(group, name, final_path);
        ::unlink(part_path.c_str());
    } else if (::rename(part_path.c_str(), final_path.c_str()) == 0) {
        shared_.complete(group, name, final_path);
    } else {
        const std::string why = std::string("cannot create final file: ") + std::strerror(errno);
        ::unlink(part_path.c_str());
        shared_.remove(group, name);
        finish(job, false, why);
        return;
    }
    announce(job);  // normally already done after the first piece
    finish(job, true, "");
}

void DownloadManager::finish(Job* job, bool ok, const std::string& error) {
    std::string line, sources;
    {
        std::lock_guard<std::mutex> lock(mu_);
        job->st.state = ok ? DownloadStatus::COMPLETED : DownloadStatus::FAILED;
        job->st.error = error;
        line = ok ? "download finished: [C] [" + job->st.group + "] " + job->st.name
                  : "download failed: [" + job->st.group + "] " + job->st.name + ": " + error;
        if (ok && job->bad > 0) line += " (" + std::to_string(job->bad.load()) + " bad piece(s) re-fetched)";
        for (const auto& kv : job->st.per_peer) sources += " " + kv.first + "=" + std::to_string(kv.second);
    }
    if (announce_) {
        std::printf("%s\n", line.c_str());
        if (ok) std::printf("  pieces from:%s\n", sources.c_str());
        std::fflush(stdout);
    }
}

std::vector<DownloadStatus> DownloadManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<DownloadStatus> out;
    for (const auto& j : jobs_) {
        DownloadStatus s = j->st;
        s.pieces_done = j->done;
        s.bad_pieces = j->bad;
        s.peers_active = j->active;
        out.push_back(s);
    }
    return out;
}

}  // namespace p2p
