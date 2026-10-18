// readyup-movement: surf, bhop and kz on community servers: the mode's physics, a run timer
// between the map's start and end zones, personal bests and the map's top times.
//
//   .restart / .rs       back to the map's start zone (the timer resets)
//   .b [n] / .bonus [n]  to bonus n's start zone (default: the bonus you are on, else 1)
//   .pb [bN]             your best time on this map (or bonus N)
//   .wr / .maptop [bN]   the map's top 10 (this mode; or bonus N)
//   .cp / .tp            kz: save a checkpoint / teleport to it, with where you looked (the run
//                        counts its teleports)
//   .timerhud            your timer panel on / off (.timer is practice's)
//
// Chat: the runner hears when the timer starts and stops (left the start, died, noclip) and
// their time. Others hear a finish per movement.cfg finish_chat (default: only server records).
//
// The mode comes from cfg/ReadyUp/movement.cfg `mode=` (auto: from the map prefix surf_ / bhop_ /
// kz_). Zones are the map's own triggers and buttons by name (map_start / map_end,
// timer_startzone / timer_endzone, climb_startbutton / climb_endbutton, ...): their boxes are read
// on map start, players are checked against them every tick. Bonus courses (b1_start / b1_end,
// bonus2_start, ...) are runs of their own with their own records. Off while a match is loaded.
//
// Records: csgo/readyup/plugins/movement/records.json (best time per player per map, mode and
// course). In fleet mode every finished run also goes to the platform (`movement.record`,
// reliable), which keeps the leaderboards across servers.
#include "movement_rules.h"

#include "readyup/essentials_iface.h"
#include "readyup/help_iface.h"
#include "readyup/fleet_iface.h"
#include "readyup/json_store.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MOVEMENT_VERSION
#define MOVEMENT_VERSION "dev"
#endif

