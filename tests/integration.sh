#!/bin/bash
# integration.sh - end-to-end tests with 2 real trackers and 3+ real clients.
# Usage: bash tests/integration.sh            (MODE=asan|tsan|valgrind optional)
source "$(dirname "$0")/lib.sh"
setup_run integration

section "startup: 2 trackers, 3 clients"
start_tracker 1
start_tracker 2
start_client c1 7101
start_client c2 7102
start_client c3 7103
for c in c1 c2 c3; do expect $c "\[connected to tracker 1 at 127.0.0.1:$TRACKER_PORT1\]"; done

section "users and login"
send c1 "list_groups";               expect c1 "^error: please log in first"
send c1 "create_user alice pa55";    expect c1 "^user alice created"
send c2 "create_user bob pw_b";      expect c2 "^user bob created"
send c3 "create_user carol pw_c";    expect c3 "^user carol created"
send c3 "create_user carol other";   expect c3 "^error: user already exists"
send c1 "login alice wrong";         expect c1 "^error: invalid user id or password"
send c1 "login nobody x";            expect c1 "^error: invalid user id or password"
send c1 "login alice pa55";          expect c1 "^logged in as alice"
send c1 "login alice pa55";          expect c1 "^error: already logged in as alice"
send c2 "login alice pa55";          expect c2 "^error: already logged in$"   # alice is busy on c1
send c2 "login bob pw_b";            expect c2 "^logged in as bob"
send c3 "login carol pw_c";          expect c3 "^logged in as carol"

section "groups, requests, ownership"
send c1 "create_group g1";           expect c1 "^group g1 created \(you are the owner\)"
send c2 "create_group g1";           expect c2 "^error: group already exists"
send c2 "join_group g1";             expect c2 "^join request sent for group g1"
send c2 "join_group g1";             expect c2 "^error: join request already pending"
send c3 "join_group g1";             expect c3 "^join request sent for group g1"
send c2 "list_requests g1";          expect c2 "^error: only the group owner can do this"
send c1 "list_requests g1";          expect c1 "^pending requests for g1 \(2\):"
                                     expect c1 "^  bob"
                                     expect c1 "^  carol"
send c1 "accept_request g1 bob";     expect c1 "^bob added to group g1"
send c1 "accept_request g1 dave";    expect c1 "^error: no such join request"
send c3 "leave_group g1";            expect c3 "^error: you are not a member of this group"
send c2 "create_group g2";           expect c2 "^group g2 created"
send c3 "list_groups";               expect c3 "^groups \(2\):"
                                     expect c3 "^  g1"
                                     expect c3 "^  g2"
send c1 "leave_group g1";            expect c1 "^left group g1"
send c2 "list_requests g1";          expect c2 "^pending requests for g1 \(1\):"   # bob inherited ownership
                                     expect c2 "^  carol"
send c2 "accept_request g1 carol";   expect c2 "^carol added to group g1"

section "input handling"
send c1 "create_group";              expect c1 "^usage: create_group <group_id>"
send c1 "fly_to_moon";               expect c1 "^error: unknown command 'fly_to_moon'"
send c1 "join_group no_such_group";  expect c1 "^error: no such group"
send c1 "show_downloads";            expect c1 "^no downloads"

section "upload_file / list_files / stop_share (group g2, owner bob on c2)"
F="$RUN/files"; mkdir -p "$F/other" "$F/copy"
printf 'hello p2p\n' >"$F/small.txt"
head -c 524288 /dev/urandom >"$F/exact512k.bin"   # exactly one piece
head -c 1572987 /dev/urandom >"$F/big.bin"         # 4 pieces, last one partial
printf 'different!\n' >"$F/other/small.txt"        # same name, different content
cp "$F/small.txt" "$F/copy/small.txt"              # same name, same content
: >"$F/empty.txt"
small_sha=$(sha1sum "$F/small.txt" | cut -d' ' -f1)
big_sha=$(sha1sum "$F/big.bin" | cut -d' ' -f1)

send c2 "upload_file g2 $F/small.txt";     expect c2 "^uploaded small.txt to group g2: 10 bytes, 1 pieces, sha1 $small_sha$"
send c2 "upload_file g2 $F/exact512k.bin"; expect c2 "^uploaded exact512k.bin to group g2: 524288 bytes, 1 pieces"
send c2 "upload_file g2 $F/big.bin";       expect c2 "^uploaded big.bin to group g2: 1572987 bytes, 4 pieces, sha1 $big_sha$"
send c2 "upload_file g2 $F/nope.bin";      expect c2 "^error: cannot open .*nope.bin"
send c2 "upload_file g2 $F";               expect c2 "^error: .* is not a regular file"
send c2 "upload_file g2 $F/empty.txt";     expect c2 "^error: .* is empty"
send c2 "upload_file nogroup $F/small.txt"; expect c2 "^error: no such group"
send c1 "list_files g2";                   expect c1 "^error: you are not a member of this group"
send c1 "upload_file g2 $F/small.txt";     expect c1 "^error: you are not a member of this group"
send c1 "join_group g2";                   expect c1 "^join request sent"
send c2 "accept_request g2 alice";         expect c2 "^alice added to group g2"
send c1 "list_files g2";                   expect c1 "^files in g2 \(3\):"
                                           expect c1 "^  big.bin  \(1572987 bytes, 4 pieces, 1 seeder\(s\)\)"
                                           expect c1 "^  exact512k.bin  \(524288 bytes, 1 pieces, 1 seeder\(s\)\)"
                                           expect c1 "^  small.txt  \(10 bytes, 1 pieces, 1 seeder\(s\)\)"
