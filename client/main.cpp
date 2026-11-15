// client/main.cpp - entry point: ./client <IP>:<PORT> tracker_info.txt
// <IP>:<PORT> is where THIS client listens for other peers asking for
// pieces (its peer server). Commands are read from stdin.
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "commands.h"
#include "downloads.h"
#include "net.h"
#include "peer_server.h"
#include "shared_files.h"
#include "tracker_link.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <IP>:<PORT> tracker_info.txt\n", argv[0]);
        return 1;
    }
    p2p::Endpoint self;
    if (!p2p::parse_endpoint(argv[1], self)) {
        std::fprintf(stderr, "client: bad address '%s' (expected IPv4:PORT)\n", argv[1]);
        return 1;
    }
    std::vector<p2p::Endpoint> trackers;
    std::string err;
    if (!p2p::read_tracker_info(argv[2], trackers, err)) {
        std::fprintf(stderr, "client: %s\n", err.c_str());
        return 1;
    }

    // Line-buffered stdout: output shows up immediately even when redirected
    // to a file or pipe (the integration tests read it while we run).
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    p2p::ignore_sigpipe();

    // Order matters: objects are destroyed in reverse, so downloads stop
    // (threads joined) before the peer server, which stops before the
    // shared-file list and the tracker link they both use.
    p2p::SharedFiles shared;
    p2p::PeerServer peer_server(shared);
    if (!peer_server.start(self, err)) {  // e.g. port already in use
        std::fprintf(stderr, "client: %s\n", err.c_str());
        return 1;
    }
    p2p::TrackerLink link(trackers);
    link.set_reannounce([&shared] {
        std::vector<std::vector<std::string>> out;
        for (const auto& f : shared.all())
            out.push_back({f.group, f.meta.name, std::to_string(f.meta.size), f.meta.file_hash,
                           p2p::join_hashes(f.meta.piece_hashes)});
        return out;
    });
    if (!link.ensure_connected(err)) std::printf("warning: %s (will retry on each command)\n", err.c_str());

    p2p::DownloadManager downloads(link, shared, self);
    p2p::ClientApp app(link, self, shared, downloads);
    const bool interactive = ::isatty(STDIN_FILENO);
    std::string line;
    while (true) {
        if (interactive) {
            std::printf("> ");
            std::fflush(stdout);
        }
        if (!std::getline(std::cin, line)) {  // EOF (Ctrl-D or input pipe closed)
            app.shutdown();
            break;
        }
        if (!app.execute(line)) break;  // "quit"
    }
    return 0;
}
