// Offline tests for the go-live card and `.admin` (readyup/card_html.h, readyup/admin_call_logic.h):
// the card HTML (escaping, size, which commands show), the go-live card's every-tick resend and
// its end (golive_card_logic.h), the admin card, the per-player cooldown,
// the message cleanup, the call id / timestamp and the `admin_called` payload.
// ctest `match_live_cards`.
#include "readyup/admin_call_logic.h"
#include "readyup/card_html.h"
#include "readyup/golive_card_logic.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace readyup;

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

static bool Has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static void TestEscape() {
  CHECK(CardHtmlEscape("a<b>&\"'c", 100) == "a&lt;b&gt;&amp;&quot;&#39;c");
  CHECK(CardHtmlEscape("\x07red\x01 name", 100) == "red name");  // chat color bytes dropped
  CHECK(CardHtmlEscape("abcdef", 3) == "abc...");
  // "é" is two bytes: a cut inside it backs off to the character start.
  CHECK(CardHtmlEscape("ab\xC3\xA9z", 3) == "ab...");
}

static void TestGoLiveCard() {
  GoLiveCardInfo m;
  m.team1 = "Team <A>";
  m.team2 = "B & Co";
  m.team1Side = 2;  // team1 on T
  m.pauses = true;
  const std::string h = GoLiveCardHtml(m);
  CHECK(Has(h, "<b>LIVE</b>") && Has(h, "GO GO GO"));
  CHECK(Has(h, "Team &lt;A&gt; (T)") && Has(h, "B &amp; Co (CT)"));
  CHECK(!Has(h, "<A>"));
  CHECK(Has(h, "#FF9D3B'>Team &lt;A&gt; (T)"));  // T orange
  CHECK(Has(h, "#5EA8FF'>B &amp; Co (CT)"));     // CT blue
  for (const char* cmd : {".p<", ".up<", ".tac<", ".admin<"}) CHECK(Has(h, cmd));
  // Short forms only.
  for (const char* cmd : {".pause<", ".tech<", ".unpause<"}) CHECK(!Has(h, cmd));
  CHECK(GoLiveCommandsLine(m, false) == "Pause: .p | Unpause: .up | Timeout: .tac | Call an admin: .admin");
  CHECK(h.size() < 1024);

  // Long names are cut: the card stays small.
  GoLiveCardInfo big = m;
  big.team1 = std::string(200, 'x');
  big.team2 = std::string(200, '&');
  CHECK(GoLiveCardHtml(big).size() < 1024);

  // Scrim / no names: no matchup line; no match: only .admin.
  GoLiveCardInfo s;
  s.pauses = true;
  const std::string hs = GoLiveCardHtml(s);
  CHECK(!Has(hs, " vs ") && Has(hs, ".p<") && Has(hs, ".admin<"));
  GoLiveCardInfo none;
  const std::string hn = GoLiveCardHtml(none);
  CHECK(!Has(hn, ".p<") && !Has(hn, ".tac<") && Has(hn, ".admin<"));

  // Unknown side: names without (CT)/(T).
  GoLiveCardInfo u;
  u.team1 = "A";
  u.team2 = "B";
  const std::string hu = GoLiveCardHtml(u);
  CHECK(Has(hu, ">A</font>") && Has(hu, ">B</font>") && !Has(hu, "(CT)"));
}

