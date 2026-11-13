// state.h - everything the tracker knows (users, sessions, groups), with no
// networking in it, so it can be unit-tested directly.
//
// KEY IDEA: every change to the state is an "operation" (Op) applied through
// ONE function, apply(). apply() validates and mutates under one lock, and it
// is deterministic: the same ops applied in the same order give the same
// state. That is what makes tracker-to-tracker sync simple later: a tracker
// applies an op locally, then forwards the same op to the other tracker,
// which applies it too (state machine replication, in miniature).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "protocol.h"

namespace p2p {

// One state change. `type` reuses the protocol's MsgType numbers; `args` are:
//   CREATE_USER    user, password_digest
//   LOGIN          user, password_digest, peer_addr, session_id
//   LOGOUT         user, session_id
//   CREATE_GROUP   actor, group
//   JOIN_GROUP     actor, group
//   LEAVE_GROUP    actor, group
//   ACCEPT_REQUEST actor, group, user
//   UPLOAD_FILE    actor, group, file_name, size, file_sha1, piece_sha1s (joined)
//   STOP_SHARE     actor, group, file_name
struct Op {
    MsgType                  type;
    std::vector<std::string> args;
};

// What we store instead of the password: SHA1(user_id + ":" + password).
// The user id acts as a salt, so two users with the same password get
// different digests. (Real systems: random salt + a deliberately slow hash
// such as bcrypt/argon2, to make brute force expensive.)
std::string password_digest(const std::string& user, const std::string& password);

// One file in a group listing (LIST_FILES).
struct FileSummary {
    std::string name;
    uint64_t    size = 0;
    size_t      seeders = 0;
};

// Everything a downloader needs (FILE_INFO).
struct FileDetails {
    uint64_t                 size = 0;
    std::string              file_hash;
    std::string              piece_hashes;  // joined, 40 hex chars per piece
    std::vector<std::string> seeder_addrs;  // "ip:port" of online seeders
};

// Id rules: 1-64 chars of [A-Za-z0-9_.-]. Passwords: 1-128 printable, no spaces.
bool valid_id(const std::string& s);
bool valid_password(const std::string& s);

class TrackerState {
public:
    // Validate + apply one op atomically. Returns OK or the reason it was
    // rejected (in which case nothing changed).
    Status apply(const Op& op);

    // Read-only queries.
    std::vector<std::string> list_groups() const;
    Status list_requests(const std::string& actor, const std::string& group,
                         std::vector<std::string>& out) const;
    Status list_files(const std::string& actor, const std::string& group,
                      std::vector<FileSummary>& out) const;
    Status file_info(const std::string& actor, const std::string& group, const std::string& name,
                     FileDetails& out) const;

    // Small helpers for tests and logs.
    bool        user_exists(const std::string& user) const;
    bool        is_online(const std::string& user) const;
    std::string group_owner(const std::string& group) const;  // "" if no such group
    std::vector<std::string> group_members(const std::string& group) const;

private:
    struct User {
        std::string digest;
    };
    struct Session {
        std::string peer_addr;   // where this user's client serves pieces
        std::string session_id;  // unique per login, so a stale disconnect can't log out a newer login
    };
    // A shared file. The tracker stores only METADATA, never file data.
    // A file exists only while someone shares it: when the last seeder stops
    // (stop_share, logout, disconnect, leave_group) the entry is removed.
    struct SharedFile {
        uint64_t                 size = 0;
        std::string              file_hash;
        std::string              piece_hashes;  // joined
        std::vector<std::string> seeders;       // user ids, in the order they started sharing
    };
    struct Group {
        // members in join order; members.front() IS the owner. When the owner
        // leaves, the next-oldest member automatically becomes owner.
        std::vector<std::string>          members;
        std::vector<std::string>          pending;  // join requests, in arrival order
        std::map<std::string, SharedFile> files;    // file name -> metadata
    };

    // Each handler assumes mu_ is held.
    Status create_user(const std::vector<std::string>& a);
    Status login(const std::vector<std::string>& a);
    Status logout(const std::vector<std::string>& a);
    Status create_group(const std::vector<std::string>& a);
    Status join_group(const std::vector<std::string>& a);
    Status leave_group(const std::vector<std::string>& a);
    Status accept_request(const std::vector<std::string>& a);
    Status upload_file(const std::vector<std::string>& a);
    Status stop_share(const std::vector<std::string>& a);

    // Remove `user` as a seeder from the files of one group / every group.
    static void drop_seeder(Group& g, const std::string& user);
    void        drop_seeder_everywhere(const std::string& user);

    // One mutex for the whole state: simple, obviously deadlock-free, and the
    // tracker's work per request is tiny (map lookups), so contention is low.
    mutable std::mutex mu_;
    std::map<std::string, User>    users_;     // std::map: sorted output for list commands
    std::map<std::string, Session> sessions_;  // user -> current session
    std::map<std::string, Group>   groups_;
};

}  // namespace p2p
