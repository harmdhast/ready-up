// Offline tests for plugins/whitelist/whitelist_rules.h: whitelist.json and who gets kicked.
// ctest `whitelist_rules`.
#include "whitelist_rules.h"

#include <cstdio>

using namespace whitelist;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  constexpr uint64_t A = 76561198000000001ull, B = 76561198000000002ull;
  bool ok = false;
  State s = ParseState("", &ok);
  CHECK(ok && !s.enabled && s.steamids.empty());
  s = ParseState(R"({"version": 1, "enabled": true, "steamids": ["76561198000000001", "nope", 5, "76561198000000002"]})", &ok);
  CHECK(ok && s.enabled && s.steamids.size() == 2 && s.steamids.count(A) && s.steamids.count(B));
  s = ParseState("{broken", &ok);
  CHECK(!ok && !s.enabled);

  State w;
  w.enabled = true;
  w.steamids = {A};
  const State back = ParseState(StateJson(w), &ok);
  CHECK(ok && back.enabled && back.steamids == w.steamids);
  CHECK(StateJson(State{}) == "{\n  \"version\": 1,\n  \"enabled\": false,\n  \"steamids\": []\n}\n");

  CHECK(ParseSteamId64("76561198000000001") == A);
  CHECK(ParseSteamId64("7656119800000000") == 0 && ParseSteamId64("12345678901234567") == 0 && ParseSteamId64("x") == 0);

  // Who gets kicked.
  CHECK(ShouldKick(w, "idle", B, false, false));
  CHECK(ShouldKick(w, "practice", B, false, false) && ShouldKick(w, "scrim_warmup", B, false, false));
  CHECK(!ShouldKick(w, "idle", A, false, false));  // on the list
  CHECK(!ShouldKick(w, "idle", B, false, true));   // admin
  CHECK(!ShouldKick(w, "idle", 0, true, false));   // bot
  for (const char* m : {"match_warmup", "match_knife", "match_live", "postgame"}) CHECK(!ShouldKick(w, m, B, false, false));
  State off = w;
  off.enabled = false;
  CHECK(!ShouldKick(off, "idle", B, false, false));

  // Guests (set_guests, tv.so's casters): never kicked, per owner, replaced as a whole.
  CHECK(!ShouldKick(w, "idle", B, false, false, /*isGuest=*/true));
  Guests g;
  bool changed = false;
  CHECK(g.Set("tv", {B}, &changed) && changed && g.Is(B) && !g.Is(A) && g.Count() == 1);
  CHECK(g.Set("tv", {B}, &changed) && !changed);  // the same list again: no change
  CHECK(g.Set("other", {A, B}, &changed) && changed && g.Count() == 2);
  CHECK(g.Set("tv", {}, &changed) && changed && g.Is(B) && g.Is(A));  // still other's guest
  CHECK(g.Set("other", {}, &changed) && changed && !g.Is(A) && !g.Is(B) && g.Count() == 0);
  CHECK(g.Set("tv", {}, &changed) && !changed);
  CHECK(!g.Set("TV!", {A}, &changed) && !g.Set("", {A}, &changed) && !g.Is(A));
  CHECK(g.Set("tv", {A, 12345}, &changed) && g.Count() == 1);  // not a SteamID64: dropped
  CHECK(!g.Set("tv", std::vector<uint64_t>(kMaxGuestsPerOwner + 1, A), &changed) && g.Count() == 1);
  std::printf("whitelist_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
