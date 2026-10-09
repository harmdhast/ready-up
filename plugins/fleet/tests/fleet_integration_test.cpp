// Integration test: the real fleet::Client (libcurl WebSocket, spool, credentials) against the
// mock platform in tests/mock_platform.cpp over 127.0.0.1. Covers enroll, hello/welcome, ping/pong,
// seq/ack both ways, unknown types, reconnect with backoff, resume with replay, reset + snapshot,
// resume after a process restart, heartbeat timeout, 4401 and a refused one-time code, the bulk
// lane's priority, and demo streaming end to end (FLEET.md §12.2).
// Timing: the waits are generous, but a heavily loaded box (game servers, parallel builds) can
// still make a WaitFor miss; rerun before suspecting the code.
//   build/fleet_integration_test
#include "fleet_client.h"
#include "fleet_demo.h"
#include "mock_platform.h"
#include "schema_check.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <thread>

using namespace fleet;

namespace {

// Payloads that match the step-3 schemas (protocol/v1/messages/event.*.json, state.snapshot):
// CheckSchemas() validates every frame, so the transport tests send real shapes.
std::string Ev(const std::string& data, int rev = 1) {
  return R"({"match_id":"m1","map_number":1,"rev":)" + std::to_string(rev) + R"(,"patch":{"live_rev":)" +
         std::to_string(rev) + R"(},"data":)" + data + "}";
}
std::string RoundEnd(int round) {
  return Ev(R"({"round":{"round_number":)" + std::to_string(round) +
                R"(,"winner_side":3,"winner_team":1,"reason":8,"team1_score":1,"team2_score":0,"team1_was_ct":true,"players":[]}})",
            round);
}
const char* kMapResultData =
    R"({"type":"map_result","winner":"team1","team1_series_score":1,"team2_series_score":0,"map_number":1,)"
    R"("map_name":"de_dust2","team1_score":13,"team2_score":5,"series_over":true,"stats":{"live":false,)"
    R"("team1_is_ct":true,"team1":{"score":13,"score_ct":7,"score_t":6},"team2":{"score":5,"score_ct":2,"score_t":3},)"
    R"("players":[],"rounds":[]}})";
const char* kSeriesEndData = R"({"type":"series_end","winner":"team1","team1_series_score":1,"team2_series_score":0})";
const char* kState =
    R"({"match_id":"m1","epoch":2,"config_rev":1,"live_rev":0,"phase":"warmup","series":{"num_maps":1,)"
    R"("current_map":1,"score":{"team1":0,"team2":0},"maps":{}},"teams":{"team1":{"name":"A","score":0,"players":{}},)"
    R"("team2":{"name":"B","score":0,"players":{}}}})";

std::string TempDir() {
  char tmpl[] = "/tmp/fleet_it_XXXXXX";
  const char* d = mkdtemp(tmpl);
  return d ? d : "/tmp";
}

ClientConfig BaseConfig(const mock::Platform& p, const std::string& dir) {
  ClientConfig c;
  c.url = p.BaseUrl();
  c.insecureDev = true;
  c.dataDir = dir;
  c.backoff.baseMs = 100;
  c.backoff.capMs = 400;
  c.backoff.resetAfterMs = 60000;
  c.connectTimeoutMs = 3000;
  c.helloTimeoutMs = 3000;
  c.rngSeed = 7;
  c.log = [](int level, const std::string& msg) { std::printf("    [client %d] %s\n", level, msg.c_str()); };
  return c;
}

HelloInfo Hello() {
  HelloInfo h;
  h.coreVersion = "0.9.0 (test)";
  h.plugins = {{"fleet", "0.9.0"}};
  h.cs2Build = 14032;
  h.cs2Patch = "1.40.3.2";
  h.hostname = "it-host";
  h.gamePort = 27055;
  h.publicAddr = "203.0.113.7:27055";
  h.bootId = NewUlid(NowMs());
  h.capabilities = {"match.v1"};
  h.startedMs = NowMs();
  h.pluginsDisabled = {{"skins", "missing CBaseModelEntity_SetModel after CS2 build 14032"}};
  // hello.plugins_state is read from here at every hello.
  static const std::string pluginsDir = [] {
    const std::string d = TempDir();
    std::ofstream(d + "/match.so") << "";
    std::ofstream(d + "/skins.so") << "";
    std::ofstream(d + "/plugins.json") << "{\"version\": 1, \"disabled\": [\"skins\"]}";
    return d;
  }();
  h.pluginsDir = pluginsDir;
  return h;
}

bool WaitState(Client& c, LinkState s, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (c.Status().state == s) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return c.Status().state == s;
}

std::string StreamOf(const mock::Received& hello) {
  return hello.payload()->Get("stream")->Get("id")->AsStr();
}

// Every frame fleet.so sent (and the last enrollment body) must validate against the platform's
// schemas in plugins/fleet/protocol/v1 (FLEET.md D18).
const schema::Set& Schemas() {
  static schema::Set set = [] {
    schema::Set s;
    std::string err;
    if (!s.LoadDir(FLEET_PROTOCOL_DIR, &err)) std::printf("  cannot load schemas: %s\n", err.c_str());
    return s;
  }();
  return set;
}
const char* kBase = "https://auto-tournament.dev/fleet/v1/";

void CheckSchemas(mock::Platform& p) {
  const auto& set = Schemas();
  CHECK(set.size() >= 10);
  int frames = 0, payloads = 0;
  for (const auto& m : p.Messages()) {
    std::vector<std::string> errs;
    const bool envOk = set.Validate(m.env, std::string(kBase) + "envelope.json", &errs);
    const std::string id = std::string(kBase) + "messages/" + m.type() + ".json";
    bool payloadOk = true;
    if (set.Has(id) && m.payload()) {
      payloadOk = set.Validate(*m.payload(), id, &errs);
      ++payloads;
    }
    ++frames;
    if (!envOk || !payloadOk) {
      std::printf("  schema: %s frame invalid:\n", m.type().c_str());
      for (const auto& e : errs) std::printf("    %s\n", e.c_str());
    }
    CHECK(envOk && payloadOk);
  }
  if (p.enrollments() > 0) {
    fleet::json::Value body;
    CHECK(fleet::json::Parse(p.lastEnrollBody(), &body));
    std::vector<std::string> errs;
    const bool ok = set.Validate(body, std::string(kBase) + "http/enroll.request.json", &errs);
    for (const auto& e : errs) std::printf("  schema: enroll body: %s\n", e.c_str());
    CHECK(ok);
  }
  std::printf("  schema: %d frames checked (%d payloads)\n", frames, payloads);
}

const char* kKey = "rfk_k1k1k1k1k1k1_c2VjcmV0c2VjcmV0c2VjcmV0c2VjcmV0c2VjcmV0c2V";

}  // namespace

