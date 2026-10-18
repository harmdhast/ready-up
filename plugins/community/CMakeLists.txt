# readyup-community -> build/plugins/community.so (csgo/readyup/plugins/community.so): the classic
# community-server commands (.rank, .top, .speclist, admin .kick). Stats per server in
# csgo/readyup/plugins/community/stats.json, counted while no match is loaded.
readyup_add_plugin(community
  community_plugin.cpp
  community_rules.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/json_store.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(readyup_plugin_community PRIVATE ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_community PRIVATE COMMUNITY_VERSION="${READYUP_SEMVER}")

# Points, ranking, stats.json round trip, name lookup (community_rules.h; ctest `community_rules`).
add_executable(community_rules_test tests/community_rules_test.cpp community_rules.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(community_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(community_rules_test PRIVATE -Wall -Wextra)
add_test(NAME community_rules COMMAND community_rules_test)
