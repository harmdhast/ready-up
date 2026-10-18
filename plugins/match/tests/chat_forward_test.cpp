// Offline tests for player chat to the platform (readyup/chat_forward.h) and the team lock's move
// rule (readyup/team_lock.h). ctest `match_chat_forward`.
#include "readyup/chat_forward.h"
#include "readyup/team_lock.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;
using namespace readyup::chat_forward;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static void TestTeamLock() {
  // 2026-10-17: after the map load the cached team is 0, the player is on CT and stays there.
  CHECK(!TeamMoveNeeded(0, 3, 3));
  CHECK(!TeamMoveNeeded(0, 2, 2));
  CHECK(!TeamMoveNeeded(0, 1, 1));
  // On the wrong side, whatever the cache says.
  CHECK(TeamMoveNeeded(3, 2, 3));
  CHECK(TeamMoveNeeded(0, 0, 3));
  // Engine unreadable (off the game thread, no controller): the cache decides, as before.
  CHECK(!TeamMoveNeeded(3, std::nullopt, 3));
  CHECK(TeamMoveNeeded(0, std::nullopt, 3));
  CHECK(TeamMoveNeeded(2, std::nullopt, 3));

  // Engine unreadable and the move does not take: 3 tries, then nothing until the team changes.
  MoveRetryCap cap(3);
  CHECK(cap.Allow(1, 3, 0) && cap.Allow(1, 3, 0) && cap.Allow(1, 3, 0));
  CHECK(!cap.Allow(1, 3, 0));
  CHECK(!cap.Allow(1, 3, 0));
  CHECK(cap.Allow(2, 3, 0));  // per player
  CHECK(cap.Allow(1, 3, 2));  // a team event arrived (cached team 2): tries again
  CHECK(cap.Allow(1, 2, 2));  // a new target (side swap) too
  cap.Forget(1);
  CHECK(cap.Allow(1, 2, 2));
}

