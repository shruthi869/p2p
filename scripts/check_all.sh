#!/bin/bash
# check_all.sh - full quality gate: clean build with warning count, tests,
# ASan/UBSan, TSan, valgrind. Run from anywhere: bash scripts/check_all.sh
set -u
cd "$(dirname "$0")/.."
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin  # skip slow /mnt/c dirs

fail=0
step() { printf '\n===== %s =====\n' "$1"; }

step "clean build"
make clean >/dev/null
make all tests >build.log 2>&1 || fail=1
warnings=$(grep -c 'warning:' build.log)
echo "warnings: $warnings"
[ "$warnings" -eq 0 ] || { grep 'warning:' build.log; fail=1; }
rm -f build.log

step "unit tests"
make --no-print-directory test >test.log 2>&1 || fail=1
grep -v '^make' test.log; rm -f test.log

step "AddressSanitizer + UBSan"
make --no-print-directory asan >asan.log 2>&1 || { cat asan.log; fail=1; }
grep -E 'checks,|leaked' asan.log; rm -f asan.log

step "ThreadSanitizer"
make --no-print-directory tsan >tsan.log 2>&1 || { cat tsan.log; fail=1; }
grep -E 'checks,|leaked' tsan.log; rm -f tsan.log

step "valgrind"
make --no-print-directory valgrind >vg.log 2>&1 || { cat vg.log; fail=1; }
grep -E 'checks,|leaked' vg.log; rm -f vg.log

step "tracker binary smoke test (valgrind)"
bash tests/smoke_tracker.sh || fail=1

echo
[ $fail -eq 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $fail
