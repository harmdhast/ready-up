#pragma once

// Server settings of the match flow (docs/PARITY.md §2, §7): one table, one value per setting.
//
//   setting                               kind  default  what it does
//   minimum_ready_required <n>            int   0        players a team needs ready to go live (0 = a full team)
//   playout_enabled_default 0|1           bool  0        play every round, no clinch (match `playout` wins)
//   autoready_enabled 0|1                 bool  0        roster players are ready as soon as they join their team
//   knife_enabled_default 0|1             bool  1        knife round for scrims and for match maps without a side
//   reset_cvars_on_series_end 0|1         bool  1        the warmup / team-name cvars are reset when a series ends
//   use_pause_command_for_tactical_pause  bool  0        `.pause` / `.p` call a tactical timeout (else technical)
//   hostname_format "<fmt>"               str   ""       hostname while a match is loaded ({TEAM1} {TEAM2} ...)
//   kick_when_no_match_loaded 0|1         bool  0        non-admins are kicked while no match is loaded
//   whitelist_enabled_default 0|1         bool  1        only roster / spectators / admins may join a match
//   scrim_when_idle 0|1                   bool  1        players joining an idle server start a scrim warmup
//   chat_prefix "<text>"                  str   ""       Ready Up's chat prefix (<Color> tokens); pushed to the
//                                                        core (ru_api set_core_setting) when set at runtime,
//                                                        empty = the core's readyup.cfg chat_prefix
//   admin_chat_prefix "<text>"            str   ""       admins' chat name prefix (<Color> tokens), empty = [Admin]
//   roster_names_enabled 0|1              bool  1        roster players' in-game names are their match roster
//                                                        names (roster_names.h; match `roster_names` wins)
//
// Where a value comes from (first one set wins):
//   1. the runtime value: console / RCON `ru_<setting> <value>`, chat `.ru settings set <setting>
//      <value>` (admins) and its shortcuts (.readyrequired, .playout, .roundknife, .whitelist), the
//      fleet `cmd settings.set` / `server.config`; saved in state.json and restored after a restart,
//      `ru_<setting> default` clears it,
//   2. readyup.cfg / match.cfg: a key of the same name (or its legacy key: `min_players_to_ready`,
//      `scrim_knife`, `admin_prefix`),
//   3. the built-in default above.
// Per-match values (match config `playout`, `whitelist`, `autoready`, `min_players_to_ready`,
// fleet `rules.*`) win over all three for that match. `ru_pause_after_restore` is the round
// restore's own console setting (round_restore.h), not in this table.
//
// Pure logic, no engine calls (ctest `match_server_settings`); the engine side is match_settings.h.

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace readyup::settings {

enum class Kind { Bool, Int, Str };

struct SettingInfo {
  const char* name;       // "minimum_ready_required"; the console command is "ru_" + name
  Kind kind;
  const char* builtin;    // built-in default, normalized ("0" / "1", "5", "")
  int min;                // Int range (inclusive); Str: max length in bytes
  int max;
  const char* cfgKey;     // legacy readyup.cfg key read as well (nullptr: none)
  const char* chat;       // chat shortcut (".playout"; nullptr: none)
  const char* help;
};

// In `.settings` order.
const std::vector<SettingInfo>& Table();
// "playout_enabled_default" or "ru_playout_enabled_default" (case-insensitive). nullptr otherwise.
const SettingInfo* Find(const std::string& name);
// A chat shortcut (".playout") -> its setting.
const SettingInfo* FindChat(const std::string& chatCommand);
std::string ConsoleName(const SettingInfo& s);  // "ru_" + name

// Canonical form of a value: Bool 1/0/true/false/on/off/yes/no -> "1" / "0"; Int in [min, max];
// Str without control bytes, quotes around it dropped, at most `max` bytes. False + *err otherwise.
bool Normalize(const SettingInfo& s, const std::string& in, std::string* out, std::string* err);
// The value `.playout` / `.whitelist` (no argument) sets: the opposite of `current` for a Bool;
// with an argument, that argument.
bool ToggleValue(const SettingInfo& s, const std::string& current, const std::string& arg, std::string* out,
                 std::string* err);
// For replies: "on" / "off", the number, "\"text\"" / "(empty)".
std::string Display(const SettingInfo& s, const std::string& value);

// ---- the values ------------------------------------------------------------------------------

class Store {
 public:
  // Runtime value (normalized), or clear it (nullopt). Saved through the persist hook.
  bool Set(const std::string& name, const std::string& value, std::string* err);
  bool Clear(const std::string& name);
  // readyup.cfg / match.cfg values (key -> raw text) of this (re)load; unknown keys are ignored.
  void SetFileValues(const std::map<std::string, std::string>& values);
  // Restores runtime values without saving them again (plugin load).
  void LoadRuntime(const std::string& name, const std::string& value);
  void Reset();  // tests

  std::string Get(const std::string& name) const;  // "" for an unknown name
  // "runtime" | "cfg" | "default"
  std::string Source(const std::string& name) const;
  std::optional<std::string> Runtime(const std::string& name) const;

  // Called with (name, value or nullopt) on every Set / Clear (the plugin saves it in state.json).
  using PersistFn = void (*)(const std::string& name, const std::optional<std::string>& value);
  void SetPersistHook(PersistFn fn);

 private:
  mutable std::mutex mu_;
  std::map<std::string, std::string> runtime_;
  std::map<std::string, std::string> file_;  // normalized
  PersistFn persist_ = nullptr;
};

Store& Global();
bool Bool(const char* name);
int Int(const char* name);
std::string Str(const char* name);

// `.settings` lines: "name = value (source)" in table order.
std::vector<std::string> ShowLines(const Store& store);

// state.json key of a setting (the console name, like the other console settings there).
std::string PersistKey(const SettingInfo& s);

// ---- hostname_format -------------------------------------------------------------------------

struct HostnameVars {
  std::string team1, team2, matchId, map;
  int mapNumber = 0, team1Score = 0, team2Score = 0, team1Series = 0, team2Series = 0;
};
// {TEAM1} {TEAM2} {MATCH_ID} {MAP} {MAPNUMBER} {TEAM1_SCORE} {TEAM2_SCORE} {TEAM1_SERIES}
// {TEAM2_SERIES} (case-insensitive); unknown tokens stay as written. The result has no quotes,
// ';', control bytes or backslashes, surrounding blanks trimmed, at most 127 bytes (UTF-8 safe):
// safe for `hostname "<result>"`.
std::string ExpandHostname(const std::string& format, const HostnameVars& v);

}  // namespace readyup::settings
