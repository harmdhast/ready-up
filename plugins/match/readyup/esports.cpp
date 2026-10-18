// Rulesets, engine side (esports.h).
#include "readyup/esports.h"

#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/server_settings.h"
#include "readyup/wingman.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace readyup {
namespace {

std::mutex g_cacheMu;
std::chrono::steady_clock::time_point g_cacheAt{};
EffectiveRuleSet g_cache;

// ---- default_models (game thread) --------------------------------------------------------

const ru_api* g_api = nullptr;
int g_teamOffset = -2;  // CBaseEntity::m_iTeamNum, -2 = not looked up yet
struct PendingModel {
  uint32_t handle = 0;
  int slot = -1;
  unsigned long long dueFrame = 0;
};
std::vector<PendingModel> g_pending;
unsigned long long g_frame = 0;
std::set<int> g_announced;  // slots logged for the current match
unsigned long long g_announcedMatch = 0;
bool g_setModelWarned = false;

// Passes after the spawn: the spawn frame, the next one, and a few later ones (the game may set
// its own model once more while the pawn finishes spawning).
constexpr unsigned kModelPasses[] = {0, 1, 8, 32};

bool MatchLoadedForModels() {
  const ReadyUpMode m = GetMode();
  return m == ReadyUpMode::MatchWarmup || m == ReadyUpMode::MatchKnife || m == ReadyUpMode::MatchLive ||
         m == ReadyUpMode::Postgame;
}

int PawnTeam(void* pawn) {
  if (g_teamOffset == -2) g_teamOffset = g_api->schema_offset(g_api->self, "CBaseEntity", "m_iTeamNum");
  if (g_teamOffset < 0) return 0;
  return *(static_cast<const unsigned char*>(pawn) + g_teamOffset);
}

void ApplyDefaultModel(const PendingModel& p) {
  void* pawn = g_api->entity_from_handle(g_api->self, p.handle);
  if (!pawn) return;
  const int team = PawnTeam(pawn);
  if (team != 2 && team != 3) return;
  const auto c = Cfg();
  const std::string& model = team == 3 ? c.default_model_ct : c.default_model_t;
  if (model.empty()) return;
  if (g_api->entity_set_model(g_api->self, pawn, model.c_str()) != 1) {
    if (!g_setModelWarned) {
      g_setModelWarned = true;
      Print("esports: default_models: entity_set_model failed (CBaseModelEntity::SetModel is resolved from "
            "engine-surface.skins.json; is it installed next to the core?)\n");
    }
    return;
  }
  const auto ctx = WebhookGetMatchContext();
  const unsigned long long mid = ctx ? ctx->matchid : 0;
  if (mid != g_announcedMatch) {
    g_announcedMatch = mid;
    g_announced.clear();
  }
  if (g_announced.insert(p.slot).second) {
    Print("esports: default_models: slot %d (%s) -> %s\n", p.slot, team == 3 ? "CT" : "T", model.c_str());
  }
}

void OnPlayerSpawn(void*, const char*, const ru_game_event* ev) {
  if (!g_api || !MatchLoadedForModels()) return;
  if (!CurrentEffectiveRules().Bool("default_models", false)) return;
  void* pawn = g_api->ev_get_player_pawn(g_api->self, ev, "userid");
  if (!pawn) return;
  const uint32_t h = g_api->entity_handle_of(g_api->self, pawn);
  if (h == 0 || h == 0xFFFFFFFFu) return;
  const int slot = g_api->ev_get_player_slot(g_api->self, ev, "userid");
  for (unsigned d : kModelPasses) g_pending.push_back(PendingModel{h, slot, g_frame + d});
}

// ---- halftime pause (game thread) ---------------------------------------------------------

std::atomic<bool> g_halftimePending{false};

// The second half just started (match_events, round start after the regulation halftime): the
// engine pauses it in freeze time (mp_halftime_pausematch 1).
void MaybeMarkHalftimePause() {
  if (!g_halftimePending.exchange(false)) return;
  if (GetMode() != ReadyUpMode::MatchLive || PauseStateGet().paused) return;
  PauseStateOnPaused("halftime", "server");
  Print("pause: halftime (mp_halftime_pausematch 1): both teams .unpause, or an admin\n");
  SendToChat("Ready Up: halftime pause. To start the second half, both teams type .up.");
}

// ---- GOTV on this map (game thread; read from any thread) ------------------------------------

std::atomic<int> g_gotv{static_cast<int>(GotvState::Unknown)};
double g_mapStartAt = 0;  // tick_info.now of the first simulating frame after a map start (0 = not yet)
int g_hltvOffset = -2;    // CBasePlayerController::m_bIsHLTV, -2 = not looked up yet
GotvState g_gotvLogged = GotvState::Unknown;

void RefreshGotv(double now) {
  if (g_mapStartAt == 0) g_mapStartAt = now;
  bool scanOk = false, seen = false;
  if (g_api->entity_system_status(g_api->self) == RU_ENTSYS_OK) {
    if (g_hltvOffset == -2) g_hltvOffset = g_api->schema_offset(g_api->self, "CBasePlayerController", "m_bIsHLTV");
    if (g_hltvOffset >= 0) {
      scanOk = true;
      for (int i = 1; i <= 64 && !seen; ++i) {
        void* e = g_api->entity_by_index(g_api->self, i);
        if (!e) continue;
        const char* cls = g_api->entity_classname(g_api->self, e);
        if (!cls || std::strcmp(cls, "cs_player_controller") != 0) continue;
        seen = *(static_cast<const unsigned char*>(e) + g_hltvOffset) != 0;
      }
    }
  }
  const GotvState st = GotvStateFrom(scanOk, seen, now - g_mapStartAt);
  g_gotv = static_cast<int>(st);
  if (st != g_gotvLogged && st != GotvState::Unknown) {
    g_gotvLogged = st;
    Debug("esports: gotv=%s\n", GotvStateName(st));
  }
}

void OnFrame(void*, const ru_tick_info* t) {
  if (!t || !t->simulating) return;
  MaybeMarkHalftimePause();
  if (g_frame % 32 == 0) RefreshGotv(t->now);
  ++g_frame;
  if (g_pending.empty()) return;
  std::vector<PendingModel> later;
  for (const auto& p : g_pending) {
    if (p.dueFrame > g_frame) {
      later.push_back(p);
      continue;
    }
    ApplyDefaultModel(p);
  }
  g_pending.swap(later);
}

Ruleset CtxRuleset(const WebhookMatchContext* ctx) {
  Ruleset r = ServerRuleset();
  if (ctx && !ctx->ruleset.empty()) (void)ParseRuleset(ctx->ruleset, &r);
  return r;
}

}  // namespace

