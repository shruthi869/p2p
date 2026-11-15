#!/bin/bash
# failures.sh - failure injection against the REAL binaries:
#   seeders killed / frozen in the middle of a download, the only seeder
#   killed, a tracker killed mid-download, a tracker frozen and resumed,
#   both trackers down, and fd/thread counts after connection bursts.
# Usage: bash tests/failures.sh   (MODE=asan|tsan|valgrind optional)
source "$(dirname "$0")/lib.sh"
setup_run failures

# Big enough that a download takes a few seconds (so we can strike mid-way),
# smaller in slow modes.
case "$MODE" in
    plain) SIZE_MB=200 ;;
    asan)  SIZE_MB=100 ;;
    tsan)  SIZE_MB=60 ;;
    *)     SIZE_MB=16 ;;
esac
F="$RUN/files"; mkdir -p "$F/b" "$F/solo"
head -c $((SIZE_MB * 1024 * 1024 + 12345)) /dev/urandom >"$F/big.bin"
cp "$F/big.bin" "$F/b/big.bin"
head -c $((SIZE_MB * 1024 * 1024)) /dev/urandom >"$F/solo/solo.bin"
echo "  test files: ${SIZE_MB} MiB"

# wait_in_progress CLIENT GROUP FILE : poll show_downloads until the download
# is visibly in progress (1-99%), so the next step strikes MID-download.
wait_in_progress() {
    retry "$1" "show_downloads" "^\[D\] \[$2\] $3 ([1-9]|[1-9][0-9])% " 60
}

# new_client NAME PORT USER : start a client, create + log in USER
new_client() {
    start_client "$1" "$2" "${4:-$RUN/tracker_info.txt}"
    expect "$1" "\[connected to tracker"
    send "$1" "create_user $3 pw";   expect "$1" "^user $3 created"
    send "$1" "login $3 pw";         expect "$1" "^logged in as $3"
}

# join NAME USER GROUP : USER (on NAME) joins GROUP, owner alice/c1 or $4 accepts
join() {
    local owner=${4:-c1}
    send "$1" "join_group $3";       expect "$1" "^join request sent"
    retry "$owner" "list_requests $3" "^  $2$"
    send "$owner" "accept_request $3 $2"; expect "$owner" "^$2 added to group $3"
}

start_tracker 1
start_tracker 2
new_client c1 7101 alice
new_client c2 7102 bob
new_client c3 7103 carol
send c1 "create_group gf";           expect c1 "^group gf created"
join c2 bob gf
join c3 carol gf
send c1 "upload_file gf $F/big.bin";   expect c1 "^uploaded big.bin" 120
send c2 "upload_file gf $F/b/big.bin"; expect c2 "^uploaded big.bin" 120

section "seeder killed (kill -9) mid-download: the other seeder finishes it"
# (We kill bob, not alice: a crashed client logs out but stays a group
#  member - alice remains gf's owner, which later steps need.)
DL3="$RUN/dl_c3"; mkdir -p "$DL3"
send c3 "download_file gf big.bin $DL3"; expect c3 "^download started"
wait_in_progress c3 gf big.bin
kill9 c2
pass "killed seeder c2 (bob) mid-download"
expect c3 "^download finished: \[C\] \[gf\] big.bin" 120
same_file "$F/big.bin" "$DL3/big.bin"
no_part_files "$DL3"

section "tracker killed mid-download: download finishes; seeding re-announced via tracker 2"
new_client c4 7104 dave
join c4 dave gf
DL4="$RUN/dl_c4"; mkdir -p "$DL4"
send c4 "download_file gf big.bin $DL4"; expect c4 "^download started"   # from alice + carol
wait_in_progress c4 gf big.bin
kill9 t1
pass "killed tracker 1 mid-download"
expect c4 "^download finished: \[C\] \[gf\] big.bin" 120      # peers talk directly: no tracker needed
same_file "$F/big.bin" "$DL4/big.bin"
for c in c1 c3 c4; do send $c "list_groups"; expect $c "\[connected to tracker 2"; done
retry c4 "list_files gf" "^  big.bin  \([0-9]+ bytes, [0-9]+ pieces, 3 seeder\(s\)\)"   # alice, carol, dave

section "seeder frozen (SIGSTOP) mid-download: its piece times out, other peers serve it"
new_client c5 7105 erin
join c5 erin gf
DL5="$RUN/dl_c5"; mkdir -p "$DL5"
send c5 "download_file gf big.bin $DL5"; expect c5 "^download started"   # from alice, carol, dave
wait_in_progress c5 gf big.bin
kill -STOP "${PID[c1]}"
pass "froze seeder c1 (alice) mid-download"
expect c5 "^download finished: \[C\] \[gf\] big.bin" 180      # waits out the 10 s peer timeout
same_file "$F/big.bin" "$DL5/big.bin"
kill -CONT "${PID[c1]}"
send c1 "list_groups";               expect c1 "^groups"       # alice is fine after resuming

