#!/bin/bash
# check_all.sh - full quality gate: clean build with warning count, then
# unit + integration tests in four flavours (plain, ASan+UBSan, TSan,
# valgrind), then the tracker binary smoke test.
# Run from anywhere: bash scripts/check_all.sh   (full logs in test_runs/logs/)
set -u
cd "$(dirname "$0")/.."
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin  # skip slow /mnt/c dirs
bash scripts/fix_crlf.sh

fail=0
step() { printf '\n===== %s =====\n' "$1"; }
# Run a make target; show only the summary lines; keep the full log.
run() {
    local name=$1; shift
    if "$@" >"$LOGS/$name.log" 2>&1; then :; else fail=1; echo "  (FAILED - see $LOGS/$name.log)"; fi
    grep -E 'checks,|leaked|RESULT|FAIL|SMOKE|CROSSCHECK' "$LOGS/$name.log" | sed 's/^/  /'
}

step "clean build"
make clean >/dev/null
LOGS=test_runs/logs
mkdir -p "$LOGS"
make all tests >"$LOGS/build.log" 2>&1 || fail=1
warnings=$(grep -c 'warning:' "$LOGS/build.log")
echo "  warnings: $warnings"
[ "$warnings" -eq 0 ] || { grep 'warning:' "$LOGS/build.log"; fail=1; }

step "unit + integration tests";      run test     make --no-print-directory test
step "AddressSanitizer + UBSan";      run asan     make --no-print-directory asan
step "ThreadSanitizer";               run tsan     make --no-print-directory tsan
step "valgrind";                      run valgrind make --no-print-directory valgrind
step "tracker binary smoke test";     run smoke    bash tests/smoke_tracker.sh

echo
[ $fail -eq 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $fail
