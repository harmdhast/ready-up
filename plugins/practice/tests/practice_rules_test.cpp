// Offline tests for plugins/practice/practice_rules.h. ctest `practice_rules`.
#include "practice_feedback.h"
#include "practice_lineups.h"
#include "practice_rules.h"
#include "practice_tools.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace practice;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  for (const char* c : {".rethrow", ".rt", ".savepos", ".loadpos", ".back", ".clear", ".noflash", ".god", ".spawn",
                        ".ctspawn", ".tspawn", ".RT"}) {
    CHECK(IsToolCommand(c) && !IsBotCommand(c));
  }
  for (const char* c : {".bot", ".cbot", ".crouchbot", ".boost", ".crouchboost", ".nobots"}) CHECK(IsBotCommand(c));
  CHECK(!IsToolCommand(".r") && !IsBotCommand(".prac"));

  CHECK(ToolsAllowed(true, "default") && ToolsAllowed(true, ""));
  CHECK(!ToolsAllowed(false, "default"));
  CHECK(!ToolsAllowed(true, "valve") && !ToolsAllowed(true, " VALVE"));

  for (const char* m : {"match_warmup", "match_knife", "match_live", "knife", "postgame"}) CHECK(MatchBlocksPractice(m));
  for (const char* m : {"", "idle", "scrim_warmup", "practice"}) CHECK(!MatchBlocksPractice(m));

  CHECK(ShouldAutoEnter(true, false, "idle") && ShouldAutoEnter(true, false, ""));
  CHECK(!ShouldAutoEnter(true, true, "practice"));
  CHECK(!ShouldAutoEnter(false, false, "idle"));
  CHECK(!ShouldAutoEnter(true, false, "match_live"));

  CHECK(ParseBool("1", false) && !ParseBool("off", true) && ParseBool("?", true));

  // Feedback lines.
  CHECK(std::string(HitgroupName(1)) == "head" && std::string(HitgroupName(0)).empty());
  CHECK(FormatHit("Simpert", "Bot Adam", 27, 8, 1, "ak47", 73) ==
        "Simpert hit Bot Adam: -27 hp, -8 armor (head, ak47), 73 hp left");
  CHECK(FormatHit("", "Simpert", 12, 0, 0, "", 88) == "Simpert took -12 hp, 88 hp left");
  CHECK(FormatHit("Simpert", "Simpert", 40, 0, 0, "hegrenade", 60) == "Simpert took -40 hp (hegrenade), 60 hp left");
  CHECK(FormatBlind("Bot Adam", "Simpert", 2.44) == "Bot Adam flashed 2.4 s by Simpert");
  CHECK(FormatBlind("Simpert", "Simpert", 1.0) == "Simpert flashed 1.0 s by themselves");
  CHECK(IsSummedWeapon("hegrenade") && IsSummedWeapon("inferno") && !IsSummedWeapon("ak47"));

  GrenadeDamage gd;
  gd.Add(10.0, "Simpert", "inferno", "Bot Adam", 8);
  gd.Add(10.3, "Simpert", "inferno", "Bot Adam", 8);
  gd.Add(10.4, "Simpert", "hegrenade", "Bot Ben", 41);
  gd.Add(10.4, "Simpert", "hegrenade", "Bot Adam", 57);
  CHECK(gd.Flush(10.9, 1.0).empty());  // still burning / just exploded
  const auto lines = gd.Flush(11.5, 1.0);
  CHECK(lines.size() == 2);
  CHECK(lines.size() == 2 && lines[0] == "HE by Simpert: 98 total (Bot Adam -57, Bot Ben -41)");
  CHECK(lines.size() == 2 && lines[1] == "Fire by Simpert: 16 total (Bot Adam -16)");
  CHECK(gd.Flush(20.0, 1.0).empty());

  // ME extras (practice_tools.h).
  for (const char* c : {".throw", ".last", ".lastindex", ".throwidx", ".throwindex", ".delay", ".rethrowsmoke", ".throwdecoy",
                        ".impacts", ".traj", ".pip", ".solid", ".break", ".timer", ".bestspawn", ".worsttspawn",
                        ".bestctspawn", ".showspawns", ".hidespawns"}) {
    CHECK(IsToolCommand(c) && !IsBotCommand(c));
  }
  CHECK(!IsToolCommand(".dryrun"));  // admin command, not a tool
  CHECK(GrenadeKind("smokegrenade") == "smoke" && GrenadeKind("weapon_flashbang") == "flash");
  CHECK(GrenadeKind("hegrenade") == "hegrenade" && GrenadeKind("incgrenade") == "molotov");
  CHECK(GrenadeKind("molotov") == "molotov" && GrenadeKind("decoy") == "decoy" && GrenadeKind("ak47").empty());
  CHECK(TypedRethrowKind(".rethrowsmoke") == "smoke" && TypedRethrowKind(".throwflash") == "flash");
  CHECK(TypedRethrowKind(".thrownade") == "hegrenade" && TypedRethrowKind(".rethrowgrenade") == "hegrenade");
  CHECK(TypedRethrowKind(".throwmolotov") == "molotov" && TypedRethrowKind(".RETHROWDECOY") == "decoy");
  CHECK(TypedRethrowKind(".rethrow").empty() && TypedRethrowKind(".throwidx").empty() && TypedRethrowKind(".last").empty());

  ThrowHistory hist(3);
  CHECK(hist.Count() == 0 && !hist.Last() && !hist.At(1));
  for (int i = 1; i <= 4; ++i) {
    Throw t;
    t.pos.x = static_cast<float>(i);
    t.kind = i % 2 ? "smoke" : "flash";
    hist.Add(t);
  }
  CHECK(hist.Count() == 3);                       // capped: throw 1 dropped
  CHECK(hist.At(1) && hist.At(1)->pos.x == 2.f);  // oldest first
  CHECK(hist.At(3) && hist.At(3)->pos.x == 4.f && !hist.At(4) && !hist.At(0));
  CHECK(hist.LastOfKind("smoke") && hist.LastOfKind("smoke")->pos.x == 3.f);
  CHECK(!hist.LastOfKind("decoy"));
  hist.Last()->delay = 1.5f;
  CHECK(hist.At(3)->delay == 1.5f);

  // Projectiles and the grenade_thrown entry they belong to.
  CHECK(ProjectileKind("smokegrenade_projectile") == "smoke" && ProjectileKind("flashbang_projectile") == "flash");
  CHECK(ProjectileKind("hegrenade_projectile") == "hegrenade" && ProjectileKind("molotov_projectile") == "molotov");
  CHECK(ProjectileKind("decoy_projectile") == "decoy" && ProjectileKind("inferno").empty() &&
        ProjectileKind("weapon_smokegrenade").empty());
  ThrowHistory lh;
  for (int i = 0; i < 3; ++i) {
    Throw t;
    t.kind = i == 1 ? "flash" : "smoke";
    t.at = 10.0 + i;  // smoke at 10, flash at 11, smoke at 12
    lh.Add(t);
  }
  CHECK(lh.AwaitingLaunch("smoke", 11.5) == lh.At(3));  // only the newest smoke is in the window
  CHECK(lh.AwaitingLaunch("smoke", 9.0) == lh.At(1));   // both are: the oldest one first
  lh.AwaitingLaunch("smoke", 9.0)->launch.set = true;
  CHECK(lh.AwaitingLaunch("smoke", 9.0) == lh.At(3));   // then the next
  lh.AwaitingLaunch("smoke", 9.0)->launch.set = true;
  CHECK(!lh.AwaitingLaunch("smoke", 0.0));
  CHECK(lh.AwaitingLaunch("flash", 11.0) == lh.At(2) && !lh.AwaitingLaunch("decoy", 0.0));

  int n = 0;
  CHECK(ParsePositiveInt("3", &n) && n == 3);
  CHECK(!ParsePositiveInt("0", &n) && !ParsePositiveInt("-1", &n) && !ParsePositiveInt("2x", &n) && !ParsePositiveInt("", &n));
  float d = 0;
  CHECK(ParseDelaySeconds("1.25", &d) && d == 1.25f);
  CHECK(!ParseDelaySeconds("0", &d) && !ParseDelaySeconds("-2", &d) && !ParseDelaySeconds("abc", &d) &&
        !ParseDelaySeconds("61", &d) && !ParseDelaySeconds("nan", &d));

  const std::vector<Vec3f> pts = {{0, 0, 0}, {100, 0, 0}, {-500, 0, 0}};
  CHECK(ClosestIndex(pts, {90, 0, 0}) == 1 && FarthestIndex(pts, {90, 0, 0}) == 2);
  CHECK(ClosestIndex({}, {0, 0, 0}) == -1 && FarthestIndex({}, {0, 0, 0}) == -1);

  CHECK(NextSolidValue(0) == 2 && NextSolidValue(1) == 2 && NextSolidValue(2) == 1);
  CHECK(FormatTimerSeconds(12.345) == "12.35" || FormatTimerSeconds(12.345) == "12.34");
  CHECK(FormatTimerSeconds(-1) == "0.00");

  // Lineup library (practice_lineups.h).
  for (const char* c : {".savenade", ".sn", ".loadnade", ".ln", ".listnades", ".lin", ".deletenade", ".delnade", ".dn",
                        ".importnade", ".in"}) {
    CHECK(IsToolCommand(c));
  }
  // .fas / .watchme are tools too, so `ru practice as <slot> .fas` runs them (bot tests).
  CHECK(IsToolCommand(".fas") && IsToolCommand(".watchme") && !IsBotCommand(".fas"));
  // `ru practice as N` (M1 play-test): a registry entry the engine no longer has (a kicked bot still
  // listed) is never picked; only a connected controller in that slot counts.
  CHECK(SlotEntryUsable(SlotStateFrom(true, true, true, 0)));                        // connected
  CHECK(!SlotEntryUsable(SlotStateFrom(true, false, false, 0)));                     // no controller
  CHECK(!SlotEntryUsable(SlotStateFrom(true, true, true, 4)));                       // disconnected
  CHECK(!SlotEntryUsable(SlotStateFrom(true, true, true, 3)));                       // disconnecting
  CHECK(!SlotEntryUsable(SlotStateFrom(true, true, true, 1)));                       // still connecting
  CHECK(SlotEntryUsable(SlotStateFrom(true, true, false, 4)));                       // field unknown: controller counts
  CHECK(SlotEntryUsable(SlotStateFrom(false, false, false, 0)));                     // entity system not ready
  CHECK(SlotStateFrom(true, false, true, 0) == SlotEngineState::kEmpty);
  CHECK(ValidLineupName("mid_window-1.a") && !ValidLineupName("") && !ValidLineupName("a b") &&
        !ValidLineupName("../x/y") && !ValidLineupName(std::string(33, 'a')));
  CHECK(MapFileStem("de_Dust2") == "de_dust2" && MapFileStem("workshop/123/de x") == "workshop_123_de_x");

  CHECK(NearestName("xbox", {"window", "xbox_smoke", "ct"}) == "xbox_smoke");
  CHECK(NearestName("CT", {"window", "ct", "ctx"}) == "ct");       // exact, case-insensitive
  CHECK(NearestName("w", {"a_site", "window", "wall"}) == "window");  // one letter: first starting with it
  CHECK(NearestName("zzz", {"window"}).empty() && NearestName("", {"a"}).empty() && NearestName("a", {}).empty());
  CHECK(NearestName("nothingthere", {"window"}).empty());  // one shared bigram (0.125) is no match
  CHECK(NearestName("win", {"window", "xbox"}) == "window");
  CHECK(DiceCoefficient("night", "nacht") > 0.2 && DiceCoefficient("night", "nacht") < 0.3);

  std::string nm;
  Vec3f p, a;
  CHECK(ParseImportCode("window -1234.5 200 -10.25 5.5 90 0", &nm, &p, &a) && nm == "window" && p.x == -1234.5f &&
        p.z == -10.25f && a.y == 90.f);
  CHECK(ParseImportCode("xbox 1, 2, 3 4, 5, 6", &nm, &p, &a) && p.y == 2.f && a.x == 4.f);  // ME codes with commas
  CHECK(!ParseImportCode("window 1 2 3 4 5", &nm, &p, &a) && !ParseImportCode("window 1 2 3 4 5 x", &nm, &p, &a) &&
        !ParseImportCode("bad/name 1 2 3 4 5 6", &nm, &p, &a) && !ParseImportCode("far 99999 0 0 0 0 0", &nm, &p, &a));

  LineupBook book;
  Lineup win;
  win.name = "window";
  win.pos = {-1234.5f, 200.f, -10.25f};
  win.ang = {5.5f, 90.f, 0.f};
  win.kind = "smoke";
  win.desc = "jump throw";
  CHECK(ExportCode(win) == "window -1234.50 200.00 -10.25 5.50 90.00 0.00");
  CHECK(book.Add("7656", win) && !book.Add("7656", win));  // ME: use .delnade first
  Lineup glob = win;
  glob.name = "xbox";
  CHECK(book.Add(kGlobalOwner, glob));
  CHECK(book.Count() == 2 && book.Find("7656", "window") && !book.Find("7656", "xbox"));
  std::string from;
  CHECK(book.Load("7656", "win", &from) && from == "7656");           // own first
  CHECK(book.Load("7656", "xbo", &from) && from == kGlobalOwner);     // then global
  CHECK(book.Load("other", "window", &from) == nullptr);              // someone else's is not theirs
  CHECK(book.List("7656", "").size() == 1 && book.List("7656", "WIN").size() == 1 && book.List("7656", "zz").empty());

  // JSON round trip, and malformed entries skipped.
  const readyup::status::Json doc = book.ToJson();
  LineupBook back;
  int skipped = -1;
  back.FromJson(doc, &skipped);
  CHECK(skipped == 0 && back.Count() == 2);
  const Lineup* w = back.Find("7656", "window");
  CHECK(w && w->kind == "smoke" && w->desc == "jump throw" && w->pos.x == -1234.5f && w->ang.y == 90.f);
  readyup::status::Json bad;
  CHECK(readyup::status::Json::Parse(
      R"({"version":1,"lineups":{"1":{"ok":{"pos":[1,2,3],"ang":[0,0,0]},"nopos":{"ang":[0,0,0]},)"
      R"("short":{"pos":[1,2],"ang":[0,0,0]},"bad name":{"pos":[1,2,3],"ang":[0,0,0]}},"2":[]}})",
      &bad));
  back.FromJson(bad, &skipped);
  CHECK(back.Count() == 1 && back.Find("1", "ok") && skipped == 4);
  CHECK(book.Remove("7656", "window") && !book.Remove("7656", "window") && book.Count() == 1);

  CHECK(ValidHomeCvar("sv_airaccelerate 150") && ValidHomeCvar("sv_staminajumpcost 0") &&
        ValidHomeCvar("mp_roundtime 60") && ValidHomeCvar("sv_gravity 800.5") && ValidHomeCvar("sv_x -1"));
  for (const char* bad : {"", "sv_cheats", "quit 1", "sv_a 1; quit", "sv_a 1 2", "rcon_password 1", "sv_A 1",
                          "sv_a x", "sv_a 1.2.3", "mp_ 1"}) {
    CHECK(!ValidHomeCvar(bad));
  }

  std::printf("practice_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
