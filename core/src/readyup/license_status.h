#pragma once

// The license key status on this server (license.h does the check). A server without a key (free
// non-commercial use) gets one console line and nothing else. A genuine paid key that has stopped
// (unpaid past its 14 days of grace, replaced by a new key a day ago, or in use on another
// Auto Tournament install; license.h StandingFor) refuses new matches until it is sorted
// (StoppedMessage). Running matches are never touched.
//
// CS2 Server Manager writes, next to the key: `readyup_license_lease "ATL1..."` (the license's
// current terms; the key never changes on renewal) and `readyup_license_state "<status>
// [<stops_on>]"` (what the license server last said).
//
// Where the key comes from: the console setting `readyup_license_key "ATL1..."`, from any cfg
// the server runs (server.cfg, or readyup_license.cfg, which CS2 Server Manager writes and execs
// from server.cfg), RCON or the console. Until such a line has been seen, the core reads that
// line from csgo/cfg/readyup_license.cfg itself (so the status is known at load, before
// server.cfg runs). `readyup_show_license 0|1` (default 0) lets players see
// "Licensed to <licensee>" in `.help` and `.ru version`.

#include <string>
#include <vector>

namespace readyup::license {

constexpr const char* kKeySetting = "readyup_license_key";
constexpr const char* kShowSetting = "readyup_show_license";
constexpr const char* kLeaseSetting = "readyup_license_lease";
constexpr const char* kStateSetting = "readyup_license_state";

// Empty while matches may be loaded; otherwise why a new match is refused (a paid license that
// has stopped). Any thread.
std::string StoppedMessage();

// The license answer install.sh records (docs/INSTALL.md "License"): `readyup_license_accepted
// "noncommercial"|"commercial"` and `readyup_license_accepted_at "<ISO time>"` in
// csgo/cfg/ReadyUp/license.cfg (read at load; the same settings also work from any cfg or the
// console). Without an answer the core prints kNotAcceptedLine once and in `ru license`. Warning
// only: nothing is blocked or limited.
constexpr const char* kAcceptedSetting = "readyup_license_accepted";
constexpr const char* kAcceptedAtSetting = "readyup_license_accepted_at";
constexpr const char* kNotAcceptedLine =
    "License terms not accepted: run the installer or set readyup_license_accepted "
    "\"noncommercial\" (or \"commercial\" with a paid license). Nothing is limited.";

// "noncommercial" / "commercial" when an answer is recorded, else "".
std::string AcceptedUse();

// Release date of this build's x.y.0 (CMake READYUP_LINE_DATE; local builds: the build date).
const char* LineDate();

// At load: the status line when csgo/cfg/readyup_license.cfg has a key; otherwise nothing yet
// (the key may still come from server.cfg; LicenseFrame prints the no-key line later).
void LogAtLoad();

// After `ru reload` / `.ru reload`: the status line again when a key is set.
void LogOnReload();

// Every simulating frame (cheap). About 10 s into the first map, with no key seen anywhere:
// the free-use line, once per process; with no license answer recorded: kNotAcceptedLine, once.
void LicenseFrame();

// A console line (AddText). `readyup_license_key ["<key>"]` / `readyup_show_license [0|1]` /
// `readyup_license_accepted [...]` / `readyup_license_accepted_at [...]`:
// applies it (a changed key logs its status line; never the key itself) and returns true so the
// engine does not see an unknown command. Without a value it prints the current state. Any
// other line: false.
bool HandleConsoleLine(const std::string& line);

// `ru license` / `.ru license`: the status line plus where the key came from, this build's line
// date and what players see.
std::vector<std::string> StatusLines();

// "Licensed to <licensee>" when readyup_show_license is 1 and a valid key names a licensee,
// else "". Never anything like "unlicensed".
std::string PlayerLineIfShown();

}  // namespace readyup::license
