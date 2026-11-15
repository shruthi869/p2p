#!/bin/bash
# large.sh - 1 GiB end-to-end checks with the REAL binaries (slow; `make test-large`).
#   - upload_file of a 1 GiB file: printed SHA-1 must equal sha1sum, 2048 pieces
#   - the client's peak memory (VmHWM) must stay small: the file is streamed,
#     never loaded whole
source "$(dirname "$0")/lib.sh"
setup_run large

BIG="$RUN/files/big1g.bin"
mkdir -p "$RUN/files"
echo "  creating 1 GiB test file..."
head -c 1073741824 /dev/urandom >"$BIG"
big_sha=$(sha1sum "$BIG" | cut -d' ' -f1)

section "upload a 1 GiB file"
start_tracker 1
start_tracker 2
start_client c1 7101
expect c1 "\[connected to tracker 1"
send c1 "create_user big pw";   expect c1 "^user big created"
send c1 "login big pw";         expect c1 "^logged in as big"
send c1 "create_group gl";      expect c1 "^group gl created"
send c1 "upload_file gl $BIG";  expect c1 "^uploaded big1g.bin to group gl: 1073741824 bytes, 2048 pieces, sha1 $big_sha$" 120
send c1 "list_files gl";        expect c1 "^  big1g.bin  \(1073741824 bytes, 2048 pieces, 1 seeder\(s\)\)"

# Peak resident memory of the client process so far (it just hashed 1 GiB).
peak_kb=$(awk '/VmHWM/ {print $2}' "/proc/${PID[c1]}/status")
if [ -n "$peak_kb" ] && [ "$peak_kb" -lt 50000 ]; then
    pass "client peak memory ${peak_kb} KB while hashing 1 GiB (streamed)"
else
    fail "client peak memory ${peak_kb:-?} KB (expected < 50 MB)"
fi

section "download the 1 GiB file from one peer"
start_client c2 7102
expect c2 "\[connected to tracker 1"
send c2 "create_user dl pw";    expect c2 "^user dl created"
send c2 "login dl pw";          expect c2 "^logged in as dl"
send c2 "join_group gl";        expect c2 "^join request sent"
send c1 "accept_request gl dl"; expect c1 "^dl added to group gl"
DL="$RUN/dl"; mkdir -p "$DL"
t0=$(date +%s.%N)
send c2 "download_file gl big1g.bin $DL"
expect c2 "^download finished: \[C\] \[gl\] big1g.bin$" 300
t1=$(date +%s.%N)
secs=$(echo "$t1 - $t0" | bc)
pass "1 GiB downloaded in ${secs}s ($(echo "1024 / $secs" | bc) MiB/s, includes final whole-file SHA-1 check)"
same_file "$BIG" "$DL/big1g.bin"
peak_kb=$(awk '/VmHWM/ {print $2}' "/proc/${PID[c2]}/status")
if [ -n "$peak_kb" ] && [ "$peak_kb" -lt 50000 ]; then
    pass "downloader peak memory ${peak_kb} KB for a 1 GiB download"
else
    fail "downloader peak memory ${peak_kb:-?} KB (expected < 50 MB)"
fi
peak_kb=$(awk '/VmHWM/ {print $2}' "/proc/${PID[c1]}/status")
pass "seeder peak memory ${peak_kb} KB after serving 1 GiB"

stop c2
stop c1
stop t1
stop t2
rm -f "$BIG" "$DL/big1g.bin"
finish
