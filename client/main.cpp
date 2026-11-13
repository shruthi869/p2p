// client/main.cpp - entry point: ./client <IP>:<PORT> tracker_info.txt
// <IP>:<PORT> is where THIS client listens for other peers asking for pieces.
// Part 1: argument and config validation only.
#include <cstdio>
#include <string>
#include <vector>

#include "net.h"

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
    std::printf("client: will serve peers on %s, trackers %s and %s (not implemented yet)\n",
                self.str().c_str(), trackers[0].str().c_str(), trackers[1].str().c_str());
    return 0;
}
