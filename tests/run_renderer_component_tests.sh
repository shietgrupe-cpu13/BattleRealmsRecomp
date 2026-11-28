#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${RUNNER_TEMP:-/tmp}/br-renderer-component-tests"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$root/runtime" \
  "$root/tests/d3d7_gl_state_cache_smoke.cpp" -o "$build_dir/d3d7_gl_state_cache_smoke"
"$build_dir/d3d7_gl_state_cache_smoke"
echo "D3D7 GLES state-cache invalidation regression passed."
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$root/runtime" \
  "$root/tests/d3d7_vertex_batch_smoke.cpp" -o "$build_dir/d3d7_vertex_batch_smoke"
"$build_dir/d3d7_vertex_batch_smoke"
echo "D3D7 vertex-batch allocation regression passed (3000 vertices, <=8 capacity growths)."
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$root/runtime" \
  "$root/tests/d3d7_texture_probe_smoke.cpp" -o "$build_dir/d3d7_texture_probe_smoke"
"$build_dir/d3d7_texture_probe_smoke"
echo "D3D7 texture source/conversion probe regression passed."
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$root/runtime" \
  "$root/tests/ms_adpcm_decoder_smoke.cpp" -o "$build_dir/ms_adpcm_decoder_smoke"
"$build_dir/ms_adpcm_decoder_smoke"
echo "MS-ADPCM wide-delta decoder regression passed."
