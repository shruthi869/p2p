// tracker/main.cpp - entry point: ./tracker tracker_info.txt tracker_no
// Part 1: argument and config validation only. The server arrives in Part 2.
#include <cstdio>
#include <string>
#include <vector>

#include "net.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s tracker_info.txt tracker_no\n", argv[0]);
        return 1;
    }
    std::vector<p2p::Endpoint> trackers;
    std::string err;
    if (!p2p::read_tracker_info(argv[1], trackers, err)) {
        std::fprintf(stderr, "tracker: %s\n", err.c_str());
        return 1;
    }
    const std::string no = argv[2];
    if (no != "1" && no != "2") {
        std::fprintf(stderr, "tracker: tracker_no must be 1 or 2\n");
        return 1;
    }
    const p2p::Endpoint& me = trackers[no == "1" ? 0 : 1];
    std::printf("tracker %s: configured for %s (server not implemented yet)\n", no.c_str(),
                me.str().c_str());
    return 0;
}
