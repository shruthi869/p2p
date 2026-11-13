// net.h - thin wrappers over POSIX sockets, plus tracker_info.txt parsing.
// Every function returns -1 / false on failure with errno (or err) set, and
// never leaks a file descriptor on its error paths.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

struct Endpoint {
    std::string ip;    // dotted IPv4, e.g. "127.0.0.1"
    uint16_t    port = 0;
    std::string str() const { return ip + ":" + std::to_string(port); }
};

// "IP:PORT" -> Endpoint. Rejects bad IPs, port 0, ports > 65535, junk.
bool parse_endpoint(const std::string& text, Endpoint& out);

// Strict decimal port parser (1..65535).
bool parse_port(const std::string& text, uint16_t& out);

// tracker_info.txt: one "IP PORT" per line; blank lines and '#' comments are
// ignored; '\r' is stripped (file may be written on Windows). Exactly two
// trackers are required. On failure returns false and fills err.
bool read_tracker_info(const std::string& path, std::vector<Endpoint>& out, std::string& err);

// socket + SO_REUSEADDR + bind + listen. SO_REUSEADDR lets a restarted
// tracker bind its port again while old connections sit in TIME_WAIT.
int listen_tcp(const Endpoint& ep, int backlog = 64);

// Connect, giving up after timeout_ms (so failover to the other tracker is
// quick even if a host silently drops packets). Returns a blocking socket.
int connect_tcp(const Endpoint& ep, int timeout_ms);

// Per-call recv/send timeout (SO_RCVTIMEO/SO_SNDTIMEO). 0 = wait forever.
// Used where we expect an answer, so a hung peer cannot block us forever.
bool set_io_timeout(int fd, int timeout_ms);

// Port a socket is bound to (useful after binding port 0 in tests).
uint16_t local_port(int fd);

// Ignore SIGPIPE process-wide: belt-and-braces with MSG_NOSIGNAL.
void ignore_sigpipe();

}  // namespace p2p
