#include "fleet_client.h"


#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#if !defined(CURLWS_TEXT)
#error "fleet.so needs libcurl >= 7.86 with WebSocket support (curl/websockets.h)"
#endif

namespace fleet {

const char* LinkStateName(LinkState s) {
  switch (s) {
    case LinkState::Standalone: return "standalone";
    case LinkState::Unenrolled: return "unenrolled";
    case LinkState::Enrolling: return "enrolling";
    case LinkState::Connecting: return "connecting";
    case LinkState::Online: return "online";
    case LinkState::Offline: return "offline";
    case LinkState::Rejected: return "rejected";
  }
  return "?";
}

// ---- curl helpers --------------------------------------------------------------------------

namespace {

json::Value NullOrStr(const std::string& s) { return s.empty() ? json::Value::Null() : json::Value::Str(s); }

#if defined(FLEET_CA_PROBE)
// Static release builds carry a compiled-in Debian CA path; use whichever bundle this host has.
const char* SystemCaBundle() {
  static const char* found = []() -> const char* {
    static const char* const kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"};
    for (const char* p : kCandidates) {
      if (access(p, R_OK) == 0) return p;
    }
    return nullptr;
  }();
  return found;
}
#endif

void ApplyTls(CURL* c, const ClientConfig& cfg) {
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  if (!cfg.caFile.empty()) {
    curl_easy_setopt(c, CURLOPT_CAINFO, cfg.caFile.c_str());
  } else {
#if defined(FLEET_CA_PROBE)
    if (const char* ca = SystemCaBundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
#endif
  }
  if (!cfg.pinSha256.empty()) {
    static thread_local std::string pin;
    pin = cfg.pinSha256.rfind("sha256//", 0) == 0 ? cfg.pinSha256 : "sha256//" + cfg.pinSha256;
    curl_easy_setopt(c, CURLOPT_PINNEDPUBLICKEY, pin.c_str());
  }
}

size_t CollectBody(char* ptr, size_t size, size_t n, void* user) {
  auto* s = static_cast<std::string*>(user);
  const size_t len = size * n;
  if (s->size() + len > (256u << 10)) return 0;  // abort absurd bodies
  s->append(ptr, len);
  return len;
}

int AbortOnStop(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
}

bool WaitSocket(curl_socket_t sock, short events, int timeoutMs) {
  pollfd p{};
  p.fd = sock;
  p.events = events;
  return poll(&p, 1, timeoutMs) > 0;
}

}  // namespace

// ---- Client --------------------------------------------------------------------------------

Client::Client(ClientConfig cfg) : cfg_(std::move(cfg)), backoff_(cfg_.backoff, cfg_.rngSeed) {
  credsPath_ = cfg_.dataDir + "/credentials.json";
}

Client::~Client() {
  Stop();
  for (int& fd : wakeFd_) {
    if (fd >= 0) close(fd);
    fd = -1;
  }
}

void Client::Log(int level, const std::string& msg) const {
  if (cfg_.log) cfg_.log(level, Redact(msg));
}

bool Client::Start(std::string* err) {
  if (thread_.joinable()) return true;
  std::string e;
  if (!MakeDirs(cfg_.dataDir, 0700, &e)) {
    if (err) *err = e;
    return false;
  }
  installId_ = LoadOrCreateInstallId(cfg_.dataDir, &e);
  if (installId_.empty()) {
    if (err) *err = "install_id: " + e;
    return false;
  }
  Credentials c;
  std::string cerr;
  haveCreds_ = LoadCredentials(credsPath_, &c, &cerr);
  if (!haveCreds_ && cerr != "missing") Log(1, "fleet: ignoring credentials.json: " + cerr);
  if (haveCreds_ && !cfg_.url.empty() && !c.url.empty() && JoinUrl(c.url, "", false) != JoinUrl(cfg_.url, "", false)) {
    Log(1, "fleet: credentials.json was enrolled against " + c.url + ", config says " + cfg_.url +
               "; enrolling again");
    haveCreds_ = false;
  }
  if (cfg_.url.empty() && haveCreds_) cfg_.url = c.url;
  std::string note;
  if (!spool_.Open(cfg_.dataDir + "/spool", cfg_.spool, &e, &note)) {
    if (err) *err = "spool: " + e;
    return false;
  }
  Log(3, "fleet: spool " + note);
  rx_.Reset(spool_.rxSeq());
  if (pipe2(wakeFd_, O_NONBLOCK | O_CLOEXEC) != 0) {
    if (err) *err = std::string("pipe: ") + std::strerror(errno);
    return false;
  }
  if (!curlInit_) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curlInit_ = true;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (haveCreds_) creds_ = c;
    if (haveCreds_ && c.insecure) insecure_ = true;
    status_.url = cfg_.url;
    status_.installId = installId_;
    status_.serverId = haveCreds_ ? c.serverId : "";
    status_.enrolled = haveCreds_;
    status_.state = haveCreds_ ? LinkState::Connecting : LinkState::Unenrolled;
    status_.sinceMs = NowMs();
    status_.offlineSinceMs = status_.sinceMs;
    status_.streamId = spool_.streamId();
  }
  PublishSpoolStatus();
  stop_ = false;
  thread_ = std::thread([this] { Run(); });
  return true;
}

void Client::Stop() {
  if (thread_.joinable()) {
    stop_ = true;
    Wake();
    thread_.join();
  }
  spool_.Close();
  if (curlInit_) {
    curl_global_cleanup();
    curlInit_ = false;
  }
}

void Client::Wake() {
  if (wakeFd_[1] >= 0) {
    const char b = 1;
    (void)!write(wakeFd_[1], &b, 1);
  }
}

void Client::DrainWakePipe() {
  char buf[64];
  while (wakeFd_[0] >= 0 && read(wakeFd_[0], buf, sizeof(buf)) > 0) {
  }
}

void Client::RequestReconnect() {
  reconnect_ = true;
  Wake();
}

void Client::RequestEnroll(const std::string& url, const std::string& secret, bool insecure) {
  if (insecure) insecure_ = true;
  {
    std::lock_guard<std::mutex> lk(mu_);
    pendingEnrollUrl_ = url;
    pendingEnrollSecret_ = secret;
    enrollRequested_ = true;
  }
  reconnect_ = true;
  Wake();
}

bool Client::Send(const std::string& type, const std::string& payloadJson, int64_t epoch, bool reliable,
                  std::string* err, const std::string& ref) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (!IsValidType(type) || IsEphemeralType(type) || type.rfind("local.", 0) == 0) {
    return fail("invalid or reserved message type \"" + type + "\"");
  }
  std::string payload = payloadJson.empty() ? std::string("{}") : payloadJson;
  if (payload.size() > kMaxFrameBytes - 1024) return fail("payload too large");
  json::Value v;
  std::string perr;
  if (!json::Parse(payload, &v, &perr) || !v.IsObj()) return fail("payload is not a JSON object: " + perr);
  // Raw newlines in valid JSON are whitespace outside strings; the spool is line-based.
  for (auto& ch : payload) {
    if (ch == '\n' || ch == '\r') ch = ' ';
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (outbox_.size() >= cfg_.outboxMax) return fail("outbox full");
    outbox_.push_back(Out{type, std::move(payload), ref, epoch, reliable, 0});
  }
  Wake();
  return true;
}