// The validator itself: it must reject what the schemas forbid.
TEST(TestSchemaValidator) {
  const auto& set = Schemas();
  fleet::json::Value v;
  std::vector<std::string> errs;
  CHECK(fleet::json::Parse(R"({"v":1,"type":"hello","id":"01J8ZQ4T8W6N3X0F2R5K7M9P1C","ts":1,"payload":{}})", &v));
  CHECK(set.Validate(v, std::string(kBase) + "envelope.json", &errs));
  CHECK(fleet::json::Parse(R"({"v":2,"type":"Hello","id":"x","ts":1,"payload":{},"extra":1})", &v));
  errs.clear();
  CHECK(!set.Validate(v, std::string(kBase) + "envelope.json", &errs));
  CHECK(errs.size() >= 4);  // v const, type pattern, id ulid, additionalProperties
  CHECK(fleet::json::Parse(R"({"server_id":"s","install_id":"short","tenant_id":"default"})", &v));
  errs.clear();
  CHECK(!set.Validate(v, std::string(kBase) + "messages/hello.json", &errs));  // missing fields, short install_id
  CHECK(fleet::json::Parse(R"({"install_id":"abcdefgh","host":{"hostname":"h","game_port":27015}})", &v));
  errs.clear();
  CHECK(!set.Validate(v, std::string(kBase) + "http/enroll.request.json", &errs));  // neither code nor key
}

