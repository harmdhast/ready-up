#pragma once

// The GOTV broadcast profile (docs/design/tv-plugin.md): delay, client limit, chat / voice relay
// and insecure observers. Shared by the tv plugin (plugins/tv, which applies it) and the match
// plugin (which validates `match.assign` config.rules.broadcast with the same rules, so a bad block
// refuses the whole assign). Engine-free; ctest `tv_rules`.

#include "readyup/minijson.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace readyup::broadcast {

enum class Key { Delay = 0, Insecure, MaxClients, TextChat, Voice };
constexpr int kKeyCount = 5;
const std::vector<Key>& AllKeys();

// Canonical name ("delay_seconds", ...).
const char* KeyName(Key k);
// Canonical names and the short aliases (delay, insecure, maxclients, textchat, voice).
bool ParseKeyName(const std::string& s, Key* out);

constexpr int kMaxDelaySeconds = 960;  // the match ruleset's tv_delay range (ruleset.cpp)
constexpr int kMaxClients = 255;
constexpr int kValveDelaySeconds = 105;  // ruleset "valve" (Premier)

struct Profile {
  std::optional<int> delay_seconds;
  std::optional<bool> allow_insecure_observers;
  std::optional<int> max_clients;
  std::optional<bool> relay_text_chat;
  std::optional<bool> relay_voice;

  bool Empty() const;
  bool Has(Key k) const;
  void Clear(Key k);
  bool operator==(const Profile& o) const;
  bool operator!=(const Profile& o) const { return !(*this == o); }
};

// A JSON object of profile keys (canonical names only). Unknown keys, wrong types and out-of-range
// values are refused. A null value is refused unless `cleared` is given, which then gets the key
// (tv.set: null = back to the default).
bool ParseProfile(const minijson::Value& obj, Profile* out, std::string* err, std::vector<Key>* cleared = nullptr);
bool ParseProfileText(const std::string& json, Profile* out, std::string* err, std::vector<Key>* cleared = nullptr);

// One key from a console / chat value: integers, 1/0/on/off/true/false/yes/no; "default" or "-"
// clears the key.
bool SetFromText(Profile* p, Key k, const std::string& value, std::string* err);

// "105" / "1" / "0", or "" when unset.
std::string ValueText(const Profile& p, Key k);

// over's set keys win.
Profile Overlay(const Profile& base, const Profile& over);

// {"delay_seconds":105,"allow_insecure_observers":true} (set keys only, canonical order).
std::string ProfileJson(const Profile& p);

// The lowest tv_delay the match itself asks for: max of config.cvars.tv_delay,
// config.rules.overrides.tv_delay and 105 under rules.ruleset "valve" (0 = none). `config` is the
// match.assign config object.
int DelayFloor(const minijson::Value& config);

// config.rules.broadcast (absent = an empty profile).
bool ProfileFromAssign(const minijson::Value& config, Profile* out, std::string* err);

// config.cvars.tv_password is a non-empty string. The value is never copied.
bool PasswordInAssign(const minijson::Value& config);

// match.assign config: rules.broadcast valid, its delay not below DelayFloor, and no key that
// disagrees with config.cvars (e.g. broadcast.max_clients 8 and cvars.tv_maxclients 16).
bool ValidateAssignConfig(const minijson::Value& config, std::string* err);
bool ValidateAssignConfigText(const std::string& configJson, std::string* err);

// config.cvars entries for cvars the profile owns (tv_secure_bypass "1", ...), as the text the match
// plugin applies. The platform sends tv_secure_bypass 1 + sv_deltaticks_enforce 0 next to
// allow_insecure_observers: the match plugin applies them too, so tv.so must neither take them as
// a baseline nor set anything else for them during that match.
std::vector<std::pair<std::string, std::string>> OwnedCvarsInAssign(const minijson::Value& config);

// The same match cvars as profile keys (tv_secure_bypass -> allow_insecure_observers, tv_maxclients,
// tv_relaytextchat, tv_relayvoice; tv_delay is the floor instead), so the server's own settings yield
// to them for that match instead of fighting the match plugin on every re-apply.
Profile ProfileFromMatchCvars(const std::vector<std::pair<std::string, std::string>>& matchCvars);

// The first cvar where `eff`'s DesiredCvars disagree with `matchCvars` ("" = none).
std::string ConflictWithMatchCvars(const Profile& eff, int delayFloor,
                                   const std::vector<std::pair<std::string, std::string>>& matchCvars);

// Every cvar the profile can set.
const std::vector<std::string>& OwnedCvars();

// The cvars `eff` asks for, in OwnedCvars order. delay_seconds is raised to `delayFloor`
// (*effectiveDelay = what goes into tv_delay, -1 when tv_delay is not set). allow_insecure_observers
// true sets tv_secure_bypass 1 and sv_deltaticks_enforce 0 together; false sets
// tv_secure_bypass 0 only (sv_deltaticks_enforce goes back to its baseline).
std::vector<std::pair<std::string, std::string>> DesiredCvars(const Profile& eff, int delayFloor,
                                                              int* effectiveDelay = nullptr);

}  // namespace readyup::broadcast
