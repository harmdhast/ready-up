# readyup-match -> build/plugins/match.so (csgo/readyup/plugins/match.so): the match flow
# (ready-up, scrims, knife, pauses, practice, match configs, webhooks, demos, stats).
# Talks to the core only through ru_api (core/include/readyup); engine-free shared code comes
# from libs/readyup. Needs libcurl (webhooks, `ru match load`, demo upload); without it those
# fall back to the `curl` binary. Settings, match recovery state and admins are JSON files in the
# plugin data dir (readyup/local_store.h, docs/FLEET.md D13).
set(MATCH_SOURCES
  readyup/admin_call.cpp
  readyup/admin_call_logic.cpp
  readyup/admin_check.cpp
  readyup/at_payloads.cpp
  readyup/backup_files.cpp
  readyup/card_html.cpp
  readyup/golive_card_logic.cpp
  readyup/coach.cpp
  readyup/coach_rules.cpp
  readyup/config.cpp
  readyup/cs2_update_check.cpp
  readyup/cs2_version.cpp
  readyup/cvar_snapshot.cpp
  readyup/cvar_snapshot_glue.cpp
  readyup/damage_ledger.cpp
  readyup/damage_report.cpp
  readyup/demo_naming.cpp
  readyup/demo_recorder.cpp
  readyup/esports.cpp
  readyup/fleet_bridge.cpp
  readyup/fleet_state.cpp
  readyup/game_timers.cpp
  readyup/golive_card.cpp
  readyup/host.cpp
  readyup/idle_refresh.cpp
  readyup/warmup_money.cpp
  readyup/weapon_cleanup.cpp
  readyup/hud_steady.cpp
  readyup/knife_tracker.cpp
  readyup/local_store.cpp
  readyup/ru_commands.cpp
  readyup/map_names.cpp
  readyup/mat_admins.cpp
  readyup/match_config_parser.cpp
  readyup/match_console.cpp
  readyup/match_end.cpp
  readyup/match_end_logic.cpp
  readyup/match_events.cpp
  readyup/match_features.cpp
  readyup/match_log.cpp
  readyup/match_plugin.cpp
  readyup/match_recovery.cpp
  readyup/match_rules.cpp
  readyup/match_router.cpp
  readyup/match_settings.cpp
  readyup/match_signals.cpp
  readyup/match_state.cpp
  readyup/match_stats.cpp
  readyup/match_status.cpp
  readyup/match_token.cpp
  readyup/modes.cpp
  readyup/pause_state.cpp
  readyup/persisted_match_state.cpp
  readyup/persisted_settings.cpp
  readyup/player_registry.cpp
  readyup/ready_hud.cpp
  readyup/reload_state.cpp
  readyup/retention.cpp
  readyup/retention_rules.cpp
  readyup/round_restore.cpp
  readyup/round_restore_rules.cpp
  readyup/ruleset.cpp
  readyup/scrim_flow.cpp
  readyup/server_settings.cpp
  readyup/simulation.cpp
  readyup/simulation_rules.cpp
  readyup/vote_logic.cpp
  readyup/votes.cpp
  readyup/webhook.cpp
  readyup/welcome.cpp
  readyup/wingman.cpp
  readyup/workers.cpp
  # Shared engine-free code (libs/readyup).
  ${CMAKE_SOURCE_DIR}/libs/readyup/http_client.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/json_store.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)

