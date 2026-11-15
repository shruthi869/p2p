// peer_server.cpp - serve pieces of shared files to other peers.
#include "peer_server.h"

#include <fcntl.h>
#include <unistd.h>

#include "file_meta.h"

namespace p2p {

namespace {
// A downloader that goes silent (crashed machine, frozen process) must not
// hold one of our threads forever.
constexpr int kPeerIdleTimeoutMs = 30000;
}  // namespace

PeerServer::PeerServer(SharedFiles& shared) : shared_(shared), server_([this](int fd) { serve(fd); }) {}

PeerServer::~PeerServer() { server_.stop(); }

void PeerServer::serve(int fd) {
    set_io_timeout(fd, kPeerIdleTimeoutMs);
    while (true) {
        Message req;
        if (recv_msg(fd, req) != IoResult::OK) break;
        if (send_msg(fd, handle(req)) != IoResult::OK) break;
    }
}

Message PeerServer::handle(const Message& req) {
    Message rep;
    rep.type = reply_type(req.type);
    if (req.type != MsgType::GET_PIECE) {
        rep.status = Status::UNKNOWN_TYPE;
        return rep;
    }
    const std::vector<std::string> f = split_fields(req.payload);
    uint64_t index = 0;
    if (f.size() != 3 || !parse_u64(f[2], index)) {
        rep.status = Status::BAD_REQUEST;
        return rep;
    }
    SharedFile sf;
    if (!shared_.find(f[0], f[1], sf)) {
        rep.status = Status::NO_SUCH_FILE;
        return rep;
    }
    if (index >= piece_count(sf.meta.size)) {
        rep.status = Status::NO_SUCH_PIECE;
        return rep;
    }
    // Read just this piece from disk (pread at its offset). Opening the file
    // per request keeps the code simple; an fd cache would save a syscall.
    const size_t len = static_cast<size_t>(piece_length(sf.meta.size, index));
    int file = ::open(sf.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        rep.status = Status::INTERNAL_ERROR;  // file moved/deleted since it was shared
        return rep;
    }
    rep.payload.resize(len);
    if (!pread_full(file, &rep.payload[0], len, static_cast<off_t>(index * kPieceSize))) {
        rep.payload.clear();
        rep.status = Status::INTERNAL_ERROR;  // e.g. file truncated since it was shared
    }
    ::close(file);
    return rep;
}

}  // namespace p2p
