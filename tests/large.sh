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

stop c1
stop t1
stop t2
rm -f "$BIG"
finish
