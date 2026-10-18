// Offline tests for readyup/roster_names_rules.h (roster display names) and the match config side:
// fleet match.assign players[].name / rules.roster_names -> roster_names / roster_names_rule.
// ctest `match_roster_names`.
#include "readyup/fleet_state.h"
#include "readyup/match_config_parser.h"
#include "readyup/roster_names_rules.h"
#include "readyup/status_snapshot.h"

#include <cstdio>
#include <string>

using namespace readyup;
namespace rn = readyup::roster_names;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static void TestSanitize() {
  CHECK(rn::Sanitize("Thomas S.") == "Thomas S.");
  // Chat color bytes, other control bytes and DEL go.
  CHECK(rn::Sanitize("\x02Red\x10Name\x7F") == "RedName");
  CHECK(rn::Sanitize(std::string("a\0b", 3)) == "ab");
  // Log-line delimiters go ("Name<2><[U:1:x]><CT>").
  CHECK(rn::Sanitize("\"Bob<3><[U:1:1]>\"") == "Bob3[U:1:1]");
  // Blanks collapsed and trimmed; tabs / newlines are blanks.
  CHECK(rn::Sanitize("  Ola \t\n  Nordmann  ") == "Ola Nordmann");
  CHECK(rn::Sanitize("   ").empty());
  CHECK(rn::Sanitize("\x01\x02").empty());
  // UTF-8 kept; invalid bytes dropped (stray continuation, overlong, truncated sequence).
  CHECK(rn::Sanitize("Bj\xC3\xB8rn \xE6\x97\xA5") == "Bj\xC3\xB8rn \xE6\x97\xA5");
  CHECK(rn::Sanitize("a\x80" "b\xC0\xAF" "c\xE6\x97") == "abc");
  CHECK(rn::Sanitize("\xF0\x9F\x98\x80!") == "\xF0\x9F\x98\x80!");
  CHECK(rn::Sanitize("\xED\xA0\x80x") == "x");  // UTF-16 surrogate
  // At most 127 bytes (char[128]), cut on a character boundary.
  CHECK(rn::Sanitize(std::string(300, 'a')).size() == rn::kMaxNameBytes);
  std::string longUtf8 = std::string(126, 'a') + "\xC3\xB8";
  CHECK(rn::Sanitize(longUtf8) == std::string(126, 'a'));
  CHECK(rn::Sanitize("abcdef", 4) == "abcd");
  CHECK(rn::Sanitize("ab cd", 3) == "ab");  // no trailing blank after the cut
}

static void TestEnabled() {
  CHECK(rn::Enabled(-1, true, true));
  CHECK(!rn::Enabled(-1, false, true));
  CHECK(rn::Enabled(1, false, true));   // the match rule wins over the server setting
  CHECK(!rn::Enabled(0, true, true));
  CHECK(!rn::Enabled(1, true, false));  // nobody named: nothing to do
}

static void TestDesired() {
  const std::unordered_map<uint64_t, std::string> names = {
      {76561198000000001ull, "Thomas S."}, {76561198000000002ull, "\x03\x04"}, {76561198000000003ull, " Kari "}};
  CHECK(rn::DesiredName(76561198000000001ull, names) == std::optional<std::string>("Thomas S."));
  CHECK(!rn::DesiredName(76561198000000002ull, names));  // sanitizes to nothing: own name stays
  CHECK(rn::DesiredName(76561198000000003ull, names) == std::optional<std::string>("Kari"));
  CHECK(!rn::DesiredName(76561198000000009ull, names));  // not on the roster
  CHECK(!rn::DesiredName(0, names));
}