section "only seeder killed mid-download: clean failure, nothing left behind"
send c5 "create_group gs";           expect c5 "^group gs created"
new_client c6 7106 fay
join c6 fay gs c5
send c5 "upload_file gs $F/solo/solo.bin"; expect c5 "^uploaded solo.bin" 120
DL6="$RUN/dl_c6"; mkdir -p "$DL6"
send c6 "download_file gs solo.bin $DL6"; expect c6 "^download started"
wait_in_progress c6 gs solo.bin
kill9 c5
pass "killed the only seeder c5 (erin) mid-download"
expect c6 "^download failed: \[gs\] solo.bin: no working peer for [0-9]+ piece\(s\)" 120
no_part_files "$DL6"
[ ! -e "$DL6/solo.bin" ] && pass "no solo.bin left in $DL6" || fail "solo.bin exists after a failed download"
send c6 "show_downloads";            expect c6 "^\[F\] \[gs\] solo.bin - no working peer"
retry c6 "list_files gs" "^no files in gs"   # erin auto-logged-out, fay withdrew her partial copy

section "downloader killed mid-download: restart it, download again, succeeds"
new_client c8 7108 hal
join c8 hal gf
DL8="$RUN/dl_c8"; mkdir -p "$DL8"
send c8 "download_file gf big.bin $DL8"; expect c8 "^download started"
wait_in_progress c8 gf big.bin
kill9 c8
pass "killed downloader c8 (hal) mid-download"
[ -e "$DL8/big.bin.part" ] && pass "a crash leaves big.bin.part behind (no resume support; documented)" || fail "expected a leftover .part"
# alice, carol, dave (erin was killed above): 3 - hal's partial share went with his session.
retry c4 "list_files gf" "^  big.bin  \([0-9]+ bytes, [0-9]+ pieces, 3 seeder\(s\)\)"
start_client c8 7108                                          # same port, straight away (SO_REUSEADDR)
expect c8 "\[connected to tracker"
send c8 "login hal pw";              expect c8 "^logged in as hal"
send c8 "download_file gf big.bin $DL8"; expect c8 "^download started"   # stale .part is simply overwritten
expect c8 "^download finished: \[C\] \[gf\] big.bin" 120
same_file "$F/big.bin" "$DL8/big.bin"
no_part_files "$DL8"

section "tracker frozen (SIGSTOP): clients time out and fail over; it resumes and rejoins"
start_tracker 1
expect t1 "sync: merged snapshot from tracker 2"
new_client c7 7107 gus                                        # prefers tracker 1
expect t1 "login gus"
kill -STOP "${PID[t1]}"
pass "froze tracker 1"
send c7 "list_groups"
expect c7 "\[connected to tracker 2" 30                        # 5 s reply timeout, then failover
expect c7 "^groups"
expect t2 "sync: tracker 1 unreachable" 30                     # heartbeats stopped
kill -CONT "${PID[t1]}"
pass "resumed tracker 1"
# Depending on how long the freeze lasted, tracker 2's link to tracker 1 may
# or may not have timed out; either way the two must CONVERGE: same counts,
# both links up.
counts_of() {  # counts_of TRACKER : "U users, O online, G groups, F files" from a fresh `status`
    send "$1" "status"; sleep 0.5
    grep -o 'status: [0-9]* users, [0-9]* online, [0-9]* groups, [0-9]* files; sync connected' "$RUN/$1.out" | tail -1
}
converged=0
for i in $(seq 40); do
    a=$(counts_of t1); b=$(counts_of t2)
    if [ -n "$a" ] && [ "$a" = "$b" ]; then converged=1; break; fi
done
[ $converged = 1 ] && pass "trackers converged after the freeze: '${a#status: }'" || fail "trackers did not converge: t1='$a' t2='$b'"

section "no fd or thread leaks after bursts of connections"
fd_count() { ls "/proc/$1/fd" 2>/dev/null | wc -l; }
thread_count() { awk '/^Threads/ {print $2}' "/proc/$1/status"; }
burst() {  # burst PORT N : open and close N TCP connections (half of them send garbage)
    local i s
    for i in $(seq "$2"); do
        exec {s}<>"/dev/tcp/127.0.0.1/$1" || continue
        [ $((i % 2)) = 0 ] && printf 'GARBAGE-NOT-A-FRAME' >&"$s"
        exec {s}>&-
    done
}
for target in "t2 $TRACKER_PORT2 tracker-2" "c7 7107 client-c7-peer-server"; do
    set -- $target
    pid=${PID[$1]}
    fd0=$(fd_count "$pid"); th0=$(thread_count "$pid")
    burst "$2" 100
    sleep 1; burst "$2" 1; sleep 1         # one more accept reaps the finished threads
    fd1=$(fd_count "$pid"); th1=$(thread_count "$pid")
    if [ "$fd1" -le $((fd0 + 2)) ] && [ "$th1" -le $((th0 + 2)) ]; then
        pass "$3: after 101 connections fds $fd0 -> $fd1, threads $th0 -> $th1"
    else
        fail "$3: fds $fd0 -> $fd1, threads $th0 -> $th1 (leak?)"
    fi
done

section "both trackers down: clients report it and keep running"
kill9 t1
kill9 t2
send c7 "list_groups";               expect c7 "^error: no tracker reachable" 30
send c7 "show_downloads";            expect c7 "^no downloads"   # still alive and responsive
send c4 "list_files gf";             expect c4 "^error: no tracker reachable" 30

section "shut down the survivors"
for c in c1 c3 c4 c6 c7 c8; do stop $c; done
finish
