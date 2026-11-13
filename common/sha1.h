// sha1.h - SHA-1 (FIPS 180-4), written from the specification.
//
// Streaming interface: call update() any number of times with any chunk
// sizes, then digest() once. This is what lets us hash a 1 GB file while
// holding only one 512 KiB buffer in memory.
//
// Note: SHA-1 is broken for security (collisions can be manufactured), but
// that is fine here: we use it to detect ACCIDENTAL corruption, which it
// does perfectly well. A system defending against malicious peers would use
// SHA-256.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace p2p {

class Sha1 {
public:
    using Digest = std::array<uint8_t, 20>;

    Sha1() { reset(); }
    void   reset();
    void   update(const void* data, size_t len);
    Digest digest();  // finishes the hash; call reset() before reusing

    static std::string to_hex(const Digest& d);  // 40 lowercase hex chars

private:
    void process_block(const uint8_t* block);  // one 64-byte block

    uint32_t h_[5];          // the running state (5 x 32-bit words)
    uint8_t  buf_[64];       // bytes waiting to fill a whole block
    size_t   buf_len_;
    uint64_t total_len_;     // total bytes hashed, for the final length field
};

// One-shot helper.
std::string sha1_hex(const std::string& data);

}  // namespace p2p
