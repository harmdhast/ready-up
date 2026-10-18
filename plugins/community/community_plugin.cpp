// readyup-community: the classic community-server commands, next to Ready Up's own.
//
//   .rank                your rank on this server: points, kills, deaths, K/D, headshots
//   .top                 the server's top 10
//   .speclist            who is spectating you right now
//   .kick <name> [why]   admins: kick a player
//
// Stats count on every map while no match is loaded (a match has its own stats on the
// platform): kills, deaths, headshots and assists per Steam ID, kept in
// csgo/readyup/plugins/community/stats.json (saved every minute and on unload). Points: 2 per
// kill, 1 per headshot, 1 per assist, -1 per death. A player needs min_kills_to_rank kills
// before they get a rank.
//
// Settings: cfg/ReadyUp/community.cfg (enabled, count_in_matches, min_kills_to_rank).
#include "community_rules.h"

#include "readyup/json_store.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/help_iface.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef COMMUNITY_VERSION
#define COMMUNITY_VERSION "dev"
#endif

namespace community {
namespace {

const ru_api* g_api = nullptr;

struct Settings {
  bool enabled = true;
  bool countInMatches = false;
  int64_t minKillsToRank = 10;
};

struct State {
  Settings s;
  std::map<uint64_t, PlayerStats> stats;
  std::string path;
  bool dirty = false;
  double nextSave = 0;
};
State g;

// Schema offsets for .speclist (-1 = not found: .speclist says it is unavailable).
struct Off {
  int ctrlPawn = -2, ctrlObserverPawn = -2, pawnObserverServices = -2, obsTarget = -2;
};
Off g_off;

void Reply(const ru_command_ctx* c, const std::string& m) {
  if (c->is_console || c->slot < 0) ru_logf(g_api, RU_LOG_INFO, "%s", m.c_str());
  else g_api->chat_to_slot(g_api->self, c->slot, m.c_str());
}

bool MatchLoaded() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (!m || !RU_API_HAS(m, mode) || !m->mode) return false;
  const char* mode = m->mode();
  const std::string s = mode ? mode : "";
  return s.rfind("match", 0) == 0 || s.rfind("knife", 0) == 0;
}

bool Counting() { return g.s.enabled && (g.s.countInMatches || !MatchLoaded()); }

void LoadSettings() {
  char buf[64] = {};
  if (g_api->config_get(g_api->self, "enabled", buf, sizeof(buf)) >= 0) g.s.enabled = std::string(buf) != "0";
  if (g_api->config_get(g_api->self, "count_in_matches", buf, sizeof(buf)) >= 0) g.s.countInMatches = std::string(buf) == "1";
  if (g_api->config_get(g_api->self, "min_kills_to_rank", buf, sizeof(buf)) >= 0) {
    const long v = std::strtol(buf, nullptr, 10);
    if (v >= 0 && v <= 100000) g.s.minKillsToRank = v;
  }
}

void LoadStats() {
  std::ifstream f(g.path);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  if (!Deserialize(ss.str(), &g.stats)) ru_logf(g_api, RU_LOG_WARN, "stats.json could not be read; starting empty");
}

void SaveStats() {
  if (!g.dirty || g.path.empty()) return;
  const std::string tmp = g.path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return;
    f << Serialize(g.stats) << "\n";
  }
  if (std::rename(tmp.c_str(), g.path.c_str()) == 0) g.dirty = false;
}

PlayerStats* StatsFor(int slot) {
  ru_player p{};
  p.struct_size = sizeof(p);
  if (slot < 0 || !g_api->get_player(g_api->self, slot, &p) || p.is_bot || p.steamid64 == 0) return nullptr;
  PlayerStats& s = g.stats[p.steamid64];
  s.name = p.name;
  s.lastSeen = static_cast<int64_t>(std::time(nullptr));
  return &s;
}

// ---- events -------------------------------------------------------------------------------------

