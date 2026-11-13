// tests/test_files.cpp - SHA-1 against the official test vectors, piece
// arithmetic, and streaming file hashing.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "file_meta.h"
#include "sha1.h"
#include "test_util.h"

using namespace p2p;

// Known answers from FIPS 180 / RFC 3174.
static void test_sha1_vectors() {
    CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(sha1_hex("The quick brown fox jumps over the lazy dog") ==
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    CHECK(sha1_hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// Feeding the data in odd-sized chunks must give the same digest as one
// call, in particular around the padding boundaries (55/56/63/64/65 bytes).
static void test_sha1_streaming() {
    std::string data;
    for (int i = 0; i < 300; ++i) data += static_cast<char>((i * 7 + 3) & 0xFF);
    for (size_t len : {0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 128, 300}) {
        const std::string part = data.substr(0, len);
        Sha1 h;
        for (size_t i = 0; i < part.size(); i += 7) h.update(part.data() + i, std::min<size_t>(7, part.size() - i));
        CHECK(Sha1::to_hex(h.digest()) == sha1_hex(part));
    }
    Sha1 h;
    h.update("ab", 2);
    h.digest();
    h.reset();  // reuse after reset
    h.update("abc", 3);
    CHECK(Sha1::to_hex(h.digest()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

static void test_piece_math() {
    const uint64_t P = kPieceSize;
    CHECK(P == 524288);
    CHECK(piece_count(0) == 0 && piece_count(1) == 1 && piece_count(P) == 1 && piece_count(P + 1) == 2);
    CHECK(piece_count(1024ull * 1024 * 1024) == 2048);  // 1 GiB
    CHECK(piece_length(P + 10, 0) == P && piece_length(P + 10, 1) == 10 && piece_length(P + 10, 2) == 0);
    CHECK(piece_length(P, 0) == P);
}

static void test_hash_helpers() {
    std::vector<std::string> v = {std::string(40, 'a'), std::string(40, '0')};
    std::vector<std::string> back;
    CHECK(split_hashes(join_hashes(v), back) && back == v);
    CHECK(!split_hashes(std::string(39, 'a'), back));
    CHECK(!split_hashes(std::string(40, 'g'), back));
    CHECK(!split_hashes(std::string(40, 'A'), back));  // lowercase only
    CHECK(split_hashes("", back) && back.empty());
    CHECK(base_name("a/b/c.txt") == "c.txt" && base_name("c.txt") == "c.txt" && base_name("dir/").empty());
    CHECK(valid_file_name("movie.mp4") && !valid_file_name("") && !valid_file_name("..") &&
          !valid_file_name("a b") && !valid_file_name(std::string(256, 'x')));
    uint64_t x = 0;
    CHECK(parse_u64("18446744073709551", x) && x == 18446744073709551ull);
    CHECK(!parse_u64("", x) && !parse_u64("12a", x) && !parse_u64("-1", x));
}

static std::string write_file(const std::string& content) {
    char path[] = "/tmp/p2p_test_file_XXXXXX";
    int fd = ::mkstemp(path);
    if (fd < 0) std::abort();
    if (!content.empty() && ::write(fd, content.data(), content.size()) != static_cast<ssize_t>(content.size()))
        std::abort();
    ::close(fd);
    return path;
}

static void test_hash_file() {
    // 512 KiB + 10 bytes -> two pieces; check every hash against in-memory SHA-1.
    std::string content(kPieceSize + 10, '\0');
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>((i * 131 + i / 7) & 0xFF);
    std::string path = write_file(content);
    FileMeta m;
    std::string err;
    CHECK(hash_file(path, m, err));
    CHECK(m.size == content.size());
    CHECK(m.piece_hashes.size() == 2);
    CHECK(m.piece_hashes[0] == sha1_hex(content.substr(0, kPieceSize)));
    CHECK(m.piece_hashes[1] == sha1_hex(content.substr(kPieceSize)));
    CHECK(m.file_hash == sha1_hex(content));
    CHECK(m.name == base_name(path));
    ::unlink(path.c_str());

    std::string empty = write_file("");
    CHECK(!hash_file(empty, m, err) && err.find("empty") != std::string::npos);
    ::unlink(empty.c_str());
    CHECK(!hash_file("/tmp", m, err) && err.find("not a regular file") != std::string::npos);
    CHECK(!hash_file("/nonexistent/x", m, err) && err.find("cannot open") != std::string::npos);
}

static void test_pread_pwrite() {
    std::string path = write_file("");
    int fd = ::open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    // Write pieces out of order at their offsets - what a downloader does.
    CHECK(pwrite_full(fd, "WORLD", 5, 5));
    CHECK(pwrite_full(fd, "HELLO", 5, 0));
    char buf[10];
    CHECK(pread_full(fd, buf, 10, 0) && std::string(buf, 10) == "HELLOWORLD");
    CHECK(!pread_full(fd, buf, 10, 5));  // reading past the end fails instead of returning garbage
    ::close(fd);
    ::unlink(path.c_str());
}

int main() {
    static const TestCase tests[] = {
        {"sha1 official vectors", test_sha1_vectors},
        {"sha1 streaming + padding boundaries", test_sha1_streaming},
        {"piece math", test_piece_math},
        {"hash list / names / numbers", test_hash_helpers},
        {"hash_file", test_hash_file},
        {"pread/pwrite at offsets", test_pread_pwrite},
    };
    return run_tests(tests);
}