namespace movement {
namespace {

const ru_api* g_api = nullptr;

struct Off {
  bool resolved = false;
  int ctrlPawn = -1, lifeState = -1, bodyComponent = -1, sceneNode = -1, absOrigin = -1, absVelocity = -1;
  int collision = -1, colMins = -1, colMaxs = -1, entityIdentity = -1, identityName = -1;
  int moveType = -1;  // CBaseEntity::m_MoveType (optional: noclip detection)
  int eyeAngles = -1; // CCSPlayerPawn::m_angEyeAngles (optional: .cp keeps where the player looked)
  bool ok() const {
    return ctrlPawn >= 0 && lifeState >= 0 && bodyComponent >= 0 && sceneNode >= 0 && absOrigin >= 0 &&
           collision >= 0 && colMins >= 0 && colMaxs >= 0 && entityIdentity >= 0 && identityName >= 0;
  }
};
Off g_off;

struct Zone {
  ZoneKind kind = ZoneKind::None;
  int course = 0;  // 0 = the map run, N = bonus N
  Box box;
  std::string name;
};

struct Run {
  bool inStart = false, running = false;
  int course = 0;  // the course of the start zone the run left
  double startedAt = 0;
  int teleports = 0;
  bool hasCheckpoint = false;
  Vec3 cp;
  float cpAngles[3] = {0, 0, 0};
  bool cpHasAngles = false;
  double unassignedSince = 0;  // joined but on no team yet (auto team after 2 s)
  bool hud = false;  // the panel is opt-in (.timerhud): chat carries the start and the time
  double nextHud = 0;
};

struct State {
  Mode configured = Mode::Off;  // from movement.cfg; Off = auto
  FinishChat finishChat = FinishChat::Records;
  std::string homeMap = "3073875025";  // movement.cfg home_map (surf_utopia_njv)
  double homeAskedAt = -1e9;
  bool autoMode = true;
  Mode mode = Mode::Off;        // this map's
  std::string map;
  std::vector<Zone> zones;
  double scanAt = 0;            // zones are read a moment after the map starts (entities spawn)
  Run runs[64];
  Records records;
  std::string path;
  bool dirty = false;
  double nextSave = 0;
};
State g;

double Now() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

void Reply(const ru_command_ctx* c, const std::string& m) {
  if (c->is_console || c->slot < 0) ru_logf(g_api, RU_LOG_INFO, "%s", m.c_str());
  else g_api->chat_to_slot(g_api->self, c->slot, m.c_str());
}

bool MatchLoaded() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (!m || !RU_API_HAS(m, mode) || !m->mode) return false;
  const char* mode = m->mode();
  const std::string s = mode ? mode : "";
  return s.rfind("match", 0) == 0 || s.rfind("knife", 0) == 0 || s.rfind("scrim", 0) == 0;
}

bool Active() { return g.mode != Mode::Off && !MatchLoaded(); }

bool Resolve() {
  if (g_off.resolved) return g_off.ok();
  g_off.resolved = true;
  auto so = [](const char* c, const char* f) { return g_api->schema_offset(g_api->self, c, f); };
  g_off.ctrlPawn = so("CCSPlayerController", "m_hPlayerPawn");
  g_off.lifeState = so("CBaseEntity", "m_lifeState");
  g_off.bodyComponent = so("CBaseEntity", "m_CBodyComponent");
  g_off.sceneNode = so("CBodyComponent", "m_pSceneNode");
  g_off.absOrigin = so("CGameSceneNode", "m_vecAbsOrigin");
  g_off.absVelocity = so("CBaseEntity", "m_vecAbsVelocity");
  g_off.collision = so("CBaseModelEntity", "m_Collision");
  g_off.colMins = so("CCollisionProperty", "m_vecMins");
  g_off.colMaxs = so("CCollisionProperty", "m_vecMaxs");
  g_off.entityIdentity = so("CEntityInstance", "m_pEntity");
  g_off.identityName = so("CEntityIdentity", "m_name");
  g_off.moveType = so("CBaseEntity", "m_MoveType");
  g_off.eyeAngles = so("CCSPlayerPawn", "m_angEyeAngles");
  if (g_off.eyeAngles < 0) g_off.eyeAngles = so("CCSPlayerPawnBase", "m_angEyeAngles");
  if (!g_off.ok()) ru_logf(g_api, RU_LOG_WARN, "schema fields missing: zones and timers off on this build");
  return g_off.ok();
}

bool Origin(void* ent, Vec3* out) {
  void* body = Rd<void*>(ent, g_off.bodyComponent);
  if (!body) return false;
  void* node = Rd<void*>(body, g_off.sceneNode);
  if (!node) return false;
  const float* v = reinterpret_cast<const float*>(static_cast<const unsigned char*>(node) + g_off.absOrigin);
  *out = {v[0], v[1], v[2]};
  return true;
}

std::string TargetName(void* ent) {
  void* ident = Rd<void*>(ent, g_off.entityIdentity);
  if (!ident) return {};
  const char* name = Rd<const char*>(ident, g_off.identityName);
  return name ? std::string(name) : std::string();
}

// The map's start and end zones from its triggers / buttons (by targetname).
void ScanZones() {
  g.zones.clear();
  if (!Resolve()) return;
  int scanned = 0;
  for (int i = 65; i < 8192; ++i) {
    void* ent = g_api->entity_by_index(g_api->self, i);
    if (!ent) continue;
    ++scanned;
    const char* cls = g_api->entity_classname(g_api->self, ent);
    if (!cls) continue;
    const bool trigger = std::strncmp(cls, "trigger_", 8) == 0;
    const bool button = std::strcmp(cls, "func_button") == 0;
    if (!trigger && !button) continue;
    const std::string name = TargetName(ent);
    int course = 0;
    const ZoneKind kind = ClassifyZone(name, &course);
    if (kind == ZoneKind::None) continue;
    Vec3 o;
    if (!Origin(ent, &o)) continue;
    const unsigned char* col = static_cast<const unsigned char*>(ent) + g_off.collision;
    const float* mn = reinterpret_cast<const float*>(col + g_off.colMins);
    const float* mx = reinterpret_cast<const float*>(col + g_off.colMaxs);
    Zone z;
    z.kind = kind;
    z.course = course;
    z.name = name;
    z.box.mins = {o.x + mn[0], o.y + mn[1], o.z + mn[2]};
    z.box.maxs = {o.x + mx[0], o.y + mx[1], o.z + mx[2]};
    g.zones.push_back(z);
  }
  int starts = 0, ends = 0, bonuses = 0;
  for (const auto& z : g.zones) {
    if (z.course > 0) bonuses = std::max(bonuses, z.course);
    else (z.kind == ZoneKind::Start ? starts : ends)++;
  }
  ru_logf(g_api, RU_LOG_INFO, "%s on %s: %d start zone(s), %d end zone(s), %d bonus(es) (%d entities scanned)",
          ModeName(g.mode), g.map.c_str(), starts, ends, bonuses, scanned);
}

// course -1 = any course.
const Zone* ZoneAt(const Vec3& p, ZoneKind kind, int course) {
  // Buttons are small: a player next to one counts.
  for (const auto& z : g.zones) {
    if (z.kind != kind || (course >= 0 && z.course != course)) continue;
    if (Inside(z.box, p, z.name.find("button") != std::string::npos ? 48.0f : 2.0f)) return &z;
  }
  return nullptr;
}

const Zone* FirstZone(ZoneKind kind, int course) {
  for (const auto& z : g.zones) {
    if (z.kind == kind && z.course == course) return &z;
  }
  return nullptr;
}

// The finished run to the platform (fleet mode); the platform keeps the cross-server leaderboards.
void SendRecord(const std::string& payload) {
  const auto* f = static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
  if (!f || f->struct_size < offsetof(ru_fleet_v1, send_event) + sizeof(f->send_event) || !f->send_event) return;
  if (f->get_status) {
    ru_fleet_status st{};
    st.struct_size = sizeof(st);
    if (!f->get_status(&st)) return;  // standalone: no platform
  }
  if (!f->send_event("movement.record", payload.c_str(), 0, RU_FLEET_RELIABLE)) {
    ru_logf(g_api, RU_LOG_WARN, "fleet: movement.record not queued");
  }
}

void ApplyModeCvars() {
  for (const auto& c : ModeCvars(g.mode)) (void)g_api->server_command(g_api->self, c.c_str());
}

void LoadSettings() {
  char buf[64] = {};
  g.autoMode = true;
  g.configured = Mode::Off;
  if (g_api->config_get(g_api->self, "mode", buf, sizeof(buf)) >= 0 && std::string(buf) != "auto") {
    bool ok = false;
    g.configured = ParseMode(buf, &ok);
    g.autoMode = !ok;
    if (!ok) ru_logf(g_api, RU_LOG_WARN, "movement.cfg: mode=%s is not auto, surf, bhop, kz or off", buf);
  }
  char hm[128] = {};
  g.homeMap = g_api->config_get(g_api->self, "home_map", hm, sizeof(hm)) >= 0 ? std::string(hm) : "3073875025";
  g.finishChat = FinishChat::Records;
  char fc[32] = {};
  if (g_api->config_get(g_api->self, "finish_chat", fc, sizeof(fc)) >= 0 && fc[0]) {
    bool ok = false;
    g.finishChat = ParseFinishChat(fc, &ok);
    if (!ok) ru_logf(g_api, RU_LOG_WARN, "movement.cfg: finish_chat=%s is not records, self or all", fc);
  }
}

void LoadRecords() {
  std::ifstream f(g.path);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  if (!Deserialize(ss.str(), &g.records)) ru_logf(g_api, RU_LOG_WARN, "records.json could not be read; starting empty");
}

void SaveRecords() {
  if (!g.dirty || g.path.empty()) return;
  const std::string tmp = g.path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return;
    f << Serialize(g.records) << "\n";
  }
  if (std::rename(tmp.c_str(), g.path.c_str()) == 0) g.dirty = false;
}

