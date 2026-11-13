// tests/sha1tool.cpp - test helper: prints what hash_file() computes, so a
// shell script can compare it with the system's sha1sum.
//   output: line 1 = whole-file SHA-1, then one line per piece.
#include <cstdio>
#include <string>

#include "file_meta.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    p2p::FileMeta m;
    std::string err;
    if (!p2p::hash_file(argv[1], m, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", m.file_hash.c_str());
    for (const auto& h : m.piece_hashes) std::printf("%s\n", h.c_str());
    return 0;
}