void Client::SetHelloInfo(HelloInfo info) {
  std::lock_guard<std::mutex> lk(mu_);
  const std::string state = hello_.stateJson, avail = hello_.availability;
  std::string reason = std::move(hello_.availabilityReason), detail = std::move(hello_.availabilityDetail);
  hello_ = std::move(info);
  if (hello_.stateJson.empty()) hello_.stateJson = state.empty() ? "null" : state;
  if (hello_.availability.empty()) hello_.availability = avail.empty() ? "available" : avail;
  hello_.availabilityReason = std::move(reason);
  hello_.availabilityDetail = std::move(detail);
}

void Client::UpdateHealth(int players, double tickMsP99) {
  players_.store(players, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(mu_);
  hello_.players = players;
  hello_.tickMsP99 = tickMsP99;
}

void Client::NoteGameFrame() { lastFrameMono_.store(MonotonicMs(), std::memory_order_relaxed); }

int64_t Client::FrameAgeMs() const {
  const int64_t last = lastFrameMono_.load(std::memory_order_relaxed);
  return last > 0 ? MonotonicMs() - last : -1;
}

bool Client::GameStalled(int64_t nowMono) const {
  if (cfg_.stallMs <= 0) return false;
  const int64_t last = lastFrameMono_.load(std::memory_order_relaxed);
  return last > 0 && nowMono - last > cfg_.stallMs && players_.load(std::memory_order_relaxed) > 0;
}

void Client::SetState(const std::string& stateJson, const std::string& availability) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!stateJson.empty()) hello_.stateJson = stateJson;
  if (!availability.empty()) {
    hello_.availability = availability;
    hello_.availabilityReason.clear();
    hello_.availabilityDetail.clear();
  }
}

void Client::SetAvailability(const std::string& availability, const std::string& reason, const std::string& detail) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!availability.empty()) hello_.availability = availability;
  hello_.availabilityReason = reason;
  hello_.availabilityDetail = detail;
}

void Client::SetHandledTypes(std::set<std::string> types, bool wildcard) {
  std::function<void()> wake;
  {
    std::lock_guard<std::mutex> lk(mu_);
    handled_ = std::move(types);
    handledAll_ = wildcard;
    wake = wake_;
  }
  if (wake) wake();  // the network thread delivers what was held for these types
}

void Client::HoldUnhandled(const std::string& typePrefix, int64_t windowMs) {
  std::lock_guard<std::mutex> lk(mu_);
  holdPrefix_ = typePrefix;
  holdUntilMono_ = MonotonicMs() + windowMs;
}

void Client::ReleaseHeld(const std::function<void(const Envelope&)>& answerUnknown) {
  if (held_.empty()) return;
  bool any = false;
  for (auto it = held_.begin(); it != held_.end();) {
    bool handled, expired;
    {
      std::lock_guard<std::mutex> lk(mu_);
      handled = handledAll_ || handled_.count(it->type) > 0;
      expired = MonotonicMs() >= holdUntilMono_;
    }
    if (handled) {
      Inbound in;
      in.reliable = true;
      in.payloadJson = json::Dump(it->payload);
      in.env = std::move(*it);
      {
        std::lock_guard<std::mutex> lk(mu_);
        inbox_.push_back(std::move(in));
      }
      any = true;
      it = held_.erase(it);
    } else if (expired && answerUnknown) {  // (the session loop only delivers; answers need a frame)
      answerUnknown(*it);
      it = held_.erase(it);
    } else {
      ++it;
    }
  }
  if (any) {
    std::function<void()> wake;
    {
      std::lock_guard<std::mutex> lk(mu_);
      wake = wake_;
    }
    if (wake) wake();
  }
}

void Client::SetWake(std::function<void()> wake) {
  std::lock_guard<std::mutex> lk(mu_);
  wake_ = std::move(wake);
}

std::deque<Inbound> Client::TakeInbound() {
  std::lock_guard<std::mutex> lk(mu_);
  std::deque<Inbound> out;
  out.swap(inbox_);
  return out;
}

void Client::MarkProcessed(int64_t seq) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    processed_.push_back(seq);
  }
  Wake();
}

ClientStatus Client::Status() const {
  std::lock_guard<std::mutex> lk(mu_);
  ClientStatus st = status_;
  st.bulkQueuedBytes = bulkBytes_;
  return st;
}

bool Client::SendBulk(const std::string& type, const std::string& payloadJson, int64_t epoch, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (!IsValidType(type) || IsEphemeralType(type) || type.rfind("local.", 0) == 0) return fail("invalid type");
  if (payloadJson.empty() || payloadJson.size() > kMaxFrameBytes - 1024) return fail("payload size");
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (status_.state != LinkState::Online) return fail("not online");
    if (!bulk_.empty() && bulkBytes_ + payloadJson.size() > cfg_.bulkMaxBytes) return fail("bulk lane full");
    bulk_.push_back(Out{type, payloadJson, {}, epoch, false, 0});
    bulkBytes_ += payloadJson.size();
  }
  Wake();
  return true;
}

void Client::HoldOutboundForTest(bool hold) {
  holdOut_ = hold;
  Wake();
}

void Client::PushLocal(const std::string& type, const std::string& payloadJson) {
  Inbound in;
  in.env.type = type;
  in.env.ts = NowMs();
  in.payloadJson = payloadJson;
  in.local = true;
  std::function<void()> wake;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!handledAll_ && !handled_.count(type)) return;
    inbox_.push_back(std::move(in));
    wake = wake_;
  }
  if (wake) wake();
}

void Client::SetLinkState(LinkState s, const std::string& err) {
  LinkState prev;
  int64_t now = NowMs();
  {
    std::lock_guard<std::mutex> lk(mu_);
    prev = status_.state;
    if (!err.empty()) status_.lastError = Redact(err);
    if (prev == s) return;
    status_.state = s;
    status_.sinceMs = now;
    if (s == LinkState::Online) {
      status_.offlineSinceMs = 0;
      status_.lastOnlineMs = now;
      status_.nextAttemptMs = 0;
    } else if (status_.offlineSinceMs == 0) {
      status_.offlineSinceMs = now;
    }
  }
  if (s == LinkState::Online || prev == LinkState::Online || s == LinkState::Rejected) {
    json::Value p = json::Value::Object();
    p.Set("state", json::Value::Str(LinkStateName(s)));
    p.Set("since", json::Value::Int(now));
    PushLocal("local.connection", json::Dump(p));
  }
}

void Client::PublishSpoolStatus() {
  std::lock_guard<std::mutex> lk(mu_);
  status_.spoolMsgs = static_cast<uint32_t>(spool_.count());
  status_.spoolBytes = spool_.bytes();
  status_.spoolDropped = spool_.dropped();
  status_.txSeq = spool_.lastSeq();
  status_.ackedSeq = spool_.ackedSeq();
  status_.rxSeq = rx_.processed();
  status_.streamId = spool_.streamId();
}

