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
send c1 "upload_file g1 /etc/hostname"; expect c1 "^error: 'upload_file' is not implemented yet"

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
