# readyup-mapvote -> build/plugins/mapvote.so (csgo/readyup/plugins/mapvote.so): .rtv, .nominate,
# an end-of-map vote and .timeleft for community servers (movement, deathmatch, the skins tryout
# server, practice). Off while the match plugin runs a match. Loads maps through essentials
# (readyup.essentials.v1 load_map).
readyup_add_plugin(mapvote mapvote_plugin.cpp mapvote_rules.cpp)
target_compile_definitions(readyup_plugin_mapvote PRIVATE MAPVOTE_VERSION="${READYUP_SEMVER}")

# Map list parsing, lookup, rtv threshold, ballots, the winner, the clock (mapvote_rules.h;
# ctest `mapvote_rules`).
add_executable(mapvote_rules_test tests/mapvote_rules_test.cpp mapvote_rules.cpp)
target_include_directories(mapvote_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mapvote_rules_test PRIVATE -Wall -Wextra)
add_test(NAME mapvote_rules COMMAND mapvote_rules_test)
