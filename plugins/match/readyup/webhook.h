#pragma once

#include "readyup/match_rules.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup {

struct AdminCallEvent;  // admin_call_logic.h
namespace stats {
struct MapStats;  // match_stats.h
}
namespace demo {
struct DemoEvent;  // demo_recorder.h
}

// Minimal team identity for MAT/MatchZy-style events.
enum class WebhookTeam {
  Unknown = 0,
  Team1,
  Team2,
};

struct WebhookPlayer {
  uint64_t steamid64 = 0;
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
};

struct WebhookPlayerStatLine {
  uint64_t steamid64 = 0;
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
  int kills = 0;
  int deaths = 0;
};

// MatchZy-style per-player stats line (nested under team1/team2).
struct WebhookPlayerStats {
  uint64_t steamid64 = 0;
  std::string name;
  int kills = 0;
  int deaths = 0;
  int assists = 0;
  int headshot_kills = 0;
  int damage = 0;
  int mvps = 0;
  int score = 0;
};

struct WebhookMatchContext {
  uint64_t matchid = 0;
  std::string slug;  // optional
  int num_maps = 0;
  std::string team1_name;
  std::string team2_name;
  // Match config team1.id / team2.id (string or number; "" when absent): the AT `team.id` in
  // round_end / map_result.
  std::string team1_id;
  std::string team2_id;

  uint64_t team1_captain_steamid64 = 0;
  uint64_t team2_captain_steamid64 = 0;

  // Knife decider decision window (seconds). If the knife-winning team does not
  // pick sides in time, sides stay.
  int knifeDecisionSeconds = 60;

  // Regulation total rounds (e.g. 24 for MR12).
  // This is sourced from MAT config `maxRounds` (preferred) or `cvars.mp_maxrounds` (fallback).
  int maxRounds = 24;

  // Overtime settings (sourced from MAT config metadata, not convars).
  bool overtime_enabled = false;
  // Rounds per overtime half (e.g. 3 => MR3 halves, 6 rounds per OT).
  int overtimeSegments = 3;

  // Optional tie-break configuration.
  // - maxOvertimes: maximum number of overtime blocks (each block is 2*overtimeSegments rounds).
  //   -1 = unlimited (default/current behavior).
  int maxOvertimes = -1;
  // When enabled, Ready Up can resolve ties using total roster-team damage.
  bool damageTiebreakEnabled = false;
  // If damage is also tied, continue playing sudden-death rounds until a winner.
  bool suddenDeathOnDamageTie = true;

  // Side mapping per map index (1-based map_number -> index map_number-1).
  // Values: "team1_ct" | "team2_ct" | "knife" (and possibly others).
  std::vector<std::string> map_sides;

  // Series can end early once a team has won more than half of num_maps
  // (match config `clinch_series`, default true).
  bool clinch_series = true;

  // Map list for the series (map 1 is maplist[0]).
  // This is sourced from MAT match config.
  std::vector<std::string> maplist;

  // SteamID64s allowed to join as spectators (whitelist).
  std::unordered_set<uint64_t> spectators;

  // SteamID64s treated as admins for this match context.
  // This is sourced from MAT match config `admins: [steamid64...]`.
  std::unordered_set<uint64_t> admins;

  // Match config cvars (e.g. mp_maxrounds, mp_overtime_enable, matchzy_*).
  // Values are stored as raw strings and applied as: `<key> <value>`.
  std::unordered_map<std::string, std::string> cvars;

  // SteamID64 -> team mapping derived from match config.
  std::unordered_map<uint64_t, WebhookTeam> roster_team;
  // Roster players who are substitutes (fleet role "sub"; MAT team1/team2 `substitutes`
  // {steamid64: name} or [steamid64]). Also in roster_team (whitelisted, may stand in), but never part
  // of the full team the ready gate and the ready count wait for (players_per_team).
  std::unordered_set<uint64_t> substitutes;

  // Optional team flags (2-letter country codes, `mp_teamflag_N`); team tags are shown by name.
  std::string team1_flag;
  std::string team2_flag;

  // Pause / ready / forfeit rules from the match config (-1 = unset; match_rules.h).
  MatchRules rules;

