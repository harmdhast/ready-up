// The fleet link's network side (docs/FLEET.md §4, §6): enrollment over HTTPS and one
// outbound WebSocket on libcurl (curl_ws_*), on its own thread. Knows nothing about ru_api, so
// tests/fleet_integration_test.cpp drives it directly against a mock platform.
//
// Threads: every public member is thread-safe. The network thread never calls back into
// the game; inbound messages are queued and `wake` is called (the plugin turns that into
// post_to_game_thread), and the game thread reports back with MarkProcessed().
#pragma once

#include "fleet_proto.h"
#include "fleet_spool.h"
#include "fleet_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fleet {

enum class LinkState { Standalone, Unenrolled, Enrolling, Connecting, Online, Offline, Rejected };
const char* LinkStateName(LinkState s);

struct ClientConfig {
  std::string url;         // platform base URL, e.g. https://tournament.example.com
  std::string enrollCode;  // one-time code (RUE-...)
  std::string enrollKey;   // fleet enrollment key (rfk_...), reusable
  bool insecureDev = false;  // fleet.cfg insecure: allow plain ws:// / http:// (any host; token unencrypted)
  std::string caFile;        // extra CA bundle (private CA)
  std::string pinSha256;     // SPKI pin, base64 sha256 (CURLOPT_PINNEDPUBLICKEY "sha256//...")
  std::string dataDir;       // credentials.json, install_id, spool/
  Spool::Limits spool;
  BackoffPolicy backoff;
  int64_t connectTimeoutMs = 10000;
  int64_t helloTimeoutMs = 10000;
  int64_t httpTimeoutMs = 20000;
  size_t outboxMax = 20000;  // messages queued by the game thread, not yet on the net thread
  size_t bulkMaxBytes = 2u << 20;  // SendBulk payload bytes that may wait (the lowest-priority lane)
  std::string userAgent = "ReadyUp-fleet";
  // Game thread watchdog: with players on the server and no game frame for this long, the
  // link closes (1011 "game thread stalled") and stays down until frames come back, so the
  // platform sees the server offline and fails the match over. 0 turns it off. Long enough
  // for a map load or a workshop download; an empty server that hibernates has no players.
  int64_t stallMs = 45000;
  uint64_t rngSeed = 0;      // tests: deterministic jitter
  // Log sink, called from the network thread (level: 0 info, 1 warn, 2 error, 3 debug).
  // Messages are already redacted.
  std::function<void(int, const std::string&)> log;
};

// What goes into hello / state.snapshot / ping health. Set by the plugin, any thread.
struct HelloInfo {
  std::string coreVersion;
  std::string pluginApi = "1.1";
  std::vector<std::pair<std::string, std::string>> plugins;  // name -> version
  int64_t cs2Build = 0;
  std::string cs2Patch;
  std::string hostname;
  int gamePort = 0;
  int tvPort = 0;
  // hello.host.steam_token: the server logs in with a Valve game server token (GSLT). The
  // platform keeps skins off on such a server (Valve bans tokens of servers that hand out items).
  bool steamToken = false;
  // hello.host.public_addr: "host:port" players connect to (PickPublicAddr); "" = left out.
  std::string publicAddr;
  std::vector<std::string> capabilities;
  std::string bootId;
  std::string stateJson = "null";  // MatchState or null
  std::string availability = "available";
  // Busy reason / detail (hello.reason / detail, state.snapshot.availability_reason / _detail);
  // "" = null. Owned by SetAvailability / SetState: SetHelloInfo keeps them.
  std::string availabilityReason, availabilityDetail;
  int64_t adminsRev = -1;  // cached admins.set rev (hello.admins_rev); -1 = none
  // Plugins the core did not load on this CS2 build (needs.json not met): name -> reason
  // (hello.plugins_disabled; core/include/readyup/plugin_needs_iface.h). Empty = left out.
  std::vector<std::pair<std::string, std::string>> pluginsDisabled;
  // csgo/readyup/plugins: hello.plugins_state is read from it (ReadPluginsState) each time a
  // hello goes out, so a plugins.set before a reconnect shows. "" = left out.
  std::string pluginsDir;
  // hello.selftest: SelftestPayload() of the core's latest selftest; "" = none yet (left out).
  std::string selftestJson;
  // ping health
  int players = 0;
  double tickMsP99 = 0.0;
  int64_t startedMs = 0;
};