EffectiveRuleSet EffectiveRulesFor(const WebhookMatchContext* ctx) {
  RulesInput in;
  in.ruleset = CtxRuleset(ctx);
  in.cfg = Cfg().rules;
  // The server's minimum-ready value is the setting (console / chat / fleet, else readyup.cfg
  // min_players_to_ready, else 0; server_settings.h): one value for scrims and the match fallback.
  in.cfg.min_players_to_ready = settings::Int("minimum_ready_required");
  if (ctx) {
    std::string err;
    if (!ParseOverridesText(ctx->overrides_json, &in.overrides, &err)) in.overrides.clear();  // validated at load
    in.match = ctx->rules;
    for (const auto& kv : ctx->cvars) in.cvars[kv.first] = kv.second;
  }
  return ResolveEffective(in);
}

EffectiveRuleSet CurrentEffectiveRules() {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    if (g_cacheAt.time_since_epoch().count() != 0 && now - g_cacheAt < std::chrono::milliseconds(250)) return g_cache;
  }
  const auto ctx = WebhookGetMatchContext();
  EffectiveRuleSet e = EffectiveRulesFor(ctx ? &*ctx : nullptr);
  std::lock_guard<std::mutex> lk(g_cacheMu);
  g_cache = e;
  g_cacheAt = now;
  return e;
}

std::string LiveCfgExecCommand() {
  const auto ctx = WebhookGetMatchContext();
  if (ctx && ctx->wingman) return std::string("exec ") + wingman::kLiveCfg;
  return std::string("exec ") + LiveCfgFor(CtxRuleset(ctx ? &*ctx : nullptr));
}

bool LiveCfgRequired() {
  const auto ctx = WebhookGetMatchContext();
  if (ctx && ctx->wingman) return true;  // without it a wingman map plays competitive rules
  return CtxRuleset(ctx ? &*ctx : nullptr) == Ruleset::Valve;
}

void AppendRuleCommands(std::vector<std::string>* cmds) {
  const auto ctx = WebhookGetMatchContext();
  for (auto& c : RuleCommands(EffectiveRulesFor(ctx ? &*ctx : nullptr))) {
    Debug("esports: rule: %s\n", c.c_str());
    cmds->push_back(std::move(c));
  }
}

bool InventoryLockedNow() { return InventoryLocked(CurrentEffectiveRules()); }