// Enroll with a one-time code, then hello/welcome/ping/pong, reliable both ways.
TEST(TestEnrollConnectPingAck) {
  mock::Platform p;
  CHECK(p.Start());
  p.heartbeatIntervalMs = 300;
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-7F3K-9QX2-LM4D";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  c.SetHandledTypes({"test.echo"}, false);
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  CHECK_EQ(p.enrollments(), 1);
  // Enrollment body: code + install_id + versions; credentials.json 0600 with the token.
  json::Value body;
  CHECK(json::Parse(p.lastEnrollBody(), &body));
  CHECK_EQ(body.Get("code")->AsStr(), std::string("RUE-7F3K-9QX2-LM4D"));
  CHECK_EQ(body.Get("install_id")->AsStr(), c.installId());
  CHECK_EQ(body.Get("cs2_build")->AsInt(), int64_t(14032));
  struct stat st {};
  CHECK(stat((dir + "/credentials.json").c_str(), &st) == 0);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  CHECK_EQ(p.lastAuthHeader(), "Bearer " + p.token);
  // hello
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("hello").empty(); }, 2000));
  const auto hello = p.MessagesOfType("hello").at(0);
  const json::Value* hp = hello.payload();
  CHECK_EQ(hp->Get("server_id")->AsStr(), p.serverId);
  CHECK_EQ(hp->Get("install_id")->AsStr(), c.installId());
  CHECK_EQ(hp->Get("tenant_id")->AsStr(), std::string("default"));
  CHECK_EQ(hp->Get("protocol")->Get("max")->AsInt(), int64_t(1));
  CHECK_EQ(hp->Get("versions")->Get("cs2_build")->AsInt(), int64_t(14032));
  CHECK_EQ(hp->Get("versions")->Get("plugins")->Get("fleet")->AsStr(), std::string("0.9.0"));
  CHECK_EQ(hp->Get("capabilities")->a.at(0).AsStr(), std::string("match.v1"));
  CHECK_EQ(hp->Get("host")->Get("game_port")->AsInt(), int64_t(27055));
  CHECK_EQ(hp->Get("host")->Get("public_addr")->AsStr(), std::string("203.0.113.7:27055"));
  CHECK_EQ(hp->Get("stream")->Get("last_tx_seq")->AsInt(), int64_t(0));
  CHECK(hp->Get("state")->IsNull());
  CHECK_EQ(hp->Get("availability")->AsStr(), std::string("available"));
  CHECK(hp->Get("reason") && hp->Get("reason")->IsNull());
  CHECK_EQ(hp->Get("plugins_disabled")->a.at(0).Get("name")->AsStr(), std::string("skins"));
  CHECK_EQ(hp->Get("plugins_state")->Get("installed")->a.size(), size_t(2));
  CHECK_EQ(hp->Get("plugins_state")->Get("installed")->a.at(0).AsStr(), std::string("match"));
  CHECK_EQ(hp->Get("plugins_state")->Get("disabled")->a.at(0).AsStr(), std::string("skins"));
  CHECK(IsUlid(hello.env.Get("id")->AsStr()));
  CHECK(hello.env.Get("seq") == nullptr);  // hello is ephemeral
  CHECK(IsUlid(c.Status().sessionId));
  // ping with health every heartbeat interval (300 ms here)
  CHECK(p.WaitFor([&] { return p.MessagesOfType("ping").size() >= 2; }, 3000));
  const auto ping = p.MessagesOfType("ping").at(0);
  CHECK(ping.payload()->Get("health") && ping.payload()->Get("health")->Get("spool_msgs"));
  CHECK(p.WaitFor([&] { return c.Status().rttMs >= 0; }, 2000));  // our ping got the mock's pong
  // platform ping -> our pong carries t and ref
  CHECK(p.SendToServer("ping", R"({"t":424242})", false));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("pong").empty(); }, 2000));
  CHECK_EQ(p.MessagesOfType("pong").at(0).payload()->Get("t")->AsInt(), int64_t(424242));
  // server -> platform reliable: seq 1, acked, spool drained
  CHECK(c.Send("event.phase", Ev(R"({"from":"warmup","to":"live","reason":"flow"})"), 3, true, &err));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("event.phase").empty(); }, 2000));
  const auto ev = p.MessagesOfType("event.phase").at(0);
  CHECK_EQ(ev.seq(), int64_t(1));
  CHECK_EQ(ev.env.Get("epoch")->AsInt(), int64_t(3));
  CHECK(p.WaitFor([&] { return c.Status().ackedSeq == 1 && c.Status().spoolMsgs == 0; }, 2000));
  // Queue order is kept between reliable and ephemeral messages: a snapshot queued between two
  // events goes out between them (the platform applies patches on top of it).
  c.SetState(kState, "busy");
  CHECK(c.Send("event.phase", Ev(R"({"from":"live","to":"paused","reason":"flow"})", 2), 3, true, &err));
  CHECK(c.SendSnapshot("assign", ""));
  CHECK(c.Send("state.patch", R"({"match_id":"m1","rev":3,"patch":{"live_rev":3}})", 3, true, &err));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("state.patch").empty(); }, 2000));
  {
    int iPhase = -1, iSnap = -1, iPatch = -1, i = 0;
    for (const auto& m : p.Messages()) {
      if (m.type() == "event.phase" && m.seq() == 2) iPhase = i;
      if (m.type() == "state.snapshot" && m.payload()->Get("reason")->AsStr() == "assign") iSnap = i;
      if (m.type() == "state.patch") iPatch = i;
      ++i;
    }
    CHECK(iPhase >= 0 && iPhase < iSnap && iSnap < iPatch);
    const auto snap = p.MessagesOfType("state.snapshot").back();
    CHECK_EQ(snap.env.Get("epoch")->AsInt(), int64_t(2));  // the published state's epoch
    CHECK_EQ(snap.payload()->Get("config_rev")->AsInt(), int64_t(1));
  }
  CHECK(!c.Send("hello", "{}", 0, true, &err));        // reserved type
  CHECK(!c.Send("event.x", "[1]", 0, true, &err));     // payload must be an object
  // platform -> server reliable, unknown type: error{unknown_type} + ack
  CHECK(p.SendToServer("server.drain", R"({"reason":"x"})", true));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("error").empty(); }, 2000));
  CHECK_EQ(p.MessagesOfType("error").at(0).payload()->Get("code")->AsStr(), std::string("unknown_type"));
  CHECK(p.WaitFor([&] { return p.lastAckFromServer() >= 1; }, 2000));
  // handled type: delivered to the "game thread", acked only after MarkProcessed
  CHECK(p.SendToServer("test.echo", R"({"n":"\u00e9"})", true));
  std::deque<Inbound> in;
  CHECK(p.WaitFor([&] {
    auto more = c.TakeInbound();
    for (auto& m : more) in.push_back(std::move(m));
    return std::any_of(in.begin(), in.end(), [](const Inbound& i) { return i.env.type == "test.echo"; });
  }, 2000));
  const auto it = std::find_if(in.begin(), in.end(), [](const Inbound& i) { return i.env.type == "test.echo"; });
  CHECK(it != in.end() && it->reliable && it->env.seq == 2);
  CHECK(it != in.end() && it->payloadJson == "{\"n\":\"\xc3\xa9\"}");
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  CHECK_EQ(p.lastAckFromServer(), int64_t(1));  // not processed yet: no ack for 2
  if (it != in.end()) c.MarkProcessed(it->env.seq);
  CHECK(p.WaitFor([&] { return p.lastAckFromServer() >= 2; }, 2500));  // standalone ack within ~1 s
  CHECK_EQ(c.Status().rxSeq, int64_t(2));
  CheckSchemas(p);
  c.Stop();
  p.Stop();
}

