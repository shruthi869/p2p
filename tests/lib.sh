#!/bin/bash
# lib.sh - helpers for integration tests that drive the REAL tracker and
# client binaries on 127.0.0.1.
#
# Each process reads its stdin from a FIFO that the test keeps open (like a
# person at a terminal) and writes stdout/stderr to files under test_runs/.
#
#   start_tracker 1                 start tracker 1
#   start_client c1 7101            start client "c1" listening on port 7101
#   send c1 "login alice pw"        type a command into c1
#   expect c1 "logged in as alice"  wait (default 5 s) for a NEW output line matching the regex
#   stop c1                         send "quit" and wait for a clean exit
#   kill9 c1                        simulate a crash
#   finish                          print the summary; exit code = number of failures
#
# MODE=plain|asan|tsan|valgrind picks the binaries / wrapper. Sanitizer and
# valgrind reports are checked for every process that exits normally.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin  # avoid slow /mnt/c entries
MODE=${MODE:-plain}
SUF=""
WRAP=""
SCALE=1  # timeout multiplier for slow modes
case "$MODE" in
    asan) SUF=-asan; SCALE=2 ;;
    tsan) SUF=-tsan; SCALE=3 ;;
    valgrind)
        WRAP="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99 --track-fds=yes"
        SCALE=6 ;;
    plain) ;;
    *) echo "unknown MODE=$MODE"; exit 2 ;;
esac

TRACKER_PORT1=6101
TRACKER_PORT2=6102

declare -A PID FD POS
PASSES=0
FAILS=0

pass() { PASSES=$((PASSES + 1)); echo "  ok   - $1"; }
fail() { FAILS=$((FAILS + 1)); echo "  FAIL - $1"; }
section() { echo; echo "## $1"; }

# setup_run NAME : fresh directory test_runs/NAME and a tracker_info.txt
setup_run() {
    RUN="$ROOT/test_runs/$1"
    rm -rf "$RUN"
    mkdir -p "$RUN"
    printf '127.0.0.1 %s\n127.0.0.1 %s\n' "$TRACKER_PORT1" "$TRACKER_PORT2" >"$RUN/tracker_info.txt"
    trap cleanup EXIT
    echo "[$1] mode=$MODE, logs in $RUN"
}

# _launch NAME CMD... : start a process with stdin from a FIFO we hold open
_launch() {
    local name=$1; shift
    rm -f "$RUN/$name.in"
    mkfifo "$RUN/$name.in"
    $WRAP "$@" <"$RUN/$name.in" >"$RUN/$name.out" 2>"$RUN/$name.err" &
    PID[$name]=$!
    local fd
    exec {fd}>"$RUN/$name.in"
    FD[$name]=$fd
    POS[$name]=0
}

start_tracker() {  # start_tracker N
    _launch "t$1" "$ROOT/tracker/tracker$SUF" "$RUN/tracker_info.txt" "$1"
    expect "t$1" "listening on" 10
}

start_client() {  # start_client NAME PORT [tracker_info]
    local info=${3:-$RUN/tracker_info.txt}
    _launch "$1" "$ROOT/client/client$SUF" "127.0.0.1:$2" "$info"
}

send() {  # send NAME "command"
    echo "$2" >&"${FD[$1]}"
    echo "> $2" >>"$RUN/$1.cmds"
}

# expect NAME REGEX [TIMEOUT_S] : wait for a line matching REGEX that appears
# after the previously matched line (so repeated messages are not confused).
expect() {
    local name=$1 pat=$2 t=${3:-5}
    local deadline=$((SECONDS + t * SCALE))
    while :; do
        local n
        n=$(tail -n +"$((POS[$name] + 1))" "$RUN/$name.out" 2>/dev/null | grep -n -m1 -E -- "$pat" | cut -d: -f1)
        if [ -n "$n" ]; then
            POS[$name]=$((POS[$name] + n))
            pass "$name: /$pat/"
            return 0
        fi
        if [ $SECONDS -ge $deadline ]; then
            fail "$name: expected /$pat/ within ${t}s (see $RUN/$name.out)"
            return 1
        fi
        sleep 0.05
    done
}

