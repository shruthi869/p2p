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

// Session ids look like "<tracker_no>-<start time>-<counter>": the tracker
// that created a session (the one its client is connected to) owns it.
// Returns 1 or 2, or 0 if the id has some other form.
int session_owner(const std::string& session_id);

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

    // ---- tracker-to-tracker sync (see sync.h for the protocol) ----

    // The whole state as text. Canonical: equal states give equal strings,
    // which is also how the tests check that two trackers have converged.
    std::string snapshot() const;

    // Merge the other tracker's snapshot into ours. The rules are written in
    // terms of "tracker 1's copy" and "tracker 2's copy", so tracker 1
    // merging 2's snapshot and tracker 2 merging 1's produce the SAME state:
    //   users    : union; same id with different passwords -> tracker 1's wins
    //   sessions : each tracker is the authority for sessions it created
    //              (clients connected to it); same user in both -> tracker 1's
    //   groups   : union; same id and same owner -> members/requests/files
    //              combined (tracker 1's order first); different owners
    //              (created independently during a partition) -> tracker 1's
    //   files    : same name+content -> seeder lists combined; different
    //              content -> tracker 1's
    // Afterwards, seeders who are not online are removed (logout = stop sharing).
    // Returns false (and changes nothing) if the snapshot is malformed.
    bool merge(const std::string& peer_snapshot, int peer_no, int my_no);

    // The other tracker is unreachable: forget the sessions it vouched for
    // (its clients will fail over and log in here, or come back in its next
    // snapshot), and their seeder entries. Returns how many were dropped.
    size_t drop_sessions_owned_by(int tracker_no);

    struct Counts {
        size_t users = 0, sessions = 0, groups = 0, files = 0;
    };
    Counts counts() const;

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

    using Users = std::map<std::string, User>;
    using Sessions = std::map<std::string, Session>;
    using Groups = std::map<std::string, Group>;
    static bool parse_snapshot(const std::string& s, Users& u, Sessions& se, Groups& g);
    // Seeders must be online: drop the others, and files left with no seeder.
    static void prune_offline_seeders(Groups& g, const Sessions& se);

    // One mutex for the whole state: simple, obviously deadlock-free, and the
    // tracker's work per request is tiny (map lookups), so contention is low.
    mutable std::mutex mu_;
    std::map<std::string, User>    users_;     // std::map: sorted output for list commands
    std::map<std::string, Session> sessions_;  // user -> current session
    std::map<std::string, Group>   groups_;
};

}  // namespace p2p