// Reconnect with backoff after a close and a TCP drop; resume replays exactly what the
// platform had not durably received; a platform that forgot the stream gets a reset + snapshot.
TEST(TestReconnectResumeReset) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollKey = kKey;
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  json::Value body;
  CHECK(json::Parse(p.lastEnrollBody(), &body) && body.Get("key") && !body.Get("code"));
  const std::string stream = StreamOf(p.MessagesOfType("hello").at(0));
  // 1. close 1001 -> reconnect, resumed
  p.CloseCurrent(1001, "going away");
  CHECK(p.WaitFor([&] { return p.connections() >= 2; }, 3000));
  CHECK(WaitState(c, LinkState::Online, 3000));
  CHECK_EQ(c.Status().sessions, uint32_t(2));
  CHECK_EQ(c.Status().resume, std::string("resumed"));
  // 2. messages the platform receives but never commits: drop, resume from its seq
  p.autoAck = false;
  for (int i = 1; i <= 3; ++i) CHECK(c.Send("event.round_end", RoundEnd(i), 1, true, &err));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("event.round_end").size() == 3; }, 2000));
  CHECK_EQ(c.Status().spoolMsgs, uint32_t(3));
  p.SetPlatformRxSeq(stream, 1);  // only seq 1 was committed
  p.autoAck = true;
  p.DropCurrent();
  CHECK(p.WaitFor([&] { return p.connections() >= 3; }, 3000));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("event.round_end").size() == 5; }, 3000));
  const auto rounds = p.MessagesOfType("event.round_end");
  CHECK_EQ(rounds.at(3).seq(), int64_t(2));  // replay 2 and 3 only
  CHECK_EQ(rounds.at(4).seq(), int64_t(3));
  CHECK_EQ(rounds.at(3).conn, 3);
  const auto hellos = p.MessagesOfType("hello");
  CHECK_EQ(hellos.back().payload()->Get("stream")->Get("last_tx_seq")->AsInt(), int64_t(3));
  CHECK_EQ(StreamOf(hellos.back()), stream);
  CHECK(p.WaitFor([&] { return c.Status().spoolMsgs == 0 && c.Status().ackedSeq == 3; }, 2000));
  // Only the very first session (a stream the platform did not know yet) was a reset.
  CHECK_EQ(p.MessagesOfType("state.snapshot").size(), size_t(1));
  // 3. platform forgot the stream: welcome says reset -> replay, then a state.snapshot
  c.SetState(kState, "busy");
  p.forgetStreams = true;
  p.CloseCurrent(4503, "deploy");
  CHECK(p.WaitFor([&] { return p.MessagesOfType("state.snapshot").size() >= 2; }, 5000));
  const auto snap = p.MessagesOfType("state.snapshot").back();
  CHECK_EQ(snap.payload()->Get("reason")->AsStr(), std::string("reset"));
  const fleet::json::Value* st = snap.payload()->Get("state");
  CHECK(st && st->IsObj() && st->Get("match_id") && st->Get("match_id")->AsStr() == "m1");
  CHECK_EQ(snap.payload()->Get("availability")->AsStr(), std::string("busy"));
  CHECK_EQ(c.Status().resume, std::string("reset"));
  p.forgetStreams = false;
  // 4. state.request -> snapshot with reason request
  CHECK(p.SendToServer("state.request", "{}", false));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("state.snapshot").size() >= 3; }, 2000));
  CHECK_EQ(p.MessagesOfType("state.snapshot").back().payload()->Get("reason")->AsStr(), std::string("request"));
  CheckSchemas(p);
  c.Stop();
  p.Stop();
}

