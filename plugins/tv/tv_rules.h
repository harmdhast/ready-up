#pragma once

// readyup-tv pure logic (ctest `tv_rules`): what to send to the engine, which match profile is
// active, the saved settings, tv_status parsing and the tvState JSON. No engine calls;
// tv_plugin.cpp is the engine side. Design: docs/design/tv-plugin.md.

#include "readyup/broadcast_profile.h"
#include "readyup/minijson.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tv {

using readyup::broadcast::Key;
using readyup::broadcast::Profile;

// ---- applying -------------------------------------------------------------------------------

// What tv.so has done to the engine. Kept across `ru plugin reload tv` (core stash).
struct Applier {
  std::map<std::string, std::string> baseline;  // cvar -> value before tv.so first changed it
  std::set<std::string> baselineUnknown;        // asked, no usable answer
  std::map<std::string, std::string> applied;   // cvar -> what tv.so last set
};

struct Plan {
  std::vector<std::string> commands;      // "tv_delay 105", in order
  std::vector<std::string> needBaseline;  // read these first (cvar_query), then plan again
  std::vector<std::string> notes;         // "sv_deltaticks_enforce left at 0 (no baseline)"
};

// desired = cvar -> value (DesiredCvars). force = send every desired value even if tv.so already
// set it (a map load / cfg exec may have reset it). A desired cvar tv.so has not touched yet and
// has no baseline for goes to needBaseline instead. A cvar tv.so set that is no longer desired
// goes back to its baseline; without one tv_secure_bypass goes to 0 and sv_deltaticks_enforce to 2
// (CS2's defaults; 2 = kick any client that acks delta ticks out of order); any other is left.
// Updates a.applied.
Plan PlanApply(Applier& a, const std::vector<std::pair<std::string, std::string>>& desired, bool force);

// A cvar_query answer for the baseline: digits / '-' / '.' only (what these cvars print), else
// unknown. Never called for tv_password.
void RecordBaseline(Applier& a, const std::string& cvar, const char* value);

// Serialized for the core stash: {"baseline":{..},"unknown":[..],"applied":{..}}.
std::string ApplierJson(const Applier& a);
bool ParseApplier(const std::string& json, Applier* out);

// ---- the active match ---------------------------------------------------------------------

struct MatchProfile {
  std::string matchId;
  Profile profile;    // rules.broadcast
  Profile override_;  // `ru tv match` / tv.set {scope: match}
  int delayFloor = 0;
  bool passwordSet = false;
  // config.cvars entries for owned cvars (OwnedCvarsInAssign): the match plugin sets these itself.
  std::vector<std::pair<std::string, std::string>> matchCvars;
};

enum class TrackEvent { None, Activated, Deactivated, Dropped };

// match.assign arrives before the match plugin has loaded the match (and it may refuse it), so the
// profile waits as `pending` until the match plugin reports that match id, then becomes `active`.
// Active ends on match.unassign or once the match plugin reports another / no match for
// kGraceSeconds (a match plugin reload is shorter than that).
struct MatchTracker {
  static constexpr double kGraceSeconds = 5.0;
  static constexpr double kPendingSeconds = 90.0;

  std::optional<MatchProfile> pending;
  double pendingSince = 0;
  double pendingTtl = kPendingSeconds;
  std::optional<MatchProfile> active;
  double mismatchSince = -1;

  // ttl: how long `pending` waits for the match plugin (longer for a profile read back from disk
  // after a restart: the match plugin restores its match after the first map load).
  void Assign(MatchProfile p, double now, double ttl = kPendingSeconds);
  // True when it ended the active or pending match `matchId` ("" = whatever is there).
  bool Unassign(const std::string& matchId);
  // loadedMatchId: what the match plugin reports now ("" = no match loaded / no match plugin).
  TrackEvent Observe(const std::string& loadedMatchId, double now);
};

// match.update `set_rules` op: its `rules` is a merge patch. rules.broadcast null clears the whole
// profile; a key null clears that key; others are set (validated like rules.broadcast). Returns
// false (profile unchanged) on a bad patch; *touched = the patch has a broadcast member at all.
bool ApplyBroadcastPatch(const readyup::minijson::Value& rulesPatch, Profile* p, bool* touched, std::string* err);

// Persisted (data_dir/active.json) so a server restart mid-match finds its profile again; the
// override and password flag ride along.
std::string MatchProfileJson(const MatchProfile& m);
bool ParseMatchProfile(const std::string& json, MatchProfile* out);

// ---- settings -----------------------------------------------------------------------------

// data_dir/settings.json: {"version":1,"server":{profile},"casters":["7656..."]}. Missing = empty.
struct Settings {
  Profile server;
  std::set<uint64_t> casters;
  bool castersSet = false;  // the casters list was changed at runtime (else tv.cfg's applies)
};
Settings ParseSettings(const std::string& json, bool* ok);
std::string SettingsJson(const Settings& s);

uint64_t ParseSteamId64(const std::string& s);
// "7656..., 7656..." (tv.cfg casters). Bad entries are skipped and counted in *bad.
std::set<uint64_t> ParseSteamIdList(const std::string& s, int* bad = nullptr);

// tv.cfg profile keys (canonical names): "delay_seconds" -> "105". Unknown / bad values are
// reported in *errors ("max_clients: an integer 0..255").
Profile ProfileFromConfig(const std::map<std::string, std::string>& kv, std::vector<std::string>* errors);

// ---- the engine's view --------------------------------------------------------------------

struct LaunchInfo {
  int tvPort = 0;
  std::optional<bool> passwordSet;  // +tv_password on the command line
  bool insecure = false;            // the server itself runs -insecure
};
LaunchInfo ParseLaunchArgs(const std::vector<std::string>& args);

// One `tv_status` output line. Fills what it finds; returns true if the line was tv_status
// output. Accepts the CS:GO shape ("Local Slots 8, Spectators 2, Proxies 0", "... delay 105",
// "Local IP 1.2.3.4:27020, ...", "SourceTV not active.") with or without a "GOTV[0]: " prefix.
struct TvStatus {
  std::optional<int> clients, slots, delay, port;
  std::optional<bool> active;
};
bool ParseTvStatusLine(const std::string& line, TvStatus* st);

// cvar_query answers of the owned cvars (+ tv_enable, tv_port); "" = no answer.
struct Observed {
  std::map<std::string, std::string> cvars;
  std::optional<int> clients;
};

// Everything the tvState JSON and `ru tv status` show.
struct View {
  bool available = true;
  int port = 0;  // 0 = unknown
  Observed observed;
  Profile effective;
  std::string source = "server";  // "server" | "match"
  std::string matchId;
  int delayFloor = 0;
  int effectiveDelay = -1;  // what tv.so puts in tv_delay; -1 = not managed
  std::optional<bool> passwordSet;
  size_t casters = 0;
  bool serverInsecure = false;
  bool whitelistLoaded = true;
};

// tvState (plugins/fleet/protocol/v1/tv.defs.json). Stable for equal views (change detection);
// never contains a password.
std::string StateJson(const View& v);
// `ru tv status` lines.
std::vector<std::string> StatusLines(const View& v);

}  // namespace tv
