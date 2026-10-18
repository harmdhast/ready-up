// Unit tests for the fleet link's I/O-free parts: JSON, envelope, ULID, backoff, close codes,
// redaction, URL rules, inbound seq/ack tracking, the disk spool and the credentials file.
//   build/fleet_unit_test
#include "fleet_cmds.h"
#include "fleet_json.h"
#include "fleet_proto.h"
#include "fleet_spool.h"
#include "fleet_store.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

using namespace fleet;

namespace {
std::string TempDir() {
  char tmpl[] = "/tmp/fleet_unit_XXXXXX";
  const char* d = mkdtemp(tmpl);
  return d ? d : "/tmp";
}
void RmRf(const std::string& d) {
  const std::string cmd = "rm -rf '" + d + "'";
  (void)!std::system(cmd.c_str());
}
}  // namespace

TEST(TestJsonRoundTrip) {
  json::Value v;
  std::string err;
  CHECK(json::Parse(R"({"b":1,"a":"x\u00e9\ud83d\ude00\n","n":-9007199254740993,"f":1.5,"z":[true,null,{}]})", &v, &err));
  CHECK_EQ(v.o[0].first, std::string("b"));  // key order kept
  CHECK_EQ(v.Get("a")->s, std::string("x\xc3\xa9\xf0\x9f\x98\x80\n"));
  CHECK_EQ(v.Get("n")->i, static_cast<int64_t>(-9007199254740993LL));  // exact beyond 2^53
  CHECK(v.Get("f")->t == json::Value::T::Num);
  CHECK_EQ(json::Dump(v), std::string(R"({"b":1,"a":"x)"
                                      "\xc3\xa9\xf0\x9f\x98\x80"
                                      R"(\n","n":-9007199254740993,"f":1.5,"z":[true,null,{}]})"));
  CHECK(!json::Parse("{\"a\":1,}", &v));
  CHECK(!json::Parse("{\"a\":\"\x01\"}", &v));
  CHECK(!json::Parse("[1] x", &v));
  std::string deep(100, '[');
  deep += std::string(100, ']');
  CHECK(!json::Parse(deep, &v));  // depth limit
}

TEST(TestUlid) {
  const std::string a = NewUlid(1790340012345);
  const std::string b = NewUlid(1790340012346);
  CHECK_EQ(a.size(), size_t(26));
  CHECK(IsUlid(a));
  CHECK(a < b);  // lexicographic = time order
  CHECK(NewUlid(1) != NewUlid(1));
  CHECK_EQ(NewUlid(0).substr(0, 10), std::string("0000000000"));
  CHECK(!IsUlid("01J8ZQ4T8W6N3X0F2R5K7M9P1I"));  // I is not Crockford
}

TEST(TestTypes) {
  CHECK(IsValidType("event.round_end"));
  CHECK(IsValidType("ping"));
  CHECK(IsValidType("state.request"));
  CHECK(!IsValidType("Event.x"));
  CHECK(!IsValidType("event..x"));
  CHECK(!IsValidType(".x"));
  CHECK(!IsValidType("x."));
  CHECK(!IsValidType("_x"));
  CHECK(IsEphemeralType("pong"));
  CHECK(!IsEphemeralType("event.phase"));
  CHECK(IsCriticalType("event.round_end"));
  CHECK(IsCriticalType("cmd.result"));
  CHECK(!IsCriticalType("event.player_ready"));
}

TEST(TestEnvelope) {
  Envelope e;
  e.type = "event.round_end";
  e.id = NewUlid(1000);
  e.seq = 1842;
  e.ack = 77;
  e.ts = 1790340012345;
  e.epoch = 3;
  e.payload.Set("round", json::Value::Int(14));
  const std::string text = Encode(e);
  Envelope d;
  std::string err;
  CHECK(Decode(text, &d, &err));
  CHECK_EQ(d.type, e.type);
  CHECK_EQ(d.id, e.id);
  CHECK_EQ(d.seq, int64_t(1842));
  CHECK_EQ(d.ack, int64_t(77));
  CHECK_EQ(d.epoch, int64_t(3));
  CHECK_EQ(d.ts, int64_t(1790340012345));
  CHECK(d.ref.empty());
  CHECK_EQ(d.payload.Get("round")->i, int64_t(14));
  // Ephemeral: no seq, no epoch, ref present.
  Envelope p;
  p.type = "pong";
  p.id = NewUlid(1);
  p.ts = 5;
  p.ref = "01J8ZQ4T8W6N3X0F2R5K7M9P1C";
  const std::string pt = EncodeRaw(p, R"({"t":5})");
  CHECK(pt.find("\"seq\"") == std::string::npos);
  CHECK(pt.find("\"ack\"") == std::string::npos);
  CHECK(pt.find("\"ref\":\"01J8ZQ4T8W6N3X0F2R5K7M9P1C\"") != std::string::npos);
  CHECK(Decode(pt, &d, &err));
  CHECK_EQ(d.seq, int64_t(0));
  CHECK_EQ(d.ack, int64_t(-1));
  // Rejects.
  CHECK(!Decode(R"({"v":1,"type":"x.y","id":"a","ts":1})", &d, &err));                  // no payload
  CHECK(!Decode(R"({"v":1,"type":"X","id":"a","ts":1,"payload":{}})", &d, &err));         // bad type
  CHECK(!Decode(R"({"v":1,"type":"x.y","seq":0,"ts":1,"payload":{}})", &d, &err));        // seq >= 1
  CHECK(!Decode(R"({"v":1,"type":"x.y","ts":1,"payload":[]})", &d, &err));                // payload object
  CHECK(Decode(R"({"v":1,"type":"x.y","ts":1,"payload":{},"extra":42})", &d, &err));      // unknown ignored
  std::string big = R"({"v":1,"type":"x.y","ts":1,"payload":{"s":")" + std::string(kMaxFrameBytes, 'a') + "\"}}";
  CHECK(!Decode(big, &d, &err));
}

