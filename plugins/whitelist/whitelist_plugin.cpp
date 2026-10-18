// readyup-whitelist: on a practice / scrim server only the listed players may stay. Anyone else
// who is connected (not a bot, not a Ready Up admin) is kicked with "Not on this server's
// whitelist". Off until turned on; while a match is loaded its roster decides instead (the match
// plugin kicks), so the whitelist stands down.
//
//   ru whitelist                       help
//   ru whitelist on|off                admin
//   ru whitelist add|remove <steamid64>  admin
//   ru whitelist list|clear            admin
//
// Chat: `.ru whitelist ...` (admins). The state is saved in the plugin's data dir
// (csgo/readyup/plugins/whitelist/whitelist.json), so it survives restarts. The platform can
// drive it over the fleet link (`cmd whitelist.set`). Other plugins add guests through
// readyup.whitelist.v1 set_guests (tv.so's casters): never kicked, not saved.
#include "whitelist_rules.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"
#include "readyup/whitelist_iface.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef WHITELIST_VERSION
#define WHITELIST_VERSION "dev"
#endif

namespace whitelist {
namespace {

const ru_api* g_api = nullptr;
State g_state;
Guests g_guests;  // readyup.whitelist.v1 set_guests (not saved)
std::string g_path;
double g_lastSweep = -1e9;
std::map<uint64_t, double> g_kickedAt;                   // steamid64 -> last kick (no repeats within 5 s)
std::map<uint64_t, std::pair<bool, double>> g_adminAt;  // steamid64 -> (admin, checked at)

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool Save() {
  if (g_path.empty()) return false;
  const std::string tmp = g_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << StateJson(g_state);
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), g_path.c_str()) == 0;
}

std::string RuMode() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (m && RU_API_HAS(m, mode) && m->mode) return m->mode() ? m->mode() : "";  // cheap
  if (!m || !m->get_status) return {};
  ru_match_status st{};
  st.struct_size = sizeof(st);
  return m->get_status(&st) == 1 && st.ru_mode ? st.ru_mode : "";
}

// is_admin may block (a lookup); ask at most once a minute per player.
bool IsAdmin(uint64_t sid, double now) {
  auto it = g_adminAt.find(sid);
  if (it != g_adminAt.end() && now - it->second.second < 60.0) return it->second.first;
  const bool admin = g_api->is_admin(g_api->self, sid) == 1;
  g_adminAt[sid] = {admin, now};
  return admin;
}

struct SweepCtx {
  std::string mode;
  double now;
};

int SweepPlayer(void* user, const ru_player* p) {
  auto* c = static_cast<SweepCtx*>(user);
  if (!p->connected || p->is_bot || p->steamid64 == 0) return 1;
  if (g_guests.Is(p->steamid64)) return 1;
  if (!ShouldKick(g_state, c->mode, p->steamid64, false, IsAdmin(p->steamid64, c->now))) return 1;
  auto it = g_kickedAt.find(p->steamid64);
  if (it != g_kickedAt.end() && c->now - it->second < 5.0) return 1;
  const int id = p->userid >= 0 ? p->userid : p->slot;  // kickid takes the log <N> (the slot)
  if (id < 0) return 1;                                  // not identified yet: next sweep
  g_kickedAt[p->steamid64] = c->now;
  char cmd[160];
  std::snprintf(cmd, sizeof(cmd), "kickid %d \"Not on this server's whitelist\"", id);
  g_api->server_command(g_api->self, cmd);
  ru_logf(g_api, RU_LOG_INFO, "kicked %s (%llu): not on the whitelist", p->name, static_cast<unsigned long long>(p->steamid64));
  return 1;
}

void Sweep(double now) {
  if (!g_state.enabled) return;
  SweepCtx c{RuMode(), now};
  if (MatchOwnsRoster(c.mode)) return;
  g_api->for_each_player(g_api->self, &SweepPlayer, &c);
}

void OnTick(void*, const ru_tick_info* t) {
  if (t->now - g_lastSweep < 2.0) return;
  g_lastSweep = t->now;
  Sweep(t->now);
}

void OnConnect(void*, const ru_event*) { g_lastSweep = -1e9; }  // check on the next tick

void Reply(const ru_command_ctx* ctx, const std::string& msg) {
  if (ctx->is_console) ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
  else if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, ("Ready Up whitelist: " + msg).c_str());
}