// ---- runs ---------------------------------------------------------------------------------------

void FinishRun(int slot, double now) {
  Run& r = g.runs[slot];
  r.running = false;
  ru_player p{};
  p.struct_size = sizeof(p);
  if (!g_api->get_player(g_api->self, slot, &p) || p.steamid64 == 0) return;
  const int64_t ms = static_cast<int64_t>((now - r.startedAt) * 1000.0 + 0.5);
  const int64_t date = static_cast<int64_t>(std::time(nullptr));
  const Finish f = AddRun(&g.records, g.map, CourseKey(ModeName(g.mode), r.course), p.steamid64, p.name, ms, date);
  g.dirty = true;
  SendRecord(RecordPayload(g.map, ModeName(g.mode), r.course, p.steamid64, p.name, ms, r.teleports, date));
  std::string tail;
  if (g.mode == Mode::Kz) tail += r.teleports > 0 ? " (" + std::to_string(r.teleports) + " TP)" : " (pro)";
  if (f.serverRecord) tail += ". New server record!";
  else if (f.personalBest && f.previousMs > 0) tail += ". New personal best (-" + FormatTime(f.previousMs - ms) + "), rank " + std::to_string(f.rank) + ".";
  else if (f.personalBest) tail += ", rank " + std::to_string(f.rank) + ".";
  else tail += " (best " + FormatTime(f.previousMs) + ").";
  const std::string where = g.map + (r.course > 0 ? " " + CourseName(r.course) : "");
  if (FinishToAll(g.finishChat, f.serverRecord)) {
    const std::string line = std::string(p.name) + " finished " + where + " in " + FormatTime(ms) + tail;
    g_api->chat_all(g_api->self, line.c_str(), 0);
  } else {
    const std::string line = "You finished " + where + " in " + FormatTime(ms) + tail;
    (void)g_api->chat_to_slot(g_api->self, slot, line.c_str());
  }
}