void OnPlayerDeath(void*, const char*, const ru_game_event* ev) {
  if (!ev || !Counting()) return;
  const int victim = g_api->ev_get_player_slot(g_api->self, ev, "userid");
  const int attacker = g_api->ev_get_player_slot(g_api->self, ev, "attacker");
  const int assister = g_api->ev_get_player_slot(g_api->self, ev, "assister");
  if (PlayerStats* v = StatsFor(victim)) {
    ++v->deaths;
    g.dirty = true;
  }
  if (attacker >= 0 && attacker != victim) {
    if (PlayerStats* a = StatsFor(attacker)) {
      ++a->kills;
      if (g_api->ev_get_int(g_api->self, ev, "headshot", 0)) ++a->headshots;
      g.dirty = true;
    }
  }
  if (assister >= 0 && assister != attacker && assister != victim) {
    if (PlayerStats* s = StatsFor(assister)) {
      ++s->assists;
      g.dirty = true;
    }
  }
}

void OnTick(void*, const ru_tick_info* t) {
  if (t->now >= g.nextSave) {
    g.nextSave = t->now + 60.0;
    SaveStats();
  }
}

// ---- commands -----------------------------------------------------------------------------------

// readyup.help.community: the `.help` line (essentials).
const char* HelpLineNow() { return "Community: .rank | .top | .speclist"; }
const ru_help_v1 g_help = {sizeof(ru_help_v1), &HelpLineNow};

void OnRank(void*, const ru_command_ctx* c) {
  if (!g.s.enabled || c->steamid64 == 0) return;
  const auto ranking = Ranking(g.stats, g.s.minKillsToRank);
  auto it = g.stats.find(c->steamid64);
  if (it == g.stats.end()) return Reply(c, "No stats for you on this server yet.");
  const PlayerStats& s = it->second;
  const int rank = RankOf(ranking, c->steamid64);
  const std::string where = rank > 0 ? "Rank " + std::to_string(rank) + " of " + std::to_string(ranking.size())
                                     : "Unranked (" + std::to_string(g.s.minKillsToRank) + " kills needed)";
  Reply(c, where + ": " + std::to_string(Points(s)) + " points, " + std::to_string(s.kills) + " kills, " +
               std::to_string(s.deaths) + " deaths, K/D " + Kdr(s) + ", " + std::to_string(s.headshots) + " headshots.");
}

void OnTop(void*, const ru_command_ctx* c) {
  if (!g.s.enabled) return;
  const auto ranking = Ranking(g.stats, g.s.minKillsToRank);
  if (ranking.empty()) return Reply(c, "Nobody is ranked on this server yet.");
  Reply(c, "Top players on this server:");
  for (size_t i = 0; i < ranking.size() && i < 10; ++i) {
    const auto& r = ranking[i];
    Reply(c, std::to_string(i + 1) + ". " + r.stats->name + ": " + std::to_string(r.points) + " points, K/D " + Kdr(*r.stats));
  }
}

bool ResolveOffsets() {
  if (g_off.ctrlPawn == -2) {
    g_off.ctrlPawn = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_hPlayerPawn");
    g_off.ctrlObserverPawn = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_hObserverPawn");
    g_off.pawnObserverServices = g_api->schema_offset(g_api->self, "CBasePlayerPawn", "m_pObserverServices");
    g_off.obsTarget = g_api->schema_offset(g_api->self, "CPlayer_ObserverServices", "m_hObserverTarget");
  }
  return g_off.ctrlPawn >= 0 && g_off.ctrlObserverPawn >= 0 && g_off.pawnObserverServices >= 0 && g_off.obsTarget >= 0;
}

uint32_t ReadHandle(void* base, int off) {
  uint32_t h = 0xFFFFFFFFu;
  std::memcpy(&h, static_cast<unsigned char*>(base) + off, sizeof(h));
  return h;
}

