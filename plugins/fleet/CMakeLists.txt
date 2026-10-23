# readyup-fleet -> build/plugins/fleet.so (csgo/readyup/plugins/fleet.so): the link to the Auto
# Tournament platform (docs/FLEET.md). Needs libcurl with WebSocket support plus OpenSSL:
#   - release (READYUP_DEPS_PREFIX): the static curl 8.22 / OpenSSL from scripts/ci/build-static-deps.sh
#   - dev Docker image (docker/build.Dockerfile): static curl 8.22 in /opt/curl-ws + system OpenSSL
#   - otherwise a system libcurl >= 8.11 (WebSockets on by default)
# Without one the plugin is skipped with a warning; the unit tests and the mock platform still build.

# Everything without network I/O: JSON, envelope, backoff, seq/ack, spool, credentials.
add_library(readyup_fleet_core STATIC fleet_json.cpp fleet_proto.cpp fleet_spool.cpp fleet_store.cpp fleet_demo.cpp
  fleet_cmds.cpp)
set_target_properties(readyup_fleet_core PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(readyup_fleet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(readyup_fleet_core PRIVATE -Wall -Wextra $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)

add_executable(fleet_unit_test tests/fleet_unit_test.cpp)
target_link_libraries(fleet_unit_test PRIVATE readyup_fleet_core)
target_compile_options(fleet_unit_test PRIVATE -Wall -Wextra)
add_test(NAME fleet_unit COMMAND fleet_unit_test)
# Demo streaming (fleet_demo.h, FLEET.md §12.2): tailer, ack / resume, backpressure, no network.
add_executable(fleet_demo_test tests/fleet_demo_test.cpp)
target_link_libraries(fleet_demo_test PRIVATE readyup_fleet_core)
target_compile_options(fleet_demo_test PRIVATE -Wall -Wextra)
add_test(NAME fleet_demo COMMAND fleet_demo_test)

# Mock platform (enroll + WebSocket) for the integration test and for trying fleet.so by hand.
add_library(readyup_fleet_mock STATIC tests/mock_platform.cpp)
target_link_libraries(readyup_fleet_mock PUBLIC readyup_fleet_core pthread)
target_compile_options(readyup_fleet_mock PRIVATE -Wall -Wextra)
add_executable(fleet_mock_platform tests/mock_platform_main.cpp)
target_link_libraries(fleet_mock_platform PRIVATE readyup_fleet_mock)

# The protocol schemas (incl. the proposed step-3 ones) against the example frames in
# protocol/examples/v1 (ctest `fleet_protocol`). No network, no libcurl.
add_executable(fleet_protocol_test tests/fleet_protocol_test.cpp tests/schema_check.cpp)
target_link_libraries(fleet_protocol_test PRIVATE readyup_fleet_core)
target_compile_definitions(fleet_protocol_test PRIVATE FLEET_PROTOCOL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/protocol/v1"
  FLEET_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/protocol/examples/v1")
target_compile_options(fleet_protocol_test PRIVATE -Wall -Wextra)
add_test(NAME fleet_protocol COMMAND fleet_protocol_test)

set(FLEET_CURL_OK OFF)
set(FLEET_CURL_INC "")
set(FLEET_CURL_LIBS "")
set(FLEET_CURL_DEFS "")
if (READYUP_DEPS_PREFIX)
  set(FLEET_CURL_OK ON)
  set(FLEET_CURL_INC "${READYUP_DEPS_PREFIX}/include")
  set(FLEET_CURL_LIBS "${READYUP_DEPS_PREFIX}/lib/libcurl.a" "${READYUP_DEPS_PREFIX}/lib/libssl.a"
    "${READYUP_DEPS_PREFIX}/lib/libcrypto.a" pthread dl)
  set(FLEET_CURL_DEFS CURL_STATICLIB FLEET_CA_PROBE=1)
  set(FLEET_CURL_WHAT "static libcurl from ${READYUP_DEPS_PREFIX}")
else()
  set(READYUP_FLEET_CURL_PREFIX "/opt/curl-ws" CACHE PATH
    "Prefix with a static libcurl built with WebSockets (dev builds; see docker/build.Dockerfile)")
  find_package(OpenSSL QUIET)
  if (EXISTS "${READYUP_FLEET_CURL_PREFIX}/lib/libcurl.a" AND OpenSSL_FOUND)
    set(FLEET_CURL_OK ON)
    set(FLEET_CURL_INC "${READYUP_FLEET_CURL_PREFIX}/include")
    set(FLEET_CURL_LIBS "${READYUP_FLEET_CURL_PREFIX}/lib/libcurl.a" OpenSSL::SSL OpenSSL::Crypto pthread dl)
    set(FLEET_CURL_DEFS CURL_STATICLIB)
    set(FLEET_CURL_WHAT "static libcurl from ${READYUP_FLEET_CURL_PREFIX}")
  elseif (CURL_FOUND AND CURL_VERSION_STRING VERSION_GREATER_EQUAL "8.11")
    set(FLEET_CURL_OK ON)
    if (TARGET CURL::libcurl)
      set(FLEET_CURL_LIBS CURL::libcurl pthread)
    else()
      set(FLEET_CURL_INC ${CURL_INCLUDE_DIRS})
      set(FLEET_CURL_LIBS ${CURL_LIBRARIES} pthread)
    endif()
    set(FLEET_CURL_WHAT "system libcurl ${CURL_VERSION_STRING}")
  endif()
endif()

if (NOT FLEET_CURL_OK)
  message(WARNING "fleet.so skipped: no libcurl with WebSocket support (need READYUP_DEPS_PREFIX, "
    "a static curl in READYUP_FLEET_CURL_PREFIX=${READYUP_FLEET_CURL_PREFIX} + OpenSSL, or libcurl >= 8.11). "
    "Use scripts/docker-build.sh or scripts/sniper-build.sh.")
  return()
endif()
message(STATUS "fleet.so: ${FLEET_CURL_WHAT}")

# The license check-in (match/readyup/license_checkin.h) runs here too: practice and fun servers have no match.so.
readyup_add_plugin(fleet fleet_plugin.cpp fleet_client.cpp ${CMAKE_SOURCE_DIR}/plugins/match/readyup/license_checkin.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(readyup_plugin_fleet PRIVATE ${CMAKE_SOURCE_DIR}/plugins/match ${CMAKE_SOURCE_DIR}/libs)
target_link_libraries(readyup_plugin_fleet PRIVATE readyup_fleet_core ${FLEET_CURL_LIBS})
target_include_directories(readyup_plugin_fleet PRIVATE ${FLEET_CURL_INC})
target_compile_definitions(readyup_plugin_fleet PRIVATE FLEET_VERSION="${READYUP_SEMVER}" ${FLEET_CURL_DEFS})
target_compile_options(readyup_plugin_fleet PRIVATE -Wpedantic)

add_executable(fleet_integration_test tests/fleet_integration_test.cpp tests/schema_check.cpp fleet_client.cpp)
target_link_libraries(fleet_integration_test PRIVATE readyup_fleet_mock ${FLEET_CURL_LIBS})
target_include_directories(fleet_integration_test PRIVATE ${FLEET_CURL_INC})
# The platform's protocol schemas (copied from the platform repo, FLEET.md D18): every frame the
# integration test sees fleet.so send is validated against them.
target_compile_definitions(fleet_integration_test PRIVATE ${FLEET_CURL_DEFS}
  FLEET_PROTOCOL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/protocol/v1")
target_compile_options(fleet_integration_test PRIVATE -Wall -Wextra)
add_test(NAME fleet_integration COMMAND fleet_integration_test)
set_tests_properties(fleet_integration PROPERTIES TIMEOUT 240)

# fleet.so inside the real plugin host (core plugin_loader.cpp + stubs) against the mock platform:
# standalone idle, then enroll + connect, selftest lines, commands, unload joins the thread.
add_executable(fleet_host_test tests/fleet_host_test.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_loader.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/perf_stats.cpp
  ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_state.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_needs.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(fleet_host_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(fleet_host_test PRIVATE -Wall -Wextra)
target_link_libraries(fleet_host_test PRIVATE readyup_fleet_mock dl)
add_test(NAME fleet_host COMMAND fleet_host_test $<TARGET_FILE:readyup_plugin_fleet>)
set_tests_properties(fleet_host PROPERTIES TIMEOUT 120)