void HudFor(int slot, double now) {
  Run& r = g.runs[slot];
  // Once a second, whole seconds: redrawing the center panel 10 times a second flickered and
  // queued up network messages for the player (2026-10-15).
  if (!r.hud || now < r.nextHud) return;
  r.nextHud = now + 1.0;
  std::string h;
  if (r.running) {
    const int64_t secs = static_cast<int64_t>(now - r.startedAt);
    char clock[32];
    if (secs >= 60) std::snprintf(clock, sizeof(clock), "%lld:%02lld", static_cast<long long>(secs / 60), static_cast<long long>(secs % 60));
    else std::snprintf(clock, sizeof(clock), "%lld s", static_cast<long long>(secs));
    h = "<font class='fontSize-l' color='#FFD34D'>" + std::string(clock) + "</font>";
    if (g.mode == Mode::Kz) h += " <font color='#9AA4B2'>" + std::to_string(r.teleports) + " TP</font>";
  } else if (r.inStart) {
    h = "<font color='#7FD7FF'>In the start zone</font>";
  } else {
    return;
  }
  (void)g_api->center_html_to_slot_prio(g_api->self, slot, h.c_str(), 2, RU_HTML_PRIO_HUD);
}

bool AnyHumanOn() {
  bool any = false;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        if (p->is_bot || !p->connected) return 1;
        *static_cast<bool*>(u) = true;
        return 0;
      },
      &any);
  return any;
}

// An empty server on a map without a movement mode (a restart boots de_dust2) goes to home_map.
// A workshop download takes a while: asked again only after 3 minutes.
// Only on a movement server (match.so off: the Movement preset). A tournament server that also has
// movement.so loaded sits empty on de_dust2 between matches; sending it to a surf map raced the
// next match's map load and the players joined a surf map (2026-10-15).
bool MatchPluginLoaded() { return g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1) != nullptr; }

void GoHomeIfEmpty(double now) {
  if (g.mode != Mode::Off || g.homeMap.empty() || now - g.homeAskedAt < 180.0) return;
  if (!g.autoMode || MatchPluginLoaded() || AnyHumanOn()) return;
  const auto* e = static_cast<const ru_essentials_v1*>(
      g_api->get_interface(g_api->self, RU_ESSENTIALS_IFACE_NAME, RU_ESSENTIALS_IFACE_VERSION));
  if (!e || !e->load_map) return;
  g.homeAskedAt = now;
  ru_logf(g_api, RU_LOG_INFO, "nobody on %s (no movement mode): to the home map %s", g.map.c_str(), g.homeMap.c_str());
  if (!e->load_map(g.homeMap.c_str())) ru_logf(g_api, RU_LOG_WARN, "the home map %s could not be loaded", g.homeMap.c_str());
}

// Nobody picks a team on a movement server: a player still on none after 2 s goes to the side
// with fewer players (CT on a tie). Spectators chose that and stay.
void AutoTeam(double now) {
  struct Count {
    int t = 0, ct = 0;
    std::vector<int> waiting;
  } n;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        auto* c = static_cast<Count*>(u);
        if (p->team == RU_TEAM_T) ++c->t;
        if (p->team == RU_TEAM_CT) ++c->ct;
        if (!p->is_bot && p->connected && p->slot >= 0 && p->slot < 64 && p->team == RU_TEAM_UNASSIGNED) c->waiting.push_back(p->slot);
        return 1;
      },
      &n);
  for (int slot = 0; slot < 64; ++slot) {
    if (std::find(n.waiting.begin(), n.waiting.end(), slot) == n.waiting.end()) g.runs[slot].unassignedSince = 0;
  }
  for (int slot : n.waiting) {
    Run& r = g.runs[slot];
    if (r.unassignedSince == 0) r.unassignedSince = now;
    if (now - r.unassignedSince < 2.0) continue;
    const int team = n.t < n.ct ? RU_TEAM_T : RU_TEAM_CT;
    if (g_api->player_change_team(g_api->self, slot, team)) {
      (team == RU_TEAM_T ? n.t : n.ct)++;
      r.unassignedSince = 0;
    } else {
      r.unassignedSince = now + 10.0;  // refused: try again later
    }
  }
}