void Client::DrainOutbox(std::vector<Out>* ephemeral) {
  std::deque<Out> q;
  {
    std::lock_guard<std::mutex> lk(mu_);
    q.swap(outbox_);
  }
  if (q.empty()) return;
  for (auto& o : q) {
    if (!o.reliable) {
      o.afterSeq = spool_.lastSeq();  // goes out after the reliable messages queued before it
      if (ephemeral) ephemeral->push_back(std::move(o));
      continue;
    }
    std::string why;
    if (spool_.Append(o.type, NewUlid(NowMs()), o.ref, NowMs(), o.epoch, o.payload, &why) == 0) {
      Log(1, "fleet: " + why);
    }
  }
  PublishSpoolStatus();
}

void Client::WaitFor(int64_t ms) {
  const int64_t deadline = MonotonicMs() + std::max<int64_t>(0, ms);
  for (;;) {
    DrainOutbox(nullptr);  // offline: reliable messages go to disk right away
    if (stop_ || reconnect_) return;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (enrollRequested_) return;
    }
    const int64_t left = deadline - MonotonicMs();
    if (left <= 0) return;
    pollfd p{};
    p.fd = wakeFd_[0];
    p.events = POLLIN;
    poll(&p, 1, static_cast<int>(std::min<int64_t>(left, 1000)));
    DrainWakePipe();
    spool_.MaybeFlush(NowMs());
  }
}

std::string Client::WsUrl() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!creds_.wsUrl.empty()) return creds_.wsUrl;
  return JoinUrl(cfg_.url, "/api/fleet/ws", true);
}

// ---- enrollment ----------------------------------------------------------------------------

bool Client::DoEnroll(const std::string& secret, const std::string& urlOverride, CloseAction* act, bool* fatal) {
  *fatal = false;
  const std::string base = urlOverride.empty() ? cfg_.url : urlOverride;
  if (base.empty()) {
    act->what = "no platform url (set url in the [fleet] section or use `ru fleet enroll <url> <code>`)";
    *fatal = true;
    return false;
  }
  if (const std::string why = CheckUrlAllowed(base, Insecure()); !why.empty()) {
    act->what = why;
    *fatal = true;
    return false;
  }
  const bool isKey = secret.rfind("rfk_", 0) == 0;
  HelloInfo h;
  {
    std::lock_guard<std::mutex> lk(mu_);
    h = hello_;
  }
  json::Value body = json::Value::Object();
  body.Set(isKey ? "key" : "code", json::Value::Str(secret));
  body.Set("install_id", json::Value::Str(installId_));
  body.Set("tenant_id", json::Value::Str("default"));
  json::Value host = json::Value::Object();
  host.Set("hostname", json::Value::Str(h.hostname));
  host.Set("game_port", json::Value::Int(h.gamePort));
  if (!h.publicAddr.empty()) host.Set("public_addr", json::Value::Str(h.publicAddr));
  host.Set("steam_token", json::Value::Bool(h.steamToken));
  body.Set("host", std::move(host));
  json::Value versions = json::Value::Object();
  versions.Set("core", json::Value::Str(h.coreVersion));
  versions.Set("plugin_api", json::Value::Str(h.pluginApi));
  json::Value plugins = json::Value::Object();
  for (const auto& p : h.plugins) plugins.Set(p.first, json::Value::Str(p.second));
  versions.Set("plugins", std::move(plugins));
  versions.Set("cs2_build", json::Value::Int(h.cs2Build));
  versions.Set("cs2_patch", json::Value::Str(h.cs2Patch));
  body.Set("versions", std::move(versions));
  body.Set("cs2_build", json::Value::Int(h.cs2Build));
  const std::string bodyText = json::Dump(body);

  const std::string url = JoinUrl(base, "/api/fleet/enroll", false);
  CURL* c = curl_easy_init();
  if (!c) {
    act->what = "curl_easy_init failed";
    return false;
  }
  std::string resp;
  char errbuf[CURL_ERROR_SIZE] = {0};
  curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: application/json");
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, bodyText.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyText.size()));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_USERAGENT, cfg_.userAgent.c_str());
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(cfg_.connectTimeoutMs));
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(cfg_.httpTimeoutMs));
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &CollectBody);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &AbortOnStop);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &stop_);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  ApplyTls(c, cfg_);
  Log(0, std::string("fleet: enrolling at ") + url + " with a " + (isKey ? "fleet key" : "one-time code") +
             " (install_id " + installId_ + ")");
  const CURLcode rc = curl_easy_perform(c);
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_off_t retryAfter = 0;
  curl_easy_getinfo(c, CURLINFO_RETRY_AFTER, &retryAfter);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK) {
    act->what = std::string("enroll request failed: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
    return false;
  }
  json::Value r;
  const bool parsed = json::Parse(resp, &r) && r.IsObj();
  if (status != 200 && status != 201) {
    *act = ClassifyHttpStatus(status, static_cast<int64_t>(retryAfter) * 1000);
    std::string msg;
    if (parsed) {
      const json::Value* e = r.Get("error");
      if (e && e->IsStr()) msg = e->s;
      else if (e && e->IsObj() && e->Get("message")) msg = e->Get("message")->AsStr();
      else if (r.Get("message")) msg = r.Get("message")->AsStr();
    }
    act->what = "enrollment refused: " + act->what + (msg.empty() ? "" : ": " + msg.substr(0, 160));
    if (status == 400 || status == 401 || status == 403 || status == 404 || status == 409 || status == 410) {
      act->rejected = true;
      act->capMs = 10 * 60 * 1000;
      *fatal = !isKey;  // a refused one-time code never becomes valid; a key may (limits, clock)
    }
    return false;
  }
  if (!parsed) {
    act->what = "enrollment response is not JSON";
    return false;
  }
  Credentials cr;
  cr.serverId = r.Get("server_id") ? r.Get("server_id")->AsStr() : "";
  cr.token = r.Get("token") ? r.Get("token")->AsStr() : "";
  cr.wsUrl = r.Get("ws_url") ? r.Get("ws_url")->AsStr() : "";
  cr.url = base;
  cr.installId = installId_;
  cr.enrolledAt = NowMs();
  cr.insecure = insecure_;
  if (!cr.Valid()) {
    act->what = "enrollment response lacks server_id/token";
    return false;
  }
  if (const std::string up = UpgradeSameHostWs(base, cr.wsUrl); !up.empty()) {
    Log(2, "fleet: the platform answered ws_url " + cr.wsUrl + "; using " + up + ", since it was reached over https");
    cr.wsUrl = up;
  }
  if (!cr.wsUrl.empty() && !CheckUrlAllowed(cr.wsUrl, Insecure()).empty()) {
    Log(1, "fleet: ignoring ws_url " + cr.wsUrl + " from the platform (" + CheckUrlAllowed(cr.wsUrl, Insecure()) +
               ")");
    cr.wsUrl.clear();
  }
  std::string e;
  if (!SaveCredentials(credsPath_, cr, &e)) {
    act->what = "cannot write credentials.json: " + e;
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    creds_ = cr;
    status_.serverId = cr.serverId;
    status_.enrolled = true;
    status_.url = base;
  }
  cfg_.url = base;
  haveCreds_ = true;
  codeFailed_ = false;
  Log(0, "fleet: enrolled as server " + cr.serverId + (isKey ? "" : " (the one-time code is used up; remove it from the config)"));
  return true;
}

