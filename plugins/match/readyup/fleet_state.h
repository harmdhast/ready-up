#pragma once
// Engine-free pieces of the fleet match link (docs/FLEET.md §7, §9, §12.3), unit-tested by
// tests/fleet_state_test.cpp. fleet_bridge.cpp wires them to the match flow and fleet.so.
//
//  - LiveStream: the server-owned live_rev counter and the last MatchState sent. Every message
//    that changes the stream (a state.patch or an event.*) bumps live_rev by exactly one and
//    carries the RFC 7386 merge patch from the previous state, so the platform applies a patch
//    when rev == stored_rev + 1 and asks for a snapshot on a gap (§8.1).
//  - Fence: epoch fencing (§9.3 rule 4, §11.4) and config_rev compare-and-set (§9.3 rule 2).
//  - Assign/update: match.assign config -> the MAT match config Ready Up already loads
//    (match_config_parser.h), match.update ops on the stored config.
//  - Codecs and small validators: sha256, base64, `say` / `exec` / password / map checks, the
//    round number in a CS2 backup file name.
#include "readyup/status_snapshot.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace readyup::fleetstate {

using status::Json;

// ---------------------------------------------------------------------------- state stream

// Copy of `v` without null object members (recursively). MatchState never carries nulls on the
// wire: in a merge patch null means "remove", so an absent member and null are the same thing.
Json StripNulls(const Json& v);

class LiveStream {
 public:
  // New baseline (match.assign, plugin reload): the next Advance() diffs against `state`.
  void Reset(const Json& state, long long rev);
  // `next` is the current MatchState (its live_rev member is ignored and overwritten). When it
  // differs from the last state, or `force` is set (an event without a state change), bumps
  // live_rev, stores next (with live_rev) and returns the merge patch (which always includes
  // live_rev). Returns false when nothing changed and !force.
  bool Advance(const Json& next, bool force, Json* patch, long long* rev);
  long long Rev() const { return rev_; }
  const Json& State() const { return state_; }
  bool HasState() const { return !state_.IsNull(); }

 private:
  Json state_;
  long long rev_ = 0;
};

// ---------------------------------------------------------------------------- fencing

struct Assignment {
  bool active = false;
  std::string match_id;
  long long epoch = 0;
  long long config_rev = 0;
};

enum class Verdict {
  Ok,           // go ahead
  Duplicate,    // match.assign replayed: same match and epoch already active (ack ok, do nothing)
  StaleEpoch,   // lower epoch than the one active or retired for this match
  Busy,         // another match (or a local non-fleet match) is active
  NotAssigned,  // match-scoped message for a match this server does not have
};
const char* VerdictCode(Verdict v);  // error code for cmd.result ("stale_epoch", "busy", ...)

class Fence {
 public:
  // match.assign. `localMatchActive`: a real match not from the platform is loaded
  // (`ru match load`); scrims do not count (they are ended by the hand-over, D16).
  Verdict CheckAssign(const Assignment& cur, const std::string& matchId, long long epoch, bool localMatchActive) const;
  // match.update / match.unassign / cmd with a match_id: exact match_id and epoch.
  Verdict CheckScoped(const Assignment& cur, const std::string& matchId, long long epoch) const;
  // Remember the highest epoch seen for a match (on assign and unassign) so a late message with a
  // lower epoch is refused even after the assignment ended. Keeps the last 64 matches.
  void Retire(const std::string& matchId, long long epoch);
  long long Retired(const std::string& matchId) const;
  Json ToJson() const;
  void FromJson(const Json& j);

 private:
  std::map<std::string, long long> retired_;
  std::vector<std::string> order_;
};

// §9.3 rule 2: an update applies only on top of the config_rev the server has.
inline bool ConfigCas(long long baseConfigRev, long long currentConfigRev) {
  return baseConfigRev == currentConfigRev;
}

// ---------------------------------------------------------------------------- assign / update

