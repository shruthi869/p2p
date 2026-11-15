// downloads.cpp - fetch pieces, verify, write at offsets, finish atomically.
#include "downloads.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha1.h"

namespace p2p {

namespace {
constexpr int kPeerConnectTimeoutMs = 2000;
constexpr int kPeerIoTimeoutMs = 10000;  // a peer silent this long is treated as dead
}  // namespace

DownloadManager::DownloadManager(TrackerLink& link, SharedFiles& shared, const Endpoint& self, bool announce)
    : link_(link), shared_(shared), self_(self), announce_(announce) {}

DownloadManager::~DownloadManager() {
    cancel_ = true;  // running jobs notice between pieces (and I/O has timeouts)
    for (auto& j : jobs_)
        if (j->thread.joinable()) j->thread.join();
}

bool DownloadManager::start(const std::string& group, const std::string& name, const std::string& dest_dir,
                            std::string& err) {
    // 1. Metadata + seeders from the tracker.
    Message rep;
    if (!link_.call(MsgType::FILE_INFO, {group, name}, rep, err)) return false;
    if (rep.status != Status::OK) {
        err = status_text(rep.status);
        return false;
    }
    const std::vector<std::string> f = split_fields(rep.payload);
    auto job = std::make_unique<Job>();
    if (f.size() < 3 || !parse_u64(f[0], job->meta.size) || !is_hex_hash(f[1]) ||
        !split_hashes(f[2], job->meta.piece_hashes) || job->meta.piece_hashes.size() != piece_count(job->meta.size)) {
        err = "tracker sent malformed file info";
        return false;
    }
    job->meta.name = name;
    job->meta.file_hash = f[1];
    for (size_t i = 3; i < f.size(); ++i) {
        Endpoint ep;
        if (parse_endpoint(f[i], ep) && ep.str() != self_.str()) job->peers.push_back(ep);  // not from ourselves
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
    Job* raw = job.get();
    jobs_.push_back(std::move(job));
    raw->thread = std::thread(&DownloadManager::run, this, raw);
    return true;
}

void DownloadManager::run(Job* job) {
    const std::string final_path = job->st.dest_path;  // set before the thread started; never changes
    const std::string part_path = final_path + ".part";
    const FileMeta& meta = job->meta;

    int out = ::open(part_path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (out < 0) {
        finish(job, false, "cannot create " + part_path + ": " + std::strerror(errno));
        return;
    }
    // Full size up front: a sparse file, so pieces can be written at any
    // offset in any order (Part 6 downloads them out of order).
    if (::ftruncate(out, static_cast<off_t>(meta.size)) != 0) {
        ::close(out);
        ::unlink(part_path.c_str());
        finish(job, false, std::string("cannot size file: ") + std::strerror(errno));
        return;
    }

    auto fail = [&](const std::string& why, int peer_fd) {
        if (peer_fd >= 0) ::close(peer_fd);
        ::close(out);
        ::unlink(part_path.c_str());  // never leave a partial file behind
        finish(job, false, why);
    };

    size_t peer_idx = 0;
    int peer = -1;
    const uint64_t n = meta.piece_hashes.size();
    for (uint64_t i = 0; i < n; ++i) {
        const size_t len = static_cast<size_t>(piece_length(meta.size, i));
        while (true) {
            if (cancel_) return fail("cancelled", peer);
            if (peer < 0) {
                if (peer_idx >= job->peers.size()) return fail("no working peer has piece " + std::to_string(i), -1);
                peer = connect_tcp(job->peers[peer_idx], kPeerConnectTimeoutMs);
                if (peer < 0) {  // peer gone (crashed, logged out): try the next one
                    ++peer_idx;
                    continue;
                }
                set_io_timeout(peer, kPeerIoTimeoutMs);
            }
            Message rep;
            const Message req{MsgType::GET_PIECE, Status::OK,
                              pack_fields({job->st.group, meta.name, std::to_string(i)})};
            const bool got = send_msg(peer, req) == IoResult::OK && recv_msg(peer, rep) == IoResult::OK &&
                             rep.type == reply_type(MsgType::GET_PIECE) && rep.status == Status::OK;
            if (got && rep.payload.size() == len && sha1_hex(rep.payload) == meta.piece_hashes[i]) {
                if (!pwrite_full(out, rep.payload.data(), len, static_cast<off_t>(i * kPieceSize)))
                    return fail(std::string("write error: ") + std::strerror(errno), peer);
                ++job->done;
                break;  // next piece, same peer
            }
            // Wrong data or no data: discard it, stop trusting this peer,
            // and fetch the same piece from the next one.
            if (got) ++job->bad;
            ::close(peer);
            peer = -1;
            ++peer_idx;
        }
    }
    if (peer >= 0) ::close(peer);

    // End-to-end check: every piece matched, but verify the whole file too
    // (catches e.g. a bug in offsets, or a disk write that went wrong).
    std::string whole;
    if (!hash_fd(out, meta.size, whole) || whole != meta.file_hash)
        return fail("whole-file SHA-1 mismatch", -1);
    ::close(out);
    if (::rename(part_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(part_path.c_str());
        finish(job, false, std::string("rename failed: ") + std::strerror(errno));
        return;
    }

    // We have the whole file now: become a seeder for it (best effort; e.g.
    // fails harmlessly if the user logged out meanwhile).
    shared_.add({job->st.group, final_path, meta});
    Message rep;
    std::string err;
    link_.call(MsgType::UPLOAD_FILE,
               {job->st.group, meta.name, std::to_string(meta.size), meta.file_hash, join_hashes(meta.piece_hashes)},
               rep, err);
    finish(job, true, "");
}

void DownloadManager::finish(Job* job, bool ok, const std::string& error) {
    std::string line;
    {
        std::lock_guard<std::mutex> lock(mu_);
        job->st.state = ok ? DownloadStatus::COMPLETED : DownloadStatus::FAILED;
        job->st.error = error;
        line = ok ? "download finished: [C] [" + job->st.group + "] " + job->st.name
                  : "download failed: [" + job->st.group + "] " + job->st.name + ": " + error;
        if (ok && job->bad > 0) line += " (" + std::to_string(job->bad.load()) + " bad piece(s) re-fetched)";
    }
    if (announce_) {
        std::printf("%s\n", line.c_str());
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
        out.push_back(s);
    }
    return out;
}

}  // namespace p2p
