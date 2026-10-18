# readyup-movement -> build/plugins/movement.so (csgo/readyup/plugins/movement.so): surf, bhop and
# kz on community servers: the mode's physics, a run timer between the map's start and end zones,
# personal bests and top times (records.json). Off while a match is loaded.
readyup_add_plugin(movement
  movement_plugin.cpp
  movement_rules.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/json_store.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(readyup_plugin_movement PRIVATE ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_movement PRIVATE MOVEMENT_VERSION="${READYUP_SEMVER}")

# Modes, cvars, zone names, boxes, times, records (movement_rules.h; ctest `movement_rules`).
add_executable(movement_rules_test tests/movement_rules_test.cpp movement_rules.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(movement_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(movement_rules_test PRIVATE -Wall -Wextra)
add_test(NAME movement_rules COMMAND movement_rules_test)
