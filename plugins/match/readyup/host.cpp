// readyup-match adapters: the functions the match flow called while it was part of the core
// (logging.h, engine.h, players.h), implemented on top of ru_api. See host.h.
#include "readyup/host.h"

#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/players.h"

#include <time.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef READYUP_SEMVER
#define READYUP_SEMVER "unknown"
#endif

namespace readyup {
namespace host {
namespace {

std::atomic<const ru_api*> g_api{nullptr};
std::thread::id g_gameThread;
std::atomic<bool> g_haveGameThread{false};

std::mutex g_qMu;
std::deque<std::function<void()>> g_queue;
constexpr size_t kMaxQueued = 4096;

}  // namespace

const ru_api* Api() { return g_api.load(std::memory_order_acquire); }

void Attach(const ru_api* api) {
  g_gameThread = std::this_thread::get_id();
  g_haveGameThread.store(true, std::memory_order_release);
  g_api.store(api, std::memory_order_release);
}

void Detach() {
  g_api.store(nullptr, std::memory_order_release);
  std::lock_guard<std::mutex> lk(g_qMu);
  g_queue.clear();
}

bool OnGameThread() {
  return g_haveGameThread.load(std::memory_order_acquire) && std::this_thread::get_id() == g_gameThread;
}

void RunOnGameThread(std::function<void()> fn) {
  if (!fn || !Api()) return;
  if (OnGameThread()) {
    fn();
    return;
  }
  std::lock_guard<std::mutex> lk(g_qMu);
  if (g_queue.size() >= kMaxQueued) g_queue.pop_front();
  g_queue.push_back(std::move(fn));
}

double NowSeconds() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

}  // namespace host

namespace {

// ---- per-frame caches for off-thread readers ---------------------------------------------

std::array<std::atomic<int>, static_cast<size_t>(Feature::kCount)> g_featureCache{};
std::atomic<int> g_clientPrintAll{0};
std::atomic<bool> g_roundTermSuppressed{false};

void RefreshFeatureCache() {
  const ru_api* a = host::Api();
  if (!a) return;
  for (int i = 0; i < static_cast<int>(Feature::kCount); ++i) {
    g_featureCache[static_cast<size_t>(i)].store(a->feature_state(a->self, FeatureName(static_cast<Feature>(i))),
                                                 std::memory_order_relaxed);
  }
  g_clientPrintAll.store(a->feature_state(a->self, "fn:UTIL_ClientPrintAll"), std::memory_order_relaxed);
}

std::string VFormat(const char* fmt, va_list ap) {
  char stack[1024];
  va_list ap2;
  va_copy(ap2, ap);
  const int n = std::vsnprintf(stack, sizeof(stack), fmt, ap);
  if (n < 0) {
    va_end(ap2);
    return {};
  }
  if (static_cast<size_t>(n) < sizeof(stack)) {
    va_end(ap2);
    return std::string(stack, static_cast<size_t>(n));
  }
  std::string out(static_cast<size_t>(n) + 1, '\0');
  std::vsnprintf(&out[0], out.size(), fmt, ap2);
  va_end(ap2);
  out.resize(static_cast<size_t>(n));
  return out;
}

void Emit(int level, std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  if (const ru_api* a = host::Api()) {
    a->log_untagged(a->self, level, s.c_str());
  } else {
    std::fprintf(stderr, "%s %s%s\n", kLogPrefix, level == RU_LOG_DEBUG ? "[dbg] " : "", s.c_str());
  }
}

}  // namespace

void host::FrameBegin() {
  std::deque<std::function<void()>> q;
  {
    std::lock_guard<std::mutex> lk(g_qMu);
    q.swap(g_queue);
  }
  for (auto& fn : q) fn();
  RefreshFeatureCache();
  PlayersRefreshCache();
}

// ---- logging.h -----------------------------------------------------------------------------

void Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string s = VFormat(fmt, ap);
  va_end(ap);
  Emit(RU_LOG_INFO, std::move(s));
}

void PrintLine(const char* msg) { Emit(RU_LOG_INFO, msg ? msg : "(null)"); }

void Debug(const char* fmt, ...) {
  if (!DebugEnabled()) return;
  va_list ap;
  va_start(ap, fmt);
  std::string s = VFormat(fmt, ap);
  va_end(ap);
  Emit(RU_LOG_DEBUG, std::move(s));
}

void DebugLine(const char* msg) {
  if (!DebugEnabled()) return;
  Emit(RU_LOG_DEBUG, msg ? msg : "(null)");
}

// ---- engine.h: chat --------------------------------------------------------------------------

