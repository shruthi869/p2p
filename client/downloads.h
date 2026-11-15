// downloads.h - background, parallel, multi-peer downloads.
//
// download_file returns at once; each download runs on its own coordinator
// thread, so several files can download at the same time while the user
// keeps typing.
//
// One download, step by step:
//   1. ask the tracker for FILE_INFO: size, file SHA-1, piece SHA-1s, peers
//   2. create "<dest>/<name>.part", size it with ftruncate (sparse file), and
//      register it as a PARTIAL share: verified pieces are served to others
//   3. start one worker thread per peer (at most kMaxWorkers). Each worker:
//      connect -> GET_PIECE_MAP -> loop { PiecePicker gives the rarest piece
//      this peer has -> GET_PIECE -> check SHA-1 -> pwrite at its offset }.
//      Bad hash -> piece goes back to the picker, this peer is not trusted
//      again. Dead/slow peer -> piece goes back, worker ends.
//   4. if pieces are still missing, ask the tracker again (new peers may have
//      appeared) - up to kMaxRounds rounds
//   5. re-read the finished file and check the whole-file SHA-1
//   6. give it its final name (atomically) and keep seeding it
#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "file_meta.h"
#include "net.h"
#include "piece_picker.h"
#include "shared_files.h"
#include "tracker_link.h"

namespace p2p {

struct DownloadStatus {
    enum State { RUNNING, COMPLETED, FAILED };
    std::string                     group, name, dest_path;
    uint64_t                        pieces_total = 0;
    uint64_t                        pieces_done = 0;
    uint64_t                        bad_pieces = 0;   // failed their SHA-1 check and were re-fetched
    uint64_t                        peers_active = 0; // workers currently running
    std::map<std::string, uint64_t> per_peer;         // "ip:port" -> pieces it gave us
    State                           state = RUNNING;
    std::string                     error;
};

class DownloadManager {
public:
    static constexpr size_t kMaxWorkers = 4;  // peers used at once per download
    static constexpr int    kMaxRounds = 3;   // times we go back to the tracker for peers

    DownloadManager(TrackerLink& link, SharedFiles& shared, const Endpoint& self, bool announce = true);
    ~DownloadManager();  // cancels running downloads and joins their threads
    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    // Checks everything that can be checked up front (file exists in the
    // group, some OTHER peer has it, destination is a directory, file not
    // already there) and starts the background thread. False + err if not.
    bool start(const std::string& group, const std::string& name, const std::string& dest_dir,
               std::string& err);

    std::vector<DownloadStatus> snapshot() const;

private:
    struct Job {
        DownloadStatus               st;  // guarded by DownloadManager::mu_ (except the atomics below)
        FileMeta                     meta;
        std::vector<Endpoint>        peers;
        std::unique_ptr<PiecePicker> picker;
        std::atomic<uint64_t>        done{0};
        std::atomic<uint64_t>        bad{0};
        std::atomic<uint64_t>        active{0};
        std::atomic<bool>            announced{false};  // told the tracker we share (part of) it
        std::set<std::string>        bad_peers;         // guarded by mu_
        std::string                  fatal;             // e.g. disk write error; guarded by mu_
        std::thread                  thread;
    };

    void run(Job* job);
    void worker(Job* job, const Endpoint& peer, int out_fd);
    bool refresh_peers(Job* job);
    void announce(Job* job);
    void finish(Job* job, bool ok, const std::string& error);

    TrackerLink&                    link_;
    SharedFiles&                    shared_;
    const Endpoint                  self_;
    const bool                      announce_;  // print "download finished/failed" lines
    mutable std::mutex              mu_;
    std::list<std::unique_ptr<Job>> jobs_;
    std::atomic<bool>               cancel_{false};
};

}  // namespace p2p