static void TestDecide() {
  rn::PlayerState st;
  std::string out;
  const std::optional<std::string> want("Thomas S.");
  // Connect: Steam name on the controller -> write, Steam name remembered.
  CHECK(rn::Decide("Snusmumrikken", want, &st, &out) == rn::Action::Write && out == "Thomas S.");
  CHECK(st.original == "Snusmumrikken" && st.written == "Thomas S.");
  // Next checks: nothing to do.
  CHECK(rn::Decide("Thomas S.", want, &st, &out) == rn::Action::None);
  // The engine put a (new) Steam name back (name change / map change): write again, remember it.
  CHECK(rn::Decide("Snusmumrikken2", want, &st, &out) == rn::Action::Write && out == "Thomas S.");
  CHECK(st.original == "Snusmumrikken2");
  // Unassign / turned off: our name still there -> their own name back, state cleared.
  CHECK(rn::Decide("Thomas S.", std::nullopt, &st, &out) == rn::Action::Restore && out == "Snusmumrikken2");
  CHECK(st.written.empty());
  CHECK(rn::Decide("Snusmumrikken2", std::nullopt, &st, &out) == rn::Action::None);

  // Unassign after the engine already put another name there: left alone.
  rn::PlayerState st2;
  CHECK(rn::Decide("Steam", want, &st2, &out) == rn::Action::Write);
  CHECK(rn::Decide("Other", std::nullopt, &st2, &out) == rn::Action::None && st2.written.empty());

  // Steam name already equals the roster name: no write, nothing to restore later.
  rn::PlayerState st3;
  CHECK(rn::Decide("Thomas S.", want, &st3, &out) == rn::Action::None);
  CHECK(rn::Decide("Thomas S.", std::nullopt, &st3, &out) == rn::Action::None);

  // Never renamed: nothing.
  rn::PlayerState st4;
  CHECK(rn::Decide("Steam", std::nullopt, &st4, &out) == rn::Action::None);

  // Roster name changes between matches: written over our old one, original kept.
  rn::PlayerState st5;
  CHECK(rn::Decide("Steam", want, &st5, &out) == rn::Action::Write);
  CHECK(rn::Decide("Thomas S.", std::optional<std::string>("Tom"), &st5, &out) == rn::Action::Write && out == "Tom");
  CHECK(st5.original == "Steam");
}

static void TestMatchConfig() {
  using status::Json;
  std::string err;
  Json cfg;
  CHECK(Json::Parse(R"({"num_maps":1,"maps":[{"number":1,"name":"de_nuke","sides":"team1_ct"}],
      "team1":{"id":"a","name":"Alpha","players":[{"steamid64":"76561198000000011","name":"Thomas S."},
                                              {"steamid64":"76561198000000031","name":"Coach A","role":"coach"}]},
      "team2":{"id":"b","name":"Bravo","players":[{"steamid64":"76561198000000012","name":"Kari N.","role":"sub"}]},
      "rules":{}})",
                    &cfg));
  auto ctx = ParseWebhookMatchContextFromJson(fleetstate::AssignToMatConfig("m-1", cfg, nullptr).Dump(), &err);
  CHECK(ctx.has_value());
  if (ctx) {
    CHECK(ctx->roster_names_rule == -1);  // absent: the server setting decides
    CHECK(ctx->roster_names.count(76561198000000011ull) && ctx->roster_names.at(76561198000000011ull) == "Thomas S.");
    CHECK(ctx->roster_names.count(76561198000000012ull) && ctx->roster_names.at(76561198000000012ull) == "Kari N.");
    CHECK(ctx->roster_names.count(76561198000000031ull) && ctx->roster_names.at(76561198000000031ull) == "Coach A");
  }
  for (const char* v : {"false", "true"}) {
    Json c2;
    CHECK(Json::Parse(std::string(R"({"num_maps":1,"maps":[{"number":1,"name":"de_nuke"}],
        "team1":{"name":"A","players":[{"steamid64":"76561198000000011","name":"x"}]},
        "team2":{"name":"B","players":[]},"rules":{"roster_names":)") + v + "}}",
                      &c2));
    auto c = ParseWebhookMatchContextFromJson(fleetstate::AssignToMatConfig("m-2", c2, nullptr).Dump(), &err);
    CHECK(c && c->roster_names_rule == (std::string(v) == "true" ? 1 : 0));
  }
  // MAT config directly.
  auto mat = ParseWebhookMatchContextFromJson(
      R"({"matchid": 9, "roster_names": false, "team1": {"players": {"76561198000000011": "x"}}, "team2": {}})", &err);
  CHECK(mat && mat->roster_names_rule == 0);
}

int main() {
  TestSanitize();
  TestEnabled();
  TestDesired();
  TestDecide();
  TestMatchConfig();
  std::printf("roster_names_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