void OnRu(void*, const ru_command_ctx* ctx) {
  const std::string sub = ctx->argc >= 3 ? ctx->argv[2] : "";
  const std::string arg = ctx->argc >= 4 ? ctx->argv[3] : "";
  if (sub.empty() || sub == "help") {
    for (const char* l : {".ru whitelist on|off: kick everyone not on the list (admins and bots stay; not during a match)",
                          ".ru whitelist add|remove <steamid64>", ".ru whitelist list | clear"}) {
      Reply(ctx, l);
    }
    return;
  }
  if (!ctx->is_console && g_api->is_admin(g_api->self, ctx->steamid64) != 1) {
    Reply(ctx, "not authorized");
    return;
  }
  bool changed = false;
  if (sub == "on" || sub == "off") {
    g_state.enabled = sub == "on";
    changed = true;
    Reply(ctx, std::string(g_state.enabled ? "on" : "off") + " (" + std::to_string(g_state.steamids.size()) +
                   " player(s) on the list)");
    g_lastSweep = -1e9;
  } else if (sub == "add" || sub == "remove") {
    const uint64_t id = ParseSteamId64(arg);
    if (!id) {
      Reply(ctx, "usage: .ru whitelist " + sub + " <steamid64>");
      return;
    }
    changed = sub == "add" ? g_state.steamids.insert(id).second : g_state.steamids.erase(id) > 0;
    Reply(ctx, (changed ? (sub == "add" ? "added " : "removed ") : "no change for ") + arg);
  } else if (sub == "clear") {
    changed = !g_state.steamids.empty();
    g_state.steamids.clear();
    Reply(ctx, "list cleared");
  } else if (sub == "list") {
    std::string l;
    for (uint64_t id : g_state.steamids) l += (l.empty() ? "" : " ") + std::to_string(id);
    Reply(ctx, std::string(g_state.enabled ? "on" : "off") + ", " + std::to_string(g_state.steamids.size()) +
                   " player(s)" + (l.empty() ? "" : ": " + l));
    if (g_guests.Count() > 0) {
      std::string owners;
      for (const auto& kv : g_guests.byOwner) owners += (owners.empty() ? "" : ", ") + kv.first + " " + std::to_string(kv.second.size());
      Reply(ctx, std::to_string(g_guests.Count()) + " guest(s) other plugins let in (" + owners + ")");
    }
    return;
  } else {
    Reply(ctx, "unknown command. Type .ru help whitelist for the list.");
    return;
  }
  if (changed && !Save()) Reply(ctx, "could not save " + g_path);
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  const std::string d = std::string(g_state.enabled ? "on" : "off") + ", " + std::to_string(g_state.steamids.size()) +
                        " player(s), " + std::to_string(g_guests.Count()) + " guest(s), " + g_path;
  add(ctx, "INFO", "whitelist", d.c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

// readyup.whitelist.v1 (fleet `whitelist.set`).
int IfaceSet(int enabled, const uint64_t* ids, uint32_t count) {
  State s;
  s.enabled = enabled != 0;
  for (uint32_t i = 0; ids && i < count; ++i) {
    if (ParseSteamId64(std::to_string(ids[i]))) s.steamids.insert(ids[i]);
  }
  g_state = std::move(s);
  g_lastSweep = -1e9;
  ru_logf(g_api, RU_LOG_INFO, "set: %s, %zu player(s)", g_state.enabled ? "on" : "off", g_state.steamids.size());
  return Save() ? 1 : 0;
}
int IfaceGet(uint32_t* count) {
  if (count) *count = static_cast<uint32_t>(g_state.steamids.size());
  return g_state.enabled ? 1 : 0;
}
int IfaceSetGuests(const char* owner, const uint64_t* ids, uint32_t count) {
  if (!owner || (count > 0 && !ids)) return 0;
  std::vector<uint64_t> v(ids, ids + count);
  bool changed = false;
  if (!g_guests.Set(owner, v, &changed)) return 0;
  if (changed) {
    ru_logf(g_api, RU_LOG_INFO, "guests of %s: %zu", owner, g_guests.byOwner.count(owner) ? g_guests.byOwner[owner].size() : 0);
  }
  return 1;
}
int IfaceIsGuest(uint64_t sid) { return g_guests.Is(sid) ? 1 : 0; }
const ru_whitelist_v1 g_iface = {sizeof(ru_whitelist_v1), &IfaceSet, &IfaceGet, &IfaceSetGuests, &IfaceIsGuest};

}  // namespace
}  // namespace whitelist

using namespace whitelist;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,  // needs API 1.2 (register_ru_subcommand, ru_player.userid)
      "whitelist",
      WHITELIST_VERSION,
      "Ready Up",
      "only listed players may stay on the server (off until `ru whitelist on`)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, register_ru_subcommand)) return 1;
  g_api = api;
  g_kickedAt.clear();
  g_adminAt.clear();
  g_guests = Guests{};  // owners set theirs again (tv.so does every few seconds)
  g_lastSweep = -1e9;
  const char* dir = api->data_dir(api->self);
  g_path = dir && *dir ? std::string(dir) + "/whitelist.json" : std::string();
  bool ok = true;
  g_state = ParseState(g_path.empty() ? std::string() : ReadFile(g_path), &ok);
  if (!ok) ru_logf(api, RU_LOG_WARN, "%s is not valid JSON; starting off with an empty list", g_path.c_str());
  if (!api->register_ru_subcommand(api->self, "whitelist", &OnRu, nullptr)) return 2;
  api->on_tick(api->self, &OnTick, nullptr);
  api->subscribe(api->self, RU_EVENT_PLAYER_CONNECT, &OnConnect, nullptr);
  if (RU_API_HAS(api, provide_interface)) {
    api->provide_interface(api->self, RU_WHITELIST_IFACE_NAME, RU_WHITELIST_IFACE_VERSION,
                           const_cast<ru_whitelist_v1*>(&g_iface));
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "whitelist", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  }
  ru_logf(api, RU_LOG_INFO, "loaded " WHITELIST_VERSION ": %s, %zu player(s) on the list", g_state.enabled ? "on" : "off",
          g_state.steamids.size());
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