void OnSpeclist(void*, const ru_command_ctx* c) {
  if (!g.s.enabled || c->slot < 0) return;
  if (!ResolveOffsets() || g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) {
    return Reply(c, "The spectator list is unavailable on this server build.");
  }
  void* me = g_api->entity_by_index(g_api->self, c->slot + 1);
  if (!me) return;
  const uint32_t myPawn = ReadHandle(me, g_off.ctrlPawn) & 0x7FFFu;  // entity index part of the handle
  std::vector<std::string> watchers;
  for (int slot = 0; slot < 64; ++slot) {
    if (slot == c->slot) continue;
    ru_player p{};
    p.struct_size = sizeof(p);
    if (!g_api->get_player(g_api->self, slot, &p) || p.is_bot || !p.connected) continue;
    void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
    if (!ctrl) continue;
    void* obsPawn = g_api->entity_from_handle(g_api->self, ReadHandle(ctrl, g_off.ctrlObserverPawn));
    if (!obsPawn) continue;
    void* svc = nullptr;
    std::memcpy(&svc, static_cast<unsigned char*>(obsPawn) + g_off.pawnObserverServices, sizeof(svc));
    if (!svc) continue;
    const uint32_t target = ReadHandle(svc, g_off.obsTarget);
    if (target != 0xFFFFFFFFu && (target & 0x7FFFu) == myPawn) watchers.push_back(p.name);
  }
  if (watchers.empty()) return Reply(c, "Nobody is spectating you.");
  std::string l;
  for (const auto& w : watchers) l += (l.empty() ? "" : ", ") + w;
  Reply(c, "Spectating you (" + std::to_string(watchers.size()) + "): " + l);
}

void OnKick(void*, const ru_command_ctx* c) {
  if (!c->is_console && !g_api->is_admin(g_api->self, c->steamid64)) return Reply(c, "not authorized");
  if (c->argc < 2) return Reply(c, "usage: .kick <name> [reason]");
  std::vector<std::pair<int, std::string>> names;
  std::vector<int> userids(64, -1);
  for (int slot = 0; slot < 64; ++slot) {
    ru_player p{};
    p.struct_size = sizeof(p);
    if (!g_api->get_player(g_api->self, slot, &p) || !p.connected) continue;
    names.emplace_back(slot, p.name);
    userids[static_cast<size_t>(slot)] = p.userid;
  }
  bool amb = false;
  const int slot = FindByName(names, c->argv[1], &amb);
  if (slot < 0) return Reply(c, amb ? "More than one player matches; type more of the name." : "No player called " + std::string(c->argv[1]) + ".");
  const int userid = userids[static_cast<size_t>(slot)];
  if (userid < 0) return Reply(c, "That player cannot be kicked yet (no user id).");
  std::string reason;
  for (int i = 2; i < c->argc; ++i) reason += (i > 2 ? " " : "") + std::string(c->argv[i]);
  std::string who;
  for (const auto& n : names) {
    if (n.first == slot) who = n.second;
  }
  const std::string cmd = "kickid " + std::to_string(userid) + (reason.empty() ? "" : " \"" + reason + "\"");
  if (g_api->server_command(g_api->self, cmd.c_str()) != 1) return Reply(c, "The kick could not be sent.");
  g_api->chat_all(g_api->self, (who + " was kicked by an admin" + (reason.empty() ? "." : ": " + reason)).c_str(), 0);
}

}  // namespace
}  // namespace community

using namespace community;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,
      "community",
      COMMUNITY_VERSION,
      "Ready Up",
      "community server commands: .rank, .top, .speclist, .kick (admins)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, data_dir)) return 1;
  g_api = api;
  g = State{};
  g_off = Off{};
  LoadSettings();
  const char* dir = api->data_dir(api->self);
  std::string data = dir && *dir ? dir : ".";
  std::string err;
  (void)readyup::json_store::MakeDirs(data, &err);
  g.path = data + "/stats.json";
  LoadStats();
  struct Cmd {
    const char* name;
    ru_command_fn fn;
  };
  for (const Cmd& c : {Cmd{".rank", &OnRank}, Cmd{".top", &OnTop}, Cmd{".speclist", &OnSpeclist}, Cmd{".kick", &OnKick}}) {
    if (!api->register_chat_command(api->self, c.name, c.fn, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `%s` (another plugin owns it)", c.name);
    }
  }
  if (!api->subscribe_game_event(api->self, "player_death", &OnPlayerDeath, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not subscribe to player_death: stats are not counted");
  }
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_HELP_IFACE_PREFIX "community", RU_HELP_IFACE_VERSION,
                         const_cast<ru_help_v1*>(&g_help));
  ru_logf(api, RU_LOG_INFO, "loaded " COMMUNITY_VERSION ": %zu player(s) in stats.json", g.stats.size());
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  SaveStats();
  if (g_api) ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
