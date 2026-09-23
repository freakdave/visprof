#!/bin/bash
# Copyright (c) 2026 David Reichelt. SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
: "${KOS_BASE:?Source the KOS environ.sh before running this check}"
check_tmp=$(mktemp -d "${TMPDIR:-/tmp}/visprof-check.XXXXXX")
trap 'rm -rf "$check_tmp"' EXIT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
run() {
    if ! "$@" >"$check_tmp/command.log" 2>&1; then
        cat "$check_tmp/command.log" >&2
        fail "$*"
    fi
}
require_file() { test -s "$1" || fail "Missing or empty file: $1"; }
implementation_symbols() {
    awk '$2 ~ /^[A-Za-z]$/ && !($2 == "W" && $3 ~ /^_visprof(_draw|_capture)?[.]c[.][[:xdigit:]]+$/) { print $3 }' "$1"
}
foreign_refs() {
    awk '$1 == "U" { print $2 }' "$1" | sort -u | awk '
        !/^_(bfont_|fs_|pvr_|thd_|vid_|visprof_)/ &&
        !/^_(malloc|free|memcpy|memmove|memset|snprintf|printf|puts|vsnprintf)$/ &&
        !/^(___dreamcast_get_ticks|___floatundisf|___udivsi3_i4i|__ctype_|_dbglog_level)$/ { print }'
}
archive_state() {
    run sh-elf-nm --defined-only -g libvisprof.a
    require_file "$check_tmp/command.log"
    implementation_symbols "$check_tmp/command.log" >"$check_tmp/symbols"
    if [ "$1" = 1 ]; then
        grep -qx '_visprof_init' "$check_tmp/symbols" || fail 'Missing visprof_init'
        grep -qx '_visprof_draw' "$check_tmp/symbols" || fail 'Missing visprof_draw'
    else
        test ! -s "$check_tmp/symbols" || fail 'Disabled archive contains implementation symbols'
    fi
}
link_hosts() {
    run make -C examples/basic -j2 VISPROF_ENABLED="$1"
    require_file examples/basic/visprof_demo.elf
    run kos-c++ -std=gnu++11 -Iinclude -UVISPROF_ENABLED -DVISPROF_ENABLED="$1" -Wall -Wextra -Werror \
        -o "$check_tmp/cxx-$1.elf" tests/cxx_smoke.cpp libvisprof.a
    require_file "$check_tmp/cxx-$1.elf"
}

printf 'Host regression tests\n'
run gcc -std=gnu99 -Itests/stubs -Iinclude -Isrc -Wall -Wextra -Werror \
    -o "$check_tmp/core" src/visprof.c tests/core.c
run "$check_tmp/core"
cat "$check_tmp/command.log"

printf 'Span sink tests\n'
run gcc -std=gnu99 -Itests/stubs -Iinclude -Isrc -Wall -Wextra -Werror \
    -o "$check_tmp/draw_sink" src/visprof.c src/visprof_draw.c tests/draw_sink.c
run "$check_tmp/draw_sink"
cat "$check_tmp/command.log"

printf 'Screen capture tests\n'
run gcc -std=gnu99 -Itests/stubs -Iinclude -Isrc -Wall -Wextra -Werror \
    -o "$check_tmp/capture" src/visprof.c src/visprof_capture.c tests/capture.c
run "$check_tmp/capture"
cat "$check_tmp/command.log"

printf 'Strict KOS compilation\n'
for source in src/visprof.c src/visprof_draw.c src/visprof_capture.c; do
    run kos-cc -Iinclude -Isrc -Wall -Wextra -Werror -Wshadow -Wcast-align \
        -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wundef \
        -Wwrite-strings -c "$source" -o "$check_tmp/strict.o"
    require_file "$check_tmp/strict.o"
done

printf 'Check failure handling\n'
if (run sh -c 'exit 1') >"$check_tmp/expected-failure.log" 2>&1; then
    fail 'Runner accepted a silent failed command'
fi
printf '         U _unrelated_library_call\n' >"$check_tmp/foreign.nm"
test -n "$(foreign_refs "$check_tmp/foreign.nm")" || fail 'Dependency check accepted an unknown symbol'

printf 'Enabled build and C/C++ links\n'
run make clean
run make -j2 VISPROF_ENABLED=1
require_file libvisprof.a
archive_state 1
link_hosts 1
run sh-elf-nm libvisprof.a
cp "$check_tmp/command.log" "$check_tmp/deps.nm"
grep -q ' U ' "$check_tmp/deps.nm" || fail 'No archive dependencies were parsed'
extra=$(foreign_refs "$check_tmp/deps.nm")
test -z "$extra" || fail "Unexpected archive dependencies: $extra"
run sh-elf-size examples/basic/visprof_demo.elf
read -r on_text on_data on_bss _ < <(tail -1 "$check_tmp/command.log")

printf 'Enabled to disabled transition and C/C++ links\n'
run make -j2 VISPROF_ENABLED=0
archive_state 0
link_hosts 0
run sh-elf-size examples/basic/visprof_demo.elf
read -r off_text off_data off_bss _ < <(tail -1 "$check_tmp/command.log")
for number in "$on_text" "$on_data" "$on_bss" "$off_text" "$off_data" "$off_bss"; do
    [[ "$number" =~ ^[0-9]+$ ]] || fail 'Invalid size output'
done
printf 'Demo size difference: text=%d data=%d bss=%d bytes\n' \
    "$((on_text - off_text))" "$((on_data - off_data))" "$((on_bss - off_bss))"

printf 'Disabled to enabled transition and final demo\n'
run make -j2 VISPROF_ENABLED=1
archive_state 1
link_hosts 1
printf 'ALL CHECKS PASSED\n'
