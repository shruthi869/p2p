// shared_files.h - the files THIS client is sharing, keyed by (group, name).
// Remembers where each file lives on disk and its metadata, so that:
//   - the peer server (Part 5) can find the bytes when someone asks for a piece;
//   - after logout + login the client can share them again without rehashing.
// Thread-safe: the command loop writes it, peer-serving threads read it.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "file_meta.h"

namespace p2p {

struct SharedFile {
    std::string group;
    std::string path;  // where the bytes are on this machine
    FileMeta    meta;
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
    bool find(const std::string& group, const std::string& name, SharedFile& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = files_.find({group, name});
        if (it == files_.end()) return false;
        out = it->second;
        return true;
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
