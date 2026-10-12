// Offline tests for readyup/log_relay.h: redaction, line cap, ring, batching and expiry of the
// fleet live log. ctest `match_log_relay`.
#include "readyup/log_relay.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static void RedactsSecrets() {
  CHECK(RedactLogLine("rcon_password hunter2") == "rcon_password ***");
  CHECK(RedactLogLine("sv_password \"abc def\"") == "sv_password ***");
  CHECK(RedactLogLine("+sv_setsteamaccount 0123ABCD") == "+sv_setsteamaccount ***");
  CHECK(RedactLogLine("readyup_license_key RUE-XYZ") == "readyup_license_key ***");
  CHECK(RedactLogLine("key rfk_abc123") == "key rfk_***");
  CHECK(RedactLogLine("Authorization: Bearer xyz") == "Authorization: Bearer ***");
  CHECK(RedactLogLine("RCON_PASSWORD x") == "RCON_PASSWORD ***");
  CHECK(RedactLogLine("say hello") == "say hello");
  CHECK(RedactLogLine("\"sv_password\" = \"secret\"") == "\"sv_password\" = ***");
  CHECK(RedactLogLine("rcon_password = \"x\"") == "rcon_password = ***");
  CHECK(RedactLogLine("\"rcon_password\" = \"x y\" ( def. \"\" )") == "\"rcon_password\" = *** ( def. \"\" )");
  CHECK(RedactLogLine("\"sv_setsteamaccount\" = \"TOK\"") == "\"sv_setsteamaccount\" = ***");
  CHECK(RedactLogLine("readyup_license_key=RUE-AB") == "readyup_license_key=***");
  CHECK(RedactLogLine("Bearer a.b.c") == "Bearer ***");
  CHECK(RedactLogLine("my_sv_password x") == "my_sv_password x");
}

static void RedactsColonForms() {
  CHECK(RedactLogLine("rcon_password   : hunter2 : sv : desc") == "rcon_password   : *** : sv : desc");
  CHECK(RedactLogLine("rcon_password:x") == "rcon_password:***");
  CHECK(RedactLogLine("\"rcon_password\" : \"x\"") == "\"rcon_password\" : ***");
  CHECK(RedactLogLine("sv_password : abc") == "sv_password : ***");
  CHECK(RedactLogLine("sv_setsteamaccount:TOK123") == "sv_setsteamaccount:***");
  CHECK(RedactLogLine("readyup_license_key : RUE-AB") == "readyup_license_key : ***");
}

static void RedactsSecretValues() {
  LogRelay r;
  r.SetSecrets({"hunter2", "ab", "", "TOKEN-9"});
  r.Start(0, false);
  r.Push("Logging into Steam with logon token 'TOKEN-9' and hunter2 hunter2", 0);
  r.Push("ab stays", 0);
  r.Start(0, true);
  auto b = r.Take(0);
  CHECK(b && b->lines.size() == 2);
  if (b && b->lines.size() == 2) {
    CHECK(b->lines[0] == "Logging into Steam with logon token '***' and *** ***");
    CHECK(b->lines[1] == "ab stays");
  }
  r.SetSecrets({});
  r.Push("hunter2", 1);
  r.Start(1, true);
  b = r.Take(1);
  CHECK(b && b->lines.back() == "hunter2");
}

static void TruncatesLongLines() {
  LogRelay r;
  r.Start(0, true);
  r.Push(std::string(600, 'a'), 0);
  r.Start(1, true);
  auto b = r.Take(1);
  CHECK(b && b->lines.size() == 1);
  if (b && b->lines.size() == 1) {
    const std::string& l = b->lines[0];
    CHECK(l.size() == 512);
    CHECK(l.substr(l.size() - 3) == "\xE2\x80\xA6");
  }
  LogRelay u;
  u.Start(0, true);
  u.Push(std::string(508, 'a') + "\xC3\xA9\xC3\xA9\xC3\xA9", 0);
  u.Start(1, true);
  auto bu = u.Take(1);
  CHECK(bu && bu->lines.size() == 1);
  if (bu && bu->lines.size() == 1) {
    const std::string& l = bu->lines[0];
    CHECK(l.size() == 511);
    CHECK(l.substr(0, 508) == std::string(508, 'a'));
    CHECK(l.substr(508) == "\xE2\x80\xA6");
  }
}