// ---- the WebSocket session -----------------------------------------------------------------

struct Client::Session {
  CURL* c = nullptr;
  curl_socket_t sock = CURL_SOCKET_BAD;
  std::string partial;
  int64_t lastRxMono = 0;
  int64_t intervalMs = 10000, timeoutMs = 30000;
  int64_t lastSentSeq = 0;
  int64_t persistedRx = -1;
  bool ackNow = false;
  bool broken = false;
  std::string brokenWhy;
  // A close frame from the peer.
  bool closed = false;
  int closeCode = 1005;
  std::string closeReason;
};

namespace {

bool SendFrame(CURL* c, curl_socket_t sock, const char* data, size_t len, unsigned flags, std::string* why) {
  size_t off = 0;
  const int64_t deadline = MonotonicMs() + 10000;
  do {
    size_t sent = 0;
    const CURLcode rc = curl_ws_send(c, data + off, len - off, &sent, 0, flags);
    off += sent;
    if (rc == CURLE_AGAIN) {
      if (MonotonicMs() > deadline) {
        *why = "send timed out";
        return false;
      }
      WaitSocket(sock, POLLOUT, 200);
      continue;
    }
    if (rc != CURLE_OK) {
      *why = std::string("send failed: ") + curl_easy_strerror(rc);
      return false;
    }
  } while (off < len);
  return true;
}

void SendClose(CURL* c, curl_socket_t sock, int code, const std::string& reason) {
  std::string p;
  p.push_back(static_cast<char>((code >> 8) & 0xFF));
  p.push_back(static_cast<char>(code & 0xFF));
  p += reason.substr(0, 120);
  std::string why;
  SendFrame(c, sock, p.data(), p.size(), CURLWS_CLOSE, &why);
}

enum class Rx { None, Message, Closed, Error };

// Reads what is available. Returns Message with a complete text message in *msg.
Rx ReadOne(Client::Session& s, std::string* msg, std::string* why) {
  char buf[65536];
  for (;;) {
    size_t n = 0;
    const struct curl_ws_frame* meta = nullptr;
    const CURLcode rc = curl_ws_recv(s.c, buf, sizeof(buf), &n, &meta);
    if (rc == CURLE_AGAIN) return Rx::None;
    if (rc != CURLE_OK) {
      *why = rc == CURLE_GOT_NOTHING ? "connection closed by peer" : std::string("recv failed: ") + curl_easy_strerror(rc);
      return Rx::Error;
    }
    s.lastRxMono = MonotonicMs();
    if (!meta) continue;
    if (meta->flags & CURLWS_CLOSE) {
      s.partial.append(buf, n);
      if (meta->bytesleft > 0) continue;
      s.closed = true;
      if (s.partial.size() >= 2) {
        s.closeCode = (static_cast<unsigned char>(s.partial[0]) << 8) | static_cast<unsigned char>(s.partial[1]);
        s.closeReason = s.partial.substr(2);
      }
      s.partial.clear();
      return Rx::Closed;
    }
    if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;  // curl answers pings itself
    s.partial.append(buf, n);
    if (s.partial.size() > kMaxFrameBytes) {
      *why = "message larger than 1 MiB";
      return Rx::Error;
    }
    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
      msg->swap(s.partial);
      s.partial.clear();
      return Rx::Message;
    }
  }
}
}  // namespace

std::string Client::BuildHello() {
  HelloInfo h;
  Credentials cr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    h = hello_;
    cr = creds_;
  }
  json::Value p = json::Value::Object();
  p.Set("server_id", json::Value::Str(cr.serverId));
  p.Set("install_id", json::Value::Str(installId_));
  p.Set("tenant_id", json::Value::Str("default"));
  json::Value proto = json::Value::Object();
  proto.Set("min", json::Value::Int(kProtocolVersion));
  proto.Set("max", json::Value::Int(kProtocolVersion));
  p.Set("protocol", std::move(proto));
  json::Value versions = json::Value::Object();
  versions.Set("core", json::Value::Str(h.coreVersion));
  versions.Set("plugin_api", json::Value::Str(h.pluginApi));
  json::Value plugins = json::Value::Object();
  for (const auto& pl : h.plugins) plugins.Set(pl.first, json::Value::Str(pl.second));
  versions.Set("plugins", std::move(plugins));
  versions.Set("cs2_build", json::Value::Int(h.cs2Build));
  versions.Set("cs2_patch", json::Value::Str(h.cs2Patch));
  p.Set("versions", std::move(versions));
  json::Value caps = json::Value::Array();
  for (const auto& cap : h.capabilities) caps.Push(json::Value::Str(cap));
  p.Set("capabilities", std::move(caps));
  json::Value host = json::Value::Object();
  host.Set("hostname", json::Value::Str(h.hostname));
  host.Set("game_port", json::Value::Int(h.gamePort));
  if (h.tvPort > 0) host.Set("tv_port", json::Value::Int(h.tvPort));
  if (!h.publicAddr.empty()) host.Set("public_addr", json::Value::Str(h.publicAddr));
  host.Set("steam_token", json::Value::Bool(h.steamToken));
  p.Set("host", std::move(host));
  p.Set("boot_id", json::Value::Str(h.bootId));
  json::Value stream = json::Value::Object();
  stream.Set("id", json::Value::Str(spool_.streamId()));
  stream.Set("last_tx_seq", json::Value::Int(spool_.lastSeq()));
  stream.Set("last_rx_seq", json::Value::Int(rx_.processed()));
  p.Set("stream", std::move(stream));
  json::Value state;
  if (!json::Parse(h.stateJson, &state)) state = json::Value::Null();
  p.Set("state", std::move(state));
  p.Set("availability", json::Value::Str(h.availability));
  p.Set("reason", NullOrStr(h.availabilityReason));
  p.Set("detail", NullOrStr(h.availabilityDetail));
  // The admins.set rev this server has cached: the platform sends admins.set only when it differs.
  if (h.adminsRev >= 0) p.Set("admins_rev", json::Value::Int(h.adminsRev));
  if (!h.pluginsDisabled.empty()) {
    json::Value off = json::Value::Array();
    for (const auto& d : h.pluginsDisabled) {
      json::Value e = json::Value::Object();
      e.Set("name", json::Value::Str(d.first));
      e.Set("reason", json::Value::Str(d.second));
      off.Push(std::move(e));
    }
    p.Set("plugins_disabled", std::move(off));
  }
  // What is installed and what plugins.json keeps off: the platform compares it with the plugin
  // set it chose for this server (cmd plugins.set) and warns about a plugin that is not there.
  PluginsState ps;
  if (!h.pluginsDir.empty() && ReadPluginsState(h.pluginsDir, &ps)) {
    json::Value st = json::Value::Object();
    json::Value inst = json::Value::Array();
    for (const auto& n : ps.installed) inst.Push(json::Value::Str(n));
    json::Value dis = json::Value::Array();
    for (const auto& n : ps.disabled) dis.Push(json::Value::Str(n));
    st.Set("installed", std::move(inst));
    st.Set("disabled", std::move(dis));
    p.Set("plugins_state", std::move(st));
  }
  // The core's latest selftest (ru_api 1.11 selftest_summary, fleet_proto.h SelftestPayload);
  // left out before the first selftest ran (it is optional).
  if (!h.selftestJson.empty()) {
    json::Value st;
    if (json::Parse(h.selftestJson, &st) && st.IsObj()) p.Set("selftest", std::move(st));
  }
  return json::Dump(p);
}

