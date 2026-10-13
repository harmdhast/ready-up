cmake_minimum_required(VERSION 3.20)
project(readyup_shim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Embed gamedata/engine-surface.json into the shim (runtime prefers an on-disk copy next to
# the shim if one exists). Re-run configure whenever the JSON changes.
set(READYUP_ENGINE_SURFACE_JSON "${CMAKE_SOURCE_DIR}/gamedata/engine-surface.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${READYUP_ENGINE_SURFACE_JSON}")
file(READ "${READYUP_ENGINE_SURFACE_JSON}" READYUP_ENGINE_SURFACE_JSON_TEXT)
set(READYUP_ENGINE_SURFACE_CPP "${CMAKE_BINARY_DIR}/generated/engine_surface_embedded.cpp")
file(WRITE "${READYUP_ENGINE_SURFACE_CPP}.tmp"
  "// Generated from gamedata/engine-surface.json by CMake. Do not edit.\n"
  "extern const char kReadyUpEmbeddedEngineSurfaceJson[];\n"
  "const char kReadyUpEmbeddedEngineSurfaceJson[] = R\"RUJSON(${READYUP_ENGINE_SURFACE_JSON_TEXT})RUJSON\";\n")
configure_file("${READYUP_ENGINE_SURFACE_CPP}.tmp" "${READYUP_ENGINE_SURFACE_CPP}" COPYONLY)
set_source_files_properties("${READYUP_ENGINE_SURFACE_CPP}" PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings")

add_library(server_shim SHARED
  core/src/readyup/admin_check.cpp
  core/src/readyup/banner.cpp
  core/src/readyup/ccommand.cpp
  core/src/readyup/chat.cpp
  core/src/readyup/center_html.cpp
  core/src/readyup/cs2_version.cpp
  core/src/readyup/client_print.cpp
  core/src/readyup/client_command_hook.cpp
  core/src/readyup/command_buffer_hook.cpp
  core/src/readyup/config.cpp
  core/src/readyup/console_command.cpp
  core/src/readyup/crash_handler.cpp
  core/src/readyup/disabled.cpp
  core/src/readyup/features.cpp
  core/src/readyup/license.cpp
  core/src/readyup/license_keys.cpp
  core/src/readyup/license_status.cpp
  core/src/readyup/game_frame_hook.cpp
  core/src/readyup/game_events.cpp
  core/src/readyup/host_say_hook.cpp
  core/src/readyup/plugin_hooks.cpp
  core/src/readyup/log_receiver.cpp
  core/src/readyup/logging.cpp
  libs/readyup/minijson.cpp
  core/src/readyup/path.cpp
  core/src/readyup/load_order.cpp
  core/src/readyup/load_order_check.cpp
  core/src/readyup/real_server.cpp
  core/src/readyup/round_termination_hook.cpp
  core/src/readyup/ru_help.cpp
  core/src/readyup/ru_help_text.cpp
  core/src/readyup/ru_router.cpp
  core/src/readyup/perf_stats.cpp
  core/src/readyup/center_panel_owner.cpp
  core/src/readyup/steam_ugc.cpp
  core/src/readyup/server_game_clients_hook.cpp
  core/src/readyup/schema.cpp
  core/src/readyup/selftest.cpp
  core/src/readyup/status_feed.cpp
  core/src/readyup/status_http.cpp
  core/src/readyup/status_server.cpp
  libs/readyup/status_snapshot.cpp
  core/src/readyup/signature_scan.cpp
  core/src/readyup/sigtest.cpp
  core/src/readyup/slot_registry.cpp
  libs/readyup/steamid.cpp
  core/src/readyup/entity.cpp
  core/src/readyup/grenades.cpp
  core/src/readyup/practice_engine.cpp
  core/src/readyup/plugin_loader.cpp
  core/src/readyup/plugin_needs.cpp
  core/src/readyup/plugin_needs_engine.cpp
  core/src/readyup/plugin_state.cpp
  core/src/readyup/plugin_engine_api.cpp
  core/src/readyup/engine_surface.cpp
  core/src/readyup/engine_surface_core.cpp
  ${READYUP_ENGINE_SURFACE_CPP}
  # core/src/exports.cpp is added below and MUST stay last: its __attribute__((constructor))
  # (readyup_ctor) is placed in .init_array in link order, and it must run AFTER every other
  # TU's static initializers. When it was first, later initializers re-constructed globals the
  # constructor had already filled in (e.g. the loaded gamedata string, the "disabled" flag).
  # scripts/check_shim_binary.sh verifies this.
)

# Vendored upstream funchook v1.1.3 + diStorm 3.5.2 (instruction-relocating detours).
# Built as a separate static PIC library so our strict warning flags don't apply to third-party code.
add_library(readyup_funchook STATIC
  third_party/funchook/src/funchook.c
  third_party/funchook/src/funchook_unix.c
  third_party/funchook/src/funchook_x86.c
  third_party/funchook/src/disasm_distorm.c
  third_party/distorm/src/decoder.c
  third_party/distorm/src/distorm.c
  third_party/distorm/src/instructions.c
  third_party/distorm/src/insts.c
  third_party/distorm/src/mnemonics.c
  third_party/distorm/src/operands.c
  third_party/distorm/src/prefix.c
  third_party/distorm/src/textdefs.c
)
set_target_properties(readyup_funchook PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(readyup_funchook
  PUBLIC ${CMAKE_SOURCE_DIR}/third_party/funchook/include
  PRIVATE ${CMAKE_SOURCE_DIR}/third_party/funchook/config
          ${CMAKE_SOURCE_DIR}/third_party/distorm/include)
target_compile_options(readyup_funchook PRIVATE -w)
target_link_libraries(readyup_funchook PRIVATE dl)

# Vendored Monocypher 4.0.3 (Ed25519 signature check for license keys, core/src/readyup/license.h).
# Its own static PIC library, like funchook, so our warning flags don't apply to it.
add_library(readyup_monocypher STATIC
  third_party/monocypher/src/monocypher.c
  third_party/monocypher/src/optional/monocypher-ed25519.c
)
set_target_properties(readyup_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(readyup_monocypher PUBLIC ${CMAKE_SOURCE_DIR}/third_party/monocypher/src)
target_compile_options(readyup_monocypher PRIVATE -w)

# Build/version strings for `ru`.
# SemVer is sourced from VERSION file, git SHA is appended as build metadata.
set(READYUP_SEMVER "unknown")
if (EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
  file(READ "${CMAKE_SOURCE_DIR}/VERSION" READYUP_SEMVER)
  string(STRIP "${READYUP_SEMVER}" READYUP_SEMVER)
endif()

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE READYUP_GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if (NOT READYUP_GIT_SHA)
  set(READYUP_GIT_SHA "unknown")
endif()

# "<semver> (<sha>)", e.g. "0.1.0 (a1b2c3d)" or "0.1.0-beta.1 (a1b2c3d)": what `ru version`, the
# crash log and hello.versions.core (docs/FLEET.md "Version strings") report. The platform parses
# it, so VERSION must stay SemVer: X.Y.Z with an optional -pre-release suffix (release.sh).
if (NOT READYUP_SEMVER MATCHES "^(unknown|[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?)$")
  message(FATAL_ERROR "VERSION must be X.Y.Z or X.Y.Z-<pre-release> (got \"${READYUP_SEMVER}\")")
endif()
set(READYUP_BUILD_VERSION "${READYUP_SEMVER} (${READYUP_GIT_SHA})")

# License coverage is by version line (core/src/readyup/license.h): the release date of this
# build's x.y.0. The release workflow passes it for tags (.github/workflows/build.yml); a new
# x.y.0, dev and local builds use the build date.
set(READYUP_LINE_DATE "" CACHE STRING "Release date of this build's x.y.0 (YYYY-MM-DD); empty = build date")
if (READYUP_LINE_DATE)
  set(READYUP_LINE_DATE_VALUE "${READYUP_LINE_DATE}")
else()
  string(TIMESTAMP READYUP_LINE_DATE_VALUE "%Y-%m-%d" UTC)
endif()
if (NOT READYUP_LINE_DATE_VALUE MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$")
  message(FATAL_ERROR "READYUP_LINE_DATE must be YYYY-MM-DD (got \"${READYUP_LINE_DATE_VALUE}\")")
endif()
message(STATUS "License line date: ${READYUP_LINE_DATE_VALUE}")

target_compile_definitions(server_shim PRIVATE
  READYUP_SEMVER="${READYUP_SEMVER}"
  READYUP_BUILD_VERSION="${READYUP_BUILD_VERSION}"
  READYUP_LINE_DATE="${READYUP_LINE_DATE_VALUE}"
)

# Produce the artifact name the engine expects.
# No DT_SONAME: Valve's library is "libserver.so" by soname, and a bare dlopen("libserver.so")
# (CounterStrikeSharp does one per module) must resolve to Valve's, not to this shim, which is
# loaded first under Metamod.
set_target_properties(server_shim PROPERTIES
  OUTPUT_NAME "libserver"
  PREFIX ""
  NO_SONAME ON
)

target_compile_options(server_shim PRIVATE
  -fvisibility=hidden
  -Wall
  -Wextra
  -Wpedantic
)

target_include_directories(server_shim PRIVATE
  ${CMAKE_SOURCE_DIR}/core/src
  ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include
  ${CMAKE_SOURCE_DIR}  # "third_party/funchook/include/funchook.h"
)

# READYUP_DEPS_PREFIX: release builds (scripts/sniper-build.sh, CI). Points at a
# prefix produced by scripts/ci/build-static-deps.sh holding static PIC OpenSSL
# and libcurl for the plugins. Everything (plus libstdc++/libgcc) is linked in and
# hidden, so the shim and the plugins only need glibc (<= 2.31, Steam Runtime sniper).
set(READYUP_DEPS_PREFIX "" CACHE PATH "Prefix with static OpenSSL/libcurl (release builds)")

if (READYUP_DEPS_PREFIX)
  foreach(lib curl ssl crypto)
    if (NOT EXISTS "${READYUP_DEPS_PREFIX}/lib/lib${lib}.a")
      message(FATAL_ERROR "READYUP_DEPS_PREFIX: missing ${READYUP_DEPS_PREFIX}/lib/lib${lib}.a (run scripts/ci/build-static-deps.sh)")
    endif()
  endforeach()
  target_link_libraries(server_shim PRIVATE pthread dl m)
  # libstdc++ is linked statically by hand (see the end of this file) instead of via
  # -static-libstdc++, because the driver appends that after every object and its static
  # initializers would then run after readyup_ctor.
  # The dynamic symbol table is pinned by core/src/exports.map (the engine entry points only);
  # everything else, including the static deps and std:: template instances, stays local
  # so nothing interposes on the engine's own copies. (--exclude-libs,ALL would also hide
  # exports.cpp, which is linked from an archive below.)
  target_link_options(server_shim PRIVATE
    -nostdlib++ -static-libgcc
    "LINKER:--version-script=${CMAKE_SOURCE_DIR}/core/src/exports.map"
    "LINKER:--no-undefined")
  set_property(TARGET server_shim APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/core/src/exports.map")
  message(STATUS "Static release deps (for the plugins) from ${READYUP_DEPS_PREFIX}")
else()
  # For the plugins (plugins/match, plugins/fleet).
  find_package(CURL QUIET)
endif()

target_link_libraries(server_shim PRIVATE readyup_funchook readyup_monocypher dl)

# readyup_ctor's translation unit goes last on the link line (see the source list above).
if (READYUP_DEPS_PREFIX)
  # Static release build: libstdc++.a's own initializers must also precede readyup_ctor, so
  # exports.cpp is built as a one-member archive and linked whole-archive AFTER libstdc++.a
  # (plain objects always precede libraries on the link line). libstdc++.a is listed again
  # after it for anything only exports.o pulls in; the post-build check fails if that ever
  # drags an initializer in behind readyup_ctor.
  if (CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "READYUP_DEPS_PREFIX builds need CMake >= 3.24 ($<LINK_LIBRARY:WHOLE_ARCHIVE>)")
  endif()
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a
    OUTPUT_VARIABLE READYUP_LIBSTDCXX_A OUTPUT_STRIP_TRAILING_WHITESPACE)
  if (NOT IS_ABSOLUTE "${READYUP_LIBSTDCXX_A}" OR NOT EXISTS "${READYUP_LIBSTDCXX_A}")
    message(FATAL_ERROR "static libstdc++.a not found for ${CMAKE_CXX_COMPILER}")
  endif()
  add_library(readyup_exports STATIC core/src/exports.cpp)
  foreach(prop COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS)
    set_property(TARGET readyup_exports PROPERTY ${prop} "$<TARGET_PROPERTY:server_shim,${prop}>")
  endforeach()
  set_target_properties(readyup_exports PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(server_shim PRIVATE
    "${READYUP_LIBSTDCXX_A}" "$<LINK_LIBRARY:WHOLE_ARCHIVE,readyup_exports>" "${READYUP_LIBSTDCXX_A}" m)
else()
  target_sources(server_shim PRIVATE core/src/exports.cpp)
endif()

# Post-build guard: fail the build if the shim contains direct calls to __cxa_pure_virtual
# (devirtualized calls through internal-linkage interface mirrors) or if readyup_ctor is not
# the last .init_array entry. Skips itself if binutils are unavailable.
add_custom_command(TARGET server_shim POST_BUILD
  COMMAND sh "${CMAKE_SOURCE_DIR}/scripts/check_shim_binary.sh" "$<TARGET_FILE:server_shim>"
  VERBATIM)

# Offline checker: verifies gamedata/engine-surface.json against a Valve libserver.so on disk
# using the same code the shim runs at load. Usage:
#   build/readyup_sigcheck <path/to/libserver.so> gamedata/engine-surface.json
add_executable(readyup_sigcheck
  tools/sigcheck.cpp
  core/src/readyup/engine_surface_core.cpp
  libs/readyup/minijson.cpp
)
target_include_directories(readyup_sigcheck PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_sigcheck PRIVATE -Wall -Wextra)

# Offline hook-site checker: for entries marked "hook": "funchook", resolves the function and
# runs funchook_prepare (prologue decode + trampoline relocation, no install) against a
# mapped copy of Valve's libserver.so. Usage:
#   build/readyup_hookcheck <path/to/libserver.so> gamedata/engine-surface.json
add_executable(readyup_hookcheck
  tools/hookcheck.cpp
  core/src/readyup/engine_surface_core.cpp
  libs/readyup/minijson.cpp
)
target_include_directories(readyup_hookcheck PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs ${CMAKE_SOURCE_DIR})
target_compile_options(readyup_hookcheck PRIVATE -Wall -Wextra)
target_link_libraries(readyup_hookcheck PRIVATE readyup_funchook)

# ---- Plugins (docs/ARCHITECTURE.md) -----------------------------------------------
# The public C ABI. Plugins link only this: no core sources, no engine SDK mirrors, no
# funchook, no gamedata. Anything engine-facing is a function pointer in ru_api.
add_library(readyup_plugin_api INTERFACE)
target_include_directories(readyup_plugin_api INTERFACE ${CMAKE_SOURCE_DIR}/core/include)

# readyup_add_plugin(<name> <sources...>)
#   -> target readyup_plugin_<name>, artifact ${CMAKE_BINARY_DIR}/plugins/<name>.so
#   (deployed to csgo/readyup/plugins/<name>.so). Build one plugin with:
#   cmake --build build --target readyup_plugin_<name>
function(readyup_add_plugin name)
  set(tgt readyup_plugin_${name})
  add_library(${tgt} MODULE ${ARGN})
  target_link_libraries(${tgt} PRIVATE readyup_plugin_api)
  target_compile_definitions(${tgt} PRIVATE READYUP_SEMVER="${READYUP_SEMVER}")
  set_target_properties(${tgt} PROPERTIES
    OUTPUT_NAME "${name}"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_compile_options(${tgt} PRIVATE -Wall -Wextra
    # STB_GNU_UNIQUE symbols make dlclose a no-op, which would break hot reload.
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-fno-gnu-unique>)
  # Everything a plugin needs from the core arrives through ru_api; an unresolved symbol
  # at link time means it reached for core internals.
  # Export exactly the three entry points; static deps (OpenSSL, libcurl, libstdc++ in release
  # builds) stay local so they never interpose on the engine's or the core's copies.
  target_link_options(${tgt} PRIVATE "LINKER:--no-undefined" "LINKER:--exclude-libs,ALL"
    "LINKER:--version-script=${CMAKE_SOURCE_DIR}/plugins/plugin.map")
  set_property(TARGET ${tgt} APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/plugins/plugin.map")
  if (READYUP_DEPS_PREFIX)
    # Release (sniper) builds: the host only guarantees glibc (scripts/ci/check-portable.sh).
    target_link_options(${tgt} PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endfunction()

add_subdirectory(plugins/hello)
add_subdirectory(plugins/skins)

# Offline plugin-host test (no CS2 server): the real loader + stubbed core functions,
# loading hello.so, then hot-reloading a second build of it with a different version.
#   cmake --build build && (cd build && ctest --output-on-failure)
readyup_add_plugin(hello_reload_test plugins/hello/hello.c)
set_target_properties(readyup_plugin_hello_reload_test PROPERTIES
  OUTPUT_NAME "hello"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
target_compile_definitions(readyup_plugin_hello_reload_test PRIVATE HELLO_VERSION="1.0.1-reloaded")
target_link_libraries(readyup_plugin_hello_reload_test PRIVATE m)  # hello_nade (cos/sin)

add_executable(readyup_plugin_host_test tools/plugin_host_test.cpp core/src/readyup/plugin_loader.cpp core/src/readyup/perf_stats.cpp
  core/src/readyup/plugin_state.cpp core/src/readyup/plugin_needs.cpp libs/readyup/minijson.cpp)
target_include_directories(readyup_plugin_host_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(readyup_plugin_host_test PRIVATE -Wall -Wextra)
target_link_libraries(readyup_plugin_host_test PRIVATE dl)

# Offline unit tests for the match stats model, map/series-end logic, demo naming and upload
# headers, and the AT webhook payloads (at_payloads.h) (engine-free sources only).
add_executable(readyup_match_flow_test
  tests/match_flow_test.cpp
  plugins/match/readyup/match_stats.cpp
  plugins/match/readyup/match_end_logic.cpp
  plugins/match/readyup/demo_naming.cpp
  plugins/match/readyup/at_payloads.cpp
  libs/readyup/minijson.cpp
  libs/readyup/status_snapshot.cpp
)
target_include_directories(readyup_match_flow_test PRIVATE ${CMAKE_SOURCE_DIR}/plugins/match ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(readyup_match_flow_test PRIVATE -Wall -Wextra)

# Offline tests for the local status endpoint (docs/FLEET.md §17): HTTP parsing, routing,
# auth, limits, SSE framing and merge patches, against a real server on an ephemeral port.
add_executable(readyup_status_http_test
  tests/status_http_test.cpp
  core/src/readyup/status_http.cpp
  core/src/readyup/status_server.cpp
  libs/readyup/status_snapshot.cpp
  libs/readyup/minijson.cpp
)
target_include_directories(readyup_status_http_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_status_http_test PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(readyup_status_http_test PRIVATE Threads::Threads)

# Versioned JSON files that replaced Postgres (docs/FLEET.md D13): atomic save, corrupt files.
add_executable(readyup_json_store_test tests/json_store_test.cpp libs/readyup/json_store.cpp libs/readyup/status_snapshot.cpp)
target_include_directories(readyup_json_store_test PRIVATE ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_json_store_test PRIVATE -Wall -Wextra)

# Running next to Metamod (docs/COMPATIBILITY.md): module-path canonicalization, Valve vs
# Metamod server library, gameinfo.gi order check, one shim per process.
add_executable(readyup_load_order_test tests/load_order_test.cpp core/src/readyup/load_order.cpp core/src/readyup/path.cpp)
target_include_directories(readyup_load_order_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_compile_options(readyup_load_order_test PRIVATE -Wall -Wextra)
target_link_libraries(readyup_load_order_test PRIVATE dl)

# License keys (core/src/readyup/license.h) against tests/fixtures/license/vectors.json.
add_executable(readyup_license_test tests/license_test.cpp core/src/readyup/license.cpp
  core/src/readyup/license_keys.cpp libs/readyup/minijson.cpp)
target_include_directories(readyup_license_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_license_test PRIVATE -Wall -Wextra)
target_compile_definitions(readyup_license_test PRIVATE
  READYUP_LICENSE_VECTORS="${CMAKE_SOURCE_DIR}/tests/fixtures/license/vectors.json")
target_link_libraries(readyup_license_test PRIVATE readyup_monocypher)
# readyup_license_key / readyup_show_license and csm's readyup_license.cfg (license_status.h).
add_executable(readyup_license_status_test tests/license_status_test.cpp core/src/readyup/license_status.cpp
  core/src/readyup/license.cpp core/src/readyup/license_keys.cpp libs/readyup/minijson.cpp)
target_include_directories(readyup_license_status_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_license_status_test PRIVATE -Wall -Wextra)
target_compile_definitions(readyup_license_status_test PRIVATE READYUP_LINE_DATE="2026-09-28")
target_link_libraries(readyup_license_status_test PRIVATE readyup_monocypher)

# `ru help` text (core/src/readyup/ru_help_text.h).
add_executable(readyup_ru_help_test tests/ru_help_test.cpp core/src/readyup/ru_help_text.cpp)
target_include_directories(readyup_ru_help_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_compile_options(readyup_ru_help_test PRIVATE -Wall -Wextra)

# Human presence registry (core/src/readyup/slot_registry.h).
add_executable(readyup_slot_registry_test tests/slot_registry_test.cpp core/src/readyup/slot_registry.cpp)
target_include_directories(readyup_slot_registry_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_compile_options(readyup_slot_registry_test PRIVATE -Wall -Wextra)

# Plugin time per server frame, `ru perf` (core/src/readyup/perf_stats.h).
add_executable(readyup_perf_stats_test tests/perf_stats_test.cpp core/src/readyup/perf_stats.cpp)
target_include_directories(readyup_perf_stats_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_compile_options(readyup_perf_stats_test PRIVATE -Wall -Wextra)
add_test(NAME perf_stats COMMAND readyup_perf_stats_test)

# ru_api grenade_spawn type table and request checks (core/src/readyup/grenade_spec.h).
add_executable(readyup_grenade_spec_test tests/grenade_spec_test.cpp)
target_include_directories(readyup_grenade_spec_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(readyup_grenade_spec_test PRIVATE -Wall -Wextra)
add_test(NAME grenade_spec COMMAND readyup_grenade_spec_test)

# ru_api 1.13 (player_change_team, player_teleport, entity_create / entity_spawn) feature table and
# request checks (core/src/readyup/practice_engine_spec.h).
add_executable(readyup_practice_engine_spec_test tests/practice_engine_spec_test.cpp)
target_include_directories(readyup_practice_engine_spec_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(readyup_practice_engine_spec_test PRIVATE -Wall -Wextra)
add_test(NAME practice_engine_spec COMMAND readyup_practice_engine_spec_test)

# One center panel per player (core/src/readyup/center_panel_owner.h).
add_executable(readyup_center_panel_owner_test tests/center_panel_owner_test.cpp core/src/readyup/center_panel_owner.cpp)
target_include_directories(readyup_center_panel_owner_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_compile_options(readyup_center_panel_owner_test PRIVATE -Wall -Wextra)

# `.skins reload` rules (plugins/skins/skins_reload.h).
add_executable(skins_reload_test plugins/skins/tests/skins_reload_test.cpp)
target_include_directories(skins_reload_test PRIVATE ${CMAKE_SOURCE_DIR}/plugins/skins)
target_compile_options(skins_reload_test PRIVATE -Wall -Wextra)

# Persistent plugin enable / disable (core/src/readyup/plugin_state.h).
add_executable(readyup_plugin_state_test tests/plugin_state_test.cpp core/src/readyup/plugin_state.cpp libs/readyup/minijson.cpp)
# needs.json -> load / disable decision (docs/CS2-COMPAT.md "Plugin needs").
add_executable(readyup_plugin_needs_test tests/plugin_needs_test.cpp core/src/readyup/plugin_needs.cpp libs/readyup/minijson.cpp)
target_include_directories(readyup_plugin_needs_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_plugin_needs_test PRIVATE -Wall -Wextra)
target_include_directories(readyup_plugin_state_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(readyup_plugin_state_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME center_panel_owner COMMAND readyup_center_panel_owner_test)
add_test(NAME skins_reload COMMAND skins_reload_test)
add_test(NAME plugin_state COMMAND readyup_plugin_state_test)
add_test(NAME plugin_needs COMMAND readyup_plugin_needs_test)
add_test(NAME ru_help COMMAND readyup_ru_help_test)
add_test(NAME slot_registry COMMAND readyup_slot_registry_test)
add_test(NAME license COMMAND readyup_license_test)
add_test(NAME license_status COMMAND readyup_license_status_test)
add_test(NAME load_order COMMAND readyup_load_order_test)
add_test(NAME match_flow COMMAND readyup_match_flow_test)
add_test(NAME status_http COMMAND readyup_status_http_test)
add_test(NAME json_store COMMAND readyup_json_store_test)
add_test(NAME plugin_host_reload
  COMMAND readyup_plugin_host_test
    $<TARGET_FILE:readyup_plugin_hello> $<TARGET_FILE:readyup_plugin_hello_reload_test>)

# readyup-essentials (plugins/essentials): admins + map commands, plus its unit test.
add_subdirectory(plugins/essentials)

# readyup-practice (plugins/practice): practice mode + tools, plus its unit test.
add_subdirectory(plugins/practice)

# readyup-midas (plugins/midas): fun plugin, plus its unit test (after enable_testing()).
add_subdirectory(plugins/midas)

# readyup-whitelist (plugins/whitelist), plus its unit test (after enable_testing()).
add_subdirectory(plugins/whitelist)

# readyup-addons (plugins/addons): Steam Workshop addons, plus its unit test.
add_subdirectory(plugins/addons)

# readyup-deathmatch (plugins/deathmatch): FFA / team deathmatch, plus its unit test.
add_subdirectory(plugins/deathmatch)

# readyup-match (plugins/match): the match flow, plus its host test (after enable_testing()).
add_subdirectory(plugins/match)

# readyup-fleet (plugins/fleet): the Auto Tournament platform link, plus its unit and
# integration tests (after enable_testing() so ctest sees them).
add_subdirectory(plugins/fleet)
