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
struct Op {
    MsgType                  type;
    std::vector<std::string> args;
};

// What we store instead of the password. Part 4 replaces this with a salted
// SHA1; until SHA1 exists it is a clearly-marked placeholder.
std::string password_digest(const std::string& user, const std::string& password);

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
    struct Group {
        // members in join order; members.front() IS the owner. When the owner
        // leaves, the next-oldest member automatically becomes owner.
        std::vector<std::string> members;
        std::vector<std::string> pending;  // join requests, in arrival order
    };

    // Each handler assumes mu_ is held.
    Status create_user(const std::vector<std::string>& a);
    Status login(const std::vector<std::string>& a);
    Status logout(const std::vector<std::string>& a);
    Status create_group(const std::vector<std::string>& a);
    Status join_group(const std::vector<std::string>& a);
    Status leave_group(const std::vector<std::string>& a);
    Status accept_request(const std::vector<std::string>& a);

    // One mutex for the whole state: simple, obviously deadlock-free, and the
    // tracker's work per request is tiny (map lookups), so contention is low.
    mutable std::mutex mu_;
    std::map<std::string, User>    users_;     // std::map: sorted output for list commands
    std::map<std::string, Session> sessions_;  // user -> current session
    std::map<std::string, Group>   groups_;
};

}  // namespace p2p
