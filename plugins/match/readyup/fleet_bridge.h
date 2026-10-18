#pragma once
// The match side of the fleet link (docs/FLEET.md §7-§10, §12.3, §13; build-order step 3).
//
// fleet.so (plugins/fleet) owns the WebSocket, the spool and the offline timer and publishes
// readyup.fleet.v1 (core/include/readyup/fleet_iface.h). This file looks it up on every tick
// (it can be absent, standalone or reloaded: then everything here is a no-op) and:
//
//   - handles platform messages on the game thread: match.assign (config from the message,
//     per-match sv_password, D16 scrim hand-over: chat notice, kick non-roster players after 5 s,
//     then load), match.update (compare-and-set on config_rev), match.unassign (epoch fenced),
//     cmd (pause, unpause, force_ready, start, restore_round, restart_map, end_match, change_map,
//     swap_teams, kick, snapshot_now, exec; fleet.so runs say itself) with exactly one cmd.result each, and
//     local.offline_timeout (D12 auto-pause) / local.connection (snapshot on connect);
//   - keeps the canonical MatchState (§9.1): platform-owned fields from the assign config,
//     server-owned fields from the match flow (match_status.h), live_rev + epoch; every change
//     goes out as state.patch or inside an event.* ({match_id, map_number, round, rev, patch,
//     data}) with rev = previous rev + 1 (fleet_state.h LiveStream);
//   - turns match facts (match_signals.h, MatchFlowEvent, DemoEvent, RoundSummary / MapStats)
//     into events, forwards CS2 round backup files inline (event.backup, base64, split in parts
//     above 384 KB) and restores a backup (cmd restore_round).
//
// Nothing is reported for scrims or `ru match load` matches: only a platform assignment turns
// the link on. Game thread only unless noted.
#include "readyup/plugin_api.h"
#include "readyup/status_snapshot.h"

#include <string>

namespace readyup::fleet_bridge {

// readyup_plugin_load: match-flow / demo listeners.
void Install(const ru_api* api);
// readyup_plugin_unload: unregisters the fleet handlers (fleet.so may stay loaded).
void Uninstall();
// on_tick.
void Tick(double now);
// Core events (player connect / disconnect / team) and server log lines (exec output capture).
void OnCoreEvent(const ru_event* e);
// RU_EVENT_MAP_START: a match load's map change is done (CheckLoaded).
void OnMapStart();
void OnLogLine(const char* line);

// True while a platform assignment is active.
bool Assigned();
// A failover resume of a started round is waiting for its go-live in match warmup: the map score
// at that round and whether team1 is CT in it (warmup_score.h shows it). Game thread.
bool ResumeWarmupScore(int* team1, int* team2, bool* team1Ct);
// A failover resume is waiting to go live (its backup not restored yet): nobody has to ready;
// the match waits for the whole roster, then the normal ready-up (modes.cpp, match_rules.h ResumeMayGoLive).
bool ResumeWaiting();
// A match just ended and its players get their goodbye kick after the series-end delay: until
// then nobody else kicks them (kick_when_no_match_loaded waits).
bool EndedKickPending();
// The enrolled fleet server id (fleet.so status), "" without fleet / before enrollment. Game thread.
std::string FleetServerId();
// The fleet MatchState (what state.snapshot / hello carry) while assigned; false otherwise.
bool CurrentState(status::Json* out);
// False while a demo of the assignment still uploads or the series is not over (FLEET.md §17).
bool UpdateSafe();
// fleet.so has a streamed demo the platform has not confirmed yet (ru_fleet_status update_blocked):
// the local file is the only copy, so the retention sweep (retention.h) leaves demos alone. Game thread.
bool DemosUnconfirmed();

// Restores the start of `round` on the current map from this server's own CS2 round backup
// (readyup_backup_<matchid>_map<N>_round<NN>.txt), the way cmd restore_round does: autopaused,
// rounds >= `round` voided in the stats / round counters, rounds_voided + match_restored. Works
// for any loaded match (`ru match load` too, not only a platform assignment). `.stop` (votes.cpp),
// `.restore` / `ru_loadbackup` (round_restore.h, reason "admin"), crash recovery (reason
// "recovery"). False + *err when there is no such backup. *autoUnpause: pause_after_restore is off
// and the match goes live again in 3 s (round_restore.h AfterRestore).
bool RestoreRoundFromLocalBackup(int round, const std::string& by, const std::string& reason, std::string* err,
                                 bool* autoUnpause = nullptr);

// Plugin reload (reload_state.cpp): assignment, config, fence, live_rev, pause counters, sent
// backups.
status::Json SnapshotJson();
void RestoreJson(const status::Json& j);

}  // namespace readyup::fleet_bridge
