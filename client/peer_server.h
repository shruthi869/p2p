// peer_server.h - the "seeder" half of every client: listens on the
// client's <IP>:<PORT> and answers other peers' piece requests.
//
//   GET_PIECE  fields: group, file_name, piece_index  -> reply payload = raw piece bytes
//
// Only files registered in SharedFiles can be served, looked up by
// (group, name). A request can never name a path on disk, so "../../etc/passwd"
// style requests simply get NO_SUCH_FILE.
#pragma once

#include <string>

#include "conn_server.h"
#include "net.h"
#include "protocol.h"
#include "shared_files.h"

namespace p2p {

class PeerServer {
public:
    explicit PeerServer(SharedFiles& shared);
    ~PeerServer();

    bool     start(const Endpoint& ep, std::string& err) { return server_.start(ep, err); }
    void     stop() { server_.stop(); }
    uint16_t port() const { return server_.port(); }

private:
    void    serve(int fd);
    Message handle(const Message& req);

    SharedFiles& shared_;
    ConnServer   server_;  // last: destroyed (= stopped) first
};

}  // namespace p2p
