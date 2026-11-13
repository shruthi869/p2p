// commands.h - parses one line of user input and runs the command.
#pragma once

#include <string>
#include <vector>

#include "net.h"
#include "protocol.h"
#include "tracker_link.h"

namespace p2p {

class ClientApp {
public:
    ClientApp(TrackerLink& link, const Endpoint& self);

    // Run one input line. Returns false when the user asked to quit.
    bool execute(const std::string& line);

    // Polite exit: log out if logged in.
    void shutdown();

private:
    using Args = std::vector<std::string>;

    // Send a request; print "error: ..." and return false on any failure.
    bool request(MsgType type, const Args& fields, Message& reply);

    void cmd_create_user(const Args& a);
    void cmd_login(const Args& a);
    void cmd_logout();
    void cmd_simple_group(MsgType type, const std::string& group, const char* done_fmt);
    void cmd_list_groups();
    void cmd_list_requests(const std::string& group);
    void cmd_accept_request(const Args& a);
    void print_help() const;

    TrackerLink& link_;
    Endpoint     self_;
    std::string  user_;  // who is logged in on this client ("" = nobody)
};

// Split a line on whitespace.
std::vector<std::string> tokenize(const std::string& line);

}  // namespace p2p