// The go-live card re-sends every tick (hud_resend_ms=0) for all of golive_card_seconds: CS2's
// center panel ignores the event's duration, so a slower cadence makes the card blink.
static void TestGoLiveCardTiming() {
  GoLiveCardStop why = GoLiveCardStop::None;
  CHECK(GoLiveCardUntilMs(5000, 10) == 15000);  // the full 10 s, no "- 2"
  CHECK(GoLiveCardUntilMs(5000, 0) == 5000);
  CHECK(GoLiveCardUntilMs(5000, -3) == 5000);

  GoLiveCardClock c;
  c.startMs = 5000;
  c.untilMs = GoLiveCardUntilMs(5000, 10);
  // Before the start: wait (also while the go-live restart has not made the mode live yet).
  CHECK(GoLiveCardDecide(c, 4999, true, false, 0, &why) == GoLiveCardAction::Wait);
  CHECK(GoLiveCardDecide(c, 4000, false, false, 0, &why) == GoLiveCardAction::Wait);
  // Every tick (64 Hz ~ 16 ms) from the start to the end sends.
  int sends = 0;
  int64_t last = -1;
  for (int64_t t = 5000; t < 15000; t += 16) {
    c.lastSendMs = last;
    if (GoLiveCardDecide(c, t, true, false, 0, &why) == GoLiveCardAction::Send) {
      ++sends;
      last = t;
    }
  }
  CHECK(sends == (15000 - 5000 + 15) / 16);
  CHECK(last >= 15000 - 16);  // sent until the very end
  // Even two sends in the same millisecond (two ticks) both go out with resend 0.
  c.lastSendMs = 6000;
  CHECK(GoLiveCardDecide(c, 6000, true, false, 0, &why) == GoLiveCardAction::Send);
  // A configured hud_resend_ms is honoured.
  CHECK(GoLiveCardDecide(c, 6100, true, false, 250, &why) == GoLiveCardAction::Wait);
  CHECK(GoLiveCardDecide(c, 6250, true, false, 250, &why) == GoLiveCardAction::Send);
  // The end.
  c.lastSendMs = 14990;
  why = GoLiveCardStop::None;
  CHECK(GoLiveCardDecide(c, 15000, true, false, 0, &why) == GoLiveCardAction::Stop);
  CHECK(why == GoLiveCardStop::TimeUp);

  // Early ends, with the reason.
  c.lastSendMs = 7000;
  CHECK(GoLiveCardDecide(c, 7010, false, false, 0, &why) == GoLiveCardAction::Stop);
  CHECK(why == GoLiveCardStop::NotLive);
  CHECK(GoLiveCardDecide(c, 7010, true, true, 0, &why) == GoLiveCardAction::Stop);
  CHECK(why == GoLiveCardStop::LivePanel);
  CHECK(GoLiveCardDecide(c, 4000, true, true, 0, &why) == GoLiveCardAction::Stop);  // before the start too
  CHECK(why == GoLiveCardStop::LivePanel);
  // The freeze time ended (a short mp_freezetime): the card never covers the played round.
  c.freezeEnded = true;
  CHECK(GoLiveCardDecide(c, 7010, true, false, 0, &why) == GoLiveCardAction::Stop);
  CHECK(why == GoLiveCardStop::FreezeEnd);
  CHECK(GoLiveCardDecide(c, 4000, true, false, 0, &why) == GoLiveCardAction::Stop);
  CHECK(why == GoLiveCardStop::FreezeEnd);
  c.freezeEnded = false;
  // golive_card_seconds=0: nothing is shown.
  GoLiveCardClock off;
  off.startMs = off.untilMs = GoLiveCardUntilMs(5000, 0);
  CHECK(GoLiveCardDecide(off, 5000, true, false, 0, &why) == GoLiveCardAction::Stop);
  CHECK(why == GoLiveCardStop::TimeUp);
  for (GoLiveCardStop s : {GoLiveCardStop::None, GoLiveCardStop::NotLive, GoLiveCardStop::LivePanel,
                           GoLiveCardStop::FreezeEnd, GoLiveCardStop::TimeUp}) {
    CHECK(std::string(GoLiveCardStopName(s)) != "?");
  }
}

static void TestAdminCard() {
  const std::string h = AdminCallCardHtml("<b>evil</b>", "Team A, CT", "bot is <stuck> & lagging");
  CHECK(Has(h, "ADMIN CALLED"));
  CHECK(Has(h, "&lt;b&gt;evil&lt;/b&gt; (Team A, CT) needs an admin"));
  CHECK(Has(h, "bot is &lt;stuck&gt; &amp; lagging"));
  CHECK(!Has(AdminCallCardHtml("n", "", ""), "()"));
  CHECK(AdminCallCardHtml(std::string(300, 'n'), std::string(300, 't'), std::string(300, '<')).size() < 1024);
}

static void TestCooldown() {
  AdminCallCooldown c;
  int left = -1;
  CHECK(c.TryCall(1, 100.0, 60, &left) && left == 0);
  CHECK(!c.TryCall(1, 100.5, 60, &left) && left == 60);
  CHECK(!c.TryCall(1, 159.2, 60, &left) && left == 1);
  CHECK(c.TryCall(2, 101.0, 60, &left));  // per player
  CHECK(c.TryCall(1, 160.0, 60, &left));  // over
  CHECK(!c.TryCall(1, 161.0, 60, &left) && left == 59);
  // 0 = no cooldown.
  CHECK(c.TryCall(3, 5.0, 0) && c.TryCall(3, 5.0, 0));
  c.Clear();
  CHECK(c.TryCall(1, 161.0, 60));
}

static void TestMessage() {
  CHECK(CleanAdminCallMessage("") == "");
  CHECK(CleanAdminCallMessage("   ") == "");
  CHECK(CleanAdminCallMessage("  need   help\tnow \n") == "need help now");
  CHECK(CleanAdminCallMessage("\x07red\x01text") == "redtext");
  CHECK(CleanAdminCallMessage(std::string(500, 'a')).size() == 200);
  // 200 characters, not bytes: "é" x 250 keeps 200 of them (400 bytes), never half of one.
  std::string e;
  for (int i = 0; i < 250; ++i) e += "\xC3\xA9";
  CHECK(CleanAdminCallMessage(e).size() == 400);
  CHECK(CleanAdminCallMessage("abc def", 4) == "abc");  // no trailing space after the cut
}