send c1 "upload_file g2 $F/other/small.txt"; expect c1 "^error: a different file with this name is already shared"
send c1 "upload_file g2 $F/copy/small.txt";  expect c1 "^uploaded small.txt to group g2"
send c1 "list_files g2";                   expect c1 "^  small.txt  \(10 bytes, 1 pieces, 2 seeder\(s\)\)"
send c1 "stop_share g2 big.bin";           expect c1 "^error: you are not sharing this file"
send c2 "stop_share g2 big.bin";           expect c2 "^stopped sharing big.bin in group g2"
send c2 "stop_share g2 big.bin";           expect c2 "^error: no such file in this group"   # last seeder left -> gone
send c1 "list_files g2";                   expect c1 "^files in g2 \(2\):"

section "logout stops sharing; login shares again"
send c2 "logout";                          expect c2 "^logged out"
send c1 "list_files g2";                   expect c1 "^files in g2 \(1\):"                   # only alice's small.txt
                                           expect c1 "^  small.txt  \(10 bytes, 1 pieces, 1 seeder\(s\)\)"
send c2 "login bob pw_b";                  expect c2 "^logged in as bob"
                                           expect c2 "^re-shared 2 of 2 file\(s\)"
send c1 "list_files g2";                   expect c1 "^files in g2 \(2\):"
                                           expect c1 "^  exact512k.bin"
                                           expect c1 "^  small.txt  \(10 bytes, 1 pieces, 2 seeder\(s\)\)"

section "download_file from one peer"
DL1="$RUN/dl_c1"; mkdir -p "$DL1"
send c1 "download_file g2 exact512k.bin $DL1";   expect c1 "^download started: \[g2\] exact512k.bin"
                                                 expect c1 "^download finished: \[C\] \[g2\] exact512k.bin$" 20
same_file "$F/exact512k.bin" "$DL1/exact512k.bin"
send c2 "upload_file g2 $F/big.bin";             expect c2 "^uploaded big.bin"
send c1 "download_file g2 big.bin $DL1";         expect c1 "^download finished: \[C\] \[g2\] big.bin$" 20
same_file "$F/big.bin" "$DL1/big.bin"
send c1 "download_file g2 small.txt $DL1";       expect c1 "^download finished: \[C\] \[g2\] small.txt$" 20
same_file "$F/small.txt" "$DL1/small.txt"
send c1 "show_downloads";                        expect c1 "^\[C\] \[g2\] exact512k.bin$"
                                                 expect c1 "^\[C\] \[g2\] big.bin$"
                                                 expect c1 "^\[C\] \[g2\] small.txt$"
no_part_files "$DL1"
send c1 "download_file g2 big.bin $DL1";         expect c1 "^error: .*big.bin already exists"
send c1 "download_file g2 nope.bin $DL1";        expect c1 "^error: no such file in this group"
send c1 "download_file g2 big.bin $F/small.txt"; expect c1 "^error: destination .* is not a directory"
send c1 "download_file g9 big.bin $DL1";         expect c1 "^error: no such group"
send c1 "list_files g2";                         expect c1 "^  big.bin  \(1572987 bytes, 4 pieces, 2 seeder\(s\)\)"  # alice now seeds it

section "logout"
send c2 "logout";                    expect c2 "^logged out"
send c2 "list_groups";               expect c2 "^error: please log in first"
send c2 "logout";                    expect c2 "^error: please log in first"
send c2 "login bob pw_b";            expect c2 "^logged in as bob"

section "crashed client is logged out by the tracker"
kill9 c3
expect t1 "carol disconnected \(auto logout\)"
start_client c4 7104
expect c4 "\[connected to tracker 1"
send c4 "login carol pw_c";          expect c4 "^logged in as carol"   # different address: only works if the old session ended

section "corrupted piece is detected and re-fetched from another peer"
# big.bin seeders, in tracker order: alice (c1, who downloaded it) first, then bob
# (bob logged out + in again, which re-added him at the end; see tracker log).
# Corrupt piece 1 of alice's copy on disk; carol must reject it by SHA-1 and get it from bob.
cp "$F/big.bin" "$F/big.ref"
printf 'CORRUPTED' | dd of="$DL1/big.bin" bs=1 seek=$((524288 + 1000)) conv=notrunc 2>/dev/null
send c4 "join_group g2";             expect c4 "^join request sent"
send c2 "accept_request g2 carol";   expect c2 "^carol added to group g2"
DL4="$RUN/dl_c4"; mkdir -p "$DL4"
send c4 "download_file g2 big.bin $DL4"
expect c4 "^download finished: \[C\] \[g2\] big.bin \(1 bad piece\(s\) re-fetched\)$" 20
same_file "$F/big.ref" "$DL4/big.bin"

section "client skips a tracker that is down"
printf '127.0.0.1 6199\n127.0.0.1 %s\n' "$TRACKER_PORT2" >"$RUN/info_dead_first.txt"
start_client c5 7105 "$RUN/info_dead_first.txt"
expect c5 "\[connected to tracker 2 at 127.0.0.1:$TRACKER_PORT2\]"
send c5 "create_user erin pw";       expect c5 "^user erin created"
stop c5

section "clean shutdown"
for c in c1 c2 c4; do stop $c; done
expect t1 "logout alice"   # "quit" logs out politely before exiting
stop t1
stop t2

finish
