# readyup-tv -> build/plugins/tv.so (csgo/readyup/plugins/tv.so): the server's GOTV broadcast
# settings (delay, client limit, chat / voice relay, insecure observers for HUD tools), kept for a
# whole series and per match from match.assign rules.broadcast; casters through whitelist.so.
# Design: docs/design/tv-plugin.md.
readyup_add_plugin(tv tv_plugin.cpp tv_rules.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/broadcast_profile.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(readyup_plugin_tv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_tv PRIVATE TV_VERSION="${READYUP_SEMVER}")

# The profile + match.assign validator, applying / restoring, the active match, settings,
# tv_status lines, tvState (tv_rules.h, libs/readyup/broadcast_profile.h; ctest `tv_rules`).
add_executable(tv_rules_test tests/tv_rules_test.cpp tv_rules.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/broadcast_profile.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(tv_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(tv_rules_test PRIVATE -Wall -Wextra)
add_test(NAME tv_rules COMMAND tv_rules_test)