std::string Client::BuildSnapshot(const char* reason, const std::string& extraJson, int64_t* epoch) {
  HelloInfo h;
  {
    std::lock_guard<std::mutex> lk(mu_);
    h = hello_;
  }
  json::Value p = json::Value::Object();
  p.Set("reason", json::Value::Str(reason));
  json::Value state;
  if (!json::Parse(h.stateJson, &state)) state = json::Value::Null();
  // config_rev and the envelope epoch come from the MatchState the match plugin published.
  int64_t configRev = 0;
  if (state.IsObj()) {
    if (const json::Value* v = state.Get("config_rev")) configRev = v->AsInt(0);
    if (const json::Value* v = state.Get("epoch"); v && epoch) *epoch = v->AsInt(0);
  }
  p.Set("state", std::move(state));
  p.Set("availability", json::Value::Str(h.availability));
  p.Set("availability_reason", NullOrStr(h.availabilityReason));
  p.Set("availability_detail", NullOrStr(h.availabilityDetail));
  p.Set("config_rev", json::Value::Int(configRev));
  p.Set("admins_rev", json::Value::Int(h.adminsRev > 0 ? h.adminsRev : 0));
  // Extra members from the match plugin (map_stats), never overriding the ones above.
  json::Value extra;
  if (!extraJson.empty() && json::Parse(extraJson, &extra) && extra.IsObj()) {
    for (const auto& kv : extra.o) {
      if (!p.Get(kv.first)) p.Set(kv.first, kv.second);
    }
  }
  return json::Dump(p);
}

bool Client::SendSnapshot(const std::string& reason, const std::string& extraJson) {
  int64_t epoch = 0;
  std::string payload = BuildSnapshot(reason.c_str(), extraJson, &epoch);
  if (payload.size() > kMaxFrameBytes - 1024) {
    payload = BuildSnapshot(reason.c_str(), {}, &epoch);  // drop map_stats rather than the snapshot
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (status_.state != LinkState::Online) return false;  // ephemeral: nothing to replay later
    if (outbox_.size() >= cfg_.outboxMax) return false;
    outbox_.push_back(Out{"state.snapshot", std::move(payload), {}, epoch > 0 ? epoch : 0, false, 0});
  }
  Wake();
  return true;
}