static void TestIdsAndTime() {
  CHECK(IsoUtcFromUnixMs(0) == "1970-01-01T00:00:00.000Z");
  CHECK(IsoUtcFromUnixMs(1790340012345LL) == "2026-09-25T12:40:12.345Z");
  unsigned char b[16];
  for (int i = 0; i < 16; ++i) b[i] = static_cast<unsigned char>(0xF0 + i);
  const std::string id = CallIdFromBytes(b);
  CHECK(id.size() == 36 && id[8] == '-' && id[13] == '-' && id[14] == '4' && id[18] == '-' && id[23] == '-');
  CHECK(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
  CHECK(id == "f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff");
}

static void TestPayload() {
  AdminCallEvent e;
  e.matchid = 4242;
  e.map_number = 1;  // 0-based: the second map
  e.server_id = "srv-eu-1";
  e.call_id = "f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff";
  e.player.steamid64 = 76561198000000001ULL;
  e.player.name = "ali \"ce\"";
  e.player.team = "team1";
  e.player.side = "ct";
  e.message = "smoke bug on B";
  e.called_at = "2026-09-25T11:20:12.345Z";
  const std::string j = AdminCalledWebhookJson(e);
  CHECK(j ==
        "{\"event\":\"admin_called\",\"matchid\":4242,\"map_number\":1,\"server_id\":\"srv-eu-1\","
        "\"call_id\":\"f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff\","
        "\"player\":{\"steamid64\":\"76561198000000001\",\"name\":\"ali \\\"ce\\\"\",\"team\":\"team1\",\"side\":\"ct\"},"
        "\"message\":\"smoke bug on B\",\"called_at\":\"2026-09-25T11:20:12.345Z\"}");

  // Scrim / no match: matchid -1; no server id: left out; unknown team / side null; empty message.
  AdminCallEvent s = e;
  s.matchid = -1;
  s.map_number = 0;
  s.server_id.clear();
  s.player.team = "";
  s.player.side = "";
  s.message = "";
  const std::string js = AdminCalledWebhookJson(s);
  CHECK(Has(js, "{\"event\":\"admin_called\",\"matchid\":-1,\"map_number\":0,\"call_id\":"));
  CHECK(!Has(js, "server_id") && Has(js, "\"team\":null,\"side\":null") && Has(js, "\"message\":\"\""));

  // The fleet data: the same fields without event / matchid / map_number.
  const std::string d = AdminCallData(e).Dump();
  CHECK(d.rfind("{\"call_id\":", 0) == 0 && !Has(d, "\"event\"") && !Has(d, "matchid") && !Has(d, "server_id"));

  CHECK(AdminCallTeamLabel("Team A", "ct", false) == "Team A, CT");
  CHECK(AdminCallTeamLabel("", "t", false) == "T");
  CHECK(AdminCallTeamLabel("Team A", "", false) == "Team A");
  CHECK(AdminCallTeamLabel("Team A", "ct", true) == "spectator");
  CHECK(AdminCallTeamLabel("", "", false).empty());
}

static void TestResultCards() {
  ResultCardInfo m;
  m.team1 = "Alpha";
  m.team2 = "Bravo <x>";
  m.winner = "team2";
  m.team1Score = 5;
  m.team2Score = 7;
  m.mapNumber = 2;
  m.mapName = "de_mirage";
  m.nextMap = "de_inferno";
  const std::string map = MapResultCardHtml(m);
  CHECK(Has(map, "MAP 2 &#183; Mirage"));
  CHECK(Has(map, "<b>Bravo &lt;x&gt;</b>") && Has(map, " wins the map"));
  CHECK(Has(map, "<b>5 - 7</b>") && Has(map, "Next: Inferno"));
  CHECK(map.size() < 1024);

  m.winner = "";
  m.nextMap = "";
  CHECK(Has(MapResultCardHtml(m), "MAP DRAWN") && !Has(MapResultCardHtml(m), "Next:"));

  ResultCardInfo s;
  s.team1 = "Alpha";
  s.team2 = "Bravo";
  s.winner = "team1";
  s.team1Score = 2;
  s.team2Score = 1;
  const std::string series = SeriesResultCardHtml(s);
  CHECK(Has(series, "<b>Alpha</b>") && Has(series, "WINS THE SERIES") && Has(series, "<b>2 - 1</b>"));
  s.forfeit = true;
  CHECK(Has(SeriesResultCardHtml(s), "by forfeit"));
  s.winner = "";
  CHECK(Has(SeriesResultCardHtml(s), "SERIES DRAWN"));
  ResultCardInfo blank;
  blank.winner = "team1";
  CHECK(Has(SeriesResultCardHtml(blank), "<b>Team 1</b>"));
}

int main() {
  TestEscape();
  TestResultCards();
  TestGoLiveCard();
  TestGoLiveCardTiming();
  TestAdminCard();
  TestCooldown();
  TestMessage();
  TestIdsAndTime();
  TestPayload();
  if (g_failures) {
    std::fprintf(stderr, "live_cards_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("live_cards_test: all %d checks passed\n", g_checks);
  return 0;
}
