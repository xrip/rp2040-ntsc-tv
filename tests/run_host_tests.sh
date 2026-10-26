#!/usr/bin/env bash
set -euo pipefail

test_repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/hdmi-host-tests.XXXXXX")"
trap 'rm -rf -- "$test_build_dir"' EXIT
test_cc="${CC:-cc}"

cd -- "$test_repo_dir"

"$test_cc" -std=c2x -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DHDMI=1 -Itests/stubs -Idrivers/graphics -Idrivers/hdmi \
    tests/test_graphics_text.c drivers/graphics/graphics_text.c \
    drivers/graphics/graphics_modes.c -o "$test_build_dir/graphics_text"

# LeakSanitizer needs /proc thread access that some build sandboxes deny.
# AddressSanitizer and UndefinedBehaviorSanitizer remain enabled and fatal.
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:halt_on_error=1" \
    "$test_build_dir/graphics_text"

for test_invert in 0 1; do
    for test_rgb_order in 0 1; do
        "$test_cc" -std=c11 -O2 -g -fno-omit-frame-pointer \
            -fsanitize=address,undefined -fno-sanitize-recover=all \
            -Wall -Wextra -Werror \
            -DHDMI_PIN_invert_diffpairs="$test_invert" \
            -DHDMI_PIN_RGB_notBGR="$test_rgb_order" \
            tests/test_hdmi_codec.c -o "$test_build_dir/hdmi_codec"
        ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:halt_on_error=1" \
            "$test_build_dir/hdmi_codec"
    done
done

python3 tests/test_hdmi_pio.py

printf '%s\n' 'All host tests passed. These tests do not replace hardware timing validation.'