static void RingKeeps200() {
  LogRelay r;
  for (int i = 0; i < 250; ++i) r.Push("line " + std::to_string(i), 0);
  r.Start(1, true);
  auto b = r.Take(1);
  CHECK(b && b->backlog && b->lines.size() == 200);
  if (b && b->lines.size() == 200) {
    CHECK(b->lines.front() == "line 50");
    CHECK(b->lines.back() == "line 249");
    for (int i = 0; i < 200; ++i) CHECK(b->lines[i] == "line " + std::to_string(50 + i));
  }
  CHECK(!r.Take(1));
  CHECK(!r.Take(2));
  r.Push("new", 2);
  auto s = r.Take(2.25);
  CHECK(s && !s->backlog && s->lines.size() == 1 && s->lines[0] == "new");
}

static void StreamsAfterStart() {
  LogRelay r;
  r.Push("early", 0);
  CHECK(!r.Take(0.5));
  r.Start(10, false);
  r.Push("a", 10);
  r.Push("b", 10);
  r.Push("c", 10);
  auto b = r.Take(10.25);
  CHECK(b && !b->backlog && b->lines.size() == 3 && b->dropped == 0);
  r.Push("d", 10.3);
  CHECK(!r.Take(10.4));
  auto d = r.Take(10.5);
  CHECK(d && d->lines.size() == 1 && d->lines[0] == "d");
}

static void RateCapCountsDropped() {
  LogRelay r;
  r.Start(0, false);
  for (int i = 0; i < 500; ++i) r.Push("x", 0);
  size_t total = 0;
  long long dropped = 0;
  for (double t = 0.25; t <= 1.0; t += 0.25) {
    auto b = r.Take(t);
    if (!b) continue;
    CHECK(b->lines.size() <= 100);
    total += b->lines.size();
    dropped += b->dropped;
  }
  CHECK(total == 100);
  CHECK(dropped == 400);
}

static void ExpiresWithoutRenew() {
  LogRelay r;
  r.Start(0, false);
  CHECK(r.Active(89.9));
  CHECK(!r.Active(90.1));
  r.Push("late", 91);
  CHECK(!r.Take(92));
  r.Start(100, false);
  CHECK(r.Active(100));
  r.Push("again", 100);
  auto b = r.Take(100.25);
  CHECK(b && b->lines.size() == 1 && b->lines[0] == "again");
  r.Start(160, false);
  CHECK(r.Active(200));
}

static void BacklogDoesNotRepeat() {
  LogRelay r;
  r.Push("a", 0);
  r.Start(1, true);
  r.Push("b", 1);
  auto b = r.Take(1);
  CHECK(b && b->backlog && b->lines.size() == 2);
  CHECK(!r.Take(2));
}

static void StripsTrailingNewlines() {
  LogRelay r;
  r.Start(0, false);
  r.Push("hostname : x\r\n\n", 0);
  r.Push("\n", 0);
  r.Push("\r\n", 0);
  auto b = r.Take(1);
  CHECK(b && b->lines.size() == 1 && b->lines[0] == "hostname : x");
  r.Start(1, true);
  auto bl = r.Take(1);
  CHECK(bl && bl->lines.size() == 1);
}

int main() {
  RedactsSecrets();
  RedactsColonForms();
  RedactsSecretValues();
  TruncatesLongLines();
  RingKeeps200();
  StreamsAfterStart();
  RateCapCountsDropped();
  ExpiresWithoutRenew();
  BacklogDoesNotRepeat();
  StripsTrailingNewlines();
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("log_relay: all checks passed\n");
  return 0;
}