// Unacked messages survive a process restart: the new client resumes the same stream.
TEST(TestResumeAfterRestart) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
  std::string stream;
  {
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Online, 5000));
    stream = StreamOf(p.MessagesOfType("hello").at(0));
    p.autoAck = false;
    CHECK(c.Send("event.map_result", Ev(kMapResultData, 1), 1, true, &err));
    CHECK(c.Send("event.series_end", Ev(kSeriesEndData, 2), 1, true, &err));
    CHECK(p.WaitFor([&] { return p.MessagesOfType("event.series_end").size() == 1; }, 2000));
    p.SetPlatformRxSeq(stream, 0);  // nothing committed
    c.Stop();
  }
  p.autoAck = true;
  Client c2(cfg);  // same data dir: install_id, credentials, spool
  c2.SetHelloInfo(Hello());
  std::string err;
  CHECK(c2.Start(&err));
  CHECK(WaitState(c2, LinkState::Online, 5000));
  CHECK_EQ(p.enrollments(), 1);  // credentials reused, no second enrollment
  const auto hellos = p.MessagesOfType("hello");
  CHECK_EQ(StreamOf(hellos.back()), stream);
  CHECK_EQ(hellos.back().payload()->Get("stream")->Get("last_tx_seq")->AsInt(), int64_t(2));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("event.series_end").size() == 2; }, 3000));
  CHECK_EQ(p.MessagesOfType("event.map_result").back().seq(), int64_t(1));
  CHECK(p.WaitFor([&] { return c2.Status().spoolMsgs == 0; }, 2000));
  CheckSchemas(p);
  c2.Stop();
  p.Stop();
}

// Offline spooling: messages queued while the platform is down go out after it comes back.
TEST(TestOfflineSpoolThenConnect) {
  mock::Platform p;
  CHECK(p.Start());
  const int port = p.port();
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  p.Stop();  // platform down
  CHECK(WaitState(c, LinkState::Offline, 3000));
  for (int i = 0; i < 5; ++i) CHECK(c.Send("event.player_ready",
                                          Ev(R"({"steamid64":"7656119800000000)" + std::to_string(i) +
                                                 R"(","team":"team1","ready_team1":1,"ready_team2":0,"required":5})",
                                             i + 1),
                                          1, true, &err));
  CHECK(p.WaitFor([&] { return c.Status().spoolMsgs == 5; }, 2000));
  CHECK(c.Status().offlineSinceMs > 0);
  mock::Platform p2;
  CHECK(p2.Start(port));
  p2.token = p.token;
  CHECK(p2.WaitFor([&] { return p2.MessagesOfType("event.player_ready").size() == 5; }, 5000));
  CHECK(WaitState(c, LinkState::Online, 2000));
  CHECK_EQ(c.Status().offlineSinceMs, int64_t(0));
  // p2 never saw the stream, so it is a reset: replay then snapshot
  CHECK(p2.WaitFor([&] { return !p2.MessagesOfType("state.snapshot").empty(); }, 2000));
  c.Stop();
  p2.Stop();
}

// No frame for timeout_ms -> the client closes and reconnects.
TEST(TestHeartbeatTimeout) {
  mock::Platform p;
  CHECK(p.Start());
  p.heartbeatIntervalMs = 1000;
  p.heartbeatTimeoutMs = 3000;
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  p.silent = true;
  CHECK(WaitState(c, LinkState::Offline, 6000));
  CHECK(c.Status().lastError.find("no frame") != std::string::npos);
  p.silent = false;
  CHECK(WaitState(c, LinkState::Online, 3000));
  CHECK(p.connections() >= 2);
  c.Stop();
  p.Stop();
}