struct Inbound {
  Envelope env;
  std::string payloadJson;
  bool reliable = false;
  bool local = false;  // synthesized by the client (local.*)
};

struct ClientStatus {
  LinkState state = LinkState::Standalone;
  int64_t sinceMs = 0;
  int64_t offlineSinceMs = 0;     // 0 when online / never configured
  int64_t lastOnlineMs = 0;
  uint32_t sessions = 0;          // sessions established (welcome received)
  uint32_t connectAttempts = 0;   // WebSocket connection attempts
  uint32_t spoolMsgs = 0;
  uint64_t spoolBytes = 0;
  uint64_t spoolDropped = 0;
  int64_t txSeq = 0, ackedSeq = 0, rxSeq = 0;
  std::string streamId;
  std::string serverId;
  std::string installId;
  std::string sessionId;
  std::string resume;             // "resumed" | "reset" from the last welcome
  std::string lastError;          // redacted
  std::string url;                // platform base URL (no credentials in it)
  bool enrolled = false;
  int64_t nextAttemptMs = 0;      // unix ms of the next connect/enroll attempt (0 = none)
  int64_t heartbeatMs = 0, rttMs = -1;
  uint64_t framesIn = 0, framesOut = 0;
  uint64_t bulkQueuedBytes = 0;  // SendBulk payloads waiting
  uint64_t bulkSent = 0;         // SendBulk messages sent (all sessions)
};

class Client {
 public:
  explicit Client(ClientConfig cfg);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Loads install_id, credentials and the spool, starts the network thread.
  bool Start(std::string* err);
  // Closes the socket (1000) and joins the thread. Idempotent.
  void Stop();

  // Drop the current connection (if any) and connect again now, without backoff.
  void RequestReconnect();
  // Enroll now with a code (RUE-...) or key (rfk_...); `url` empty keeps the configured one.
  // insecure: `ru fleet enroll ... --insecure`; allows plain http/ws and is kept in credentials.json.
  void RequestEnroll(const std::string& url, const std::string& secret, bool insecure = false);

  // Queue an outbound message. Reliable ones get a seq and are spooled by the network thread.
  // `ref`: the id of the message this answers (cmd.result), empty = none.
  bool Send(const std::string& type, const std::string& payloadJson, int64_t epoch, bool reliable, std::string* err,
            const std::string& ref = {});
  // The lowest-priority lane (FLEET.md §12.2, demo.chunk): ephemeral, only while online, and sent
  // only when nothing else is waiting, one message per pass of the network loop, so events,
  // state.patch, acks and pings never wait behind more than the one bulk frame being written.
  // Dropped when the session ends. False (and *err) = not online, lane full (bulkMaxBytes) or
  // invalid: the caller tries again later.
  bool SendBulk(const std::string& type, const std::string& payloadJson, int64_t epoch, std::string* err);
  // Tests: hold every outbound message (both lanes) in its queue until released.
  void HoldOutboundForTest(bool hold);
  // state.snapshot {reason, state, availability, config_rev, admins_rev, ...extraJson} now (ephemeral,
  // envelope epoch = state.epoch). False when not online (a snapshot is never spooled).
  bool SendSnapshot(const std::string& reason, const std::string& extraJson);

