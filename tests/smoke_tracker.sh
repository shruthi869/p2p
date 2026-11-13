#!/bin/bash
# smoke_tracker.sh - run the REAL tracker binary under valgrind and check:
#   1. console "quit" -> clean exit, no leaks
#   2. SIGTERM        -> graceful shutdown, no leaks
#   3. port already in use -> clear error, exit code 1
#   4. restart right after quit -> port binds again (SO_REUSEADDR)
set -u
cd "$(dirname "$0")/.."
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
RUN=test_runs/smoke_tracker
rm -rf "$RUN"; mkdir -p "$RUN"
# No --quiet: we need valgrind's summary lines to check them.
VG="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=99 --track-fds=yes"
fail=0
ok()  { echo "  ok   - $1"; }
bad() { echo "  FAIL - $1"; fail=1; }

# Clean = 0 errors, no leaked memory, and every fd open at exit was inherited
# (stdin/stdout/stderr + valgrind's own), i.e. we opened nothing we didn't close.
vg_clean() {
    local log=$1
    grep -q "ERROR SUMMARY: 0 errors" "$log" || return 1
    grep -q "definitely lost: [1-9]" "$log" && return 1
    grep -qE "FILE DESCRIPTORS: ([0-9]+) open \(\1 inherited\)" "$log" || return 1
    return 0
}

# 1. quit from the console
mkfifo "$RUN/in1"
$VG ./tracker/tracker tracker_info.txt 1 <"$RUN/in1" >"$RUN/t1.out" 2>"$RUN/t1.err" &
pid=$!
exec 3>"$RUN/in1"                       # keep the fifo open = an interactive console
sleep 2
echo "hello" >&3                        # unknown command
echo "quit" >&3
wait $pid; code=$?
exec 3>&-
[ $code -eq 0 ] && ok "quit -> exit 0" || bad "quit -> exit $code"
grep -q "unknown command 'hello'" "$RUN/t1.out" && ok "unknown console command reported" || bad "no unknown-command message"
grep -q "stopped" "$RUN/t1.out" && ok "printed 'stopped'" || bad "no 'stopped' line"
vg_clean "$RUN/t1.err" && ok "valgrind clean: $(grep -o 'FILE DESCRIPTORS.*' "$RUN/t1.err")" || bad "valgrind (see $RUN/t1.err)"

# 4. immediately start again on the same port
./tracker/tracker tracker_info.txt 1 </dev/null >"$RUN/t2.out" 2>&1 &
pid=$!
sleep 0.5
kill -0 $pid 2>/dev/null && ok "restart binds the same port at once" || bad "restart failed: $(cat "$RUN/t2.out")"

# 3. a second tracker 1 while the first is running
./tracker/tracker tracker_info.txt 1 </dev/null >"$RUN/t3.out" 2>&1; code=$?
[ $code -eq 1 ] && grep -q "Address already in use" "$RUN/t3.out" && ok "port in use -> clear error" || bad "port in use: exit $code, $(cat "$RUN/t3.out")"
kill $pid; wait $pid 2>/dev/null

# 2. SIGTERM under valgrind (stdin = /dev/null, i.e. running in the background)
$VG ./tracker/tracker tracker_info.txt 2 </dev/null >"$RUN/t4.out" 2>"$RUN/t4.err" &
pid=$!
sleep 2
kill -TERM $pid
wait $pid; code=$?
[ $code -eq 0 ] && ok "SIGTERM -> graceful exit 0" || bad "SIGTERM -> exit $code"
grep -q "shutting down" "$RUN/t4.out" && ok "printed shutdown message" || bad "no shutdown message"
vg_clean "$RUN/t4.err" && ok "valgrind clean: $(grep -o 'FILE DESCRIPTORS.*' "$RUN/t4.err")" || bad "valgrind (see $RUN/t4.err)"

[ $fail -eq 0 ] && echo "SMOKE TRACKER: PASS" || echo "SMOKE TRACKER: FAIL"
exit $fail