// Checks a match.assign payload (§7.1). False + *err (one line) when it cannot be loaded.
bool ValidateAssign(const Json& payload, std::string* err);

// Numeric id for Ready Up's match context (demo names, stats, backups use a number): the
// match_id itself when it is all digits (1..15 of them), else a 52-bit FNV-1a hash, never 0.
unsigned long long NumericMatchId(const std::string& matchId);

// The MAT MatchConfig document Ready Up's parser (match_config_parser.h) and the recovery
// store take, built from match.assign.config: matchid, num_maps, maplist, map_sides, team1/2
// {name, tag, players{steamid64: name}, captain_steamid64}, spectators, admins, maxRounds,
// overtimeMode / overtimeSegments / maxOvertimes, damageTiebreak*, knifeDecisionSeconds,
// clinch_series and cvars (engine cvars only: mp_*, sv_*, tv_*, bot_*; never sv_password, which
// the link sets itself, or ru_*; dropped names go to *dropped), demo_record / demo_upload
// (rules.demo.record / .upload; absent = the server's demo settings). slug = match_id.
Json AssignToMatConfig(const std::string& matchId, const Json& config, std::vector<std::string>* dropped);

// ---------------------------------------------------------------------------- server.config

// server.config {rev, settings} (FLEET.md §7.5) -> what the match plugin sets. fleet.so reads
// settings.offline_pause_minutes and settings.status_http.token itself (fleet_plugin.cpp).
//
//   settings field                          -> Ready Up
//   hostname_format                         server setting hostname_format
//   scrim_knife                             server setting knife_enabled_default
//   scrim_when_idle                         server setting scrim_when_idle
//   chat_prefix / admin_chat_prefix         server settings chat_prefix / admin_chat_prefix
//   series_end_kick_delay.{no_demo, demo_no_upload, demo_upload}
//                                           ru_series_end_kick_delay_* (match_settings::Set)
//   demo.path / demo.name_format            ru_demo_path / ru_demo_name_format
//   warmup.message_html / respawn / money   ru_warmup_message_html / ru_warmup_respawn /
//                                           ru_warmup_startmoney
//
// Demos of platform matches are streamed over the link (FLEET.md §12.2), so server.config has
// no upload target. A field that is absent leaves its setting alone; a field of the wrong type is
// skipped and named in `skipped`.
struct ServerConfigPlan {
  // Server settings (server_settings.h) and ru_series_end_kick_delay_*: match_settings::Set.
  std::vector<std::pair<std::string, std::string>> settings;
  // Console settings (persisted_settings.h ApplyConsoleSetting): {"ru_demo_path", "ReadyUp/"}.
  std::vector<std::pair<std::string, std::string>> console;
  std::vector<std::string> skipped;
};
ServerConfigPlan PlanServerConfig(const Json& settings);

// Applies match.update ops (§7.3) to the stored assign config. False + *err on a bad op (the
// config is then unchanged). *passwordChanged when a set_password op ran.
bool ApplyUpdateOps(Json* config, const Json& ops, std::string* err, bool* passwordChanged);

// ---------------------------------------------------------------------------- failover resume