# Reports from sanitizers / valgrind in a process's stderr.
_check_reports() {
    local name=$1 log="$RUN/$1.err"
    if grep -qE "ERROR: AddressSanitizer|ERROR: LeakSanitizer|WARNING: ThreadSanitizer|runtime error:" "$log"; then
        fail "$name: sanitizer report (see $log)"
        return
    fi
    if [ "$MODE" = valgrind ]; then
        if ! grep -q "ERROR SUMMARY: 0 errors" "$log" ||
           ! grep -qE "FILE DESCRIPTORS: ([0-9]+) open \(\1 inherited\)" "$log"; then
            fail "$name: valgrind found errors, leaks or open fds (see $log)"
            return
        fi
        pass "$name: valgrind clean ($(grep -o 'FILE DESCRIPTORS: [0-9]* open ([0-9]* inherited)' "$log"))"
    fi
}

# _wait_exit NAME : wait up to 10 s (scaled) for the process; sets EXIT_CODE.
# (Must run in THIS shell, not in $(...): only the parent shell can `wait`.)
_wait_exit() {
    local pid=${PID[$1]}
    local deadline=$((SECONDS + 10 * SCALE))
    while kill -0 "$pid" 2>/dev/null; do
        if [ $SECONDS -ge $deadline ]; then
            kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            EXIT_CODE=124
            return
        fi
        sleep 0.05
    done
    wait "$pid"
    EXIT_CODE=$?
}

_close_fd() {
    local fd=${FD[$1]:-}
    [ -n "$fd" ] && exec {fd}>&-
    unset "FD[$1]"
}

stop() {  # stop NAME : "quit" + clean exit + sanitizer/valgrind check
    local name=$1
    send "$name" "quit"
    _close_fd "$name"
    _wait_exit "$name"
    unset "PID[$name]"
    [ "$EXIT_CODE" = 0 ] && pass "$name: exited cleanly" || fail "$name: exit code $EXIT_CODE (see $RUN/$name.err)"
    _check_reports "$name"
}

kill9() {  # kill9 NAME : simulate a crash (no cleanup at all)
    kill -9 "${PID[$1]}" 2>/dev/null
    wait "${PID[$1]}" 2>/dev/null
    unset "PID[$1]"
    _close_fd "$1"
}

# retry NAME "command" REGEX [TIMEOUT_S] : send the command again and again
# until its output contains a matching line. For state that arrives
# asynchronously (tracker-to-tracker sync is eventually consistent).
retry() {
    local name=$1 cmd=$2 pat=$3 t=${4:-10}
    local deadline=$((SECONDS + t * SCALE))
    while :; do
        send "$name" "$cmd"
        sleep 0.3
        if tail -n +"$((POS[$name] + 1))" "$RUN/$name.out" | grep -q -E -- "$pat"; then
            sleep 0.2
            POS[$name]=$(wc -l <"$RUN/$name.out")  # skip the rest of these listings
            pass "$name: /$pat/ (after retrying '$cmd')"
            return 0
        fi
        if [ $SECONDS -ge $deadline ]; then
            fail "$name: '$cmd' never printed /$pat/ within ${t}s (see $RUN/$name.out)"
            return 1
        fi
    done
}

same_file() {  # same_file ORIGINAL COPY : byte-for-byte identical?
    if cmp -s "$1" "$2"; then
        pass "identical: $(basename "$2") ($(stat -c %s "$2") bytes, sha1 $(sha1sum "$2" | cut -c1-12)...)"
    else
        fail "files differ: $1 vs $2"
    fi
}

no_part_files() {  # no_part_files DIR : no leftover partial downloads
    if compgen -G "$1/*.part" >/dev/null; then fail "leftover .part files in $1"; else pass "no .part files left in $(basename "$1")"; fi
}

cleanup() {
    for name in "${!PID[@]}"; do kill -9 "${PID[$name]}" 2>/dev/null; done
    wait 2>/dev/null
}

finish() {
    echo
    echo "RESULT: $PASSES passed, $FAILS failed"
    [ "$FAILS" -eq 0 ]
    exit $?
}