void EsportsInstall(const ru_api* api) {
  g_api = api;
  g_teamOffset = -2;
  g_hltvOffset = -2;
  g_pending.clear();
  api->subscribe_game_event(api->self, "player_spawn", &OnPlayerSpawn, nullptr);
  api->on_frame(api->self, &OnFrame, nullptr);
}

void EsportsOnMatchLoaded(const WebhookMatchContext& ctx) {
  {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    g_cacheAt = {};
  }
  g_halftimePending = false;
  const EffectiveRuleSet e = EffectiveRulesFor(&ctx);
  const auto differs = e.Differs();
  std::string d;
  for (const auto& k : differs) d += (d.empty() ? "" : ",") + k;
  Print("esports: ruleset=%s (%s) go-live cfg=%s differs=%s\n", RulesetName(e.ruleset),
        ctx.ruleset.empty() ? "readyup.cfg" : "match config", ctx.wingman ? wingman::kLiveCfg : LiveCfgFor(e.ruleset),
        d.empty() ? "none" : d.c_str());
  if ((e.ruleset == Ruleset::Valve || ctx.wingman) && !ctx.ruleset_notes.empty()) {
    for (const auto& n : ctx.ruleset_notes) Print("esports: %s\n", n.c_str());
  }
  if (InventoryLocked(e)) Print("esports: players' inventories are not modified (skins plugin inert)\n");
}

void EsportsOnMapStart() {
  g_mapStartAt = 0;
  g_gotv = static_cast<int>(GotvState::Unknown);
  g_gotvLogged = GotvState::Unknown;
}

GotvState EsportsGotvState() { return static_cast<GotvState>(g_gotv.load()); }

bool EsportsGoLiveAllowed(bool forced) {
  const auto ctx = WebhookGetMatchContext();
  const GoLiveVerdict v = GotvGoLiveCheck(CtxRuleset(ctx ? &*ctx : nullptr), EsportsGotvState(), forced);
  static std::chrono::steady_clock::time_point lastSaid{};
  static std::string lastLog;
  const auto now = std::chrono::steady_clock::now();
  if (!v.allowed) {
    if (lastSaid.time_since_epoch().count() == 0 || now - lastSaid >= std::chrono::seconds(30)) {
      lastSaid = now;
      Print("%s\n", v.log.c_str());
      SendToChat(v.chat.c_str());
      // Takes effect when the map (re)loads: a reload then brings the GOTV client up.
      (void)EnqueueServerCommand("tv_enable 1");
    }
    return false;
  }
  lastSaid = {};
  if (!v.log.empty() && v.log != lastLog) Print("%s\n", v.log.c_str());
  lastLog = v.log;
  if (!v.chat.empty()) SendToChat(v.chat.c_str());
  return true;
}

bool AutoPause5v5Enabled() {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return false;
  const auto it = ctx->cvars.find("sv_matchpause_auto_5v5");
  return AutoPause5v5On(CtxRuleset(&*ctx), it == ctx->cvars.end() ? nullptr : &it->second);
}

void EsportsOnHalftime() {
  if (!CurrentEffectiveRules().Bool("halftime_pausematch", false)) return;
  g_halftimePending = true;
}

std::vector<std::string> EsportsRulesReport() {
  const auto ctx = WebhookGetMatchContext();
  const EffectiveRuleSet e = EffectiveRulesFor(ctx ? &*ctx : nullptr);
  auto lines = EffectiveRulesText(e);
  const std::string src = ctx ? (ctx->ruleset.empty() ? "readyup.cfg" : "match config") : "readyup.cfg (no match loaded)";
  // Two short lines: one chat message each.
  lines.insert(lines.begin() + 1, "rules: ruleset source: " + src);
  lines.insert(lines.begin() + 2, std::string("rules: skins ") + (InventoryLocked(e) ? "inert (inventory)" : "allowed") +
                                      ", coaches " + (CoachesAdmitted(e) ? "admitted" : "not admitted"));
  return lines;
}

std::string EsportsSelftestLine() {
  const EffectiveRuleSet e = CurrentEffectiveRules();
  const auto differs = e.Differs();
  std::string s = std::string("ruleset=") + RulesetName(e.ruleset) + " cfg=" + LiveCfgFor(e.ruleset) +
                  " skins=" + (InventoryLocked(e) ? "inert" : "allowed") + " differs=";
  if (differs.empty()) s += "none";
  for (size_t i = 0; i < differs.size(); ++i) s += (i ? "," : "") + differs[i];
  return s;
}

}  // namespace readyup
