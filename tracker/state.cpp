// state.cpp - tracker state and the rules for every operation.
#include "state.h"

#include <algorithm>

#include "file_meta.h"
#include "sha1.h"

namespace p2p {

namespace {

bool contains(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

bool erase_value(std::vector<std::string>& v, const std::string& x) {
    auto it = std::find(v.begin(), v.end(), x);
    if (it == v.end()) return false;
    v.erase(it);  // erase (not swap-and-pop) keeps join order, which decides the next owner
    return true;
}

}  // namespace

std::string password_digest(const std::string& user, const std::string& password) {
    return sha1_hex(user + ":" + password);
}

bool valid_id(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool valid_password(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s)
        if (c <= ' ' || c == 0x7F) return false;  // no spaces or control characters
    return true;
}

Status TrackerState::apply(const Op& op) {
    std::lock_guard<std::mutex> lock(mu_);
    switch (op.type) {
        case MsgType::CREATE_USER:    return create_user(op.args);
        case MsgType::LOGIN:          return login(op.args);
        case MsgType::LOGOUT:         return logout(op.args);
        case MsgType::CREATE_GROUP:   return create_group(op.args);
        case MsgType::JOIN_GROUP:     return join_group(op.args);
        case MsgType::LEAVE_GROUP:    return leave_group(op.args);
        case MsgType::ACCEPT_REQUEST: return accept_request(op.args);
        case MsgType::UPLOAD_FILE:    return upload_file(op.args);
        case MsgType::STOP_SHARE:     return stop_share(op.args);
        default:                      return Status::UNKNOWN_TYPE;
    }
}

Status TrackerState::create_user(const std::vector<std::string>& a) {
    if (a.size() != 2 || !valid_id(a[0])) return Status::BAD_REQUEST;
    if (users_.count(a[0])) return Status::USER_EXISTS;
    users_[a[0]] = User{a[1]};
    return Status::OK;
}

Status TrackerState::login(const std::vector<std::string>& a) {
    if (a.size() != 4) return Status::BAD_REQUEST;
    const std::string& user = a[0];
    auto u = users_.find(user);
    // Unknown user and wrong password give the SAME answer: don't reveal which ids exist.
    if (u == users_.end() || u->second.digest != a[1]) return Status::AUTH_FAILED;

    auto s = sessions_.find(user);
    // One active session per user. The same client address may log in again
    // (it reconnected, e.g. after its tracker failed); that replaces the session.
    if (s != sessions_.end() && s->second.peer_addr != a[2]) return Status::ALREADY_LOGGED_IN;
    sessions_[user] = Session{a[2], a[3]};
    return Status::OK;
}

Status TrackerState::logout(const std::vector<std::string>& a) {
    if (a.size() != 2) return Status::BAD_REQUEST;
    auto s = sessions_.find(a[0]);
    // Only the session that is still current can be ended. A late "connection
    // closed" from an older login must not log out the user's newer login.
    if (s == sessions_.end() || s->second.session_id != a[1]) return Status::NOT_LOGGED_IN;
    sessions_.erase(s);
    drop_seeder_everywhere(a[0]);  // spec: logout = "stop sharing files"
    return Status::OK;
}

Status TrackerState::create_group(const std::vector<std::string>& a) {
    if (a.size() != 2 || !valid_id(a[1])) return Status::BAD_REQUEST;
    if (!users_.count(a[0])) return Status::NO_SUCH_USER;
    if (groups_.count(a[1])) return Status::GROUP_EXISTS;
    groups_[a[1]].members.push_back(a[0]);  // creator is the first member = owner
    return Status::OK;
}

Status TrackerState::join_group(const std::vector<std::string>& a) {
    if (a.size() != 2) return Status::BAD_REQUEST;
    auto g = groups_.find(a[1]);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (contains(g->second.members, a[0])) return Status::ALREADY_MEMBER;
    if (contains(g->second.pending, a[0])) return Status::REQUEST_PENDING;
    g->second.pending.push_back(a[0]);
    return Status::OK;
}

Status TrackerState::leave_group(const std::vector<std::string>& a) {
    if (a.size() != 2) return Status::BAD_REQUEST;
    auto g = groups_.find(a[1]);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (!erase_value(g->second.members, a[0])) return Status::NOT_MEMBER;
    drop_seeder(g->second, a[0]);  // a non-member can't share in the group
    // Owner left -> members.front() is now the next-oldest member (automatic).
    // Nobody left -> the group (and its pending requests) disappears.
    if (g->second.members.empty()) groups_.erase(g);
    return Status::OK;
}

Status TrackerState::accept_request(const std::vector<std::string>& a) {
    if (a.size() != 3) return Status::BAD_REQUEST;
    auto g = groups_.find(a[1]);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (g->second.members.front() != a[0]) return Status::NOT_OWNER;
    if (!erase_value(g->second.pending, a[2])) return Status::NO_SUCH_REQUEST;
    g->second.members.push_back(a[2]);
    return Status::OK;
}

// Share a file's metadata with a group. If the group already has a file with
// this name: same content (same SHA-1 and size) -> the uploader becomes one
// more seeder; different content -> FILE_CONFLICT (names are unique per group).
Status TrackerState::upload_file(const std::vector<std::string>& a) {
    if (a.size() != 6) return Status::BAD_REQUEST;
    auto g = groups_.find(a[1]);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (!contains(g->second.members, a[0])) return Status::NOT_MEMBER;

    uint64_t size = 0;
    std::vector<std::string> hashes;
    if (!valid_file_name(a[2]) || !parse_u64(a[3], size) || size == 0 || !is_hex_hash(a[4]) ||
        !split_hashes(a[5], hashes) || hashes.size() != piece_count(size))
        return Status::BAD_REQUEST;

    auto f = g->second.files.find(a[2]);
    if (f != g->second.files.end()) {
        if (f->second.file_hash != a[4] || f->second.size != size) return Status::FILE_CONFLICT;
        if (!contains(f->second.seeders, a[0])) f->second.seeders.push_back(a[0]);
        return Status::OK;  // sharing the same file again is harmless (idempotent)
    }
    g->second.files[a[2]] = SharedFile{size, a[4], a[5], {a[0]}};
    return Status::OK;
}

Status TrackerState::stop_share(const std::vector<std::string>& a) {
    if (a.size() != 3) return Status::BAD_REQUEST;
    auto g = groups_.find(a[1]);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (!contains(g->second.members, a[0])) return Status::NOT_MEMBER;
    auto f = g->second.files.find(a[2]);
    if (f == g->second.files.end()) return Status::NO_SUCH_FILE;
    if (!erase_value(f->second.seeders, a[0])) return Status::NOT_SHARING;
    if (f->second.seeders.empty()) g->second.files.erase(f);  // nobody has it any more
    return Status::OK;
}

void TrackerState::drop_seeder(Group& g, const std::string& user) {
    for (auto f = g.files.begin(); f != g.files.end();) {
        erase_value(f->second.seeders, user);
        if (f->second.seeders.empty()) f = g.files.erase(f);
        else ++f;
    }
}

void TrackerState::drop_seeder_everywhere(const std::string& user) {
    for (auto& kv : groups_) drop_seeder(kv.second, user);
}

Status TrackerState::list_files(const std::string& actor, const std::string& group,
                                std::vector<FileSummary>& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto g = groups_.find(group);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (!contains(g->second.members, actor)) return Status::NOT_MEMBER;
    out.clear();
    for (const auto& kv : g->second.files) out.push_back({kv.first, kv.second.size, kv.second.seeders.size()});
    return Status::OK;
}

Status TrackerState::file_info(const std::string& actor, const std::string& group, const std::string& name,
                               FileDetails& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto g = groups_.find(group);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (!contains(g->second.members, actor)) return Status::NOT_MEMBER;
    auto f = g->second.files.find(name);
    if (f == g->second.files.end()) return Status::NO_SUCH_FILE;
    out.size = f->second.size;
    out.file_hash = f->second.file_hash;
    out.piece_hashes = f->second.piece_hashes;
    out.seeder_addrs.clear();
    for (const auto& u : f->second.seeders) {
        auto s = sessions_.find(u);
        if (s != sessions_.end()) out.seeder_addrs.push_back(s->second.peer_addr);
    }
    return Status::OK;
}

std::vector<std::string> TrackerState::list_groups() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (const auto& kv : groups_) out.push_back(kv.first);  // std::map -> already sorted
    return out;
}

Status TrackerState::list_requests(const std::string& actor, const std::string& group,
                                   std::vector<std::string>& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto g = groups_.find(group);
    if (g == groups_.end()) return Status::NO_SUCH_GROUP;
    if (g->second.members.front() != actor) return Status::NOT_OWNER;
    out = g->second.pending;
    return Status::OK;
}

bool TrackerState::user_exists(const std::string& user) const {
    std::lock_guard<std::mutex> lock(mu_);
    return users_.count(user) != 0;
}

bool TrackerState::is_online(const std::string& user) const {
    std::lock_guard<std::mutex> lock(mu_);
    return sessions_.count(user) != 0;
}

std::string TrackerState::group_owner(const std::string& group) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto g = groups_.find(group);
    return g == groups_.end() ? "" : g->second.members.front();
}

std::vector<std::string> TrackerState::group_members(const std::string& group) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto g = groups_.find(group);
    return g == groups_.end() ? std::vector<std::string>{} : g->second.members;
}

}  // namespace p2p