Client::SessionResult Client::RunSession() {
  SessionResult res;
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++status_.connectAttempts;
  }
  const std::string url = WsUrl();
  if (const std::string why = CheckUrlAllowed(url, Insecure()); !why.empty()) {
    res.end = SessionEnd::NetError;
    res.action.what = why;
    res.action.capMs = 10 * 60 * 1000;
    return res;
  }
  std::string auth;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auth = "Authorization: Bearer " + creds_.token;
  }
  Session s;
  s.c = curl_easy_init();
  if (!s.c) {
    res.action.what = "curl_easy_init failed";
    return res;
  }
  char errbuf[CURL_ERROR_SIZE] = {0};
  curl_slist* hdrs = curl_slist_append(nullptr, auth.c_str());
  auth.assign(auth.size(), '\0');
  curl_easy_setopt(s.c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(s.c, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(s.c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(s.c, CURLOPT_USERAGENT, cfg_.userAgent.c_str());
  curl_easy_setopt(s.c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(s.c, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(cfg_.connectTimeoutMs));
  curl_easy_setopt(s.c, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(s.c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(s.c, CURLOPT_XFERINFOFUNCTION, &AbortOnStop);
  curl_easy_setopt(s.c, CURLOPT_XFERINFODATA, &stop_);
  ApplyTls(s.c, cfg_);
  auto cleanup = [&] {
    if (s.c) curl_easy_cleanup(s.c);
    s.c = nullptr;
    if (hdrs) curl_slist_free_all(hdrs);
    hdrs = nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    bulk_.clear();  // ephemeral: the sender resumes from the platform's ack (demo.ack)
    bulkBytes_ = 0;
  };

  const CURLcode rc = curl_easy_perform(s.c);
  long status = 0;
  curl_easy_getinfo(s.c, CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK || (status != 0 && status != 101)) {
    if (status >= 300 && status != 101) {
      curl_off_t retryAfter = 0;
      curl_easy_getinfo(s.c, CURLINFO_RETRY_AFTER, &retryAfter);
      res.action = ClassifyHttpStatus(status, static_cast<int64_t>(retryAfter) * 1000);
      res.action.what = "upgrade refused: " + res.action.what;
    } else if (rc == CURLE_OK) {
      res.action.what = "the platform did not upgrade to a WebSocket (HTTP " + std::to_string(status) + ")";
    } else {
      res.action.what = std::string("connect to ") + url + " failed: " + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
    }
    cleanup();
    res.end = stop_ ? SessionEnd::Stopped : SessionEnd::NetError;
    return res;
  }
  curl_easy_getinfo(s.c, CURLINFO_ACTIVESOCKET, &s.sock);
  s.lastRxMono = MonotonicMs();

  // Everything the session sends goes through here: stamps ack (persisted first), counts frames.
  auto sendEnv = [&](Envelope e, const std::string& payload) -> bool {
    if (e.id.empty()) e.id = NewUlid(NowMs());
    if (e.ts == 0) e.ts = NowMs();
    const int64_t ack = rx_.processed();
    if (ack != s.persistedRx) {
      spool_.SetRxSeq(ack);
      spool_.FlushMeta();  // never ack what a restart would not remember
      s.persistedRx = ack;
    }
    e.ack = ack;
    const std::string text = EncodeRaw(e, payload);
    std::string why;
    if (!SendFrame(s.c, s.sock, text.data(), text.size(), CURLWS_TEXT, &why)) {
      s.broken = true;
      s.brokenWhy = why;
      return false;
    }
    rx_.OnAckSent(ack, NowMs());
    std::lock_guard<std::mutex> lk(mu_);
    ++status_.framesOut;
    return true;
  };
  auto sendRecord = [&](const SpoolRecord& r) {
    Envelope e;
    e.type = r.type;
    e.id = r.id;
    e.seq = r.seq;
    e.ts = r.ts;
    e.ref = r.ref;
    e.epoch = r.epoch;
    if (sendEnv(e, r.payload)) s.lastSentSeq = r.seq;
  };
  auto sendEphemeral = [&](const std::string& type, const std::string& payload, const std::string& ref = {},
                           int64_t epoch = 0) {
    Envelope e;
    e.type = type;
    e.ref = ref;
    e.epoch = epoch;
    return sendEnv(e, payload);
  };
  auto finish = [&](SessionEnd end) {
    res.end = end;
    cleanup();
    return res;
  };

  // 1. hello -> welcome
  const bool rotated = spool_.RotateIfGap();
  if (rotated) Log(1, "fleet: spool had a gap; started a new stream " + spool_.streamId() + " (resume will reset)");
  if (!sendEphemeral("hello", BuildHello())) {
    res.action.what = s.brokenWhy;
    return finish(SessionEnd::NetError);
  }
  const int64_t helloDeadline = MonotonicMs() + cfg_.helloTimeoutMs;
  Envelope welcome;
  bool gotWelcome = false;
  while (!gotWelcome) {
    if (stop_) {
      SendClose(s.c, s.sock, 1000, "server stopping");
      return finish(SessionEnd::Stopped);
    }
    const int64_t left = helloDeadline - MonotonicMs();
    if (left <= 0) {
      SendClose(s.c, s.sock, 1002, "no welcome");
      res.action.what = "no welcome within " + std::to_string(cfg_.helloTimeoutMs / 1000) + " s";
      return finish(SessionEnd::HelloFailed);
    }
    WaitSocket(s.sock, POLLIN, static_cast<int>(std::min<int64_t>(left, 200)));
    std::string msg, why;
    for (;;) {
      const Rx r = ReadOne(s, &msg, &why);
      if (r == Rx::None) break;
      if (r == Rx::Error) {
        res.action.what = why;
        return finish(SessionEnd::NetError);
      }
      if (r == Rx::Closed) {
        res.action = ClassifyClose(s.closeCode, s.closeReason);
        return finish(SessionEnd::Closed);
      }
      Envelope e;
      std::string derr;
      if (!Decode(msg, &e, &derr)) {
        Log(1, "fleet: bad frame before welcome: " + derr);
        continue;
      }
      if (e.type == "welcome") {
        welcome = std::move(e);
        gotWelcome = true;
        break;
      }
      if (e.type == "ping") {
        json::Value pong = json::Value::Object();
        if (const json::Value* t = e.payload.Get("t")) pong.Set("t", *t);
        sendEphemeral("pong", json::Dump(pong), e.id);
      }
    }
  }

  // 2. welcome: resume
  const json::Value& wp = welcome.payload;
  const json::Value* hb = wp.Get("heartbeat");
  if (hb && hb->IsObj()) {
    s.intervalMs = std::clamp<int64_t>(hb->Get("interval_ms") ? hb->Get("interval_ms")->AsInt(10000) : 10000, 1000, 120000);
    s.timeoutMs = std::clamp<int64_t>(hb->Get("timeout_ms") ? hb->Get("timeout_ms")->AsInt(30000) : 30000, 3000, 600000);
  }
  std::string resume = "resumed";
  int64_t platformRx = 0;
  if (const json::Value* r = wp.Get("resume"); r && r->IsObj()) {
    resume = r->Get("result") ? r->Get("result")->AsStr("resumed") : "resumed";
    platformRx = r->Get("platform_last_rx_seq") ? r->Get("platform_last_rx_seq")->AsInt(0) : 0;
  }
  if (welcome.ack > 0) platformRx = std::max(platformRx, welcome.ack);
  spool_.AckUpTo(platformRx);
  const bool reset = resume == "reset" || rotated;
  const int64_t established = MonotonicMs();
  res.established = true;
  {
    std::lock_guard<std::mutex> lk(mu_);
    status_.sessionId = wp.Get("session_id") ? wp.Get("session_id")->AsStr() : "";
    status_.resume = resume;
    status_.heartbeatMs = s.intervalMs;
    ++status_.sessions;
  }
  SetLinkState(LinkState::Online);  // the backoff resets only once a session lasted 60 s (6.3)
  PublishSpoolStatus();
  Log(0, "fleet: online (session " + (wp.Get("session_id") ? wp.Get("session_id")->AsStr("?") : std::string("?")) + ", resume " + resume + ", platform had " +
             std::to_string(platformRx) + ", replaying " + std::to_string(spool_.count()) + ")");

  // 3. replay, then the snapshot on a reset (§6.4)
  s.lastSentSeq = spool_.ackedSeq();
  for (const auto& r : spool_.records()) {
    if (r.seq <= s.lastSentSeq) continue;
    sendRecord(r);
    if (s.broken) break;
  }
  if (!s.broken && reset) {
    int64_t epoch = 0;
    const std::string snap = BuildSnapshot("reset", {}, &epoch);
    sendEphemeral("state.snapshot", snap, {}, epoch > 0 ? epoch : 0);
  }

  // 4. steady state
  int64_t nextPing = MonotonicMs() + s.intervalMs;
  std::vector<Out> eph;
  while (!s.broken) {
    if (stop_) {
      SendClose(s.c, s.sock, 1000, "server stopping");
      res.durationMs = MonotonicMs() - established;
      return finish(SessionEnd::Stopped);
    }
    if (reconnect_) {
      SendClose(s.c, s.sock, 1000, "reconnect");
      res.durationMs = MonotonicMs() - established;
      return finish(SessionEnd::Reconnect);
    }
    // Outbound: new reliable messages (already spooled) and ephemeral ones, in the order the
    // plugins queued them (a state.snapshot queued before an event goes out before it).
    auto sendEph = [&](const Out& o) {
      Envelope e;
      e.type = o.type;
      e.epoch = o.epoch;
      e.ref = o.ref;
      return sendEnv(e, o.payload);
    };
    bool bulkWaiting = false;
    if (!holdOut_) {
      eph.clear();
      DrainOutbox(&eph);
      size_t ei = 0;
      for (const auto& r : spool_.records()) {
        if (r.seq <= s.lastSentSeq) continue;
        while (ei < eph.size() && eph[ei].afterSeq < r.seq && !s.broken) sendEph(eph[ei++]);
        if (s.broken) break;
        sendRecord(r);
        if (s.broken) break;
      }
      while (ei < eph.size() && !s.broken) sendEph(eph[ei++]);
      // The bulk lane (SendBulk): one message, and only when the normal lane is empty again.
      Out b;
      bool haveBulk = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!bulk_.empty() && outbox_.empty()) {
          b = std::move(bulk_.front());
          bulk_.pop_front();
          bulkBytes_ -= std::min(bulkBytes_, b.payload.size());
          haveBulk = true;
        }
        bulkWaiting = !bulk_.empty();
      }
      if (haveBulk && !s.broken && sendEph(b)) {
        std::lock_guard<std::mutex> lk(mu_);
        ++status_.bulkSent;
      }
    }
    // What the game thread finished handling.
    std::vector<int64_t> done;
    {
      std::lock_guard<std::mutex> lk(mu_);
      done.swap(processed_);
    }
    const int64_t nowWall = NowMs();
    for (int64_t seq : done) rx_.MarkDone(seq, nowWall);
    if (!s.broken && (s.ackNow || rx_.AckDue(nowWall))) {
      s.ackNow = false;
      sendEphemeral("ack", "{}");
    }
    const int64_t now = MonotonicMs();
    if (!s.broken && now >= nextPing) {
      HelloInfo h;
      {
        std::lock_guard<std::mutex> lk(mu_);
        h = hello_;
      }
      json::Value p = json::Value::Object();
      p.Set("t", json::Value::Int(NowMs()));
      json::Value health = json::Value::Object();
      health.Set("players", json::Value::Int(h.players));
      health.Set("tick_ms_p99", json::Value::Num(std::round(h.tickMsP99 * 100.0) / 100.0));
      health.Set("spool_msgs", json::Value::Int(static_cast<int64_t>(spool_.count())));
      health.Set("uptime_s", json::Value::Int(h.startedMs > 0 ? (NowMs() - h.startedMs) / 1000 : 0));
      health.Set("frame_age_ms", json::Value::Int(FrameAgeMs()));
      p.Set("health", std::move(health));
      sendEphemeral("ping", json::Dump(p));
      nextPing = now + s.intervalMs;
    }
    if (GameStalled(now)) {
      // The network thread is fine but the game thread is stuck: pinging on would keep a dead
      // server "online" and its match would never fail over.
      const int64_t age = FrameAgeMs();
      SendClose(s.c, s.sock, 1011, "game thread stalled");
      Log(2, "fleet: no game frame for " + std::to_string(age / 1000) +
                 " s with players on the server: closing the link so the platform fails the match over");
      res.action.what = "game thread stalled (no frame for " + std::to_string(age / 1000) + " s)";
      res.action.fixedDelayMs = 1000;
      res.durationMs = now - established;
      return finish(SessionEnd::Closed);
    }
    if (now - s.lastRxMono > s.timeoutMs) {
      SendClose(s.c, s.sock, 1001, "heartbeat timeout");
      res.action.what = "no frame from the platform for " + std::to_string(s.timeoutMs / 1000) + " s";
      res.durationMs = now - established;
      return finish(SessionEnd::Closed);
    }
    spool_.MaybeFlush(nowWall);
    ReleaseHeld(nullptr);  // a handler registered since: deliver what was held for it
    PublishSpoolStatus();
    if (s.broken) break;

    // Wait for the socket, a wake-up, the next ping or a due ack (no wait while bulk is queued:
    // the next pass sends the normal lane first, then the next bulk message).
    int64_t waitMs = bulkWaiting ? 0 : std::min<int64_t>(250, nextPing - now);
    const int64_t ackDue = rx_.NextAckDueMs();
    if (ackDue >= 0) waitMs = std::min<int64_t>(waitMs, ackDue - nowWall);
    pollfd pf[2] = {};
    pf[0].fd = s.sock;
    pf[0].events = POLLIN;
    pf[1].fd = wakeFd_[0];
    pf[1].events = POLLIN;
    poll(pf, 2, static_cast<int>(std::max<int64_t>(0, waitMs)));
    DrainWakePipe();

    // Inbound.
    std::string msg, why;
    for (;;) {
      const Rx r = ReadOne(s, &msg, &why);
      if (r == Rx::None) break;
      if (r == Rx::Error) {
        s.broken = true;
        s.brokenWhy = why;
        break;
      }
      if (r == Rx::Closed) {
        SendClose(s.c, s.sock, s.closeCode == 1005 ? 1000 : s.closeCode, "");
        res.action = ClassifyClose(s.closeCode, s.closeReason);
        res.durationMs = MonotonicMs() - established;
        return finish(SessionEnd::Closed);
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        ++status_.framesIn;
      }
      HandleInbound(s, msg);
      if (s.broken) break;
    }
  }
  res.action.what = s.brokenWhy.empty() ? "connection lost" : s.brokenWhy;
  res.durationMs = MonotonicMs() - established;
  return finish(SessionEnd::NetError);
}

void Client::HandleInbound(Session& s, const std::string& text) {
  Envelope e;
  std::string derr;
  if (!Decode(text, &e, &derr)) {
    Log(1, "fleet: dropped a bad frame from the platform: " + derr);
    return;
  }
  if (e.v != kProtocolVersion) {
    Log(1, "fleet: dropped a frame with protocol v" + std::to_string(e.v));
    return;
  }
  if (e.ack > 0) {
    spool_.AckUpTo(e.ack);
  }
  auto sendEph = [&](const std::string& type, const json::Value& payload, const std::string& ref,
                     int64_t epoch = 0) {
    Envelope o;
    o.type = type;
    o.ref = ref;
    o.epoch = epoch;
    o.id = NewUlid(NowMs());
    o.ts = NowMs();
    const int64_t ack = rx_.processed();
    if (ack != s.persistedRx) {
      spool_.SetRxSeq(ack);
      spool_.FlushMeta();
      s.persistedRx = ack;
    }
    o.ack = ack;
    const std::string t = EncodeRaw(o, json::Dump(payload));
    std::string why;
    if (!SendFrame(s.c, s.sock, t.data(), t.size(), CURLWS_TEXT, &why)) {
      s.broken = true;
      s.brokenWhy = why;
      return;
    }
    rx_.OnAckSent(ack, NowMs());
  };
  auto answerUnknown = [&](const Envelope& m) {
    json::Value err = json::Value::Object();
    err.Set("code", json::Value::Str("unknown_type"));
    err.Set("message", json::Value::Str("no handler for " + m.type + " on this server"));
    err.Set("type", json::Value::Str(m.type));
    rx_.MarkDone(m.seq, NowMs());
    sendEph("error", err, m.id);
  };
  ReleaseHeld(answerUnknown);

  if (e.type == "ping") {
    json::Value pong = json::Value::Object();
    if (const json::Value* t = e.payload.Get("t")) pong.Set("t", *t);
    sendEph("pong", pong, e.id);
    return;
  }
  if (e.type == "pong") {
    if (const json::Value* t = e.payload.Get("t"); t && t->IsNum()) {
      std::lock_guard<std::mutex> lk(mu_);
      status_.rttMs = std::max<int64_t>(0, NowMs() - t->AsInt(0));
    }
    return;
  }
  if (e.type == "ack" || e.type == "welcome") return;
  if (e.type == "error") {
    const json::Value* code = e.payload.Get("code");
    const json::Value* msg = e.payload.Get("message");
    Log(1, "fleet: platform error " + (code ? code->AsStr("?") : std::string("?")) +
               (msg ? ": " + msg->AsStr() : std::string()) + (e.ref.empty() ? "" : " (ref " + e.ref + ")"));
    return;
  }

  bool handled;
  std::function<void()> wake;
  {
    std::lock_guard<std::mutex> lk(mu_);
    handled = handledAll_ || handled_.count(e.type) > 0;
    wake = wake_;
  }

  if (e.seq == 0) {  // ephemeral
    if (!rx_.OnEphemeralId(e.id)) return;
    if (e.type == "state.request") {
      json::Value snap;
      int64_t epoch = 0;
      json::Parse(BuildSnapshot("request", {}, &epoch), &snap);
      sendEph("state.snapshot", snap, e.id, epoch > 0 ? epoch : 0);
      return;
    }
    if (!handled) return;  // unknown ephemeral: ignore (§5)
    Inbound in;
    in.payloadJson = json::Dump(e.payload);
    in.env = std::move(e);
    {
      std::lock_guard<std::mutex> lk(mu_);
      inbox_.push_back(std::move(in));
    }
    if (wake) wake();
    return;
  }

  switch (rx_.OnReliable(e.seq, e.id)) {
    case RxTracker::Verdict::Duplicate:
      s.ackNow = true;  // the platform may have missed our ack
      return;
    case RxTracker::Verdict::OutOfOrder:
      Log(3, "fleet: dropped out-of-order seq " + std::to_string(e.seq) + " (have " + std::to_string(rx_.received()) +
                 "); the platform replays it");
      return;
    case RxTracker::Verdict::Accept:
      break;
  }
  const int64_t now = NowMs();
  if (e.type == "auth.rotate") {
    const std::string token = e.payload.Get("token") ? e.payload.Get("token")->AsStr() : "";
    if (token.empty()) {
      Log(1, "fleet: auth.rotate without a token ignored");
    } else {
      Credentials cr;
      {
        std::lock_guard<std::mutex> lk(mu_);
        creds_.token = token;
        cr = creds_;
      }
      std::string err;
      if (SaveCredentials(credsPath_, cr, &err)) {
        Log(0, "fleet: token rotated");
        std::string why;
        spool_.Append("auth.rotated", NewUlid(now), e.id, now, 0, "{}", &why);
      } else {
        Log(2, "fleet: cannot store the rotated token: " + err);
      }
    }
    rx_.MarkDone(e.seq, now);
    return;
  }
  if (!handled) {
    bool hold;
    {
      std::lock_guard<std::mutex> lk(mu_);
      hold = !holdPrefix_.empty() && e.type.compare(0, holdPrefix_.size(), holdPrefix_) == 0 &&
             MonotonicMs() < holdUntilMono_ && held_.size() < 64;
    }
    if (hold) {
      Log(3, "fleet: holding " + e.type + " seq " + std::to_string(e.seq) + " until its handler is there");
      held_.push_back(std::move(e));
      return;
    }
    answerUnknown(e);
    return;
  }
  Inbound in;
  in.reliable = true;
  in.payloadJson = json::Dump(e.payload);
  in.env = std::move(e);
  {
    std::lock_guard<std::mutex> lk(mu_);
    inbox_.push_back(std::move(in));
  }
  if (wake) wake();
}

// ---- the network thread --------------------------------------------------------------------

void Client::Run() {
  while (!stop_) {
    DrainOutbox(nullptr);
    bool enrollNow = false;
    std::string secret, url;
    bool manual = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (enrollRequested_) {
        enrollNow = manual = true;
        secret = pendingEnrollSecret_;
        url = pendingEnrollUrl_;
        pendingEnrollSecret_.assign(pendingEnrollSecret_.size(), '\0');
        pendingEnrollSecret_.clear();
        enrollRequested_ = false;
      }
    }
    reconnect_ = false;
    if (!enrollNow && !haveCreds_) {
      secret = !cfg_.enrollKey.empty() ? cfg_.enrollKey : (codeFailed_ ? std::string() : cfg_.enrollCode);
      enrollNow = !secret.empty() && !cfg_.url.empty();
    }
    if (enrollNow) {
      SetLinkState(LinkState::Enrolling);
      CloseAction act;
      bool fatal = false;
      if (DoEnroll(secret, url, &act, &fatal)) {
        backoff_.Reset();
        continue;
      }
      Log(act.rejected ? 2 : 1, "fleet: " + act.what);
      if (fatal) {
        if (!manual && secret == cfg_.enrollCode) codeFailed_ = true;
        SetLinkState(haveCreds_ ? LinkState::Offline : LinkState::Rejected, act.what);
        if (!haveCreds_) {
          WaitFor(24ll * 3600 * 1000);  // until `ru fleet enroll` / reconnect / unload
          continue;
        }
        continue;  // a manual enroll failed but we still have working credentials
      }
      const int64_t delay = act.fixedDelayMs >= 0 ? act.fixedDelayMs : backoff_.Next(act.capMs, act.baseMs);
      SetLinkState(act.rejected ? LinkState::Rejected : LinkState::Offline, act.what);
      {
        std::lock_guard<std::mutex> lk(mu_);
        status_.nextAttemptMs = NowMs() + delay;
      }
      if (manual) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enrollRequested_) {  // retry the same manual secret after the delay
          pendingEnrollSecret_ = secret;
          pendingEnrollUrl_ = url;
        }
      }
      WaitFor(delay);
      if (manual) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!pendingEnrollSecret_.empty()) enrollRequested_ = true;
      }
      continue;
    }
    if (!haveCreds_) {
      SetLinkState(cfg_.url.empty() ? LinkState::Standalone : LinkState::Unenrolled);
      WaitFor(24ll * 3600 * 1000);
      continue;
    }

    if (GameStalled(MonotonicMs())) {
      // Stay offline until the game thread runs again (see ClientConfig::stallMs).
      SetLinkState(LinkState::Offline, "game thread stalled");
      WaitFor(1000);
      continue;
    }
    SetLinkState(LinkState::Connecting);
    SessionResult r = RunSession();
    PublishSpoolStatus();
    if (r.end == SessionEnd::Stopped || stop_) break;
    if (r.established) backoff_.OnSessionEnded(r.durationMs);
    if (r.end == SessionEnd::Reconnect) {
      SetLinkState(LinkState::Offline, "reconnect requested");
      backoff_.Reset();
      continue;
    }
    const int64_t delay = r.action.fixedDelayMs >= 0 ? r.action.fixedDelayMs : backoff_.Next(r.action.capMs, r.action.baseMs);
    if (r.action.rejected) {
      Log(2, "fleet: credentials rejected, re-enroll (" + r.action.what + ")" +
                 (cfg_.enrollKey.empty() ? "; run `ru fleet enroll <code>`" : "; enrolling again with the fleet key"));
      SetLinkState(LinkState::Rejected, r.action.what);
      if (!cfg_.enrollKey.empty()) haveCreds_ = false;  // the key gets our record back (same install_id)
    } else {
      Log(r.established ? 1 : 3, "fleet: " + r.action.what + "; reconnecting in " + std::to_string(delay / 1000) + "." +
                                     std::to_string((delay % 1000) / 100) + " s");
      SetLinkState(LinkState::Offline, r.action.what);
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      status_.nextAttemptMs = NowMs() + delay;
    }
    WaitFor(delay);
  }
  spool_.FlushMeta();
}

}  // namespace fleet