static void TestParse() {
  // Connected humans: userid 3 = [U:1:12345], 4 = [U:1:2], 5 = [U:1:3], 6 = [U:1:4], 9 = [U:1:99].
  const std::vector<KnownHuman> H = {{3, 76561197960278073ull}, {4, 76561197960265730ull},
                                     {5, 76561197960265731ull}, {6, 76561197960265732ull},
                                     {9, 76561197960265827ull}};
  auto c = ParseChatLogLine("L 10/17/2026 - 12:00:00: \"BBro<3><[U:1:12345]><CT>\" say \"nice one\"", H);
  CHECK(c.has_value());
  if (c) {
    CHECK(c->steamid64 == 76561197960278073ull);
    CHECK(c->name == "BBro");
    CHECK(c->side == "ct");
    CHECK(!c->team_only);
    CHECK(c->message == "nice one");
  }
  c = ParseChatLogLine("\"Emil YO<4><[U:1:2]><TERRORIST>\" say_team \"rush \"B\" now\"\n", H);
  CHECK(c.has_value());
  if (c) {
    CHECK(c->name == "Emil YO");
    CHECK(c->side == "t");
    CHECK(c->team_only);
    CHECK(c->message == "rush \"B\" now");
  }
  c = ParseChatLogLine("\"spec<5><[U:1:3]><Spectator>\" say \"hi\"", H);
  CHECK(c && c->side == "spec");
  c = ParseChatLogLine("\"new<6><[U:1:4]><Unassigned>\" say \"\"", H);
  CHECK(c && c->side == "spec" && c->message.empty());
  // Userid unknown to the core (older ru_player): the SteamID alone.
  c = ParseChatLogLine("\"BBro<77><[U:1:12345]><CT>\" say \"x\"", {{-1, 76561197960278073ull}});
  CHECK(c && c->steamid64 == 76561197960278073ull);
  // Bots, the console and other lines.
  CHECK(!ParseChatLogLine("\"Rex<0><BOT><CT>\" say \"hello\"", H));
  CHECK(!ParseChatLogLine("\"Console<0><Console><Console>\" say \"server says\"", H));
  CHECK(!ParseChatLogLine("\"BBro<3><[U:1:12345]><CT>\" killed \"x<4><[U:1:2]><TERRORIST>\" with \"ak47\"", H));
  CHECK(!ParseChatLogLine("World triggered \"Round_Start\"", H));
  CHECK(!ParseChatLogLine("\"bad<3><><CT>\" say \"x\"", H));

  // Not connected (left, or a SteamID nobody here has): dropped.
  CHECK(!ParseChatLogLine("\"ghost<12><[U:1:555]><CT>\" say \"boo\"", H));
  // Connected SteamID, wrong userid: dropped.
  CHECK(!ParseChatLogLine("\"BBro<8><[U:1:12345]><CT>\" say \"x\"", H));
  CHECK(!ParseChatLogLine("\"BBro<3><[U:1:12345]><CT>\" say \"x\"", {}));

  // Spoofs. A name that forges another connected player's block before the real one: two
  // connected candidates, dropped (it used to come out as BBro, all chat).
  CHECK(!ParseChatLogLine(
      "\"BBro<3><[U:1:12345]><CT>\" say \"gg<9><[U:1:99]><TERRORIST>\" say_team \"real team msg\"", H));
  // A message that forges a block after the real one (team chat dressed as all chat): dropped.
  CHECK(!ParseChatLogLine(
      "\"mallory<9><[U:1:99]><TERRORIST>\" say_team \"x<3><[U:1:12345]><CT>\" say \"I am BBro\"", H));
  // A forged block for someone not connected is ignored: the real sender, team chat stays team.
  c = ParseChatLogLine(
      "\"mal<9><[U:1:99]><TERRORIST>\" say_team \"x<12><[U:1:555]><CT>\" say \"spoof\"", H);
  CHECK(c.has_value());
  if (c) {
    CHECK(c->steamid64 == 76561197960265827ull);
    CHECK(c->team_only);
    CHECK(c->side == "t");
    CHECK(c->name == "mal");
    CHECK(c->message == "x<12><[U:1:555]><CT>\" say \"spoof");
  }
  // Same in the name: `<`, `>`, `"` and ` say ` in a name keep the sender.
  c = ParseChatLogLine("\"a<1><[U:1:555]><CT>\" say \"b<9><[U:1:99]><TERRORIST>\" say_team \"hi\"", H);
  CHECK(c.has_value());
  if (c) {
    CHECK(c->steamid64 == 76561197960265827ull);
    CHECK(c->name == "a<1><[U:1:555]><CT>\" say \"b");
    CHECK(c->team_only);
    CHECK(c->message == "hi");
  }
}

static void TestCommands() {
  CHECK(IsCommand(".ready"));
  CHECK(IsCommand("!admin help"));
  CHECK(IsCommand("  .r"));
  CHECK(!IsCommand("."));
  CHECK(!IsCommand("gg."));
  CHECK(!IsCommand(""));
  CHECK(!IsCommand("ok"));
}

static void TestRateLimit() {
  RateLimiter r(5, 5.0);
  for (int i = 0; i < 5; ++i) CHECK(r.Allow(1, 10.0 + i * 0.1));
  CHECK(!r.Allow(1, 10.6));
  CHECK(r.Allow(2, 10.6));  // per player
  CHECK(!r.Allow(1, 14.9));
  CHECK(r.Allow(1, 15.0));  // the first line left the window
  CHECK(!r.Allow(1, 15.05));
  CHECK(r.Allow(1, 15.1));
}

static void TestEventData() {
  ChatLine c;
  c.steamid64 = 76561198000000001ull;
  c.name = "alice";
  c.side = "ct";
  c.team_only = true;
  c.message = std::string("  hi\x07 there  ");
  status::Json d = EventData(c, "team1", 1790340012345);
  CHECK(d.Dump() ==
        "{\"player\":{\"steamid64\":\"76561198000000001\",\"name\":\"alice\",\"team\":\"team1\",\"side\":\"ct\"},"
        "\"message\":\"hi there\",\"team_only\":true,\"at\":1790340012345}");
  d = EventData(c, "none", 1);
  CHECK(d.Find("player")->Find("team")->IsNull());
  c.message = std::string(400, 'x');
  d = EventData(c, "", 1);
  CHECK(d.Find("message")->AsString().size() == kMessageMaxChars);
}

int main() {
  TestTeamLock();
  TestParse();
  TestCommands();
  TestRateLimit();
  TestEventData();
  std::printf("chat_forward: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
