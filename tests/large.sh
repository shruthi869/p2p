#!/bin/bash
# large.sh - 1 GiB end-to-end checks with the REAL binaries (slow; `make test-large`).
#   - upload_file of a 1 GiB file: printed SHA-1 must equal sha1sum, 2048 pieces
#   - download it from 1, then 2, then 3 seeders: identical (cmp), speed of each
#   - peak memory (VmHWM) of uploader and downloaders stays small: files are
#     streamed piece by piece, never loaded whole
source "$(dirname "$0")/lib.sh"
setup_run large

BIG="$RUN/files/big1g.bin"
mkdir -p "$RUN/files"
echo "  creating 1 GiB test file..."
head -c 1073741824 /dev/urandom >"$BIG"
big_sha=$(sha1sum "$BIG" | cut -d' ' -f1)

check_peak() {  # check_peak CLIENT WHAT
    local kb
    kb=$(awk '/VmHWM/ {print $2}' "/proc/${PID[$1]}/status")
    if [ -n "$kb" ] && [ "$kb" -lt 50000 ]; then pass "$2: peak memory ${kb} KB"; else fail "$2: peak memory ${kb:-?} KB (expected < 50 MB)"; fi
}

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
check_peak c1 "uploader hashing 1 GiB"

# download_from CLIENT PORT USER EXPECTED_PEERS : new client joins gl and downloads the file
download_from() {
    local c=$1 port=$2 user=$3 npeers=$4
    start_client "$c" "$port"
    expect "$c" "\[connected to tracker 1"
    send "$c" "create_user $user pw";    expect "$c" "^user $user created"
    send "$c" "login $user pw";          expect "$c" "^logged in as $user"
    send "$c" "join_group gl";           expect "$c" "^join request sent"
    send c1 "accept_request gl $user";   expect c1 "^$user added to group gl"
    local dl="$RUN/dl_$c"; mkdir -p "$dl"
    local t0 t1 secs
    t0=$(date +%s.%N)
    send "$c" "download_file gl big1g.bin $dl"
    expect "$c" "^download finished: \[C\] \[gl\] big1g.bin$" 300
    t1=$(date +%s.%N)
    secs=$(echo "$t1 - $t0" | bc)
    expect "$c" "^  pieces from:( 127.0.0.1:[0-9]+=[1-9][0-9]*){$npeers}$"
    pass "SPEED with $npeers seeder(s): 1 GiB in $(printf '%.1f' "$secs")s = $(echo "1024 / $secs" | bc) MiB/s  [$(grep '^  pieces from:' "$RUN/$c.out" | tail -1 | sed 's/^  //')]"
    same_file "$BIG" "$dl/big1g.bin"
    check_peak "$c" "downloader ($npeers seeder(s))"
}

section "download the 1 GiB file from 1, 2 and 3 seeders"
download_from c2 7102 dl1 1   # seeders: c1
download_from c3 7103 dl2 2   # seeders: c1, c2
download_from c4 7104 dl3 3   # seeders: c1, c2, c3
check_peak c1 "original seeder after serving three downloads"

for c in c4 c3 c2 c1; do stop $c; done
stop t1
stop t2
rm -f "$BIG" "$RUN"/dl_*/big1g.bin
finish