TEST(TestBackoffSchedule) {
  Backoff b(BackoffPolicy{}, 42);
  const int64_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (int64_t w : want) {
    CHECK_EQ(b.Ceiling(), w);
    const int64_t d = b.Next();
    CHECK(d >= 0 && d <= w);
  }
  // Full jitter spreads: 200 draws at the cap are not all the same and stay in range.
  std::set<int64_t> seen;
  for (int i = 0; i < 200; ++i) {
    const int64_t d = b.Next();
    CHECK(d >= 0 && d <= 30000);
    seen.insert(d / 1000);
  }
  CHECK(seen.size() > 10);
  // A short session keeps the attempt count, a 60 s one resets it.
  b.OnSessionEnded(59000);
  CHECK_EQ(b.Ceiling(), int64_t(30000));
  b.OnSessionEnded(60000);
  CHECK_EQ(b.Ceiling(), int64_t(1000));
  // 4401/4403: cap 10 min; 4503: base 2 s.
  Backoff r(BackoffPolicy{}, 1);
  for (int i = 0; i < 20; ++i) r.Next(600000);
  CHECK_EQ(r.Ceiling(600000), int64_t(600000));
  Backoff dr(BackoffPolicy{}, 1);
  CHECK_EQ(dr.Ceiling(0, 2000), int64_t(2000));
  dr.Next(0, 2000);
  CHECK_EQ(dr.Ceiling(0, 2000), int64_t(4000));
}

TEST(TestCloseCodes) {
  CloseAction a = ClassifyClose(1000, "");
  CHECK(!a.rejected && a.fixedDelayMs < 0 && a.capMs == 0);
  a = ClassifyClose(4401, "bad token");
  CHECK(a.rejected);
  CHECK_EQ(a.capMs, int64_t(600000));
  a = ClassifyClose(4403, "");
  CHECK(a.rejected);
  a = ClassifyClose(4409, "");
  CHECK_EQ(a.fixedDelayMs, int64_t(30000));
  a = ClassifyClose(4426, "");
  CHECK(a.versionUnsupported);
  CHECK_EQ(a.capMs, int64_t(600000));
  a = ClassifyClose(4429, R"({"retry_after_ms":4500})");
  CHECK_EQ(a.fixedDelayMs, int64_t(4500));
  a = ClassifyClose(4429, "");
  CHECK_EQ(a.fixedDelayMs, int64_t(30000));
  a = ClassifyClose(4503, "");
  CHECK_EQ(a.baseMs, int64_t(2000));
  a = ClassifyClose(4400, "schema: rus_ABCDEFGHIJKL_c2VjcmV0 leaked");
  CHECK(a.what.find("c2VjcmV0") == std::string::npos);
  a = ClassifyHttpStatus(401, 0);
  CHECK(a.rejected);
  a = ClassifyHttpStatus(429, 7000);
  CHECK_EQ(a.fixedDelayMs, int64_t(7000));
  a = ClassifyHttpStatus(502, 0);
  CHECK(!a.rejected && a.fixedDelayMs < 0);
}

TEST(TestRedaction) {
  const std::string s = Redact(
      "Authorization: Bearer rus_ABCDEFGHIJKL_c2VjcmV0LXNlY3JldA code=RUE-7F3K-9QX2-LM4D key rfk_k1_s3cr3t host "
      "trust_me");
  CHECK(s.find("c2VjcmV0") == std::string::npos);
  CHECK(s.find("7F3K") == std::string::npos);
  CHECK(s.find("s3cr3t") == std::string::npos);
  CHECK(s.find("trust_me") != std::string::npos);  // "trust_" is not a token prefix
  CHECK(s.find("rus_***") != std::string::npos || s.find("Bearer ***") != std::string::npos);
}