  void SetHelloInfo(HelloInfo info);
  void UpdateHealth(int players, double tickMsP99);
  // Game thread, every frame (simulating or not): the watchdog's heartbeat.
  void NoteGameFrame();
  // Milliseconds since the last NoteGameFrame, or -1 before the first.
  int64_t FrameAgeMs() const;
  // A non-empty availability also clears the busy reason and detail.
  void SetState(const std::string& stateJson, const std::string& availability);
  // Availability with its busy reason and detail ("" = null).
  void SetAvailability(const std::string& availability, const std::string& reason, const std::string& detail);
  // Types some handler wants. Reliable messages of other types are answered with
  // error{unknown_type} and acked (§5).
  void SetHandledTypes(std::set<std::string> types, bool wildcard);
  // Called (from the network thread) whenever TakeInbound() has something.
  void SetWake(std::function<void()> wake);

  std::deque<Inbound> TakeInbound();
  // The game thread finished handling reliable message `seq`.
  void MarkProcessed(int64_t seq);

  ClientStatus Status() const;
  const std::string& installId() const { return installId_; }

 public:
  struct Session;  // one WebSocket connection; defined in fleet_client.cpp

 private:
  struct Out {
    std::string type, payload, ref;
    int64_t epoch = 0;
    bool reliable = false;
    int64_t afterSeq = 0;  // ephemeral: spool seq it was queued after (keeps the queue order)
  };
  enum class SessionEnd { Stopped, Reconnect, Closed, NetError, HelloFailed };
  struct SessionResult {
    SessionEnd end = SessionEnd::NetError;
    CloseAction action;
    int64_t durationMs = 0;
    bool established = false;
  };

  void Run();
  bool DoEnroll(const std::string& secret, const std::string& url, CloseAction* action, bool* fatal);
  SessionResult RunSession();
  void HandleInbound(Session& s, const std::string& text);
  void WaitFor(int64_t ms);  // interruptible sleep (stop / reconnect / enroll)
  void Wake();
  void DrainWakePipe();
  void SetLinkState(LinkState s, const std::string& err = {});
  // Moves queued messages into the spool (reliable) or `ephemeral` (NULL = drop them).
  void DrainOutbox(std::vector<Out>* ephemeral);
  void PushLocal(const std::string& type, const std::string& payloadJson);
  void PublishSpoolStatus();
  std::string BuildHello();
  // *epoch (optional) = the published state's epoch (0 = none).
  std::string BuildSnapshot(const char* reason, const std::string& extraJson = {}, int64_t* epoch = nullptr);
  std::string WsUrl() const;
  // Plain http/ws allowed: fleet.cfg insecure, or an explicit --insecure enrollment.
  bool Insecure() const { return cfg_.insecureDev || insecure_; }
  void Log(int level, const std::string& msg) const;

  ClientConfig cfg_;
  std::string installId_;
  std::string credsPath_;

  // Guarded by mu_.
  mutable std::mutex mu_;
  ClientStatus status_;
  Credentials creds_;
  HelloInfo hello_;
  std::deque<Out> outbox_;
  std::deque<Out> bulk_;  // SendBulk
  size_t bulkBytes_ = 0;
  std::deque<Inbound> inbox_;
  std::vector<int64_t> processed_;  // seqs the game thread finished
  std::set<std::string> handled_;
  bool handledAll_ = false;
  std::function<void()> wake_;
  std::string pendingEnrollSecret_, pendingEnrollUrl_;
  bool enrollRequested_ = false;

  std::atomic<bool> stop_{false};
  std::atomic<bool> insecure_{false};
  std::atomic<bool> reconnect_{false};
  std::atomic<bool> holdOut_{false};
  std::atomic<int64_t> lastFrameMono_{0};
  std::atomic<int> players_{0};
  // True while the watchdog holds the link down (see ClientConfig::stallMs).
  bool GameStalled(int64_t nowMono) const;
  int wakeFd_[2] = {-1, -1};
  std::thread thread_;

  // Network thread only.
  Spool spool_;
  RxTracker rx_;
  Backoff backoff_;
  bool haveCreds_ = false;
  bool codeFailed_ = false;  // the configured one-time code was refused; wait for `ru fleet enroll`
  bool curlInit_ = false;
};

}  // namespace fleet
