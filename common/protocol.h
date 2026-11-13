// protocol.h - the wire protocol shared by tracker and client.
//
// TCP is a byte STREAM, not a message stream: one send() can arrive as
// several recv()s, and several send()s can arrive in one recv(). So every
// message is "framed": a fixed-size header that says how long the payload is,
// followed by exactly that many payload bytes.
//
// Frame layout (8-byte header, integers big-endian = "network byte order"):
//
//   byte:   0     1     2      3       4..7          8 ...
//         +-----+-----+------+--------+-------------+------------------+
//         | 'P' | '2' | type | status | payload len | payload (len B)  |
//         +-----+-----+------+--------+-------------+------------------+
//
//   magic  : 'P','2' - detects a desynchronised or foreign stream early.
//   type   : what the message is (MsgType). A reply uses the request's type
//            with the top bit set (see reply_type()).
//   status : OK in requests; the result code (Status) in replies.
//   length : payload size, capped at kMaxPayload so a corrupt or hostile
//            header can never make us allocate gigabytes.
//
// Payload: for text messages, fields joined by '\n' (pack_fields/split_fields).
// For piece data, the raw bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

constexpr char     kMagic0      = 'P';
constexpr char     kMagic1      = '2';
constexpr size_t   kHeaderSize  = 8;
constexpr uint32_t kMaxPayload  = 4u * 1024 * 1024;  // 4 MiB: > one 512 KiB piece and a 1 GB file's hash list
constexpr uint8_t  kReplyFlag   = 0x80;              // top bit of type marks a reply

// Message types. Grouped by who talks to whom; numbers never get reused.
enum class MsgType : uint8_t {
    // client -> tracker
    PING           = 0x01,
    CREATE_USER    = 0x02,
    LOGIN          = 0x03,
    LOGOUT         = 0x04,
    CREATE_GROUP   = 0x05,
    JOIN_GROUP     = 0x06,
    LEAVE_GROUP    = 0x07,
    LIST_GROUPS    = 0x08,
    LIST_REQUESTS  = 0x09,
    ACCEPT_REQUEST = 0x0A,
    UPLOAD_FILE    = 0x0B,
    LIST_FILES     = 0x0C,
    FILE_INFO      = 0x0D,  // hashes + seeders, used to start a download
    ADD_SEEDER     = 0x0E,  // "I now have this file too"
    STOP_SHARE     = 0x0F,
    // tracker -> tracker
    SYNC_HELLO     = 0x20,
    SYNC_OP        = 0x21,
    SYNC_SNAPSHOT  = 0x22,
    // client -> client
    GET_PIECE      = 0x40,
    GET_PIECE_MAP  = 0x41,  // which pieces does this peer have?
};

// Result codes carried in the status byte of replies.
enum class Status : uint8_t {
    OK               = 0,
    BAD_REQUEST      = 1,   // malformed payload / wrong field count
    UNKNOWN_TYPE     = 2,
    NOT_LOGGED_IN    = 3,
    ALREADY_LOGGED_IN= 4,
    AUTH_FAILED      = 5,   // same code for "no such user" and "wrong password"
    USER_EXISTS      = 6,
    NO_SUCH_USER     = 7,
    GROUP_EXISTS     = 8,
    NO_SUCH_GROUP    = 9,
    NOT_MEMBER       = 10,
    ALREADY_MEMBER   = 11,
    NOT_OWNER        = 12,
    REQUEST_PENDING  = 13,
    NO_SUCH_REQUEST  = 14,
    FILE_CONFLICT    = 15,  // same name, different content
    NO_SUCH_FILE     = 16,
    NO_SUCH_PIECE    = 17,
    INTERNAL_ERROR   = 18,
    NOT_SHARING      = 19,  // stop_share on a file you are not sharing
};

struct Message {
    MsgType     type    = MsgType::PING;
    Status      status  = Status::OK;
    std::string payload;  // std::string holds arbitrary bytes, including '\0'
};

// Outcome of a socket read/write. Anything other than OK means the
// connection should be treated as unusable (except TIMEOUT on a fresh
// recv_msg, which the caller may choose to retry).
enum class IoResult {
    OK,
    CLOSED,      // peer closed cleanly between messages
    TRUNCATED,   // peer closed in the middle of a message
    TIMEOUT,     // SO_RCVTIMEO / SO_SNDTIMEO expired
    BAD_HEADER,  // magic mismatch: not our protocol or stream out of sync
    TOO_LARGE,   // length field above kMaxPayload
    ERROR,       // any other socket error (errno is kept)
};

// Loop until all len bytes are written / read. These are what make the
// protocol robust to partial sends and receives.
IoResult send_all(int fd, const char* buf, size_t len);
IoResult recv_all(int fd, char* buf, size_t len);

// Header + payload as one contiguous buffer.
std::string encode_message(const Message& msg);

// Send / receive exactly one framed message.
IoResult send_msg(int fd, const Message& msg);
IoResult recv_msg(int fd, Message& out);

// Convenience: build and send a reply to a request type.
inline MsgType reply_type(MsgType request) {
    return static_cast<MsgType>(static_cast<uint8_t>(request) | kReplyFlag);
}
inline bool is_reply(MsgType t) { return (static_cast<uint8_t>(t) & kReplyFlag) != 0; }

// Text payloads: fields joined with '\n'. Fields must not contain '\n'
// (user input is whitespace-separated tokens, so it never can).
// split_fields("") returns no fields.
std::string              pack_fields(const std::vector<std::string>& fields);
std::vector<std::string> split_fields(const std::string& payload);

// Human-readable names for logs and user-facing error messages.
const char* status_text(Status s);
const char* io_result_name(IoResult r);

}  // namespace p2p
