#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 ]]; then
    printf 'Usage: %s OUTPUT_DIR\n' "$0" >&2
    exit 2
fi

render_repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
mkdir -p -- "$1"
render_output_dir="$(cd -- "$1" && pwd)"
render_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/hdmi-demo-render.XXXXXX")"
trap 'rm -rf -- "$render_build_dir"' EXIT
render_cc="${CC:-cc}"

cd -- "$render_repo_dir"
"$render_cc" -std=c2x -O2 -DHDMI=1 \
    -Itests/stubs -Idrivers/graphics -Idrivers/hdmi \
    tests/render_hdmi_text_demo.c drivers/graphics/graphics_text.c \
    drivers/graphics/graphics_modes.c -o "$render_build_dir/render_hdmi_text_demo"
"$render_build_dir/render_hdmi_text_demo" "$render_output_dir"
printf '%s\n' 'Software renders only: these images do not validate hardware timing or signal quality.'
