// file_meta.h - pieces, file metadata, and low-level file I/O helpers.
//
// A file is cut into pieces of exactly 512 KiB; the last piece may be
// shorter. Each piece has its own SHA-1 and the whole file has one too.
// That metadata is what the tracker stores and hands to downloaders.
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace p2p {

constexpr uint64_t kPieceSize = 512 * 1024;  // 524288 bytes
constexpr size_t   kHashHexLen = 40;         // one SHA-1 as hex

struct FileMeta {
    std::string              name;          // base name, e.g. "movie.mp4"
    uint64_t                 size = 0;      // bytes
    std::string              file_hash;     // SHA-1 of the whole file (hex)
    std::vector<std::string> piece_hashes;  // SHA-1 of each piece (hex)
};

uint64_t piece_count(uint64_t file_size);                    // ceil(size / 512 KiB)
uint64_t piece_length(uint64_t file_size, uint64_t index);   // 512 KiB, or less for the last

// Read the file ONE piece at a time (one reusable 512 KiB buffer) and
// compute every piece hash and the whole-file hash in a single pass.
// Memory use is constant whatever the file size.
bool hash_file(const std::string& path, FileMeta& out, std::string& err);

// SHA-1 of the first `size` bytes of an open file, read in 512 KiB chunks
// with one buffer (constant memory). Used to verify a finished download.
bool hash_fd(int fd, uint64_t size, std::string& hex_out);

// Piece hashes travel as one string: 40 hex chars per piece, no separator
// (fixed width, so no escaping is ever needed).
std::string              join_hashes(const std::vector<std::string>& hashes);
bool                     split_hashes(const std::string& joined, std::vector<std::string>& out);
bool                     is_hex_hash(const std::string& s);  // exactly 40 lowercase hex

// "dir/sub/name.txt" -> "name.txt"; "" if the path ends in '/'.
std::string base_name(const std::string& path);
// A name we accept for a shared file: 1-255 chars, no '/', no whitespace or
// control characters, not "." or "..".
bool        valid_file_name(const std::string& name);

bool parse_u64(const std::string& s, uint64_t& out);  // strict decimal

// read/write loops (EINTR-safe, handle short reads/writes).
// read_full: false on error or premature EOF.
bool read_full(int fd, char* buf, size_t len);
bool pread_full(int fd, char* buf, size_t len, off_t offset);
bool pwrite_full(int fd, const char* buf, size_t len, off_t offset);

}  // namespace p2p
