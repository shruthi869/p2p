// server.cpp - accept loop, per-connection request loop, request dispatch.
#include "server.h"

#include <chrono>
#include <cstdio>

namespace p2p {

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const char* op_name(MsgType t) {
    switch (t) {
        case MsgType::CREATE_USER:    return "create_user";
        case MsgType::LOGIN:          return "login";
        case MsgType::LOGOUT:         return "logout";
        case MsgType::CREATE_GROUP:   return "create_group";
        case MsgType::JOIN_GROUP:     return "join_group";
        case MsgType::LEAVE_GROUP:    return "leave_group";
        case MsgType::ACCEPT_REQUEST: return "accept_request";
        case MsgType::UPLOAD_FILE:    return "upload_file";
        case MsgType::STOP_SHARE:     return "stop_share";
        default:                      return "?";
    }
}

}  // namespace

TrackerServer::TrackerServer(TrackerState& state, int tracker_no, bool verbose)
    : state_(state), tracker_no_(tracker_no), verbose_(verbose), start_ms_(now_ms()),
      server_([this](int fd) { serve(fd); }) {}

// Stop the connection threads BEFORE the members they use are destroyed.
TrackerServer::~TrackerServer() { server_.stop(); }

// One thread per client: read a request, handle it, send the reply, repeat.
// No recv timeout here: a logged-in client may legitimately stay idle for hours.
void TrackerServer::serve(int fd) {
    ClientSession cs;
    while (true) {
        Message req;
        IoResult r = recv_msg(fd, req);
        if (r != IoResult::OK) {
            if (r == IoResult::BAD_HEADER || r == IoResult::TOO_LARGE)
                log(std::string("dropping connection: ") + io_result_name(r));
            break;
        }
        Message rep = handle(cs, req);
        if (send_msg(fd, rep) != IoResult::OK) break;
    }
    // Client went away without "logout" (crash, Ctrl-C, network loss):
    // end its session so it stops being offered as a seeder.
    if (!cs.user.empty() && state_.apply({MsgType::LOGOUT, {cs.user, cs.session_id}}) == Status::OK)
        log(cs.user + " disconnected (auto logout)");
}

std::string TrackerServer::new_session_id() {
    // tracker number + this run's start time + counter: unique even across
    // tracker restarts and between the two trackers.
    return std::to_string(tracker_no_) + "-" + std::to_string(start_ms_) + "-" +
           std::to_string(++session_counter_);
}

Message TrackerServer::handle(ClientSession& cs, const Message& req) {
    Message rep;
    rep.type = reply_type(req.type);
    const std::vector<std::string> f = split_fields(req.payload);
    const bool logged_in = !cs.user.empty();

    // Apply a state change and log it if it succeeded.
    auto apply = [&](const Op& op) {
        Status s = state_.apply(op);
        if (s == Status::OK && verbose_) {
            std::string line = op_name(op.type);
            for (size_t i = 0; i < op.args.size(); ++i) {
                const bool secret = (op.type == MsgType::CREATE_USER || op.type == MsgType::LOGIN) && i == 1;
                const bool bulky = op.type == MsgType::UPLOAD_FILE && i >= 4;  // hash lists
                if (!secret && !bulky) line += " " + op.args[i];  // never log password digests
            }
            log(line);
        }
        return s;
    };

    switch (req.type) {
        case MsgType::PING:
            break;

        case MsgType::CREATE_USER:
            if (f.size() != 2 || !valid_id(f[0]) || !valid_password(f[1])) {
                rep.status = Status::BAD_REQUEST;
            } else {
                rep.status = apply({MsgType::CREATE_USER, {f[0], password_digest(f[0], f[1])}});
            }
            break;

        case MsgType::LOGIN: {
            Endpoint peer;
            if (logged_in) {
                rep.status = Status::ALREADY_LOGGED_IN;
            } else if (f.size() != 3 || !parse_endpoint(f[2], peer)) {
                rep.status = Status::BAD_REQUEST;
            } else {
                std::string sid = new_session_id();
                rep.status = apply({MsgType::LOGIN, {f[0], password_digest(f[0], f[1]), peer.str(), sid}});
                if (rep.status == Status::OK) cs = ClientSession{f[0], sid};
            }
            break;
        }

        case MsgType::LOGOUT:
            if (!logged_in) {
                rep.status = Status::NOT_LOGGED_IN;
            } else {
                rep.status = apply({MsgType::LOGOUT, {cs.user, cs.session_id}});
                cs = ClientSession{};  // this connection is logged out either way
            }
            break;

        case MsgType::CREATE_GROUP:
        case MsgType::JOIN_GROUP:
        case MsgType::LEAVE_GROUP:
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (f.size() != 1)  rep.status = Status::BAD_REQUEST;
            else                     rep.status = apply({req.type, {cs.user, f[0]}});
            break;

        case MsgType::ACCEPT_REQUEST:
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (f.size() != 2)  rep.status = Status::BAD_REQUEST;
            else                     rep.status = apply({req.type, {cs.user, f[0], f[1]}});
            break;

        case MsgType::LIST_GROUPS:
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (!f.empty())     rep.status = Status::BAD_REQUEST;
            else                     rep.payload = pack_fields(state_.list_groups());
            break;

        case MsgType::LIST_REQUESTS: {
            std::vector<std::string> pending;
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (f.size() != 1)  rep.status = Status::BAD_REQUEST;
            else {
                rep.status = state_.list_requests(cs.user, f[0], pending);
                rep.payload = pack_fields(pending);
            }
            break;
        }

        case MsgType::UPLOAD_FILE:  // group, name, size, file_sha1, piece_sha1s
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (f.size() != 5)  rep.status = Status::BAD_REQUEST;
            else rep.status = apply({req.type, {cs.user, f[0], f[1], f[2], f[3], f[4]}});
            break;

        case MsgType::STOP_SHARE:   // group, name
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (f.size() != 2)  rep.status = Status::BAD_REQUEST;
            else                     rep.status = apply({req.type, {cs.user, f[0], f[1]}});
            break;

        case MsgType::LIST_FILES: {  // reply: one field per file, "name\tsize\tseeders"
            std::vector<FileSummary> files;
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (f.size() != 1)  rep.status = Status::BAD_REQUEST;
            else if ((rep.status = state_.list_files(cs.user, f[0], files)) == Status::OK) {
                std::vector<std::string> out;
                for (const auto& x : files)
                    out.push_back(x.name + "\t" + std::to_string(x.size) + "\t" + std::to_string(x.seeders));
                rep.payload = pack_fields(out);
            }
            break;
        }

        case MsgType::FILE_INFO: {  // reply: size, file_sha1, piece_sha1s, seeder addresses...
            FileDetails d;
            if (!logged_in)          rep.status = Status::NOT_LOGGED_IN;
            else if (f.size() != 2)  rep.status = Status::BAD_REQUEST;
            else if ((rep.status = state_.file_info(cs.user, f[0], f[1], d)) == Status::OK) {
                std::vector<std::string> out = {std::to_string(d.size), d.file_hash, d.piece_hashes};
                out.insert(out.end(), d.seeder_addrs.begin(), d.seeder_addrs.end());
                rep.payload = pack_fields(out);
            }
            break;
        }

        default:
            rep.status = Status::UNKNOWN_TYPE;
            break;
    }
    return rep;
}

void TrackerServer::log(const std::string& line) const {
    if (!verbose_) return;
    std::printf("[tracker %d] %s\n", tracker_no_, line.c_str());
    std::fflush(stdout);
}

}  // namespace p2p