// The game thread stops while players are on: the link closes and stays down until frames
// come back. With nobody on (an empty server hibernates) a frame gap is not a stall.
TEST(TestGameThreadStall) {
  mock::Platform p;
  CHECK(p.Start());
  p.heartbeatIntervalMs = 500;
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
  cfg.stallMs = 1500;
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  c.NoteGameFrame();
  CHECK(WaitState(c, LinkState::Online, 5000));

  // No players: frames may stop (hibernation) and the link stays up.
  c.UpdateHealth(0, 0.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  CHECK(c.Status().state == LinkState::Online);

  // Players on and no frames: offline, and it stays offline while frames are missing.
  c.UpdateHealth(2, 0.0);
  CHECK(WaitState(c, LinkState::Offline, 4000));
  CHECK(c.Status().lastError.find("game thread stalled") != std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(c.Status().state == LinkState::Offline);

  // Frames again: back online.
  std::atomic<bool> ticking{true};
  std::thread frames([&] {
    while (ticking) {
      c.NoteGameFrame();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  CHECK(WaitState(c, LinkState::Online, 5000));
  ticking = false;
  frames.join();
  c.Stop();
  p.Stop();
}

// 4401 -> rejected; a refused one-time code stops retrying; a bad token never connects.
TEST(TestRejections) {
  {
    mock::Platform p;
    CHECK(p.Start());
    ClientConfig cfg = BaseConfig(p, TempDir());
    cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Online, 5000));
    p.CloseCurrent(4401, "token revoked");
    CHECK(WaitState(c, LinkState::Rejected, 3000));
    CHECK(c.Status().lastError.find("4401") != std::string::npos);
    // A manual reconnect goes straight back (the mock accepts the token again).
    c.RequestReconnect();
    CHECK(WaitState(c, LinkState::Online, 3000));
    c.Stop();
    p.Stop();
  }
  {
    mock::Platform p;
    CHECK(p.Start());
    ClientConfig cfg = BaseConfig(p, TempDir());
    cfg.enrollCode = "RUE-BAD0-BAD0-BAD0";
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Rejected, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK_EQ(p.enrollments(), 1);  // no retry storm with a dead code
    CHECK(c.Status().lastError.find("403") != std::string::npos);
    CHECK(c.Status().lastError.find("BAD0") == std::string::npos);  // code redacted
    // `ru fleet enroll <new code>` path
    c.RequestEnroll("", "RUE-GOOD-GOOD-GOOD");
    CHECK(WaitState(c, LinkState::Online, 5000));
    CHECK_EQ(p.enrollments(), 2);
    c.Stop();
    p.Stop();
  }
  {
    mock::Platform p;
    CHECK(p.Start());
    p.rejectUpgradeStatus = 403;
    ClientConfig cfg = BaseConfig(p, TempDir());
    cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Rejected, 5000));
    CHECK(c.Status().lastError.find("403") != std::string::npos);
    c.Stop();
    p.Stop();
  }
  {
    // Plain http:// without the explicit insecure opt-in is refused before any network I/O.
    ClientConfig cfg;
    cfg.url = "http://8.8.8.8:1";
    cfg.insecureDev = false;
    cfg.dataDir = TempDir();
    cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
    Client c(cfg);
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Rejected, 2000));
    CHECK(c.Status().lastError.find("--insecure") != std::string::npos);
    c.Stop();
  }
}

// auth.rotate: the new token is written (0600) and used on the next connect; auth.rotated is
// sent reliably with ref = the rotate message.
TEST(TestTokenRotation) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollKey = kKey;
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  const std::string oldToken = p.token;
  const std::string newToken = "rus_n2n2n2n2n2n2_bmV3LXNlY3JldC1uZXctc2VjcmV0LW5ldy1zZWNyZXQ";
  CHECK(p.SendToServer("auth.rotate", "{\"token\":\"" + newToken + "\",\"old_valid_until\":" +
                                          std::to_string(NowMs() + 86400000) + "}", true));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("auth.rotated").empty(); }, 3000));
  const auto rotated = p.MessagesOfType("auth.rotated").at(0);
  CHECK(rotated.seq() >= 1);
  CHECK(rotated.env.Get("ref") && rotated.env.Get("ref")->IsStr());
  CHECK(p.WaitFor([&] { return p.lastAckFromServer() >= 1; }, 3000));
  Credentials cr;
  CHECK(LoadCredentials(dir + "/credentials.json", &cr, &err));
  CHECK(cr.token == newToken);
  // The platform now only accepts the new token.
  p.token = newToken;
  p.CloseCurrent(1001, "");
  CHECK(p.WaitFor([&] { return p.connections() >= 2; }, 3000));
  CHECK(WaitState(c, LinkState::Online, 3000));
  CHECK(p.lastAuthHeader() == "Bearer " + newToken);
  CHECK(oldToken != newToken);
  CheckSchemas(p);
  c.Stop();
  p.Stop();
}