// The first player on an empty server never spawns: CS2's "game commencing" restart needs a
// round to end, and mp_ignore_round_win_conditions keeps it from ending. A player on a team who is
// still not alive after 3 s while nobody else is alive gets a restart (the round has no runs to
// lose). Once someone is alive, a newcomer spawns through mp_join_grace_time.
double g_waitingSince = 0, g_lastRestart = -1e9;
void SpawnFirstPlayer(double now) {
  struct S {
    int waiting = 0, alive = 0;
  } st;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        auto* x = static_cast<S*>(u);
        if (p->is_bot || !p->connected || (p->team != RU_TEAM_T && p->team != RU_TEAM_CT) || p->slot < 0) return 1;
        void* ctrl = g_api->entity_by_index(g_api->self, p->slot + 1);
        void* pawn = ctrl ? g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrlPawn)) : nullptr;
        if (pawn && Rd<uint8_t>(pawn, g_off.lifeState) == 0) ++x->alive;
        else ++x->waiting;
        return 1;
      },
      &st);
  if (st.waiting == 0 || st.alive > 0) {
    g_waitingSince = 0;
    return;
  }
  if (g_waitingSince == 0) g_waitingSince = now;
  if (now - g_waitingSince < 3.0 || now - g_lastRestart < 10.0) return;
  g_lastRestart = now;
  g_waitingSince = 0;
  ru_logf(g_api, RU_LOG_INFO, "nobody alive, %d waiting to spawn: restarting the round", st.waiting);
  (void)g_api->server_command(g_api->self, "mp_restartgame 1");
}

void OnTick(void*, const ru_tick_info* t) {
  const double now = t->now;
  // A match server (match.so on): movement.so stays out of the way entirely. The full bundle
  // installs it on every server; only the Movement preset (match off) runs it.
  if (MatchPluginLoaded()) return;
  GoHomeIfEmpty(now);
  // Teams and spawns belong to match.so where it runs (a match's rosters, warmup).
  if (Active() && !MatchPluginLoaded()) AutoTeam(now);
  if (Active() && !MatchPluginLoaded() && Resolve()) SpawnFirstPlayer(now);
  if (g.scanAt > 0 && now >= g.scanAt) {
    g.scanAt = 0;
    ScanZones();
    ApplyModeCvars();
  }
  if (now >= g.nextSave) {
    g.nextSave = now + 30.0;
    SaveRecords();
  }
  if (!Active() || g.zones.empty() || !Resolve()) return;
  for (int slot = 0; slot < 64; ++slot) {
    Run& r = g.runs[slot];
    void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
    if (!ctrl) {
      r = Run{};
      continue;
    }
    void* pawn = g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrlPawn));
    if (!pawn || Rd<uint8_t>(pawn, g_off.lifeState) != 0) {
      if (r.running) {
        (void)g_api->chat_to_slot(g_api->self, slot, "Timer stopped: you died. Start again from the start zone (.rs).");
      }
      r.running = false;
      r.inStart = false;
      continue;
    }
    Vec3 p;
    if (!Origin(pawn, &p)) continue;
    // Noclip (practice's .noclip, sv_cheats): the run does not count. It counts again from the start zone.
    constexpr uint8_t kMoveTypeNoclip = 8;
    if (g_off.moveType >= 0 && Rd<uint8_t>(pawn, g_off.moveType) == kMoveTypeNoclip) {
      if (r.running) (void)g_api->chat_to_slot(g_api->self, slot, "Timer stopped: noclip. Go back to the start (.rs) for a run that counts.");
      r.running = false;
      r.inStart = false;
      continue;
    }
    const Zone* start = ZoneAt(p, ZoneKind::Start, -1);
    const bool inStart = start != nullptr;
    if (inStart && r.running && now - r.startedAt > 1.0) {
      // The map's teleport (a fall) or .rs put the runner back at the start.
      (void)g_api->chat_to_slot(g_api->self, slot, "Back at the start: timer reset.");
    }
    if (inStart) {
      r.running = false;
      r.teleports = 0;
      r.course = start->course;
    } else if (r.inStart) {
      r.running = true;  // left the start zone: go
      r.startedAt = now;
      (void)g_api->chat_to_slot(g_api->self, slot, r.course > 0 ? ("Timer started (" + CourseName(r.course) + ").").c_str() : "Timer started.");
    }
    r.inStart = inStart;
    if (r.running && ZoneAt(p, ZoneKind::End, r.course)) FinishRun(slot, now);
    HudFor(slot, now);
  }
}

