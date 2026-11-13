// sha1.cpp - SHA-1, step by step.
//
// 1. PAD the message: append bit '1' (byte 0x80), then zero bytes until the
//    length is 56 mod 64, then the original length in BITS as a 64-bit
//    big-endian number. Now the message is a whole number of 64-byte blocks.
// 2. Start from five fixed 32-bit words h0..h4.
// 3. For each 64-byte block: expand its 16 words into 80 words (the
//    "message schedule"), run 80 rounds that mix them into a..e, and add
//    a..e back into h0..h4.
// 4. The digest is h0..h4 written big-endian = 20 bytes = 40 hex chars.
#include "sha1.h"

#include <cstring>

namespace p2p {

namespace {

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t load_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

void Sha1::reset() {
    h_[0] = 0x67452301;
    h_[1] = 0xEFCDAB89;
    h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476;
    h_[4] = 0xC3D2E1F0;
    buf_len_ = 0;
    total_len_ = 0;
}

void Sha1::process_block(const uint8_t* block) {
    // Message schedule: 16 words from the block, 64 more derived from them.
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(block + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {        // "choose": if b then c else d
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) { // parity
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) { // majority
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {             // parity
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    total_len_ += len;
    // Top up a partly filled block first.
    if (buf_len_ > 0) {
        const size_t take = len < 64 - buf_len_ ? len : 64 - buf_len_;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ < 64) return;
        process_block(buf_);
        buf_len_ = 0;
    }
    // Whole blocks straight from the input (no copy).
    while (len >= 64) {
        process_block(p);
        p += 64;
        len -= 64;
    }
    // Keep the tail for next time.
    std::memcpy(buf_, p, len);
    buf_len_ = len;
}

Sha1::Digest Sha1::digest() {
    const uint64_t bit_len = total_len_ * 8;
    // Padding: 0x80, zeros up to 56 mod 64, then the 8-byte length.
    uint8_t pad[72] = {0x80};
    const size_t zeros_and_marker = (buf_len_ < 56) ? 56 - buf_len_ : 120 - buf_len_;
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
    const uint64_t saved = total_len_;
    update(pad, zeros_and_marker);
    update(len_be, 8);
    total_len_ = saved;  // (update() counted the padding; not that it matters now)

    Digest out;
    for (int i = 0; i < 5; ++i) {
        out[4 * i]     = static_cast<uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
    }
    return out;
}

std::string Sha1::to_hex(const Digest& d) {
    static const char* kHex = "0123456789abcdef";
    std::string s(40, '0');
    for (size_t i = 0; i < d.size(); ++i) {
        s[2 * i] = kHex[d[i] >> 4];
        s[2 * i + 1] = kHex[d[i] & 0x0F];
    }
    return s;
}

std::string sha1_hex(const std::string& data) {
    Sha1 h;
    h.update(data.data(), data.size());
    return Sha1::to_hex(h.digest());
}

}  // namespace p2p