// The bulk lane (demo.chunk) only moves when the normal lane is empty: an event queued after
// bulk messages still goes out before them.
TEST(TestBulkLanePriority) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollKey = kKey;
  cfg.bulkMaxBytes = 4096;
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(!c.SendBulk("demo.chunk", "{}", 1, &err) && err == "not online");
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  const std::string id = NewUlid(NowMs());
  auto chunk = [&](int off) {
    return R"({"demo_id":")" + id + R"(","offset":)" + std::to_string(off) + R"(,"size":3,"data":"YWJj"})";
  };
  c.HoldOutboundForTest(true);
  CHECK(c.SendBulk("demo.chunk", chunk(0), 1, &err));
  CHECK(c.SendBulk("demo.chunk", chunk(3), 1, &err));
  CHECK(c.SendBulk("demo.chunk", chunk(6), 1, &err));
  CHECK(c.Status().bulkQueuedBytes > 0);
  // Lane full: refused, the caller keeps the bytes (the file is the buffer).
  std::string big = R"({"demo_id":")" + id + R"(","offset":9,"size":3,"data":")" + std::string(4000, 'A') + R"("})";
  CHECK(!c.SendBulk("demo.chunk", big, 1, &err) && err == "bulk lane full");
  CHECK(c.Send("event.phase", Ev(R"({"from":"warmup","to":"live","reason":"flow"})"), 1, true, &err));
  c.HoldOutboundForTest(false);
  CHECK(p.WaitFor([&] { return p.MessagesOfType("demo.chunk").size() >= 3; }, 3000));
  int evAt = -1, firstChunk = -1, i = 0;
  for (const auto& m : p.Messages()) {
    if (m.type() == "event.phase" && evAt < 0) evAt = i;
    if (m.type() == "demo.chunk" && firstChunk < 0) firstChunk = i;
    ++i;
  }
  CHECK(evAt >= 0 && firstChunk > evAt);
  CHECK(c.Status().bulkSent >= 3);
  CheckSchemas(p);
  c.Stop();
  p.Stop();
}