TEST(TestUrls) {
  CHECK_EQ(JoinUrl("https://at.example.com/", "/api/fleet/ws", true), std::string("wss://at.example.com/api/fleet/ws"));
  CHECK_EQ(JoinUrl("http://127.0.0.1:8080", "/api/fleet/enroll", false),
           std::string("http://127.0.0.1:8080/api/fleet/enroll"));
  CHECK(CheckUrlAllowed("https://at.example.com", false).empty());
  CHECK(CheckUrlAllowed("wss://at.example.com/api/fleet/ws", false).empty());
  CHECK(!CheckUrlAllowed("http://127.0.0.1:3000", false).empty());  // needs insecure
  CHECK(CheckUrlAllowed("http://203.0.113.7:3069", false).find("--insecure") != std::string::npos);
  CHECK(CheckUrlAllowed("http://127.0.0.1:3000", true).empty());
  CHECK(CheckUrlAllowed("ws://192.168.1.20:3000", true).empty());
  CHECK(CheckUrlAllowed("ws://[::1]:3000", true).empty());
  CHECK(CheckUrlAllowed("http://localhost:3000", true).empty());
  CHECK(CheckUrlAllowed("http://203.0.113.7:3069", true).empty());  // public host: explicit opt-in
  CHECK(CheckUrlAllowed("ws://203.0.113.7:3069/api/fleet/ws", true).empty());
  CHECK(IsPrivateHostUrl("http://172.31.255.1") && !IsPrivateHostUrl("http://172.32.0.1"));
  // https enroll, ws:// answer for the same host: wss:// at the enroll host.
  CHECK_EQ(UpgradeSameHostWs("https://cs.example.com", "ws://cs.example.com/api/fleet/ws"),
           std::string("wss://cs.example.com/api/fleet/ws"));
  CHECK_EQ(UpgradeSameHostWs("https://cs.example.com/", "ws://CS.example.com:80/api/fleet/ws"),
           std::string("wss://cs.example.com/api/fleet/ws"));
  CHECK_EQ(UpgradeSameHostWs("https://cs.example.com:8443", "ws://cs.example.com:8443/api/fleet/ws"),
           std::string("wss://cs.example.com:8443/api/fleet/ws"));
  CHECK(UpgradeSameHostWs("https://cs.example.com", "ws://cs.example.com:3069/api/fleet/ws").empty());
  CHECK(UpgradeSameHostWs("https://cs.example.com", "ws://other.example.com/api/fleet/ws").empty());
  CHECK(UpgradeSameHostWs("http://cs.example.com", "ws://cs.example.com/api/fleet/ws").empty());
  CHECK(!CheckUrlAllowed("ftp://x", true).empty());
}

TEST(TestRxTracking) {
  RxTracker rx;
  rx.Reset(0);
  CHECK(rx.OnReliable(1, "A") == RxTracker::Verdict::Accept);
  CHECK(rx.OnReliable(2, "B") == RxTracker::Verdict::Accept);
  CHECK(rx.OnReliable(2, "B") == RxTracker::Verdict::Duplicate);
  CHECK(rx.OnReliable(4, "D") == RxTracker::Verdict::OutOfOrder);
  CHECK(rx.OnReliable(3, "A") == RxTracker::Verdict::Duplicate);  // same id, new seq: replayed copy
  CHECK(rx.OnReliable(3, "C") == RxTracker::Verdict::Accept);
  CHECK_EQ(rx.received(), int64_t(3));
  // Done out of order: processed only advances over the contiguous prefix.
  CHECK(!rx.MarkDone(2, 1000));
  CHECK_EQ(rx.processed(), int64_t(0));
  CHECK(rx.MarkDone(1, 1000));
  CHECK_EQ(rx.processed(), int64_t(2));
  CHECK(!rx.AckDue(1500));  // < 1 s and < 32
  CHECK_EQ(rx.NextAckDueMs(), int64_t(2000));
  CHECK(rx.AckDue(2000));
  rx.OnAckSent(2, 2000);
  CHECK(!rx.AckDue(5000));
  CHECK(rx.MarkDone(3, 6000));
  CHECK_EQ(rx.processed(), int64_t(3));
  // 32 processed messages force an ack at once.
  for (int64_t s = 4; s <= 35; ++s) {
    CHECK(rx.OnReliable(s, "id" + std::to_string(s)) == RxTracker::Verdict::Accept);
    rx.MarkDone(s, 6001);
  }
  CHECK(rx.AckDue(6001));
  // Ephemeral dedupe by id.
  CHECK(rx.OnEphemeralId("P1"));
  CHECK(!rx.OnEphemeralId("P1"));
  // After a restart the persisted processed seq is the new baseline.
  RxTracker r2;
  r2.Reset(35);
  CHECK(r2.OnReliable(35, "x") == RxTracker::Verdict::Duplicate);
  CHECK(r2.OnReliable(36, "y") == RxTracker::Verdict::Accept);
}

