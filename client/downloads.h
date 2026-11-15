// downloads.h - background downloads.
//
// download_file returns at once; each download runs on its own thread, so
// several files can download at the same time while the user keeps typing.
//
// One download, step by step (Part 5: one peer at a time):
//   1. ask the tracker for FILE_INFO: size, file SHA-1, piece SHA-1s, seeders
//   2. create "<dest>/<name>.part" and size it with ftruncate (sparse file)
//   3. for each piece: GET_PIECE from the current peer -> check its SHA-1 ->
//      pwrite it at offset index * 512 KiB. A bad or missing piece (wrong
//      hash, peer died, timeout) -> drop that peer, ask the next one
//   4. re-read the finished file and check the whole-file SHA-1
//   5. rename ".part" -> final name (atomic: nobody ever sees a half file
//      under the real name), then share it: we become a seeder too
#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "file_meta.h"
#include "net.h"
#include "shared_files.h"
#include "tracker_link.h"

namespace p2p {

struct DownloadStatus {
    enum State { RUNNING, COMPLETED, FAILED };
    std::string group, name, dest_path;
    uint64_t    pieces_total = 0;
    uint64_t    pieces_done = 0;
    uint64_t    bad_pieces = 0;  // pieces that failed their SHA-1 check and were re-fetched
    State       state = RUNNING;
    std::string error;
};

class DownloadManager {
public:
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
        DownloadStatus        st;     // guarded by DownloadManager::mu_ (except the atomics below)
        FileMeta              meta;
        std::vector<Endpoint> peers;
        std::atomic<uint64_t> done{0};
        std::atomic<uint64_t> bad{0};
        std::thread           thread;
    };

    void run(Job* job);
    void finish(Job* job, bool ok, const std::string& error);

    TrackerLink&                     link_;
    SharedFiles&                     shared_;
    const Endpoint                   self_;
    const bool                       announce_;  // print "download finished/failed" lines
    mutable std::mutex               mu_;
    std::list<std::unique_ptr<Job>>  jobs_;
    std::atomic<bool>                cancel_{false};
};

}  // namespace p2p