// Demo streaming against the mock platform: a growing file, a dropped connection mid-way
// (resume from the platform's offset, not from zero), the final pass, stored + verified, and
// the local copy deleted only after that.
TEST(TestDemoStreaming) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollKey = kKey;
  Client c(cfg);
  c.SetHelloInfo(Hello());
  c.SetHandledTypes({"demo.ack"}, false);
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));

  demo::Config dc;
  dc.chunkBytes = 16 * 1024;
  dc.windowBytes = 64 * 1024;
  dc.settleMs = 300;
  dc.ackTimeoutMs = 3000;
  dc.keepAfterStoredMs = 500;
  demo::Streams streams(dc, dir + "/demos.json",
                        [](int level, const std::string& msg) { std::printf("    [demo %d] %s\n", level, msg.c_str()); });
  demo::Link link;
  link.online = [&] { return c.Status().state == LinkState::Online; };
  link.session = [&] { return c.Status().sessions; };
  link.send = [&](const std::string& t, const std::string& pl, int64_t e) {
    std::string why;
    return c.Send(t, pl, e, false, &why);
  };
  link.sendBulk = [&](const std::string& t, const std::string& pl, int64_t e) {
    std::string why;
    return c.SendBulk(t, pl, e, &why);
  };
  auto pump = [&](int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      for (auto& in : c.TakeInbound()) {
        if (in.env.type == "demo.ack") streams.OnAck(in.payloadJson);
      }
      streams.Tick(link, NowMs());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  };
  const std::string path = dir + "/2026-09-29_it-1_de_nuke_A_vs_B.dem";
  auto grow = [&](size_t n, unsigned seed) {
    std::string s(n, '\0');
    uint32_t x = seed;
    for (auto& ch : s) ch = static_cast<char>((x = x * 1103515245u + 12345u) >> 16);
    std::ofstream(path, std::ios::binary | std::ios::app) << s;
  };
  demo::Spec spec;
  spec.matchId = "it-1";
  spec.epoch = 4;
  spec.mapNumber = 1;
  spec.path = path;
  const std::string id = streams.Begin(spec, NowMs());
  CHECK(!id.empty());
  grow(200 * 1024, 1);
  pump(1500);
  auto demos = p.Demos();
  CHECK(demos.count(id) == 1 && demos[id].stored == 192 * 1024);  // full chunks while recording
  CHECK_EQ(demos[id].matchId, std::string("it-1"));
  CHECK_EQ(demos[id].epoch, int64_t(4));
  // Connection lost mid-demo: the next session resumes from the platform's offset.
  const int64_t before = demos[id].chunkBytes;
  p.DropCurrent();
  grow(300 * 1024, 2);
  CHECK(p.WaitFor([&] { return p.connections() >= 2; }, 5000));
  pump(2500);
  demos = p.Demos();
  CHECK(demos[id].begins >= 2);
  CHECK(demos[id].stored == 496 * 1024);
  // Not from zero: at most one window was sent twice.
  CHECK(demos[id].chunkBytes - before <= (496 - 192) * 1024 + dc.windowBytes);
  // The recording stops: the header changes (CS2 does that), the tail goes out, then demo.end.
  {
    FILE* f = std::fopen(path.c_str(), "r+b");
    std::fwrite("PBDEMS2", 1, 7, f);
    std::fclose(f);
  }
  grow(5000, 3);
  streams.End(path, NowMs());
  CHECK(p.WaitFor([&] { pump(100); return p.Demos()[id].complete; }, 6000));
  std::ifstream in(path, std::ios::binary);
  const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(p.Demos()[id].data == file);
  CHECK(!p.MessagesOfType("demo.end").empty());
  // Deleted after keepAfterStoredMs, never before the platform confirmed it.
  CHECK(p.WaitFor([&] { pump(100); return access(path.c_str(), F_OK) != 0; }, 3000));
  CHECK_EQ(streams.Unfinished(), size_t(0));
  CheckSchemas(p);
  c.Stop();
  p.Stop();
}

// The busy reason rides on hello (reason / detail) and state.snapshot (availability_reason /
// availability_detail); publishing a plain availability clears it.
TEST(TestAvailabilityReason) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-7F3K-9QX2-LM4D";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  c.SetAvailability("busy", "local_match", "cup-final");
  {
    HelloInfo again = Hello();  // a later refresh (capabilities), as fleet.so sends it: no availability
    again.availability.clear();
    c.SetHelloInfo(again);
  }
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("hello").empty(); }, 2000));
  const auto hello = p.MessagesOfType("hello").at(0);
  CHECK_EQ(hello.payload()->Get("availability")->AsStr(), std::string("busy"));
  CHECK_EQ(hello.payload()->Get("reason")->AsStr(), std::string("local_match"));
  CHECK_EQ(hello.payload()->Get("detail")->AsStr(), std::string("cup-final"));
  CHECK(c.SendSnapshot("request", ""));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("state.snapshot").empty(); }, 2000));
  const auto snap = p.MessagesOfType("state.snapshot").back();
  CHECK_EQ(snap.payload()->Get("availability_reason")->AsStr(), std::string("local_match"));
  CHECK_EQ(snap.payload()->Get("availability_detail")->AsStr(), std::string("cup-final"));
  auto requests = [&] {
    std::vector<mock::Received> out;
    for (const auto& m : p.MessagesOfType("state.snapshot"))
      if (m.payload()->Get("reason")->AsStr() == "request") out.push_back(m);
    return out;
  };
  c.SetState("null", "available");
  CHECK(c.SendSnapshot("request", ""));
  CHECK(p.WaitFor([&] { return requests().size() >= 2; }, 2000));
  const auto snap2 = requests().back();
  CHECK(snap2.payload()->Get("availability_reason")->IsNull());
  CHECK(snap2.payload()->Get("availability_detail")->IsNull());
  CheckSchemas(p);
  c.Stop();
  p.Stop();
}

int main() {
  RUN(TestSchemaValidator);
  RUN(TestEnrollConnectPingAck);
  RUN(TestAvailabilityReason);
  RUN(TestReconnectResumeReset);
  RUN(TestResumeAfterRestart);
  RUN(TestOfflineSpoolThenConnect);
  RUN(TestHeartbeatTimeout);
  RUN(TestGameThreadStall);
  RUN(TestRejections);
  RUN(TestTokenRotation);
  RUN(TestBulkLanePriority);
  RUN(TestDemoStreaming);
  return ftest::Finish("fleet_integration_test");
}