  // Ruleset (ruleset.h): "" = the match config has none (readyup.cfg `ruleset=` applies),
  // "default" | "valve". overrides_json: the validated "overrides" object ("" = none).
  std::string ruleset;
  std::string overrides_json;
  // Coaches (MAT "coaches": [steamid64]; fleet role "coach"). Whitelisted as spectators unless the
  // ruleset keeps them out (valve online: lan false, coaches_online false).
  std::unordered_set<uint64_t> coaches;
  // The team of each coach listed per team (MAT team1/team2 "coaches": {steamid64: name} as get5,
  // or [steamid64]; fleet role "coach" inside a team). Also in `coaches`. They coach that team
  // without typing anything (coach.h).
  std::unordered_map<uint64_t, WebhookTeam> coach_team;
  // Coaches per team who are not listed (scrim `.coach`, admin-assigned); MAT / get5
  // `coaches_per_team`. <= 0: no limit.
  int coaches_per_team = 2;
  // Per-match values of server settings (server_settings.h); -1 / 0 = not set (the server's).
  // MAT keys `players_per_team`, `playout`, `whitelist`, `autoready`; fleet rules.playout,
  // rules.whitelist, rules.ready.autoready and the team size from the player roles.
  int players_per_team = 0;  // a full team for the ready gate (0 = 5); substitutes on top
  int playout = -1;
  int whitelist = -1;
  int autoready = -1;
  // What the parser changed for the ruleset (logged at load).
  std::vector<std::string> ruleset_notes;

  // Roster player names (team1/2.players {steamid64: name}); simulated bots play as them.
  std::unordered_map<uint64_t, std::string> roster_names;
  // `roster_names` (fleet rules.roster_names): roster players play under those names in game
  // (roster_names.h). -1 = unset (server setting roster_names_enabled), 0 = off, 1 = on.
  int roster_names_rule = -1;
  // `wingman: true`: 2v2 on CS2's wingman mode (wingman.h).
  bool wingman = false;
  // `demo_record` / `demo_upload` (fleet match.assign rules.demo.record / .upload): -1 = the
  // server's demo settings (demo_recorder.h), 0 = off, 1 = on for this match.
  int demo_record = -1;
  int demo_upload = -1;
  // `simulation: true`: bots play the match (simulation.h); `simulation_timescale` (0.1 .. 4)
  // is host_timescale while a map is live.
  bool bot_fill = false;  // anonymous fillers alongside real roster players
  bool simulation = false;
  double simulation_timescale = 1.0;
};

// Configure base events URL (e.g. https://mat.example.com/api/events).
// Empty string disables webhook sending.
void WebhookConfigure(std::string baseEventsUrl);

std::string WebhookBaseUrl();

// Configure the MAT heartbeat URL (full URL including /api/servers/:id/heartbeat).
// Empty string disables heartbeats.
void WebhookConfigureHeartbeatUrl(std::string heartbeatUrl);
std::string WebhookHeartbeatUrl();

// Sets optional Bearer token for webhook requests.
// This is expected to be the same token used for `ru match load` auth.
void WebhookSetBearerToken(std::optional<std::string> token);

// Set/clear match context (called when we load match config).
void WebhookSetMatchContext(WebhookMatchContext ctx);
void WebhookClearMatchContext();
std::optional<WebhookMatchContext> WebhookGetMatchContext();

// Override heartbeat status even when no match is loaded.
// Accepted values: "idle"|"loading"|"warmup"|"live"|"postgame"|"error"
// (Any other value clears the override to "idle" semantics.)
void WebhookSetHeartbeatStatus(const char* status);

// Enqueue an event JSON payload. This is non-blocking best-effort.
// The payload must already be valid JSON.
void WebhookEnqueueEvent(std::string json);

// Convenience helpers for core events.
void WebhookEmitServerConfigured(const char* configuredBy);
void WebhookEmitServerHealth(const char* reason = nullptr);
void WebhookEmitSeriesStart();
void WebhookEmitSeriesEnd(int team1_series_score, int team2_series_score, const char* winner, int time_until_restore);
// map_result in the AT shape (at_payloads.h MapResultJson): winner {side, team}, team1 / team2
// {id, name, series_score (after this map), score, score_ct, score_t, players[] with the map's final
// stats}. `stats`: the stats model's snapshot of the map (empty when stats were not recorded).
void WebhookEmitMapResult(int map_number, const char* map_name, int team1_score, int team2_score, const char* winner,
                          const stats::MapStats& stats, int team1_series_score, int team2_series_score);
