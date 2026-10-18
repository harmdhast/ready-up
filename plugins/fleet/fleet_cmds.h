// The platform commands fleet.so runs itself (FLEET.md §7.4), so a server without the match plugin
// (a practice server: core + fleet + practice) still takes them: plugins.set, whitelist.set,
// practice.set, say and tv.set (the tv plugin validates its own args, readyup.tv.v1). Every other cmd goes to the plugins that registered for "cmd" (the match
// plugin); without one, fleet.so answers it as unsupported. No engine calls here: parsing and the
// cmd.result payload (fleet_plugin.cpp runs them).
#pragma once

#include "fleet_json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fleet::cmds {

// plugins.set, whitelist.set, practice.set, say, tv.set.
bool OwnedByFleet(const std::string& name);

// plugins.set {enable?: [name], disable?: [name]}: [a-z0-9_-]{1,32}, at most 16 each, not both
// lists, fleet cannot be disabled (the link runs in it).
bool ParsePluginsSet(const json::Value& args, std::vector<std::string>* enable, std::vector<std::string>* disable,
                     std::string* err);
// whitelist.set {enabled, steamids?: [SteamID64 string]} (at most 1000).
bool ParseWhitelistSet(const json::Value& args, bool* enabled, std::vector<uint64_t>* steamids, std::string* err);
// practice.set {on?: bool, always?: bool}: at least one. *on / *always: -1 = not given, else 0 / 1.
// practice.set {on?, always?, home_map?}: -1 = not given. home_map (a map name, workshop id or link;
// "" clears) sets *hasHomeMap; essentials.so checks the map itself.
bool ParsePracticeSet(const json::Value& args, int* on, int* always, std::string* err, bool* hasHomeMap = nullptr,
                      std::string* homeMap = nullptr, bool* hasHomeCvars = nullptr,
                      std::vector<std::string>* homeCvars = nullptr);
// say: control characters dropped, at most `maxBytes` bytes (cut on a UTF-8 boundary).
constexpr size_t kSayLineBytes = 190;
constexpr size_t kSayMaxLines = 3;
std::string SanitizeSay(const std::string& text, size_t maxBytes = kSayLineBytes);
// say {text}: sanitized, then split into at most kSayMaxLines chat lines of at most kSayLineBytes
// bytes, at a space where there is one (never inside a UTF-8 character). Longer text is cut.
// Empty = nothing to say.
std::vector<std::string> SplitSay(const std::string& text);
// One chat line of an admin's say (as_admin, optional from): " \x07[ADMIN]\x01 <from>: <text>" (from in
// gold), or " \x07[ADMIN]\x01 <text>" without `from`. Sent raw (no Ready Up prefix).
std::string AdminSayLine(const std::string& from, const std::string& text);
// say {team}: "team1" -> 1, "team2" -> 2, absent / "" -> 0, anything else -> -1.
int SayTeam(const std::string& team);
// A say line as the center panel's HTML: escaped, so the text never becomes markup.
std::string SayCenterHtml(const std::string& text);
// say {center_seconds}: 0 = chat only; otherwise 1..15 s on the center panel too.
int SayCenterSeconds(int64_t asked);

struct Result {
  std::string status = "ok";  // ok | rejected | failed | expired
  std::string code, message;
};
Result Ok();
Result Rejected(const std::string& code, const std::string& message);
Result Failed(const std::string& code, const std::string& message);
// The cmd.result payload: {status, error?: {code, message}, audit_id?}.
std::string ResultPayload(const Result& r, const std::string& auditId);

}  // namespace fleet::cmds
