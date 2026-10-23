// Roster display names, engine side (roster_names.h).
#include "readyup/roster_names.h"

#include "readyup/logging.h"
#include "readyup/roster_names_rules.h"
#include "readyup/server_settings.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace readyup {
namespace {

constexpr double kCheckSeconds = 1.0;
constexpr size_t kNameField = 128;  // CBasePlayerController::m_iszPlayerName is char[128]

const ru_api* g_api = nullptr;
int g_nameOff = -2;   // -2 = not looked up, -1 = missing
int g_steamOff = -2;
double g_nextCheck = 0;
std::unordered_map<uint64_t, roster_names::PlayerState> g_state;  // players we renamed (or may)
std::unordered_set<uint64_t> g_announced;                         // first rename logged

int Offset(const char* field) {
  for (const char* cls : {"CCSPlayerController", "CBasePlayerController"}) {
    const int off = g_api->schema_offset(g_api->self, cls, field);
    if (off >= 0 && off < 0x20000) return off;
  }
  return -1;
}

bool Offsets() {
  if (g_nameOff == -2 || g_steamOff == -2) {
    if (g_api->schema_offset(g_api->self, "CCSPlayerController", "m_iTeamNum") < 0) return false;  // schema not up yet
    g_nameOff = Offset("m_iszPlayerName");
    g_steamOff = Offset("m_steamID");
    if (g_nameOff < 0) PrintLine("roster-names: m_iszPlayerName offset not found; roster names are off.");
  }
  return g_nameOff >= 0 && g_steamOff >= 0;
}

// The controller of `steamid64` (checked against its m_steamID), or nullptr. This frame only.
unsigned char* ControllerOf(uint64_t steamid64) {
  const int slot = g_api->slot_for_steamid(g_api->self, steamid64);
  if (slot < 0 || slot >= 64) return nullptr;
  void* ent = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ent) return nullptr;
  const char* cls = g_api->entity_classname(g_api->self, ent);
  if (!cls || std::strcmp(cls, "cs_player_controller") != 0) return nullptr;
  auto* c = static_cast<unsigned char*>(ent);
  uint64_t sid = 0;
  std::memcpy(&sid, c + g_steamOff, sizeof(sid));
  return sid == steamid64 ? c : nullptr;
}

std::string NameOf(const unsigned char* controller) {
  char buf[kNameField];
  std::memcpy(buf, controller + g_nameOff, sizeof(buf));
  buf[sizeof(buf) - 1] = 0;
  return buf;
}

bool WriteName(unsigned char* controller, const std::string& name) {
  char buf[kNameField] = {};
  std::memcpy(buf, name.data(), std::min(name.size(), kNameField - 1));
  if (std::memcmp(controller + g_nameOff, buf, sizeof(buf)) == 0) return true;
  std::memcpy(controller + g_nameOff, buf, sizeof(buf));
  return g_api->entity_mark_changed(g_api->self, controller) == 1;
}

// One player: roster name on, or their own name back. False when they have no controller (not
// connected, or not yet).
bool CheckPlayer(uint64_t sid, const std::optional<std::string>& desired) {
  unsigned char* c = ControllerOf(sid);
  if (!c) return false;
  auto& st = g_state[sid];
  std::string name;
  const std::string current = NameOf(c);
  const auto act = roster_names::Decide(current, desired, &st, &name);
  if (act == roster_names::Action::None) return true;
  const bool sent = WriteName(c, name);
  if (act == roster_names::Action::Restore) {
    Print("roster-names: %llu back to \"%s\"\n", static_cast<unsigned long long>(sid), name.c_str());
  } else if (g_announced.insert(sid).second) {
    Print("roster-names: %llu \"%s\" -> \"%s\"%s\n", static_cast<unsigned long long>(sid), current.c_str(),
          name.c_str(), sent ? "" : " (not marked changed; clients may see it late)");
  } else {
    Debug("roster-names: %llu reset to \"%s\" by the engine; \"%s\" again\n", static_cast<unsigned long long>(sid),
          current.c_str(), name.c_str());
  }
  return true;
}