TEST(TestSpoolPersistence) {
  const std::string dir = TempDir() + "/spool";
  std::string err, note;
  std::string streamId;
  {
    Spool s;
    CHECK(s.Open(dir, Spool::Limits{}, &err, &note));
    streamId = s.streamId();
    CHECK(IsUlid(streamId));
    CHECK_EQ(s.Append("event.phase", NewUlid(1), "", 1, 1, R"({"to":"live"})", &err), int64_t(1));
    CHECK_EQ(s.Append("event.round_end", NewUlid(2), "", 2, 1, R"({"round":1,"name":"\u00e9"})", &err), int64_t(2));
    CHECK_EQ(s.Append("cmd.result", NewUlid(3), "01J8ZQ4T8W6N3X0F2R5K7M9P1C", 3, 1, R"({"status":"ok"})", &err),
             int64_t(3));
    s.AckUpTo(1);
    s.SetRxSeq(9);
    CHECK_EQ(s.count(), size_t(2));
    s.Close();
  }
  struct stat st {};
  CHECK(stat((dir + "/stream.log").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  {
    Spool s;
    CHECK(s.Open(dir, Spool::Limits{}, &err, &note));
    CHECK_EQ(s.streamId(), streamId);  // same stream across a restart
    CHECK_EQ(s.count(), size_t(2));
    CHECK_EQ(s.ackedSeq(), int64_t(1));
    CHECK_EQ(s.rxSeq(), int64_t(9));
    CHECK_EQ(s.records().front().seq, int64_t(2));
    CHECK_EQ(s.records().front().payload, std::string("{\"round\":1,\"name\":\"\xc3\xa9\"}"));
    CHECK_EQ(s.records().back().ref, std::string("01J8ZQ4T8W6N3X0F2R5K7M9P1C"));
    CHECK_EQ(s.Append("event.phase", NewUlid(4), "", 4, 1, "{}", &err), int64_t(4));
    CHECK(!s.gap());
    s.AckUpTo(4);
    CHECK_EQ(s.count(), size_t(0));
    s.Close();
  }
  {
    // A torn last line (crash mid-write) is skipped and flags a gap.
    std::ofstream f(dir + "/stream.log", std::ios::app);
    f << "{\"seq\":5,\"type\":\"event.phase\",\"id\":\"x\",\"ts\":5,\"epoch\":1,\"crit\":false,\"payl";
  }
  {
    Spool s;
    CHECK(s.Open(dir, Spool::Limits{}, &err, &note));
    CHECK(s.gap());
    const std::string before = s.streamId();
    CHECK(s.RotateIfGap());
    CHECK(s.streamId() != before);
    CHECK(!s.gap());
    CHECK_EQ(s.lastSeq(), int64_t(0));
  }
  RmRf(dir.substr(0, dir.size() - 6));
}

TEST(TestSpoolLimits) {
  const std::string dir = TempDir() + "/spool";
  std::string err, note;
  Spool s;
  Spool::Limits lim;
  lim.maxMsgs = 10;
  CHECK(s.Open(dir, lim, &err, &note));
  // 3 critical + 7 compactable fill it.
  for (int i = 0; i < 3; ++i) CHECK(s.Append("event.round_end", NewUlid(i), "", i, 1, "{}", &err) > 0);
  for (int i = 0; i < 7; ++i) CHECK(s.Append("event.player_ready", NewUlid(i), "", i, 1, "{}", &err) > 0);
  CHECK_EQ(s.count(), size_t(10));
  CHECK(!s.gap());
  // One more compactable: the oldest compactables are dropped (to 90 %), criticals stay.
  CHECK(s.Append("event.player_ready", NewUlid(11), "", 11, 1, "{}", &err) > 0);
  CHECK(s.gap());
  CHECK(s.count() <= size_t(10));
  size_t crit = 0;
  for (const auto& r : s.records()) crit += r.critical;
  CHECK_EQ(crit, size_t(3));
  // Only criticals left: new compactables are refused, criticals still fit (up to 2x).
  for (int i = 0; i < 20 && s.count() > crit; ++i) {
    s.Append("event.round_end", NewUlid(100 + i), "", 1, 1, "{}", &err);
    crit = 0;
    for (const auto& r : s.records()) crit += r.critical;
  }
  CHECK(s.count() >= size_t(10));
  CHECK_EQ(s.Append("event.player_ready", NewUlid(200), "", 1, 1, "{}", &err), int64_t(0));
  CHECK(err.find("spool full") != std::string::npos);
  int64_t accepted = 0;
  for (int i = 0; i < 30; ++i) accepted += s.Append("event.round_end", NewUlid(300 + i), "", 1, 1, "{}", &err) > 0;
  CHECK(s.count() <= size_t(20));  // hard stop at 2x
  CHECK(accepted < 30);
  RmRf(dir.substr(0, dir.size() - 6));
}

TEST(TestCredentialsFile) {
  const std::string dir = TempDir();
  Credentials c;
  c.serverId = "srv_1";
  c.token = "rus_ABCDEFGHIJKL_c2VjcmV0";
  c.wsUrl = "wss://x/api/fleet/ws";
  c.url = "https://x";
  c.installId = "01J8ZQ4T8W6N3X0F2R5K7M9P1C";
  c.enrolledAt = 123;
  std::string err;
  umask(022);
  CHECK(SaveCredentials(dir + "/credentials.json", c, &err));
  struct stat st {};
  CHECK(stat((dir + "/credentials.json").c_str(), &st) == 0);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  Credentials r;
  CHECK(LoadCredentials(dir + "/credentials.json", &r, &err));
  CHECK_EQ(r.token, c.token);
  CHECK_EQ(r.serverId, c.serverId);
  CHECK_EQ(r.enrolledAt, int64_t(123));
  // Loosened by hand: tightened again on load.
  chmod((dir + "/credentials.json").c_str(), 0644);
  CHECK(LoadCredentials(dir + "/credentials.json", &r, &err));
  stat((dir + "/credentials.json").c_str(), &st);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  CHECK(!LoadCredentials(dir + "/missing.json", &r, &err));
  CHECK_EQ(err, std::string("missing"));
  const std::string id1 = LoadOrCreateInstallId(dir, &err);
  const std::string id2 = LoadOrCreateInstallId(dir, &err);
  CHECK(IsUlid(id1));
  CHECK_EQ(id1, id2);
  RmRf(dir);
}

// hello.plugins_state: the .so files and plugins.json's disabled list (the core's format,
// core/src/readyup/plugin_state.cpp), sorted, bad names skipped.
TEST(TestPluginsState) {
  const std::string dir = TempDir();
  auto touch = [&](const std::string& f, const std::string& text = "") {
    std::ofstream(dir + "/" + f) << text;
  };
  touch("match.so");
  touch("fleet.so");
  touch("skins.so");
  touch("Bad Name.so");
  touch("notes.txt");
  touch("essentials.needs.json", "{}");
  mkdir((dir + "/fleet").c_str(), 0755);  // a plugin's data dir, not a plugin
  PluginsState ps;
  CHECK(ReadPluginsState(dir, &ps));
  CHECK_EQ(ps.installed.size(), size_t(3));
  CHECK_EQ(ps.installed.at(0), std::string("fleet"));
  CHECK_EQ(ps.installed.at(1), std::string("match"));
  CHECK_EQ(ps.installed.at(2), std::string("skins"));
  CHECK(ps.disabled.empty());  // no plugins.json yet
  touch("plugins.json", "{\n  \"version\": 1,\n  \"disabled\": [\n    \"skins\",\n    \"midas\",\n    \"../x\"\n  ]\n}\n");
  CHECK(ReadPluginsState(dir, &ps));
  CHECK_EQ(ps.disabled.size(), size_t(2));
  CHECK_EQ(ps.disabled.at(0), std::string("midas"));  // not installed, still reported
  CHECK_EQ(ps.disabled.at(1), std::string("skins"));
  touch("plugins.json", "not json");
  CHECK(ReadPluginsState(dir, &ps));
  CHECK(ps.disabled.empty());
  CHECK_EQ(ps.installed.size(), size_t(3));
  CHECK(!ReadPluginsState(dir + "/missing", &ps));
  CHECK(!ReadPluginsState("", &ps));
  RmRf(dir);
}

TEST(TestSelftestPayload) {
  // The core's selftest_summary JSON (status_feed.cpp NoteSelftest) -> hello.selftest / server.selftest.
  const std::string core =
      R"({"pass":false,"passed":40,"total":41,"pending":2,"failures":["fn:Host_Say"],"summary":"1 FAIL","ran_at":1790000000})";
  CHECK_EQ(SelftestPayload(core), std::string(R"({"pass":false,"passed":40,"total":41,"failures":["fn:Host_Say"]})"));
  // Another run with the same outcome gives the same payload (no server.selftest resend).
  CHECK_EQ(SelftestPayload(R"({"ran_at":5,"summary":"x","failures":["fn:Host_Say"],"total":41,"passed":40,"pass":false})"),
           SelftestPayload(core));
  CHECK_EQ(SelftestPayload(R"({"pass":true,"passed":3,"total":3})"),
           std::string(R"({"pass":true,"passed":3,"total":3,"failures":[]})"));
  CHECK_EQ(SelftestPayload(""), std::string());
  CHECK_EQ(SelftestPayload("not json"), std::string());
  CHECK_EQ(SelftestPayload("[1]"), std::string());
  CHECK_EQ(SelftestPayload(R"({"pass":"yes","passed":1,"total":1})"), std::string());
  CHECK_EQ(SelftestPayload(R"({"pass":true,"total":1})"), std::string());
  // Schema limits: at most 256 failures of at most 512 bytes (cut on a UTF-8 boundary), strings only.
  std::string many = R"({"pass":false,"passed":0,"total":300,"failures":[)";
  for (int i = 0; i < 300; ++i) many += (i ? "," : "") + std::string("\"f") + std::to_string(i) + "\"";
  many += R"(,1]})";
  json::Value v;
  CHECK(json::Parse(SelftestPayload(many), &v));
  CHECK_EQ(v.Get("failures")->a.size(), size_t(256));
  std::string longName(600, 'x');
  longName.replace(510, 2, "\xc3\xa9");  // a 2-byte character across the 512 byte cut
  CHECK(json::Parse(SelftestPayload(R"({"pass":false,"passed":0,"total":1,"failures":[")" + longName + "\"]}"), &v));
  const std::string cut = v.Get("failures")->a[0].s;
  CHECK(cut.size() <= size_t(512));
  CHECK(cut.size() >= size_t(510));
  CHECK((static_cast<unsigned char>(cut.back()) & 0x80) == 0);  // no half character left
}

// server.config fields fleet.so applies itself (offline timer, status token), and their file.
TEST(TestServerConfigLocal) {
  ServerConfigLocal c;
  std::string ignored;
  CHECK(ParseServerConfigLocal(
      R"({"rev":3,"settings":{"offline_pause_minutes":5,"status_http":{"token":"platform_token_0123456789"},"chat_prefix":"x"}})",
      &c, &ignored));
  CHECK_EQ(c.offlinePauseMinutes, 5);
  CHECK_EQ(c.statusToken, std::string("platform_token_0123456789"));
  CHECK(ignored.empty());
  // Absent = not set (readyup.cfg applies again).
  CHECK(ParseServerConfigLocal(R"({"rev":4,"settings":{}})", &c, &ignored));
  CHECK_EQ(c.offlinePauseMinutes, -1);
  CHECK(c.statusToken.empty());
  CHECK(ParseServerConfigLocal(R"({"rev":4,"settings":{"offline_pause_minutes":0}})", &c, &ignored));
  CHECK_EQ(c.offlinePauseMinutes, 0);  // 0 = timer off, a real value
  // Out of range / wrong type / a token the core would refuse: left unset, named.
  CHECK(ParseServerConfigLocal(
      R"({"rev":5,"settings":{"offline_pause_minutes":-1,"status_http":{"token":"short"}}})", &c, &ignored));
  CHECK_EQ(c.offlinePauseMinutes, -1);
  CHECK(c.statusToken.empty());
  CHECK(ignored.find("offline_pause_minutes") != std::string::npos && ignored.find("status_http.token") != std::string::npos);
  CHECK(ParseServerConfigLocal(R"({"settings":{"offline_pause_minutes":"3","status_http":{"token":"has space 0123456789"}}})",
                               &c, &ignored));
  CHECK_EQ(c.offlinePauseMinutes, -1);
  CHECK(c.statusToken.empty());
  CHECK(!ParseServerConfigLocal(R"({"rev":1})", &c, &ignored));
  CHECK(!ParseServerConfigLocal("not json", &c, &ignored));

  // The file: 0600, round trip, absent fields stay absent.
  const std::string dir = TempDir();
  std::string err;
  ServerConfigLocal w;
  w.offlinePauseMinutes = 7;
  w.statusToken = "platform_token_0123456789";
  CHECK(SaveServerConfigLocal(dir + "/server-config.json", w, &err));
  struct stat st {};
  stat((dir + "/server-config.json").c_str(), &st);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  ServerConfigLocal r;
  CHECK(LoadServerConfigLocal(dir + "/server-config.json", &r));
  CHECK_EQ(r.offlinePauseMinutes, 7);
  CHECK_EQ(r.statusToken, w.statusToken);
  CHECK(SaveServerConfigLocal(dir + "/server-config.json", ServerConfigLocal{}, &err));
  CHECK(LoadServerConfigLocal(dir + "/server-config.json", &r));
  CHECK_EQ(r.offlinePauseMinutes, -1);
  CHECK(r.statusToken.empty());
  CHECK(!LoadServerConfigLocal(dir + "/missing.json", &r));
  RmRf(dir);
}

TEST(TestPublicAddr) {
  CHECK_EQ(NormalizePublicAddr("203.0.113.7", 27055), std::string("203.0.113.7:27055"));
  CHECK_EQ(NormalizePublicAddr(" \"203.0.113.7:27016\" ", 27055), std::string("203.0.113.7:27016"));
  CHECK_EQ(NormalizePublicAddr("play.example.com", 27015), std::string("play.example.com:27015"));
  CHECK_EQ(NormalizePublicAddr("2001:db8::7", 27015), std::string("[2001:db8::7]:27015"));
  CHECK_EQ(NormalizePublicAddr("[2001:db8::7]:27020", 27015), std::string("[2001:db8::7]:27020"));
  CHECK_EQ(NormalizePublicAddr("[2001:db8::7]", 27015), std::string("[2001:db8::7]:27015"));
  CHECK_EQ(NormalizePublicAddr("", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("0.0.0.0", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("0.0.0.0:27015", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("::", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("host:0", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("host:99999", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("host:", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("bad host", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("x;rm -rf", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("[zz::1]:1", 27015), std::string());
  CHECK_EQ(NormalizePublicAddr("h", 0), std::string());  // no usable port
  // fleet.cfg wins, even loopback (the admin said so); detected values skip wildcards and loopback.
  CHECK_EQ(PickPublicAddr("127.0.0.1", {"203.0.113.7"}, 27015), std::string("127.0.0.1:27015"));
  CHECK_EQ(PickPublicAddr("", {"", "0.0.0.0"}, 27015), std::string());
  CHECK_EQ(PickPublicAddr("", {"127.0.0.1", "192.168.50.196"}, 27055), std::string("192.168.50.196:27055"));
  CHECK_EQ(PickPublicAddr("", {"localhost", "[::1]", "10.0.0.2"}, 27055), std::string("10.0.0.2:27055"));
  CHECK_EQ(PickPublicAddr("bad host", {"198.51.100.4:27100"}, 27055), std::string("198.51.100.4:27100"));
}

// The commands fleet.so runs itself (fleet_cmds.h): plugins.set, whitelist.set, practice.set, say,
// tv.set (args checked by tv.so).
TEST(TestOwnedCmds) {
  auto J = [](const char* text) {
    json::Value v;
    CHECK(json::Parse(text, &v));
    return v;
  };
  using namespace fleet::cmds;
  CHECK(OwnedByFleet("plugins.set") && OwnedByFleet("practice.set") && OwnedByFleet("say") && OwnedByFleet("tv.set"));
  CHECK(!OwnedByFleet("pause") && !OwnedByFleet("settings.set") && !OwnedByFleet("exec"));

  std::vector<std::string> on, off;
  std::string err;
  CHECK(ParsePluginsSet(J(R"({"enable": ["practice", "skins"], "disable": ["midas"]})"), &on, &off, &err));
  CHECK(on.size() == 2 && on[0] == "practice" && off.size() == 1 && off[0] == "midas");
  CHECK(ParsePluginsSet(J(R"({"disable": ["skins"]})"), &on, &off, &err) && on.empty());
  CHECK(ParsePluginsSet(J(R"({"disable": ["match"]})"), &on, &off, &err) && off[0] == "match");  // practice servers
  CHECK(!ParsePluginsSet(J(R"({})"), &on, &off, &err));
  CHECK(!ParsePluginsSet(J(R"({"disable": ["fleet"]})"), &on, &off, &err) && err.find("fleet") != std::string::npos);
  CHECK(!ParsePluginsSet(J(R"({"enable": ["skins"], "disable": ["skins"]})"), &on, &off, &err));
  CHECK(!ParsePluginsSet(J(R"({"enable": ["../x"]})"), &on, &off, &err));
  CHECK(!ParsePluginsSet(J(R"({"enable": ["skins; quit"]})"), &on, &off, &err));
  CHECK(!ParsePluginsSet(J(R"({"enable": "skins"})"), &on, &off, &err));

  bool enabled = false;
  std::vector<uint64_t> ids;
  CHECK(ParseWhitelistSet(J(R"({"enabled": true, "steamids": ["76561198000000001", "76561198000000002"]})"), &enabled,
                          &ids, &err));
  CHECK(enabled && ids.size() == 2 && ids[1] == 76561198000000002ull);
  CHECK(ParseWhitelistSet(J(R"({"enabled": false})"), &enabled, &ids, &err) && !enabled && ids.empty());
  CHECK(!ParseWhitelistSet(J(R"({"steamids": []})"), &enabled, &ids, &err));
  CHECK(!ParseWhitelistSet(J(R"({"enabled": true, "steamids": [76561198000000001]})"), &enabled, &ids, &err));
  CHECK(!ParseWhitelistSet(J(R"({"enabled": true, "steamids": ["123"]})"), &enabled, &ids, &err));

  int pon = 0, palways = 0;
  CHECK(ParsePracticeSet(J(R"({"on": true})"), &pon, &palways, &err) && pon == 1 && palways == -1);
  CHECK(ParsePracticeSet(J(R"({"on": true, "always": true})"), &pon, &palways, &err) && pon == 1 && palways == 1);
  CHECK(ParsePracticeSet(J(R"({"always": false})"), &pon, &palways, &err) && pon == -1 && palways == 0);
  CHECK(!ParsePracticeSet(J(R"({})"), &pon, &palways, &err));
  CHECK(!ParsePracticeSet(J(R"({"on": 1})"), &pon, &palways, &err));
  {
    bool hasHome = false;
    std::string home;
    CHECK(ParsePracticeSet(J(R"({"home_map": "3070284539"})"), &pon, &palways, &err, &hasHome, &home) && hasHome &&
          home == "3070284539" && pon == -1 && palways == -1);
    CHECK(ParsePracticeSet(J(R"({"home_map": ""})"), &pon, &palways, &err, &hasHome, &home) && hasHome && home.empty());
    CHECK(!ParsePracticeSet(J(R"({"home_map": "de_dust2; quit"})"), &pon, &palways, &err, &hasHome, &home));
    CHECK(!ParsePracticeSet(J(R"({"home_map": 5})"), &pon, &palways, &err, &hasHome, &home));
    bool hasCvars = false;
    std::vector<std::string> cvars;
    CHECK(ParsePracticeSet(J(R"({"home_cvars": ["sv_airaccelerate 150"]})"), &pon, &palways, &err, &hasHome, &home,
                           &hasCvars, &cvars) &&
          hasCvars && cvars.size() == 1 && !hasHome);
    CHECK(!ParsePracticeSet(J(R"({"home_cvars": "sv_x 1"})"), &pon, &palways, &err, &hasHome, &home, &hasCvars, &cvars));
  }

  CHECK_EQ(SanitizeSay("hi\x01 there\n"), std::string("hi there"));
  CHECK_EQ(SayCenterHtml("Map 2: <b>Nuke</b> & \"more\""), std::string("Map 2: &lt;b&gt;Nuke&lt;/b&gt; &amp; &quot;more&quot;"));
  CHECK(SayCenterSeconds(0) == 0 && SayCenterSeconds(-3) == 0 && SayCenterSeconds(5) == 5 && SayCenterSeconds(99) == 15);
  CHECK(SanitizeSay(std::string(300, 'x')).size() == 190);
  std::string utf;
  for (int i = 0; i < 100; ++i) utf += "\xc3\xa6";  // 'ae' x 100 = 200 bytes
  const std::string cut = SanitizeSay(utf);
  CHECK(cut.size() == 190);
  CHECK((static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);  // ends after a whole character

  // say: up to 3 lines of 190 bytes, broken at a space; the admin prefix; team.
  CHECK(SplitSay("  ").empty());
  CHECK(SplitSay("hello").size() == 1 && SplitSay("hello")[0] == "hello");
  {
    std::string words;
    for (int i = 0; i < 60; ++i) words += "word" + std::to_string(i % 10) + " ";  // 360 bytes
    const auto lines = SplitSay(words);
    CHECK(lines.size() == 2);
    for (const auto& l : lines) CHECK(l.size() <= 190 && l.front() != ' ' && l.back() != ' ');
    CHECK(SplitSay(std::string(1000, 'x')).size() == 3);
    for (const auto& l : SplitSay(std::string(1000, 'x'))) CHECK(l.size() == 190);
  }
  CHECK_EQ(AdminSayLine("", "go"), std::string(" \x07[ADMIN]\x01 go"));
  CHECK_EQ(AdminSayLine("Sivert", "go"), std::string(" \x07[ADMIN]\x01 \x10Sivert\x01: go"));
  CHECK(SayTeam("") == 0 && SayTeam("team1") == 1 && SayTeam("team2") == 2 && SayTeam("ct") == -1);

  CHECK_EQ(ResultPayload(Ok(), ""), std::string(R"({"status":"ok"})"));
  CHECK_EQ(ResultPayload(Rejected("unsupported", "no match.so"), "a1"),
           std::string(R"({"status":"rejected","error":{"code":"unsupported","message":"no match.so"},"audit_id":"a1"})"));
}

int main() {
  RUN(TestSelftestPayload);
  RUN(TestPublicAddr);
  RUN(TestJsonRoundTrip);
  RUN(TestUlid);
  RUN(TestTypes);
  RUN(TestEnvelope);
  RUN(TestBackoffSchedule);
  RUN(TestCloseCodes);
  RUN(TestRedaction);
  RUN(TestUrls);
  RUN(TestRxTracking);
  RUN(TestSpoolPersistence);
  RUN(TestSpoolLimits);
  RUN(TestCredentialsFile);
  RUN(TestPluginsState);
  RUN(TestServerConfigLocal);
  RUN(TestOwnedCmds);
  return ftest::Finish("fleet_unit_test");
}