void SendToChat(const char* msg) {
  if (!msg || !*msg) return;
  host::RunOnGameThread([m = std::string(msg)] {
    if (const ru_api* a = host::Api()) a->chat_all(a->self, m.c_str(), 0);
  });
}

void SendRawToChat(const char* payload) {
  if (!payload || !*payload) return;
  host::RunOnGameThread([m = std::string(payload)] {
    if (const ru_api* a = host::Api()) a->chat_all(a->self, m.c_str(), RU_CHAT_RAW);
  });
}

void AnnounceToChat(const char* msg) {
  if (!ChatDebugEnabled()) return;
  SendToChat(msg);
}

bool ClientPrintChat(int slot, const char* msg) {
  if (!msg || !*msg || slot < 0) return false;
  const ru_api* a = host::Api();
  if (!a) return false;
  if (host::OnGameThread()) return a->chat_to_slot(a->self, slot, msg) == 1;
  host::RunOnGameThread([slot, m = std::string(msg)] {
    if (const ru_api* x = host::Api()) x->chat_to_slot(x->self, slot, m.c_str());
  });
  return true;
}

bool ClientPrintAvailable() {
  const ru_api* a = host::Api();
  if (a && host::OnGameThread()) return a->feature_state(a->self, "fn:UTIL_ClientPrintAll") == 1;
  return g_clientPrintAll.load(std::memory_order_relaxed) == 1;
}

// ---- engine.h: center HTML, commands, features -------------------------------------------------

int SendCenterHtml(int slot, const std::string& html, int durationSeconds, int priority) {
  const ru_api* a = host::Api();
  if (!a || !host::OnGameThread() || slot < 0 || html.empty()) return 0;
  const int seconds = durationSeconds > 0 ? durationSeconds : 1;
  if (RU_API_HAS(a, center_html_to_slot_prio) && a->center_html_to_slot_prio) {
    return a->center_html_to_slot_prio(a->self, slot, html.c_str(), seconds, priority);
  }
  return a->center_html_to_slot(a->self, slot, html.c_str(), seconds) == 1 ? 1 : 0;
}

bool PrintCenterHtmlToClientOnly(int slot, const std::string& html, int durationSeconds, int priority) {
  return SendCenterHtml(slot, html, durationSeconds, priority) == 1;
}

bool EnqueueServerCommand(const char* text) {
  if (!text || !*text) return false;
  const ru_api* a = host::Api();
  if (!a) return false;
  if (host::OnGameThread()) return a->server_command(a->self, text) == 1;
  host::RunOnGameThread([c = std::string(text)] {
    if (const ru_api* x = host::Api()) x->server_command(x->self, c.c_str());
  });
  return true;
}

bool MoveSlotToSpectators(int slot) {
  const ru_api* a = host::Api();
  if (!a || !host::OnGameThread() || slot < 0 || !RU_API_HAS(a, player_change_team) || !a->player_change_team) return false;
  return a->player_change_team(a->self, slot, RU_TEAM_SPECTATOR) == 1;
}

bool ForceJoinTeamForSlot(int slot, int joinTeam) {
  const ru_api* a = host::Api();
  if (!a || !host::OnGameThread() || slot < 0 || !RU_API_HAS(a, player_change_team) || !a->player_change_team) return false;
  const int team = joinTeam == 3 ? RU_TEAM_CT : joinTeam == 2 ? RU_TEAM_T : RU_TEAM_SPECTATOR;
  return a->player_change_team(a->self, slot, team) == 1;
}

bool RespawnAvailable() {
  const ru_api* a = host::Api();
  if (!a || !host::OnGameThread() || !RU_API_HAS(a, player_respawn) || !a->player_respawn) return false;
  return a->engine_feature_available && a->engine_feature_available(a->self, "respawn") == 1;
}

int RespawnSlot(int slot) {
  const ru_api* a = host::Api();
  if (!a || !host::OnGameThread() || slot < 0 || !RU_API_HAS(a, player_respawn) || !a->player_respawn) return 0;
  return a->player_respawn(a->self, slot);
}

const char* FeatureName(Feature f) {
  switch (f) {
    case Feature::ChatCommands: return "chat_commands";
    case Feature::MatchFlow: return "match_flow";
    case Feature::Pauses: return "pauses";
    case Feature::WelcomeHtml: return "welcome_html";
    case Feature::RoundTermSuppression: return "round_term_suppression";
    case Feature::Events: return "events";
    case Feature::ClientCommandHook: return "client_command_hook";
    case Feature::PlayerChatPrint: return "player_chat_print";
    case Feature::ReadyHud: return "ready_hud";
    case Feature::HudBrand: return "hud_brand";
    case Feature::Knife: return "knife";
    case Feature::Plugins: return "plugins";
    default: return "?";
  }
}