void OnMapStart(void*, const ru_event* e) {
  SaveRecords();
  const char* cm = g_api->current_map(g_api->self);
  g.map = cm ? cm : (e && e->map ? e->map : "");
  LoadSettings();
  g.mode = g.autoMode ? ModeForMap(g.map) : g.configured;
  g.zones.clear();
  for (auto& r : g.runs) r = Run{};
  g.scanAt = g.mode != Mode::Off ? Now() + 3.0 : 0;
}

// ---- commands -----------------------------------------------------------------------------------

bool CheckActive(const ru_command_ctx* c) {
  if (Active()) return true;
  Reply(c, MatchLoaded() ? "Movement commands are off during a match." : "This map has no movement mode (surf_, bhop_, kz_).");
  return false;
}

void TeleportTo(int slot, const Vec3& p, const float* angles) {
  const float origin[3] = {p.x, p.y, p.z};
  const float zero[3] = {0, 0, 0};
  (void)g_api->player_teleport(g_api->self, slot, origin, angles, zero);
}

void ToStart(const ru_command_ctx* c, int course) {
  const Zone* z = FirstZone(ZoneKind::Start, course);
  if (!z) {
    return Reply(c, course > 0 ? "This map has no " + CourseName(course) + "." : "This map has no start zone the timer knows.");
  }
  g.runs[c->slot].running = false;
  TeleportTo(c->slot, Center(z->box), nullptr);
}

// readyup.help.movement: the `.help` line (essentials), none while movement is off.
const char* HelpLineNow() {
  if (!Active()) return nullptr;
  return "Movement: .rs restart | .b [n] bonus | .cp / .tp checkpoint | .pb .wr records | .timerhud";
}
const ru_help_v1 g_help = {sizeof(ru_help_v1), &HelpLineNow};

void OnRestart(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->slot < 0) return;
  ToStart(c, 0);
}

void OnBonus(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->slot < 0) return;
  int course = g.runs[c->slot].course > 0 ? g.runs[c->slot].course : 1;
  if (c->argc > 1) {
    course = ParseCourse(c->argv[1]);
    if (course <= 0) return Reply(c, "Usage: .b <number>, e.g. .b 2");
  }
  ToStart(c, course);
}

// `.pb b2` / `.wr 2`: the course the player asked for; -1 after a usage reply.
int CourseArg(const ru_command_ctx* c) {
  if (c->argc < 2) return 0;
  const int course = ParseCourse(c->argv[1]);
  if (course < 0) Reply(c, std::string("Usage: ") + c->argv[0] + " [b<number>], e.g. " + c->argv[0] + " b1");
  return course;
}

std::string Where(int course) { return course > 0 ? g.map + " " + CourseName(course) : g.map; }

void OnPb(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->steamid64 == 0) return;
  const int course = CourseArg(c);
  if (course < 0) return;
  const auto all = Top(g.records, g.map, CourseKey(ModeName(g.mode), course), 100000);
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].steamid64 == c->steamid64) {
      return Reply(c, "Your best on " + Where(course) + ": " + FormatTime(all[i].rec.ms) + ", rank " + std::to_string(i + 1) +
                          " of " + std::to_string(all.size()) + ".");
    }
  }
  Reply(c, "No time on " + Where(course) + " yet.");
}

void OnTop(void*, const ru_command_ctx* c) {
  if (!CheckActive(c)) return;
  const int course = CourseArg(c);
  if (course < 0) return;
  const auto top = Top(g.records, g.map, CourseKey(ModeName(g.mode), course), 10);
  if (top.empty()) return Reply(c, "No times on " + Where(course) + " yet.");
  Reply(c, "Top times on " + Where(course) + " (" + ModeName(g.mode) + "):");
  for (size_t i = 0; i < top.size(); ++i) {
    Reply(c, std::to_string(i + 1) + ". " + FormatTime(top[i].rec.ms) + "  " + top[i].rec.name);
  }
}