void WebhookEmitPlayerConnect(const WebhookPlayer& p);
void WebhookEmitPlayerDisconnect(const WebhookPlayer& p);
// Emits MatchZy-style ready/unready events with counts.
// - If ready==true: emits `player_ready`
// - If ready==false: emits `player_unready`
void WebhookEmitPlayerReady(const WebhookPlayer& p,
                            bool ready,
                            int ready_count_team1,
                            int ready_count_team2,
                            int total_ready,
                            int expected_total);
void WebhookEmitMatchPaused(int map_number, const WebhookPlayer& paused_by, bool is_tactical, bool is_admin, int pause_time);
void WebhookEmitUnpauseRequested(int map_number, WebhookTeam team, int teams_ready, int teams_needed);
void WebhookEmitMatchUnpaused(int map_number, int pause_duration);
void WebhookEmitWarmupEnded(int map_number);
void WebhookEmitGoingLive(int map_number);
void WebhookEmitHalftimeStarted(int map_number, int team1_score, int team2_score);
void WebhookEmitOvertimeStarted(int map_number, int overtime_number);
void WebhookEmitSideSwap(int map_number, const char* team1_side, const char* team2_side);
void WebhookEmitKnifeRoundStarted(int map_number);
void WebhookEmitKnifeRoundEnded(int map_number, const char* winner);
// MatchZy-style side pick event (typically after knife).
// - side: "ct"|"t" (lowercase MatchZy convention)
// - picked_by: player name or "server"
// - team: "team1"|"team2"
void WebhookEmitSidePicked(int map_number, const char* map_name, const char* side, const char* picked_by, const char* team);
// `.admin [message]` (admin_call.h): `admin_called`, also without a match (matchid -1). Body:
// admin_call_logic.h AdminCalledWebhookJson.
void WebhookEmitAdminCalled(const AdminCallEvent& e);
// Thread-safe match context mutation: replaces map_sides[map_number-1] with "team1_ct" or "team2_ct".
bool WebhookUpdateMapSide(int map_number, const char* map_side);
// Admin-triggered recovery request. This tells MAT to rebuild its UI state from DB events,
// optionally rewinding to a specific round number.
// - round_number == 0 => recover to latest
void WebhookEmitRecoverRequested(int map_number, int round_number);
// A round backup was loaded (round_restore.h): Auto Tournament CS2 `backup_loaded`
// {map_number, round_number, filename}; round_number = rounds played before the restored round
// (the NN of the backup's file name).
void WebhookEmitBackupLoaded(int map_number, int round_number, const std::string& filename);
void WebhookEmitRoundStarted(int map_number, int round_number, int team1_score, int team2_score);
void WebhookEmitRoundEnd(int map_number, int round_number, int round_time, int reason, const char* winner, int team1_score, int team2_score);
void WebhookEmitRoundEndWithPlayerStats(int map_number,
                                        int round_number,
                                        int round_time,
                                        int reason,
                                        const char* winner,
                                        int team1_score,
                                        int team2_score,
                                        const std::vector<WebhookPlayerStatLine>& players);

// Preferred: MatchZy-compatible nested shape so MAT can parse it.
void WebhookEmitRoundEndMatchzy(int map_number,
                                int round_number,
                                int round_time,
                                int reason,
                                const char* winner,
                                int team1_score,
                                int team2_score,
                                const std::vector<WebhookPlayerStats>& team1_players,
                                const std::vector<WebhookPlayerStats>& team2_players);

// round_end in the AT shape (at_payloads.h RoundEndJson) with the full per-player stat set, from
// the stats model after it closed the round. Used while the stats model is live; the MatchZy
// shape above is the fallback without it.
void WebhookEmitRoundEndStats(int map_number, int round_number, int round_time_ms, int reason, int winner_side,
                              const stats::MapStats& stats, int team1_series_score, int team2_series_score);

// demo_recording_start / _stop, demo_upload_start / _success / _fail / _ended (at_payloads.h
// DemoEventJsons). A demo::AddListener listener: any thread. Needs only the webhook URL, not a
// loaded match (a demo upload ends after the series unloaded).
void WebhookEmitDemoEvent(const demo::DemoEvent& e);

// Starts sender thread (idempotent). Safe to call early during startup.
void WebhookStartSenderThread();

// Plugin reload (reload_state.cpp): events not delivered yet (the old image's sender stopped),
// re-queued by the new image; and the current heartbeat status string.
std::vector<std::string> WebhookTakePending();
void WebhookRestorePending(std::vector<std::string> events);
std::string WebhookHeartbeatStatusString();

}  // namespace readyup