bool FeatureEnabled(Feature f) {
  const size_t i = static_cast<size_t>(f);
  if (i >= g_featureCache.size()) return false;
  const ru_api* a = host::Api();
  if (a && host::OnGameThread()) {
    const int v = a->feature_state(a->self, FeatureName(f));
    g_featureCache[i].store(v, std::memory_order_relaxed);
    return v == 1;
  }
  return g_featureCache[i].load(std::memory_order_relaxed) == 1;
}

void SetRoundTerminationSuppressed(bool suppress) {
  host::RunOnGameThread([suppress] {
    const ru_api* a = host::Api();
    if (!a) return;
    if (a->set_round_termination_suppressed(a->self, suppress ? 1 : 0) == 1) {
      g_roundTermSuppressed.store(suppress, std::memory_order_relaxed);
    }
  });
}

bool RoundTerminationSuppressed() { return g_roundTermSuppressed.load(std::memory_order_relaxed); }

// ---- engine.h: paths / versions ----------------------------------------------------------------

std::string GetThisModuleDir() {
  static std::mutex mu;
  static std::string cached;
  std::lock_guard<std::mutex> lk(mu);
  if (cached.empty()) {
    if (const ru_api* a = host::Api()) {
      const char* d = a->config_dir(a->self);
      if (d) cached = d;
    }
  }
  return cached;
}

std::string GetCsgoDirFromModuleDir() {
  // core dir: <csgo>/readyup/bin/linuxsteamrt64 -> <csgo>
  std::string d = GetThisModuleDir();
  if (d.empty()) return {};
  for (int i = 0; i < 3; ++i) {
    const size_t slash = d.find_last_of('/');
    if (slash == std::string::npos) return {};
    d = d.substr(0, slash);
  }
  return d;
}

const char* SemVer() { return READYUP_SEMVER; }

const char* BuildVersion() {
  const ru_api* a = host::Api();
  return a && a->core_version ? a->core_version : READYUP_SEMVER;
}

// ---- players.h ------------------------------------------------------------------------------------

namespace {

struct PlayersView {
  std::vector<HumanIdentity> humans;
  std::vector<BotIdentity> bots;
};

std::mutex g_playersMu;
PlayersView g_players;  // last frame's view (off-thread readers)

PlayersView ReadPlayers() {
  PlayersView v;
  const ru_api* a = host::Api();
  if (!a) return v;
  a->for_each_player(
      a->self,
      [](void* user, const ru_player* p) -> int {
        auto* out = static_cast<PlayersView*>(user);
        const int userid = p->struct_size >= offsetof(ru_player, userid) + sizeof(p->userid) ? p->userid : -1;
        if (p->is_bot) {
          BotIdentity b;
          b.userid = userid;
          b.pseudo_id = userid >= 0 ? DevBotIdForUserid(userid) : 0;
          b.name = p->name;
          b.team = p->team;
          if (b.userid >= 0) out->bots.push_back(std::move(b));
        } else {
          HumanIdentity h;
          h.userid = userid;
          h.slot = p->slot;
          h.steamid64 = p->steamid64;
          h.name = p->name;
          h.team = p->team;
          out->humans.push_back(std::move(h));
        }
        return 1;
      },
      &v);
  return v;
}

PlayersView Players() {
  if (host::OnGameThread() && host::Api()) {
    PlayersView v = ReadPlayers();
    std::lock_guard<std::mutex> lk(g_playersMu);
    g_players = v;
    return v;
  }
  std::lock_guard<std::mutex> lk(g_playersMu);
  return g_players;
}

}  // namespace

void PlayersRefreshCache() { (void)Players(); }

std::vector<HumanIdentity> ListHumans() { return Players().humans; }

std::vector<BotIdentity> ListBots() { return Players().bots; }

std::vector<SlotIdentity> ListSlotIdentities() {
  std::vector<SlotIdentity> out;
  for (const auto& h : ListHumans()) {
    if (h.userid < 0 || h.steamid64 == 0) continue;
    out.push_back(SlotIdentity{h.userid, h.steamid64, h.name});
  }
  return out;
}

std::optional<SlotIdentity> GetSlotIdentity(int slot) {
  if (slot < 0) return std::nullopt;
  for (const auto& h : ListHumans()) {
    if (h.userid == slot && h.steamid64 != 0) return SlotIdentity{h.userid, h.steamid64, h.name};
  }
  return std::nullopt;
}

const char* TeamSourceName(TeamSource s) {
  switch (s) {
    case TeamSource::None: return "none";
    case TeamSource::Log: return "log";
    case TeamSource::Event: return "event";
  }
  return "?";
}

}  // namespace readyup