readyup_add_plugin(match ${MATCH_SOURCES})
# plugins/match first: its readyup/config.h, logging.h, ... are what libs/ sources include here.
target_include_directories(readyup_plugin_match BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_definitions(readyup_plugin_match PRIVATE MATCH_VERSION="${READYUP_SEMVER}")
target_link_libraries(readyup_plugin_match PRIVATE pthread dl)

# libcurl.
if (READYUP_DEPS_PREFIX)
  target_compile_definitions(readyup_plugin_match PRIVATE CURL_STATICLIB READYUP_CURL_CA_PROBE=1)
  target_link_libraries(readyup_plugin_match PRIVATE
    "${READYUP_DEPS_PREFIX}/lib/libcurl.a" "${READYUP_DEPS_PREFIX}/lib/libssl.a"
    "${READYUP_DEPS_PREFIX}/lib/libcrypto.a" m)
elseif (CURL_FOUND)
  if (TARGET CURL::libcurl)
    target_link_libraries(readyup_plugin_match PRIVATE CURL::libcurl)
  else()
    target_include_directories(readyup_plugin_match PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(readyup_plugin_match PRIVATE ${CURL_LIBRARIES})
  endif()
else()
  target_compile_definitions(readyup_plugin_match PRIVATE READYUP_NO_CURL=1)
  message(WARNING "libcurl dev files not found; readyup-match falls back to the `curl` binary at runtime.")
endif()

# match.so inside the real plugin host with a fake engine API: commands, readyup.match.v1,
# `ru plugin reload match` keeping the scrim / the loaded match, unload (ctest `match_host`).
add_executable(match_host_test tests/match_host_test.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_loader.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/perf_stats.cpp
  ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_state.cpp ${CMAKE_SOURCE_DIR}/core/src/readyup/plugin_needs.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(match_host_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_host_test PRIVATE -Wall -Wextra)
target_link_libraries(match_host_test PRIVATE dl pthread)
add_test(NAME match_host COMMAND match_host_test $<TARGET_FILE:readyup_plugin_match> $<TARGET_FILE:readyup_plugin_practice>
  $<TARGET_FILE:readyup_plugin_essentials> $<TARGET_FILE:readyup_plugin_deathmatch>)
set_tests_properties(match_host PROPERTIES TIMEOUT 120)

# The engine-free half of the fleet match link (readyup/fleet_state.h: merge patch + live_rev,
# epoch fence, config_rev CAS, match.assign -> MAT config through the real parser, update ops,
# sha256 / base64, validators, resume blocks), workshop map names (readyup/map_names.h) and the stats rewind
# of round restores (ctest `match_fleet_state`).
add_executable(match_fleet_state_test tests/fleet_state_test.cpp readyup/fleet_state.cpp readyup/map_names.cpp
  readyup/match_stats.cpp readyup/match_config_parser.cpp readyup/match_rules.cpp readyup/ruleset.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)
target_include_directories(match_fleet_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_fleet_state_test PRIVATE -Wall -Wextra)
add_test(NAME match_fleet_state COMMAND match_fleet_state_test)

# CS2 build check (Steam UpToDateCheck parsing, report-once rule; ctest `match_cs2_update`).
add_executable(match_cs2_update_test tests/cs2_update_check_test.cpp readyup/cs2_update_check.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp)
target_include_directories(match_cs2_update_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(match_cs2_update_test PRIVATE -Wall -Wextra)
add_test(NAME match_cs2_update COMMAND match_cs2_update_test)

# Pause limits / unpause rule / auto-unpause countdown, .forceready threshold, team name
# sanitizing, the team-left forfeit timer, warmup money and the warmup weapon cleanup (its cvars
# against cfg/ReadyUp/*.cfg) (readyup/match_rules.h, weapon_cleanup.h; ctest `match_rules`).
add_executable(match_rules_test tests/match_rules_test.cpp readyup/match_rules.cpp)
target_include_directories(match_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_rules_test PRIVATE -Wall -Wextra)
target_compile_definitions(match_rules_test PRIVATE READYUP_CFG_DIR="${CMAKE_SOURCE_DIR}/cfg/ReadyUp")
add_test(NAME match_rules COMMAND match_rules_test)

# Server settings (readyup/server_settings.h): the table, value checks, chat toggles, runtime >
# readyup.cfg > built-in, the persist hook, `.settings` lines, hostname_format; the ready gate with
# substitutes and playout (match_rules.h) (ctest `match_server_settings`).
add_executable(match_server_settings_test tests/server_settings_test.cpp readyup/server_settings.cpp
  readyup/match_rules.cpp)
target_include_directories(match_server_settings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_server_settings_test PRIVATE -Wall -Wextra)
target_link_libraries(match_server_settings_test PRIVATE pthread)
add_test(NAME match_server_settings COMMAND match_server_settings_test)

# Damage report bookkeeping (readyup/damage_ledger.h), the .gg / .stop vote logic
# (readyup/vote_logic.h) and their rules (ctest `match_damage_votes`).
add_executable(match_damage_votes_test tests/damage_votes_test.cpp readyup/damage_ledger.cpp readyup/vote_logic.cpp
  readyup/match_rules.cpp)
target_include_directories(match_damage_votes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_damage_votes_test PRIVATE -Wall -Wextra)
add_test(NAME match_damage_votes COMMAND match_damage_votes_test)

# Rulesets: presets, override parsing / validation, preset + overrides resolution, what differs,
# the commands after the go-live cfg, knife refusal (readyup/ruleset.h; ctest `match_ruleset`).
add_executable(match_ruleset_test tests/ruleset_test.cpp readyup/ruleset.cpp readyup/match_rules.cpp
  readyup/match_config_parser.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)
target_include_directories(match_ruleset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_ruleset_test PRIVATE -Wall -Wextra)
add_test(NAME match_ruleset COMMAND match_ruleset_test)

# Coaches: the policy, the side a coach watches, the admin command's player lookup, and the
# match config side (team coaches, coaches_per_team, fleet role coach) (readyup/coach_rules.h;
# ctest `match_coach_rules`).
add_executable(match_coach_rules_test tests/coach_rules_test.cpp readyup/coach_rules.cpp readyup/fleet_state.cpp
  readyup/map_names.cpp readyup/match_stats.cpp readyup/match_config_parser.cpp readyup/match_rules.cpp readyup/ruleset.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)
target_include_directories(match_coach_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_coach_rules_test PRIVATE -Wall -Wextra)
add_test(NAME match_coach_rules COMMAND match_coach_rules_test)

# `ru` main commands + subcommands of the match plugin (readyup/ru_commands.h): the help, the
# lookup, `.ru map change` / `.ru match load` parsing, `ru map reload`'s entry for workshop maps
# (ctest `match_ru_commands`).
add_executable(match_ru_commands_test tests/ru_commands_test.cpp readyup/ru_commands.cpp readyup/map_names.cpp)
target_include_directories(match_ru_commands_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_ru_commands_test PRIVATE -Wall -Wextra)
target_link_libraries(match_ru_commands_test PRIVATE pthread)
add_test(NAME match_ru_commands COMMAND match_ru_commands_test)

# Simulation mode and wingman (readyup/simulation_rules.h, readyup/wingman.h): timescale, roster
# identities, bot -> identity assignment, when to add a bot, the setup / teardown commands, the game
# mode, the match config fields and cfg/ReadyUp/live_wingman.cfg (ctest `match_simulation`).
add_executable(match_simulation_test tests/simulation_test.cpp readyup/simulation_rules.cpp readyup/wingman.cpp
  readyup/match_config_parser.cpp readyup/ruleset.cpp readyup/match_rules.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/steamid.cpp)
target_include_directories(match_simulation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_simulation_test PRIVATE -Wall -Wextra)
target_compile_definitions(match_simulation_test PRIVATE READYUP_CFG_DIR="${CMAKE_SOURCE_DIR}/cfg/ReadyUp")
add_test(NAME match_simulation COMMAND match_simulation_test)

# Idle map refresh decision (readyup/idle_refresh.h; ctest `match_idle_refresh`).
add_executable(match_idle_refresh_test tests/idle_refresh_test.cpp)
target_include_directories(match_idle_refresh_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_idle_refresh_test PRIVATE -Wall -Wextra)
add_test(NAME match_idle_refresh COMMAND match_idle_refresh_test)

# Go-live card and `.admin` (readyup/card_html.h, readyup/admin_call_logic.h, readyup/golive_card_logic.h):
# card HTML, escaping, the go-live card's send cadence and end,
# the per-player cooldown, message cleanup, call id / timestamp, the `admin_called` payload
# (ctest `match_live_cards`).
add_executable(match_live_cards_test tests/live_cards_test.cpp readyup/card_html.cpp readyup/admin_call_logic.cpp
  readyup/golive_card_logic.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(match_live_cards_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(match_live_cards_test PRIVATE -Wall -Wextra)
add_test(NAME match_live_cards COMMAND match_live_cards_test)

# Round restore + crash recovery (readyup/round_restore_rules.h): backup file names and the list,
# `.restore <round>` parsing and refusals, pause_after_restore resolution, the progress record and
# what a restarted server recovers (ctest `match_restore`).
add_executable(match_restore_test tests/restore_test.cpp readyup/round_restore_rules.cpp readyup/match_end_logic.cpp
  readyup/match_rules.cpp readyup/match_stats.cpp ${CMAKE_SOURCE_DIR}/libs/readyup/minijson.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(match_restore_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs
  ${CMAKE_SOURCE_DIR}/core/include)
target_compile_options(match_restore_test PRIVATE -Wall -Wextra)
add_test(NAME match_restore COMMAND match_restore_test)

# Pre-match values of the match config's cvars (readyup/cvar_snapshot.h): which names are read,
# first value wins, the restore commands, reload / restart JSON (ctest `match_cvar_snapshot`).
add_executable(match_cvar_snapshot_test tests/cvar_snapshot_test.cpp readyup/cvar_snapshot.cpp
  ${CMAKE_SOURCE_DIR}/libs/readyup/status_snapshot.cpp)
target_include_directories(match_cvar_snapshot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs)
target_compile_options(match_cvar_snapshot_test PRIVATE -Wall -Wextra)
add_test(NAME match_cvar_snapshot COMMAND match_cvar_snapshot_test)

# File retention (readyup/retention_rules.h): which old round backups, resume files and demos the
# sweep deletes (names, backup_keep_hours / demo_keep_hours, the loaded match, the recording)
# (ctest `match_retention`).
add_executable(match_retention_test tests/retention_test.cpp readyup/retention_rules.cpp)
target_include_directories(match_retention_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_retention_test PRIVATE -Wall -Wextra)
add_test(NAME match_retention COMMAND match_retention_test)

# Center panel blink workaround decision (readyup/hud_steady_logic.h; ctest `match_hud_steady`).
add_executable(match_hud_steady_test tests/hud_steady_test.cpp)
target_include_directories(match_hud_steady_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(match_hud_steady_test PRIVATE -Wall -Wextra)
add_test(NAME match_hud_steady COMMAND match_hud_steady_test)
