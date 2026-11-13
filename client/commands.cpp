// commands.cpp - the client's command-line interface.
//
// Output convention (the integration tests rely on it):
//   success  -> a plain sentence, e.g. "group g1 created (you are the owner)"
//   failure  -> "error: <reason>"
//   bad args -> "usage: <command> <args>"
#include "commands.h"

#include <cstdio>
#include <sstream>

namespace p2p {

std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream ss(line);
    std::vector<std::string> out;
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

ClientApp::ClientApp(TrackerLink& link, const Endpoint& self) : link_(link), self_(self) {}

bool ClientApp::request(MsgType type, const Args& fields, Message& reply) {
    std::string err;
    if (!link_.call(type, fields, reply, err)) {
        std::printf("error: %s\n", err.c_str());
        return false;
    }
    if (reply.status != Status::OK) {
        std::printf("error: %s\n", status_text(reply.status));
        return false;
    }
    return true;
}

namespace {

struct CommandSpec {
    const char* name;
    size_t      nargs;
    const char* usage;
};

const CommandSpec kCommands[] = {
    {"create_user", 2, "create_user <user_id> <password>"},
    {"login", 2, "login <user_id> <password>"},
    {"create_group", 1, "create_group <group_id>"},
    {"join_group", 1, "join_group <group_id>"},
    {"leave_group", 1, "leave_group <group_id>"},
    {"list_groups", 0, "list_groups"},
    {"list_requests", 1, "list_requests <group_id>"},
    {"accept_request", 2, "accept_request <group_id> <user_id>"},
    {"logout", 0, "logout"},
    {"upload_file", 2, "upload_file <group_id> <file_path>"},
    {"list_files", 1, "list_files <group_id>"},
    {"download_file", 3, "download_file <group_id> <file_name> <destination_path>"},
    {"show_downloads", 0, "show_downloads"},
    {"stop_share", 2, "stop_share <group_id> <file_name>"},
    {"help", 0, "help"},
    {"quit", 0, "quit"},
};

const CommandSpec* find_command(const std::string& name) {
    for (const auto& c : kCommands)
        if (name == c.name) return &c;
    return nullptr;
}

}  // namespace

bool ClientApp::execute(const std::string& line) {
    const Args tok = tokenize(line);
    if (tok.empty()) return true;
    const CommandSpec* spec = find_command(tok[0]);
    if (!spec) {
        std::printf("error: unknown command '%s' (type 'help')\n", tok[0].c_str());
        return true;
    }
    const Args a(tok.begin() + 1, tok.end());
    if (a.size() != spec->nargs) {
        std::printf("usage: %s\n", spec->usage);
        return true;
    }
    const std::string cmd = tok[0];

    if (cmd == "quit") {
        shutdown();
        return false;
    }
    if (cmd == "help")           print_help();
    else if (cmd == "create_user")    cmd_create_user(a);
    else if (cmd == "login")          cmd_login(a);
    else if (cmd == "logout")         cmd_logout();
    else if (cmd == "create_group")   cmd_simple_group(MsgType::CREATE_GROUP, a[0], "group %s created (you are the owner)");
    else if (cmd == "join_group")     cmd_simple_group(MsgType::JOIN_GROUP, a[0], "join request sent for group %s");
    else if (cmd == "leave_group")    cmd_simple_group(MsgType::LEAVE_GROUP, a[0], "left group %s");
    else if (cmd == "list_groups")    cmd_list_groups();
    else if (cmd == "list_requests")  cmd_list_requests(a[0]);
    else if (cmd == "accept_request") cmd_accept_request(a);
    else std::printf("error: '%s' is not implemented yet\n", cmd.c_str());
    return true;
}

void ClientApp::cmd_create_user(const Args& a) {
    Message rep;
    if (request(MsgType::CREATE_USER, {a[0], a[1]}, rep)) std::printf("user %s created\n", a[0].c_str());
}

void ClientApp::cmd_login(const Args& a) {
    if (!user_.empty()) {
        std::printf("error: already logged in as %s (logout first)\n", user_.c_str());
        return;
    }
    Message rep;
    // We tell the tracker our own listening address: that is where other
    // peers will connect to fetch pieces from us.
    if (!request(MsgType::LOGIN, {a[0], a[1], self_.str()}, rep)) return;
    user_ = a[0];
    link_.remember_login(a[0], a[1], self_.str());
    std::printf("logged in as %s\n", user_.c_str());
}

void ClientApp::cmd_logout() {
    if (user_.empty()) {
        std::printf("error: %s\n", status_text(Status::NOT_LOGGED_IN));
        return;
    }
    Message rep;
    std::string err;
    if (!link_.call(MsgType::LOGOUT, {}, rep, err)) {
        std::printf("error: %s\n", err.c_str());
        return;
    }
    // NOT_LOGGED_IN means the tracker had already ended our session: we are
    // logged out either way, so clear local state too.
    if (rep.status != Status::OK && rep.status != Status::NOT_LOGGED_IN) {
        std::printf("error: %s\n", status_text(rep.status));
        return;
    }
    user_.clear();
    link_.forget_login();
    std::printf("logged out\n");
}

void ClientApp::cmd_simple_group(MsgType type, const std::string& group, const char* done_fmt) {
    Message rep;
    if (request(type, {group}, rep)) {
        std::printf(done_fmt, group.c_str());
        std::printf("\n");
    }
}

void ClientApp::cmd_list_groups() {
    Message rep;
    if (!request(MsgType::LIST_GROUPS, {}, rep)) return;
    const Args groups = split_fields(rep.payload);
    if (groups.empty()) {
        std::printf("no groups\n");
        return;
    }
    std::printf("groups (%zu):\n", groups.size());
    for (const auto& g : groups) std::printf("  %s\n", g.c_str());
}

void ClientApp::cmd_list_requests(const std::string& group) {
    Message rep;
    if (!request(MsgType::LIST_REQUESTS, {group}, rep)) return;
    const Args users = split_fields(rep.payload);
    if (users.empty()) {
        std::printf("no pending requests for %s\n", group.c_str());
        return;
    }
    std::printf("pending requests for %s (%zu):\n", group.c_str(), users.size());
    for (const auto& u : users) std::printf("  %s\n", u.c_str());
}

void ClientApp::cmd_accept_request(const Args& a) {
    Message rep;
    if (request(MsgType::ACCEPT_REQUEST, {a[0], a[1]}, rep))
        std::printf("%s added to group %s\n", a[1].c_str(), a[0].c_str());
}

void ClientApp::print_help() const {
    std::printf("commands:\n");
    for (const auto& c : kCommands) std::printf("  %s\n", c.usage);
}

void ClientApp::shutdown() {
    if (user_.empty()) return;
    Message rep;
    std::string err;
    link_.call(MsgType::LOGOUT, {}, rep, err);  // best effort; tracker also logs us out on disconnect
    user_.clear();
    link_.forget_login();
}

}  // namespace p2p
