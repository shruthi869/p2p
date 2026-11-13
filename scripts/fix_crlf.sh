#!/bin/bash
# fix_crlf.sh - strip Windows '\r' from TEXT files only (sources, scripts,
# makefiles, docs). Never touch build outputs: object files are binary.
cd "$(dirname "$0")/.."
find . -path ./.git -prune -o -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.mk' -o -name 'Makefile' \
       -o -name '*.sh' -o -name '*.md' -o -name '*.txt' -o -name '.gitignore' \) \
    -print0 | xargs -0 sed -i 's/\r$//'
