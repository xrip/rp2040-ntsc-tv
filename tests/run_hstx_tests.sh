#!/usr/bin/env bash
set -euo pipefail

test_repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/hstx-host-tests.XXXXXX")"
trap 'rm -rf -- "$test_build_dir"' EXIT
test_cc="${CC:-cc}"

cd -- "$test_repo_dir"

test_flags=(
    -std=c2x -O2 -g -fno-omit-frame-pointer
    -fsanitize=address,undefined -fno-sanitize-recover=all
    -Wall -Wextra -Wno-unused-variable
    -DHDMI_HSTX=1 -DNTSC_TV=1
    -Itests/stubs -Idrivers/graphics -Idrivers/hdmi-hstx -Idrivers/ntsc-composite
)
# graphics.h includes the existing application font arrays, which these tests
# deliberately do not use. Keep other warnings enabled for the production code.
test_sources=(drivers/graphics/graphics_text.c drivers/graphics/graphics_modes.c)

"$test_cc" "${test_flags[@]}" tests/test_hstx_render.c "${test_sources[@]}" \
    -o "$test_build_dir/hstx_render"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:halt_on_error=1" \
    "$test_build_dir/hstx_render"

# Reuse the existing descriptor suite under the new build macros as a guard
# against changes to font/VRAM ownership, geometry and mode-switch behavior.
# Its original introductory // comment contains shell continuation backslashes.
"$test_cc" "${test_flags[@]}" -Wno-comment tests/test_graphics_text.c "${test_sources[@]}" \
    -o "$test_build_dir/graphics_text_hstx"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:halt_on_error=1" \
    "$test_build_dir/graphics_text_hstx"

printf '%s\n' 'HSTX host tests passed. Hardware signal and timing validation remain separate.'
