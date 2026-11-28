#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ra3_runtime="${RA3_RUNTIME_DIR:-$root/references/RA3-Native/runtime}"
game_exe="${BR_GAME_EXE:?BR_GAME_EXE must point to the private Battle Realms executable}"
scripts_dir="${BR_GAME_SCRIPTS:?BR_GAME_SCRIPTS must point to the private Scripts directory}"
build_dir="${BR_HOST_TEST_BUILD_DIR:-${RUNNER_TEMP:-/tmp}/br-host-tests}"

test -f "$ra3_runtime/guest.h"
test -f "$game_exe"
test -d "$scripts_dir"
python3 "$root/tests/runtime_roots_smoke.py"
python3 "$root/tests/incremental_runtime_lift_smoke.py"
python3 "$root/tests/dispatch_coverage_smoke.py"
python3 "$root/tests/dispatch_site_trace_smoke.py"
python3 "$root/tests/lift_object_inventory_smoke.py"
python3 "$root/tests/unicorn_differential_suite_smoke.py"
python3 "$root/tests/backward_jump_table_patch_smoke.py"
python3 "$root/tests/lifted_function_overlay_smoke.py"
mkdir -p "$root/analysis"
cmake -S "$root/runtime/pe32_loader" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DRA3_RUNTIME_DIR="$ra3_runtime"
cmake --build "$build_dir" --parallel 2

"$build_dir/pe32_loader_smoke" "$game_exe" "$scripts_dir"
"$build_dir/game_runtime_smoke" "$game_exe"
"$build_dir/guest_bridge_smoke"
"$build_dir/lift_dispatch_smoke"
"$build_dir/android_guest_profile_scope_smoke"

test_build="$build_dir/focused"
mkdir -p "$test_build"
common=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$root/runtime")
"${CXX:-c++}" "${common[@]}" "$root/tools/d3d7_fixed_function_smoke.cpp" \
  -o "$test_build/d3d7_fixed_function_smoke"
"$test_build/d3d7_fixed_function_smoke"
"${CXX:-c++}" "${common[@]}" "$root/tools/d3d7_scanline_smoke.cpp" \
  -o "$test_build/d3d7_scanline_smoke"
"$test_build/d3d7_scanline_smoke"
"${CXX:-c++}" "${common[@]}" "$root/tools/framebuffer_convert_smoke.cpp" \
  -o "$test_build/framebuffer_convert_smoke"
"$test_build/framebuffer_convert_smoke"
"${CXX:-c++}" "${common[@]}" "$root/tools/android_audio_monitor_smoke.cpp" \
  -o "$test_build/android_audio_monitor_smoke"
"$test_build/android_audio_monitor_smoke"
"${CXX:-c++}" "${common[@]}" "$root/tools/android_touch_gesture_smoke.cpp" \
  "$root/runtime/android_touch_gestures.cpp" \
  -o "$test_build/android_touch_gesture_smoke"
"$test_build/android_touch_gesture_smoke"
"${CXX:-c++}" "${common[@]}" "$root/tools/d3d7_gpu_backend_smoke.cpp" \
  "$root/runtime/d3d7_gpu_backend_stub.cpp" \
  -o "$test_build/d3d7_gpu_backend_smoke"
"$test_build/d3d7_gpu_backend_smoke"

echo "BattleRealms host runtime and compatibility smoke tests passed."
