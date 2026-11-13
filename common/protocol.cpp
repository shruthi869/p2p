// protocol.cpp - framing and the send/recv loops. See protocol.h for the format.
#include "protocol.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>

namespace p2p {

namespace {

// Big-endian encode/decode done by hand with shifts: works the same on any
// CPU, and is easy to explain (equivalent to htonl/ntohl).
void put_u32(char* p, uint32_t v) {
    p[0] = static_cast<char>((v >> 24) & 0xFF);
    p[1] = static_cast<char>((v >> 16) & 0xFF);
    p[2] = static_cast<char>((v >> 8) & 0xFF);
    p[3] = static_cast<char>(v & 0xFF);
}

uint32_t get_u32(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
           (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
}

}  // namespace

// send() may write fewer bytes than asked (socket buffer full), so keep
// going from where it stopped. MSG_NOSIGNAL: if the peer has gone away we
// get EPIPE back instead of a SIGPIPE that would kill the whole process.
IoResult send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;  // interrupted by a signal: retry
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return IoResult::TIMEOUT;
        if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) return IoResult::CLOSED;
        return IoResult::ERROR;
    }
    return IoResult::OK;
}

// recv() may return fewer bytes than asked (data still in flight), so loop.
// recv() == 0 means the peer closed: CLOSED if nothing of this block had
// arrived yet, TRUNCATED if we were part-way through it.
IoResult recv_all(int fd, char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, buf + got, len - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) return got == 0 ? IoResult::CLOSED : IoResult::TRUNCATED;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IoResult::TIMEOUT;
        if (errno == ECONNRESET) return got == 0 ? IoResult::CLOSED : IoResult::TRUNCATED;
        return IoResult::ERROR;
    }
    return IoResult::OK;
}

std::string encode_message(const Message& msg) {
    std::string frame(kHeaderSize + msg.payload.size(), '\0');
    frame[0] = kMagic0;
    frame[1] = kMagic1;
    frame[2] = static_cast<char>(msg.type);
    frame[3] = static_cast<char>(msg.status);
    put_u32(&frame[4], static_cast<uint32_t>(msg.payload.size()));
    if (!msg.payload.empty()) std::memcpy(&frame[kHeaderSize], msg.payload.data(), msg.payload.size());
    return frame;
}

// Header and payload go out in ONE buffer. Sending a tiny header and then
// the payload as two writes can stall ~40 ms (Nagle + delayed ACK).
IoResult send_msg(int fd, const Message& msg) {
    if (msg.payload.size() > kMaxPayload) return IoResult::TOO_LARGE;
    const std::string frame = encode_message(msg);
    return send_all(fd, frame.data(), frame.size());
}

// Read the fixed header, validate it, then read exactly `length` bytes.
IoResult recv_msg(int fd, Message& out) {
    char header[kHeaderSize];
    IoResult r = recv_all(fd, header, kHeaderSize);
    if (r != IoResult::OK) return r;

    if (header[0] != kMagic0 || header[1] != kMagic1) return IoResult::BAD_HEADER;
    const uint32_t len = get_u32(header + 4);
    if (len > kMaxPayload) return IoResult::TOO_LARGE;  // checked BEFORE allocating

    out.type = static_cast<MsgType>(static_cast<uint8_t>(header[2]));
    out.status = static_cast<Status>(static_cast<uint8_t>(header[3]));
    out.payload.assign(len, '\0');
    if (len == 0) return IoResult::OK;

    r = recv_all(fd, &out.payload[0], len);
    if (r == IoResult::CLOSED) return IoResult::TRUNCATED;  // header arrived, payload didn't
    return r;
}

std::string pack_fields(const std::vector<std::string>& fields) {
    std::string out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out += '\n';
        out += fields[i];
    }
    return out;
}

std::vector<std::string> split_fields(const std::string& payload) {
    std::vector<std::string> fields;
    if (payload.empty()) return fields;
    size_t start = 0;
    while (true) {
        size_t nl = payload.find('\n', start);
        if (nl == std::string::npos) {
            fields.push_back(payload.substr(start));
            break;
        }
        fields.push_back(payload.substr(start, nl - start));
        start = nl + 1;
    }
    return fields;
}

const char* status_text(Status s) {
    switch (s) {
        case Status::OK:                return "ok";
        case Status::BAD_REQUEST:       return "malformed request";
        case Status::UNKNOWN_TYPE:      return "unknown message type";
        case Status::NOT_LOGGED_IN:     return "please log in first";
        case Status::ALREADY_LOGGED_IN: return "already logged in";
        case Status::AUTH_FAILED:       return "invalid user id or password";
        case Status::USER_EXISTS:       return "user already exists";
        case Status::NO_SUCH_USER:      return "no such user";
        case Status::GROUP_EXISTS:      return "group already exists";
        case Status::NO_SUCH_GROUP:     return "no such group";
        case Status::NOT_MEMBER:        return "you are not a member of this group";
        case Status::ALREADY_MEMBER:    return "already a member of this group";
        case Status::NOT_OWNER:         return "only the group owner can do this";
        case Status::REQUEST_PENDING:   return "join request already pending";
        case Status::NO_SUCH_REQUEST:   return "no such join request";
        case Status::FILE_CONFLICT:     return "a different file with this name is already shared in the group";
        case Status::NO_SUCH_FILE:      return "no such file in this group";
        case Status::NO_SUCH_PIECE:     return "peer does not have that piece";
        case Status::INTERNAL_ERROR:    return "internal error";
    }
    return "unknown status";
}

const char* io_result_name(IoResult r) {
    switch (r) {
        case IoResult::OK:         return "OK";
        case IoResult::CLOSED:     return "CLOSED";
        case IoResult::TRUNCATED:  return "TRUNCATED";
        case IoResult::TIMEOUT:    return "TIMEOUT";
        case IoResult::BAD_HEADER: return "BAD_HEADER";
        case IoResult::TOO_LARGE:  return "TOO_LARGE";
        case IoResult::ERROR:      return "ERROR";
    }
    return "?";
}

}  // namespace p2p
