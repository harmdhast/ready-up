#pragma once

// Auto Tournament license keys (offline check). Nothing here ever blocks, disables or degrades
// anything: a problem is a warning in the console, never a lockout, and a missing key is fine
// (free non-commercial use). Port of scripts/license-verify.mjs in Auto-Tournament/website
// (README "License keys"):
//
//   ATL1.<base64url(JSON payload)>.<base64url(Ed25519 signature over "ATL1.<payload>")>
//
// Pure: no engine, no config, no clock (the caller passes today's date). Ed25519 is Monocypher's
// (third_party/monocypher). The public keys are in license_keys.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace readyup::license {

// A signing key: kid (first 16 base64url chars of SHA-256 of the raw key) and the raw 32-byte
// Ed25519 public key as base64url.
struct PublicKey {
  const char* kid;
  const char* x;
};

// The keys this build trusts (license_keys.cpp). Rotation adds entries; old ones stay.
const std::vector<PublicKey>& EmbeddedPublicKeys();

constexpr const char* kTokenPrefix = "ATL1";
constexpr size_t kMaxTokenLength = 4096;

enum class Status { Ok, Warning, Invalid };

// Same codes as the website's verifier. Invalid: malformed, unknown_kid, bad_signature,
// unsupported_version. Warning: updates_expired, period_ended, period_not_started.
// (Ready Up can't see the fleet size or be the platform: no too_many_servers / wrong_product.)
struct Issue {
  std::string code;
  std::string message;
};

// The signed payload (only filled for a valid key).
struct Payload {
  std::string kid;
  std::string id;
  std::string customer;
  std::string licensee;  // empty when not set
  std::string product;   // servers | platform
  std::string pack;      // S | M | L
  int64_t max_servers = 0;
  std::string kind;  // month | year | founder (event: older keys)
  std::string issued_at;
  std::string updates_until;  // YYYY-MM-DD, inclusive
  std::string valid_from;     // YYYY-MM-DD, event licenses only
  std::string valid_to;
  // Only on a lease: the license's current terms from a check-in (csm writes it as
  // readyup_license_lease). Used for limits next to the key, never as the key.
  bool lease = false;
};

struct Result {
  Status status = Status::Invalid;
  std::vector<Issue> issues;  // warnings, or the one reason it is invalid
  Payload license;
  bool valid() const { return status != Status::Invalid; }
};

struct Options {
  // Release date of this build's x.y.0 (YYYY-MM-DD). The build is covered when
  // line_date <= updates_until.
  std::string line_date;
  // Today, UTC (YYYY-MM-DD), for an event license's window.
  std::string today;
  // nullptr = EmbeddedPublicKeys().
  const std::vector<PublicKey>* keys = nullptr;
};

Result Verify(const std::string& token, const Options& opts);

// True for a real calendar day written YYYY-MM-DD.
bool IsDate(const std::string& s);

// Base64url without padding (A-Z a-z 0-9 - _), decoded the way Node's Buffer.from(s, 'base64url')
// does it (a trailing single character is ignored). False if a character is outside the alphabet.
bool Base64UrlDecode(const std::string& in, std::vector<uint8_t>* out);

// Today's date in UTC, YYYY-MM-DD.
std::string TodayUtc();

// ---- paid license standing (pricing v4) ------------------------------------------------

// Where a paid license stands, the same rule as the platform and csm (api/src/services/license/
// gate.ts, csm license_enforce.go). Only a genuine key is ever enforced; no key (free use) or a
// key that doesn't verify never stops anything.
struct Standing {
  bool paid = false;
  // "free" | "invalid" | "active" | "past_due" | "expired"
  std::string status = "free";
  // Why past due / expired: "unpaid" | "replaced" | "in_use_elsewhere" (else empty).
  std::string reason;
  std::string stops_on;  // YYYY-MM-DD: the first day it no longer works, or empty
  int64_t max_servers = 0;
};

constexpr int kGraceDays = 14;

// key: the license key. lease: the license's current terms (a signed lease from the check-in, may
// be empty). state / state_stops_on: what the license server last said ("active", "past_due",
// "expired", "replaced", "in_use_elsewhere"; may be empty). today: YYYY-MM-DD (UTC).
Standing StandingFor(const std::string& key, const std::string& lease, const std::string& state,
                     const std::string& state_stops_on, const std::string& today,
                     const std::vector<PublicKey>* keys = nullptr);

// YYYY-MM-DD plus `days` (negative to go back). Empty for a bad date.
std::string AddDays(const std::string& day, int days);

// ---- presentation (console / chat text) -------------------------------------------------

// Free-use line for a server without a key.
constexpr const char* kNoKeyLine =
    "No commercial license key set (free for non-commercial use: autotournament.gg/pricing)";

// One console line for a checked key (never contains the key itself):
//   "License: Example LAN · Servers M (15 servers) · yearly, updates until 2027-10-01 · valid (license L-1)"
//   "License warning: updates for this version line ended 2026-10-16 (license L-1, Example LAN)"
//   "License warning: the license key is not valid: ... Ready Up runs as normal."
std::string ConsoleLine(const Result& r);

// "Licensed to <licensee>" for a valid key that names a licensee, else "".
std::string PlayerLine(const Result& r);

// Control characters dropped and cut to max_bytes at a UTF-8 boundary: payload strings are
// signed, but they still go to a console and to chat.
std::string Printable(const std::string& s, size_t max_bytes = 80);

}  // namespace readyup::license
