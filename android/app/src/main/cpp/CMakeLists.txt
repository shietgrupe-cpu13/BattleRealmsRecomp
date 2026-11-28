cmake_minimum_required(VERSION 3.22)
project(brnative C CXX)
set(CMAKE_CXX_STANDARD 17)
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
get_filename_component(BR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

# Configure this before the runtime subdirectory so the dispatch target can
# exclude host API work from lifted-function residency samples.
option(BR_ANDROID_GUEST_PROFILE "Enable sampled per-function guest profiling" OFF)
option(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS "Record bounded D3D7 texture/draw diagnostics" OFF)
add_subdirectory("${BR_ROOT}/runtime/pe32_loader" "${CMAKE_CURRENT_BINARY_DIR}/battle_realms_runtime")

# Profiling samples scoped current guest functions from traced lifted code.
# Select distinct traced object caches; never silently link the normal caches
# (whose CMakeCache has BR_TRACE_LIFTED_FUNCTIONS=OFF) into a profile APK.
if(BR_ANDROID_GUEST_PROFILE)
	set(BR_MAIN_LIFT_BUILD_DIR "${BR_ROOT}/build/arm64_hybrid96_profile")
	set(BR_PROFILE_CACHE "${BR_MAIN_LIFT_BUILD_DIR}/CMakeCache.txt")
	if(NOT EXISTS "${BR_PROFILE_CACHE}")
		message(FATAL_ERROR "Per-function profiling requires the main lifted objects built with BR_TRACE_LIFTED_FUNCTIONS=ON and BR_PROFILE_GUEST_FUNCTION_SCOPE=ON: ${BR_PROFILE_CACHE}")
	endif()
	file(STRINGS "${BR_PROFILE_CACHE}" BR_TRACE_CACHE_LINE REGEX "^BR_TRACE_LIFTED_FUNCTIONS:BOOL=ON$")
	if(NOT BR_TRACE_CACHE_LINE)
		message(FATAL_ERROR "Per-function profiling requires BR_TRACE_LIFTED_FUNCTIONS=ON in ${BR_PROFILE_CACHE}.")
	endif()
	file(STRINGS "${BR_PROFILE_CACHE}" BR_SCOPE_CACHE_LINE REGEX "^BR_PROFILE_GUEST_FUNCTION_SCOPE:BOOL=ON$")
	if(NOT BR_SCOPE_CACHE_LINE)
		message(FATAL_ERROR "Per-function profiling requires scoped current-function tracking in ${BR_PROFILE_CACHE}.")
	endif()
	set(BR_MAIN_OBJECT_DIR "${BR_MAIN_LIFT_BUILD_DIR}/CMakeFiles/battle_realms_lifted.dir")
	# Only the 13,743 main-PE functions are the current Ghidra correlation target.
	# Keep dynamically loaded campaign/script code on its ordinary object cache.
	set(BR_SCRIPT_OBJECT_DIR "${BR_ROOT}/build/arm64_rebased_scripts/CMakeFiles/battle_realms_lifted.dir")
	set(BR_COMBINED_TABLE_OBJECT "${BR_ROOT}/build/arm64_rebased_scripts/lifted_table_combined.o")
else()
	set(BR_MAIN_OBJECT_DIR "${BR_ROOT}/build/arm64_hybrid96/CMakeFiles/battle_realms_lifted.dir")
	set(BR_SCRIPT_OBJECT_DIR "${BR_ROOT}/build/arm64_rebased_scripts/CMakeFiles/battle_realms_lifted.dir")
	set(BR_COMBINED_TABLE_OBJECT "${BR_ROOT}/build/arm64_rebased_scripts/lifted_table_combined.o")
endif()

# Reuse the deterministic ARM64 objects produced by the existing lifting build.
# The previous Android preview linked preview_empty_lift_table.cpp, so the
# shipped runtime's dispatcher always saw lifted_count == 0 even though the
# generated code and the full link probe already existed in this workspace.
file(GLOB_RECURSE BR_MAIN_LIFT_OBJECTS CONFIGURE_DEPENDS "${BR_MAIN_OBJECT_DIR}/*.o")
file(GLOB_RECURSE BR_SCRIPT_LIFT_OBJECTS CONFIGURE_DEPENDS "${BR_SCRIPT_OBJECT_DIR}/*.o")
list(FILTER BR_MAIN_LIFT_OBJECTS EXCLUDE REGEX "(^|/)lifted_table\\.cpp\\.o$")
list(FILTER BR_SCRIPT_LIFT_OBJECTS EXCLUDE REGEX "(^|/)lifted_table\\.cpp\\.o$")
list(LENGTH BR_MAIN_LIFT_OBJECTS BR_MAIN_LIFT_COUNT)
list(LENGTH BR_SCRIPT_LIFT_OBJECTS BR_SCRIPT_LIFT_COUNT)
set(BR_LIFT_OBJECT_INVENTORY "${BR_ROOT}/build/lift_object_inventory.json")
if(NOT EXISTS "${BR_LIFT_OBJECT_INVENTORY}")
	message(FATAL_ERROR "Validated lift-object inventory is missing: ${BR_LIFT_OBJECT_INVENTORY}. Run tools/write_lift_object_inventory.py after compiling the generated lift objects.")
endif()
file(READ "${BR_LIFT_OBJECT_INVENTORY}" BR_LIFT_OBJECT_INVENTORY_JSON)
string(JSON BR_LIFT_OBJECT_INVENTORY_SCHEMA ERROR_VARIABLE BR_INVENTORY_JSON_ERROR GET "${BR_LIFT_OBJECT_INVENTORY_JSON}" schema)
if(BR_INVENTORY_JSON_ERROR OR NOT BR_LIFT_OBJECT_INVENTORY_SCHEMA EQUAL 1)
	message(FATAL_ERROR "Lift-object inventory is invalid or has an unsupported schema: ${BR_INVENTORY_JSON_ERROR}")
endif()
string(JSON BR_EXPECTED_MAIN_LIFT_COUNT ERROR_VARIABLE BR_MAIN_INVENTORY_ERROR GET "${BR_LIFT_OBJECT_INVENTORY_JSON}" main_object_count)
string(JSON BR_EXPECTED_SCRIPT_LIFT_COUNT ERROR_VARIABLE BR_SCRIPT_INVENTORY_ERROR GET "${BR_LIFT_OBJECT_INVENTORY_JSON}" script_object_count)
string(JSON BR_INVENTORY_PASSED ERROR_VARIABLE BR_PASSED_INVENTORY_ERROR GET "${BR_LIFT_OBJECT_INVENTORY_JSON}" passed)
string(JSON BR_INVENTORY_OBJECTS_VERIFIED ERROR_VARIABLE BR_OBJECTS_VERIFIED_ERROR GET "${BR_LIFT_OBJECT_INVENTORY_JSON}" objects_verified)
if(BR_MAIN_INVENTORY_ERROR OR BR_SCRIPT_INVENTORY_ERROR OR BR_PASSED_INVENTORY_ERROR OR
		BR_OBJECTS_VERIFIED_ERROR OR NOT BR_INVENTORY_PASSED OR NOT BR_INVENTORY_OBJECTS_VERIFIED)
	message(FATAL_ERROR "Lift-object inventory manifest is incomplete or its source/object validation did not pass.")
endif()
if(NOT BR_MAIN_LIFT_COUNT EQUAL BR_EXPECTED_MAIN_LIFT_COUNT OR
		NOT BR_SCRIPT_LIFT_COUNT EQUAL BR_EXPECTED_SCRIPT_LIFT_COUNT)
	message(FATAL_ERROR "Lift-object inventory mismatch: manifest expects ${BR_EXPECTED_MAIN_LIFT_COUNT} main and ${BR_EXPECTED_SCRIPT_LIFT_COUNT} script/runtime-root objects; CMake found ${BR_MAIN_LIFT_COUNT} and ${BR_SCRIPT_LIFT_COUNT}.")
endif()
if(NOT EXISTS "${BR_COMBINED_TABLE_OBJECT}")
	message(FATAL_ERROR "Combined 97,602-entry ARM64 lift table object is missing: ${BR_COMBINED_TABLE_OBJECT}")
endif()
set(BR_LIFT_OBJECTS ${BR_MAIN_LIFT_OBJECTS} ${BR_SCRIPT_LIFT_OBJECTS} "${BR_COMBINED_TABLE_OBJECT}")
set_source_files_properties(${BR_LIFT_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_library(brnative SHARED brnative.cpp "${BR_ROOT}/runtime/android_diagnostics.cpp"
	"${BR_ROOT}/runtime/android_touch_gestures.cpp" ${GLUE}/android_native_app_glue.c ${BR_LIFT_OBJECTS})
target_include_directories(brnative PRIVATE ${GLUE} "${BR_ROOT}/references/RA3-Native/runtime")
target_link_libraries(brnative PRIVATE br_game_runtime br_win32_startup br_win32_file br_win32_memory br_win32_sync br_win32_base br_lift_dispatch aaudio android log)
target_compile_options(brnative PRIVATE -O2 -fno-strict-aliasing -fno-emulated-tls)
target_link_options(brnative PRIVATE "-Wl,--no-undefined" "-Wl,-z,max-page-size=16384")

# Lifted call tracing and its sampler distort the game loop measurably. Keep
# profiling available for deliberate diagnostics, but leave it out of normal
# Android builds so benchmark APKs reflect actual gameplay performance.
if(BR_ANDROID_GUEST_PROFILE)
	target_compile_definitions(brnative PRIVATE BR_ANDROID_GUEST_PROFILE=1)
endif()