void Check() {
  if (!g_api || !Offsets()) return;
  const auto ctx = WebhookGetMatchContext();
  const bool on = ctx && roster_names::Enabled(ctx->roster_names_rule, settings::Bool("roster_names_enabled"),
                                               !ctx->roster_names.empty());
  std::unordered_set<uint64_t> seen;
  if (on) {
    for (const auto& kv : ctx->roster_names) {
      CheckPlayer(kv.first, roster_names::DesiredName(kv.first, ctx->roster_names));
      seen.insert(kv.first);
    }
  } else {
    g_announced.clear();
  }
  // Players we renamed who are no longer named by the match (unloaded, turned off, another match).
  // Gone from the server: their next controller has their own name anyway.
  for (auto it = g_state.begin(); it != g_state.end();) {
    if (seen.count(it->first)) {
      ++it;
      continue;
    }
    if (!CheckPlayer(it->first, std::nullopt) || it->second.written.empty()) it = g_state.erase(it);
    else ++it;
  }
}

template <typename F>
void Guarded(F&& f) {
  try {
    f();
  } catch (...) {
  }
}

void OnTick(void*, const ru_tick_info* t) {
  if (t->now < g_nextCheck) return;
  g_nextCheck = t->now + kCheckSeconds;
  Guarded(&Check);
}

// The client sent a new name, a player came in / changed team, a new map: check on the next tick.
void OnChangeName(void*, const char*, const ru_game_event*) { g_nextCheck = 0; }
void OnEvent(void*, const ru_event* ev) {
  if (ev->type == RU_EVENT_PLAYER_CONNECT || ev->type == RU_EVENT_PLAYER_TEAM || ev->type == RU_EVENT_MAP_START) {
    g_nextCheck = 0;
  }
}

}  // namespace

void RosterNamesInstall(const ru_api* api) {
  g_api = api;
  g_nameOff = g_steamOff = -2;
  g_nextCheck = 0;
  g_state.clear();
  g_announced.clear();
  if (!RU_API_HAS(api, entity_mark_changed)) {
    PrintLine("roster-names: core has no entity_mark_changed; roster names are off.");
    return;
  }
  api->on_tick(api->self, &OnTick, nullptr);
  api->subscribe(api->self, RU_EVENT_ANY, &OnEvent, nullptr);
  if (!api->subscribe_game_event(api->self, "player_changename", &OnChangeName, nullptr)) {
    PrintLine("roster-names: player_changename not subscribed; a changed Steam name is put back within a second.");
  }
}

void RosterNamesNameBot(const std::string& botName, const std::string& name) {
  Guarded([&] {
    if (!g_api || !Offsets() || name.empty() || botName == name) return;
    for (int index = 1; index <= 64; ++index) {
      void* ent = g_api->entity_by_index(g_api->self, index);
      const char* cls = ent ? g_api->entity_classname(g_api->self, ent) : nullptr;
      if (!cls || std::strcmp(cls, "cs_player_controller") != 0) continue;
      auto* c = static_cast<unsigned char*>(ent);
      uint64_t sid = 0;
      std::memcpy(&sid, c + g_steamOff, sizeof(sid));
      if (sid != 0 || NameOf(c) != botName) continue;  // bots have no SteamID
      WriteName(c, name);
      Print("roster-names: bot \"%s\" plays as \"%s\"\n", botName.c_str(), name.c_str());
      return;
    }
  });
}

void RosterNamesUninstall() {
  Guarded([] {
    if (!g_api || !Offsets()) return;
    for (const auto& kv : std::unordered_map<uint64_t, roster_names::PlayerState>(g_state)) CheckPlayer(kv.first, std::nullopt);
  });
  g_state.clear();
  g_api = nullptr;
}

}  // namespace readyup