void OnCheckpoint(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->slot < 0) return;
  if (g.mode != Mode::Kz) return Reply(c, "Checkpoints are for kz maps.");
  void* ctrl = g_api->entity_by_index(g_api->self, c->slot + 1);
  void* pawn = ctrl ? g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrlPawn)) : nullptr;
  Vec3 p;
  if (!pawn || !Origin(pawn, &p)) return;
  Run& r = g.runs[c->slot];
  r.cp = p;
  r.hasCheckpoint = true;
  r.cpHasAngles = g_off.eyeAngles >= 0;
  if (r.cpHasAngles) {
    std::memcpy(r.cpAngles, static_cast<const unsigned char*>(pawn) + g_off.eyeAngles, sizeof(r.cpAngles));
    r.cpAngles[2] = 0;  // no roll
  }
  Reply(c, "Checkpoint saved.");
}

void OnTeleport(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->slot < 0) return;
  Run& r = g.runs[c->slot];
  if (!r.hasCheckpoint) return Reply(c, "No checkpoint yet: .cp saves one.");
  if (r.running) ++r.teleports;
  TeleportTo(c->slot, r.cp, r.cpHasAngles ? r.cpAngles : nullptr);
}

void OnTimer(void*, const ru_command_ctx* c) {
  if (c->slot < 0) return;
  Run& r = g.runs[c->slot];
  r.hud = !r.hud;
  if (!r.hud) g_api->center_html_release(g_api->self, c->slot);
  Reply(c, r.hud ? "Timer panel on." : "Timer panel off.");
}

}  // namespace
}  // namespace movement

using namespace movement;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 13u,  // needs API 1.13 (player_teleport)
      "movement",
      MOVEMENT_VERSION,
      "Ready Up",
      "surf, bhop, kz: the mode's physics, a run timer between the map's zones, personal bests and top times",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, player_teleport) || !RU_API_HAS(api, data_dir)) return 1;
  g_api = api;
  g = State{};
  g_off = Off{};
  const char* dir = api->data_dir(api->self);
  const std::string data = dir && *dir ? dir : ".";
  std::string err;
  (void)readyup::json_store::MakeDirs(data, &err);
  g.path = data + "/records.json";
  LoadRecords();
  LoadSettings();
  const char* cm = api->current_map(api->self);
  g.map = cm ? cm : "";
  g.mode = g.autoMode ? ModeForMap(g.map) : g.configured;
  g.scanAt = g.mode != Mode::Off ? Now() + 1.0 : 0;
  // match.so loaded first (plugins load by name): a match server. No chat commands, so .restart
  // and .b stay match.so's; the tick does nothing either (OnTick).
  const bool matchServer = api->get_interface(api->self, RU_MATCH_IFACE_NAME, 1) != nullptr;
  struct Cmd {
    const char* name;
    ru_command_fn fn;
  };
  for (const Cmd& c : {Cmd{".restart", &OnRestart}, Cmd{".rs", &OnRestart}, Cmd{".b", &OnBonus},
                       Cmd{".bonus", &OnBonus}, Cmd{".pb", &OnPb}, Cmd{".wr", &OnTop},
                       Cmd{".maptop", &OnTop}, Cmd{".cp", &OnCheckpoint}, Cmd{".tp", &OnTeleport}, Cmd{".timerhud", &OnTimer}}) {
    if (matchServer) break;
    if (!api->register_chat_command(api->self, c.name, c.fn, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `%s` (another plugin owns it)", c.name);
    }
  }
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_HELP_IFACE_PREFIX "movement", RU_HELP_IFACE_VERSION,
                         const_cast<ru_help_v1*>(&g_help));
  if (matchServer) {
    ru_logf(api, RU_LOG_INFO, "loaded " MOVEMENT_VERSION ": off (match.so runs on this server; the Movement preset turns it off)");
  } else {
    ru_logf(api, RU_LOG_INFO, "loaded " MOVEMENT_VERSION ": %s on %s", ModeName(g.mode), g.map.c_str());
  }
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  SaveRecords();
  if (g_api) ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