// match.assign `resume` (FLEET.md §11.3), checked and reduced to what the server needs:
//
//   resume {
//     from_epoch?: int,                 // the failed assignment's epoch (< epoch; logged)
//     map_number: int,                  // the map to continue (1..num_maps)
//     round?: int,                      // the round to replay (1-based); 0 / absent without a
//                                       //   backup = restart that map from warmup
//     backup?: InlineBackup,            // the chosen round backup (single part), or
//     backup_ref?: {file?, sha256?},    // a backup file the server already has (restart in place);
//                                       //   neither: the server's own file for map_number / round
//     series_score?: {team1, team2},    // maps won before map_number (else state.series.score)
//     maps?: {"<n>": {score, winner}},  // results of the maps before (else state.series.maps)
//     sides?: team1_ct | team2_ct,      // starting sides of the resumed map (else state / config)
//     score?: {team1, team2},           // map score at the start of `round` (else backup.score)
//     map_stats?: MapStats,             // the platform's stats of the map (state.snapshot map_stats)
//     state?: MatchState                // the platform's state at that round
//   }
struct ResumeMapResult {
  int map_number = 0;
  int team1 = 0, team2 = 0;
  std::string winner;  // team1 | team2 | none
};
struct ResumePlan {
  bool present = false;
  long long from_epoch = 0;
  int map_number = 1;
  int round = 0;                 // 0 = no backup: the map restarts from warmup
  bool inline_backup = false;    // backup given inline (raw holds the file)
  std::string file;              // inline / backup_ref file name ("" = find by map / round)
  std::string raw;               // inline: the decoded file (not kept across reloads)
  std::string sha256;            // inline: verified; backup_ref: expected ("" = any)
  int series_team1 = 0, series_team2 = 0;
  std::vector<ResumeMapResult> maps_done;
  std::string sides;             // "" = keep the config's
  int score_team1 = -1, score_team2 = -1;  // -1 = unknown
  std::string team1_side;        // "ct" | "t" | "" at the start of `round`
  Json map_stats;                // null = none
  bool pause_after_restore = true;
};
// False + *code ("invalid_config" | "checksum" | "unsupported") + *err (one line) when `resume`
// cannot be used with `config` (the match.assign config, already valid) and `epoch`.
bool ParseResume(const Json& resume, const Json& config, long long epoch, ResumePlan* out, std::string* code,
                 std::string* err);
// Plugin reload: everything but `raw`.
Json ResumeToJson(const ResumePlan& p);
ResumePlan ResumeFromJson(const Json& j);

// D16 hand-over: players allowed to stay when a match is assigned (roster incl. subs and
// coaches, spectators, match admins).
bool InAssignedMatch(const Json& config, uint64_t steamid64);
// "team1" | "team2" | "spectator" | "" for a SteamID64 in the assign config.
std::string TeamOf(const Json& config, uint64_t steamid64);

// ---------------------------------------------------------------------------- codecs

std::string Sha256Hex(const std::string& data);
std::string Base64Encode(const std::string& data);
bool Base64Decode(const std::string& text, std::string* out);

// ---------------------------------------------------------------------------- validators

// `cmd exec` (§7.4, D10): one line, 1..512 bytes, no `;`-chained `ru fleet` / `fleet`
// (credentials) and no quotes that could smuggle a second command past the check.
bool ValidateExec(const std::string& command, std::string* err);
// sv_password value: printable ASCII without quotes, `;` or spaces, at most 64 bytes ("" = none).
bool ValidPassword(const std::string& password);
// Map entries (a name, or a workshop id as "123", "ws:123", "workshop/123[/name]"; map_names.h)
// and workshop ids safe to put on a command line.
bool SafeMapName(const std::string& name);
bool SafeWorkshopId(const std::string& id);
// CS2 round backup files: "<prefix>round07.txt" -> 7; -1 when the name has no round number.
int BackupRoundFromName(const std::string& fileName);
// A backup file name from the platform that is safe to write into csgo/ (no directories,
// ends in .txt, [A-Za-z0-9_.-] only).
bool SafeBackupFileName(const std::string& name);
// A round backup the scan forwards (fleet_bridge.cpp): "<prefix>...roundNN.txt" written since the
// match went live (`liveSinceMs`, the file clock, -1 = not live). The go-live round's own file is
// written in the frame the mode turns live, so kLiveBackupSlackMs before counts too. Older files
// (a warmup, an earlier live session, before a server restart) stay local.
constexpr long long kLiveBackupSlackMs = 2000;
bool LiveSessionBackup(const std::string& fileName, const std::string& prefix, long long mtimeMs,
                       long long liveSinceMs);

}  // namespace readyup::fleetstate
