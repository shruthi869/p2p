// file_meta.cpp - piece math, streaming file hashing, file I/O loops.
#include "file_meta.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha1.h"

namespace p2p {

uint64_t piece_count(uint64_t file_size) { return (file_size + kPieceSize - 1) / kPieceSize; }

uint64_t piece_length(uint64_t file_size, uint64_t index) {
    const uint64_t start = index * kPieceSize;
    if (start >= file_size) return 0;
    const uint64_t left = file_size - start;
    return left < kPieceSize ? left : kPieceSize;
}

bool read_full(int fd, char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::read(fd, buf + got, len - got);
        if (n > 0) {
            got += static_cast<size_t>(n);
        } else if (n == 0) {
            errno = EIO;  // file shorter than expected (changed while reading?)
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool pread_full(int fd, char* buf, size_t len, off_t offset) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::pread(fd, buf + got, len - got, offset + static_cast<off_t>(got));
        if (n > 0) {
            got += static_cast<size_t>(n);
        } else if (n == 0) {
            errno = EIO;
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool pwrite_full(int fd, const char* buf, size_t len, off_t offset) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pwrite(fd, buf + done, len - done, offset + static_cast<off_t>(done));
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n == 0) {
            errno = EIO;  // never expected for a regular file; avoid looping forever
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool hash_file(const std::string& path, FileMeta& out, std::string& err) {
    const std::string name = base_name(path);
    if (!valid_file_name(name)) {
        err = "invalid file name '" + name + "'";
        return false;
    }
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        err = path + " is not a regular file";
        ::close(fd);
        return false;
    }
    if (st.st_size == 0) {
        err = path + " is empty (nothing to share)";
        ::close(fd);
        return false;
    }

    FileMeta meta;
    meta.name = name;
    meta.size = static_cast<uint64_t>(st.st_size);
    const uint64_t n = piece_count(meta.size);
    meta.piece_hashes.reserve(n);

    std::string buf(kPieceSize, '\0');  // the ONLY data buffer, reused for every piece
    Sha1 whole, piece;
    for (uint64_t i = 0; i < n; ++i) {
        const size_t len = static_cast<size_t>(piece_length(meta.size, i));
        if (!read_full(fd, &buf[0], len)) {
            err = "error reading " + path + ": " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        piece.reset();
        piece.update(buf.data(), len);
        meta.piece_hashes.push_back(Sha1::to_hex(piece.digest()));
        whole.update(buf.data(), len);
    }
    ::close(fd);
    meta.file_hash = Sha1::to_hex(whole.digest());
    out = std::move(meta);
    return true;
}

std::string join_hashes(const std::vector<std::string>& hashes) {
    std::string s;
    s.reserve(hashes.size() * kHashHexLen);
    for (const auto& h : hashes) s += h;
    return s;
}

bool is_hex_hash(const std::string& s) {
    if (s.size() != kHashHexLen) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool split_hashes(const std::string& joined, std::vector<std::string>& out) {
    if (joined.size() % kHashHexLen != 0) return false;
    std::vector<std::string> v;
    for (size_t i = 0; i < joined.size(); i += kHashHexLen) {
        v.push_back(joined.substr(i, kHashHexLen));
        if (!is_hex_hash(v.back())) return false;
    }
    out = std::move(v);
    return true;
}

std::string base_name(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool valid_file_name(const std::string& name) {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    for (char c : name)
        if (c == '/' || static_cast<unsigned char>(c) <= ' ' || c == 0x7F) return false;
    return true;
}

bool parse_u64(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 19) return false;  // 19 digits always fit in 64 bits
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace p2p
