// shared_files.h - the files THIS client is sharing, keyed by (group, name).
// Remembers where each file lives on disk and its metadata, so that:
//   - the peer server can find the bytes when someone asks for a piece;
//   - after logout + login the client can share them again without rehashing.
// A file still being downloaded is shared too ("partial seeding"): `have`
// says which pieces are already verified on disk; only those are served.
// Thread-safe: the command loop and download threads write it, peer-serving
// threads read it.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "file_meta.h"

namespace p2p {

struct SharedFile {
    std::string       group;
    std::string       path;  // where the bytes are on this machine
    FileMeta          meta;
    std::vector<bool> have;  // empty = complete file; otherwise one flag per piece

    bool has_piece(uint64_t i) const { return have.empty() || (i < have.size() && have[i]); }
    // Piece map as sent on the wire: one '1'/'0' character per piece.
    std::string piece_map() const {
        std::string m(piece_count(meta.size), '1');
        if (!have.empty())
            for (size_t i = 0; i < m.size(); ++i) m[i] = have[i] ? '1' : '0';
        return m;
    }
};

class SharedFiles {
public:
    void add(const SharedFile& f) {
        std::lock_guard<std::mutex> lock(mu_);
        files_[{f.group, f.meta.name}] = f;
    }
    bool remove(const std::string& group, const std::string& name) {
        std::lock_guard<std::mutex> lock(mu_);
        return files_.erase({group, name}) > 0;
    }
    void remove_group(const std::string& group) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = files_.begin(); it != files_.end();) {
            if (it->first.first == group) it = files_.erase(it);
            else ++it;
        }
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        files_.clear();
    }
    bool contains(const std::string& group, const std::string& name) const {
        std::lock_guard<std::mutex> lock(mu_);
        return files_.count({group, name}) != 0;
    }
    bool find(const std::string& group, const std::string& name, SharedFile& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = files_.find({group, name});
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    // A download verified and wrote piece i.
    void mark_piece(const std::string& group, const std::string& name, uint64_t i) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = files_.find({group, name});
        if (it != files_.end() && i < it->second.have.size()) it->second.have[i] = true;
    }
    // The download finished: the file now lives at `path` and is complete.
    void complete(const std::string& group, const std::string& name, const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = files_.find({group, name});
        if (it == files_.end()) return;
        it->second.path = path;
        it->second.have.clear();
    }
    std::vector<SharedFile> all() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<SharedFile> v;
        for (const auto& kv : files_) v.push_back(kv.second);
        return v;
    }

private:
    mutable std::mutex                                         mu_;
    std::map<std::pair<std::string, std::string>, SharedFile> files_;
};

}  // namespace p2p
