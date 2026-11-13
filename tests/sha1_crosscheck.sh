#!/bin/bash
# sha1_crosscheck.sh - our SHA-1 / piece hashing vs. the system's sha1sum.
# For each test file: whole-file hash must equal `sha1sum FILE`, and piece i
# must equal sha1sum of bytes [i*512KiB, (i+1)*512KiB) cut out with `split`.
#   LARGE=1 also checks a 1 GiB file (slow; used by `make test-large`).
set -u
cd "$(dirname "$0")/.."
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SUF=${SUF:-}
TOOL=./tests/sha1tool$SUF
RUN=test_runs/sha1_crosscheck
rm -rf "$RUN"; mkdir -p "$RUN"
fail=0

sizes="1 55 56 64 1000 524287 524288 524289 1048576 1572987"
[ "${LARGE:-0}" = 1 ] && sizes="$sizes 1073741824"

for size in $sizes; do
    f="$RUN/file_$size"
    head -c "$size" /dev/urandom >"$f"
    # /usr/bin/time -f %M = peak memory (max RSS, KB) of the hashing process.
    ours=$(/usr/bin/time -f '%M' -o "$RUN/rss" "$TOOL" "$f") || { echo "  FAIL - sha1tool failed on $size bytes"; fail=1; continue; }
    rss_kb=$(tail -1 "$RUN/rss")
    expected_file=$(sha1sum "$f" | cut -d' ' -f1)
    rm -f "$RUN"/piece_*
    split -b 524288 -a 5 -d "$f" "$RUN/piece_"
    expected_pieces=$(sha1sum "$RUN"/piece_* | cut -d' ' -f1)
    expected=$(printf '%s\n%s' "$expected_file" "$expected_pieces")
    npieces=$(echo "$expected_pieces" | wc -l)
    if [ "$ours" = "$expected" ]; then
        echo "  ok   - $size bytes: file hash + $npieces piece hash(es) match sha1sum (peak memory ${rss_kb} KB)"
    else
        echo "  FAIL - $size bytes: hashes differ from sha1sum"
        fail=1
    fi
    # Streaming means memory must NOT grow with file size (sanitizer builds use more, so plain only).
    if [ -z "$SUF" ] && [ "$rss_kb" -gt 20000 ]; then
        echo "  FAIL - $size bytes: peak memory ${rss_kb} KB > 20 MB (file not streamed?)"
        fail=1
    fi
    rm -f "$f" "$RUN"/piece_*
done
[ $fail -eq 0 ] && echo "SHA1 CROSSCHECK: PASS" || echo "SHA1 CROSSCHECK: FAIL"
exit $fail
