// readyup-practice: practice mode and its tools, as a plugin of its own so a server can run it
// without the match flow (a dedicated practice server) or next to it.
//
//   .prac / .tactics        admin: practice mode on / off (.exitprac: off). On: ReadyUp/prac.cfg (cheats, full
//                           grenade set, infinite ammo, ...) and everyone respawns with it. Off:
//                           ReadyUp/idle.cfg and everyone respawns without it.
//   ru practice on|off|status   the same from the console / `.ru practice ...` (admin)
//   ru practice as <slot> <.command> [args]   console only: run a tool as that player (bot tests)
//   ru practice dryrun      the same as .dryrun (admin)
//   cfg/ReadyUp/practice.cfg (or readyup.cfg [practice]):
//     always=0              1 = dedicated practice server: practice is switched on at load and on
//                           every map, whenever nothing blocks it. The platform's practice.set
//                           {always} stores it in <data_dir>/always.txt, which wins over the cfg
//
// Feedback (practice mode, everyone sees it; feedback=0 turns it off): "X flashed 2.4 s by Y" and
// "Y hit X: -27 hp, -8 armor (head, ak47), 73 hp left" per hit; HE and molotov / incendiary fire
// summed per throw ("HE by Y: 98 total (A -57, B -41)").
//
// Tools (only in practice mode, never under the valve ruleset, docs/ESPORTS-MODE.md):
//   .rethrow / .rt        your own last grenade again, from where it left your hand with the same
//                         velocity, you as the thrower (ru_api grenade_spawn, 1.12). Its launch is
//                         read from the projectile (m_vInitialPosition / m_vInitialVelocity, see
//                         ScanProjectiles). Falls back to the server-wide sv_rethrow_last_grenade
//                         when the launch was not recorded or that grenade type cannot be spawned
//   .savepos [name]       remember your position (pawn scene node origin), per map
//   .loadpos [name]       move back to it
//   .back                 back to where you threw your last grenade (grenade_thrown), else to
//                         where you were before your last .loadpos / .spawn
//   .clear                ent_remove_all smoke / molotov / decoy projectiles and fires (inferno)
//   .noflash              toggle: your pawn's m_flFlashMaxAlpha 0 (+ m_flFlashDuration 0 on player_blind)
//   .god                  toggle: your pawn's m_bTakesDamage (falls back to toggling buddha)
//   .bot .cbot .boost ... a bot on the other team, moved to where you stand once it spawns
//   .nobots               bot_kick
//   .spawn N / .ctspawn N / .tspawn N   to spawn point N (1-based) of your / the CT / the T team
//   .scen ...             scenarios: replay a recorded pro round with bots (practice_scenarios.h)
//
// ME (Auto Tournament plugin) extras, same names and messages; replies go to the caller only:
//   .last                 to where you threw your last grenade; .back N: to throw N of your history
//   .lastindex            how many grenades you have thrown (your history is numbered 1..N, max 100)
//   .delay <s>            delay for your last grenade: every rethrow of it waits that long first
//   .throw                alias of .rethrow
//   .throwidx N [M ...] / .throwindex   rethrow throw N (M, ...) of your history, each after its .delay
//   .rethrow{smoke,flash,nade,grenade,molotov,decoy} / .throw{...}   your last grenade of that kind
//   Your history counts your throws (grenade_thrown) and projectiles you own that nobody threw
//   (a bot's hello_nade); the plugin's own rethrows and .scen replays are not added to it.
//   .impacts              toggle sv_showimpacts (prac.cfg: 1)
//   .traj / .pip          toggle sv_grenade_trajectory_prac_pipreview (prac.cfg: true)
//   .solid                mp_solid_teammates 0/1 -> 2, else -> 1 (prac.cfg: 2)
//   .break                removes every func_breakable / func_breakable_surf and every prop_dynamic
//                         with health > 0 that takes damage (Mirage's vents and windows): ME fired
//                         the Break input, which the server console cannot (ent_fire needs a
//                         client), so they vanish without the break effect. ru_api entity_remove (v1.5)
//   .timer                start / stop a stopwatch shown in your center panel
//   .bestspawn / .worstspawn, .bestctspawn / .worstctspawn, .besttspawn / .worsttspawn
//                         to the competitive spawn (lowest priority) closest to / farthest from you
//   .showspawns / .hidespawns   a vertical beam on every competitive spawn (CT blue, T orange), like
//                         ME: ru_api 1.13 entity_create "beam" + entity_spawn; .hidespawns removes them
//   .fas / .watchme       everyone else (humans) to the spectators: ru_api 1.13 player_change_team
//   .savenade / .sn <name> [description]   save where you stand and look as a lineup of this map
//                         (yours, or the global ones while .globalnades is on); the reply has its code
//   .loadnade / .ln <name>   to your lineup with the nearest name (then a global one), looking where
//                         you looked (ru_api 1.13 player_teleport; without it the reply has the setang
//                         to type). ME also switched to the grenade (client `slotN`); a server cannot
//                         send client commands, so the reply names the grenade instead
//   .listnades / .lin [filter], .deletenade / .delnade / .dn <name>, .importnade / .in <code>
//                         ("<name> x y z pitch yaw roll", the code .savenade prints)
//   .globalnades          admin: .savenade / .deletenade use the global lineups (owner "default")
//   Lineups live in <plugin data dir>/lineups/<map>.json (practice_lineups.h), read on first use
//   per map and written after every change by a writer thread (joined on unload).
//   .dry / .dryrun        admin, practice only: one round with normal rules (ME defaults: money,
//                         freeze time, round time, no infinite ammo / cheats / buddha; then
//                         cfg/ReadyUp/dryrun.cfg if it exists, for overrides) after bot_kick and
//                         mp_restartgame. When that round ends, or on .dryrun / .prac again,
//                         prac.cfg comes back. Tools keep working; the feedback lines are off.
//
// CS2 1.41.8.4: setpos, setpos_player, setang, ent_setpos, ent_fire and bot_place need a client of
// their own, so from the server console they do nothing. Moves use ru_api entity_set_abs_origin
// (v1.3, engine surface CBaseEntity_SetAbsOrigin); the view angles of .loadnade / .last / .back N /
// .loadpos use player_teleport (1.13, CCSPlayerPawn::Teleport from the practice gamedata) when the
// build has it.
//
// With the match plugin (readyup.match.v1 set_practice) the match flow owns the mode ("practice"
// in ru_mode, no scrim warmup) and refuses while a match is loaded; without it this plugin keeps
// the flag itself. It publishes readyup.practice.v1 (the match plugin hands `.ru mode practice`
// to it). Log lines: `practice: ...`.
#include "practice_feedback.h"
#include "practice_lineups.h"
#include "practice_rules.h"
#include "readyup/map_names.h"
#include "practice_scenarios.h"
#include "practice_tools.h"

#include "readyup/json_store.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/essentials_iface.h"
#include "readyup/practice_iface.h"
#include "readyup/help_iface.h"
#include "readyup/selftest_iface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef PRACTICE_VERSION
#define PRACTICE_VERSION "dev"
#endif

namespace practice {
namespace {

const ru_api* g_api = nullptr;
const ru_api* A() { return g_api; }

void Log(const char* fmt, ...) RU_PRINTF(1, 2);
void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_api) g_api->log_untagged(g_api->self, RU_LOG_INFO, buf);
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- the match plugin ----------------------------------------------------------------------------

const ru_match_v1* Match() {
  return static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
}
bool MatchOwnsMode() {
  const ru_match_v1* m = Match();
  return m && RU_API_HAS(m, set_practice) && m->set_practice;
}
// The match plugin's mode. Asked every tick (IsActive), so: the cheap `mode` member when the
// match plugin has it; otherwise get_status (builds the whole status JSON) at most twice a second.
std::string RuMode() {
  const ru_match_v1* m = Match();
  if (!m) return {};
  if (RU_API_HAS(m, mode) && m->mode) return m->mode() ? m->mode() : "";
  static std::string cached;
  static double at = -1e9;
  const double now = Now();
  if (now - at < 0.5) return cached;
  at = now;
  ru_match_status st{};
  st.struct_size = sizeof(st);
  cached = m->get_status && m->get_status(&st) == 1 && st.ru_mode ? st.ru_mode : "";
  return cached;
}
std::string Ruleset() {
  const ru_match_v1* m = Match();
  if (m && RU_API_HAS(m, ruleset) && m->ruleset && m->ruleset()) return m->ruleset();
  char buf[32] = {};
  return g_api->config_get(g_api->self, "ruleset", buf, sizeof(buf)) > 0 ? buf : "default";
}

bool g_standaloneActive = false;  // practice on, when no match plugin owns the mode
bool g_manualOff = false;         // an admin turned it off: always=1 waits for the next map
bool IsActive() { return MatchOwnsMode() ? RuMode() == "practice" : g_standaloneActive; }

// ---- schema -------------------------------------------------------------------------------------

struct Offsets {
  bool looked = false;
  int ctrlPawn = -1;       // CCSPlayerController::m_hPlayerPawn
  int bodyComponent = -1;  // CBaseEntity::m_CBodyComponent (pointer)
  int sceneNode = -1;      // CBodyComponent::m_pSceneNode (pointer)
  int absOrigin = -1;      // CGameSceneNode::m_vecAbsOrigin
  int lifeState = -1;      // CBaseEntity::m_lifeState
  int eyeAngles = -1;      // CCSPlayerPawn::m_angEyeAngles
  int flashAlpha = -1;     // CCSPlayerPawn::m_flFlashMaxAlpha
  int flashDuration = -1;  // CCSPlayerPawn::m_flFlashDuration
  int takesDamage = -1;    // CBaseEntity::m_bTakesDamage
  int spawnEnabled = -1;   // SpawnPoint::m_bEnabled
  int spawnPriority = -1;  // SpawnPoint::m_iPriority
  int health = -1;         // CBaseEntity::m_iHealth (.break)
  int beamWidth = -1;      // CBeam::m_fWidth (.showspawns)
  int beamEnd = -1;        // CBeam::m_vecEndPos
  int renderColor = -1;    // CBaseModelEntity::m_clrRender
  // Grenade projectiles (per-player rethrow): who threw it and how it left the hand.
  int thrower = -1;      // CBaseGrenade::m_hThrower (the pawn)
  int initPos = -1;      // CBaseCSGrenadeProjectile::m_vInitialPosition
  int initVel = -1;      // CBaseCSGrenadeProjectile::m_vInitialVelocity
  int isInc = -1;        // CMolotovProjectile::m_bIsIncGrenade
  int absVelocity = -1;  // CBaseEntity::m_vecAbsVelocity (fallback for m_vInitialVelocity)
  int absRotation = -1;  // CGameSceneNode::m_angAbsRotation
  int ctrlConnected = -1;  // CBasePlayerController::m_iConnected (PlayerConnectedState)
};
Offsets g_off;

int FirstOffset(std::initializer_list<const char*> classes, const char* field) {
  for (const char* c : classes) {
    const int off = g_api->schema_offset(g_api->self, c, field);
    if (off >= 0) return off;
  }
  return -1;
}

const Offsets& Off() {
  Offsets& o = g_off;
  if (o.looked || g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return o;
  o.looked = true;
  o.ctrlPawn = FirstOffset({"CCSPlayerController"}, "m_hPlayerPawn");
  o.bodyComponent = FirstOffset({"CBaseEntity"}, "m_CBodyComponent");
  o.sceneNode = FirstOffset({"CBodyComponent"}, "m_pSceneNode");
  o.absOrigin = FirstOffset({"CGameSceneNode"}, "m_vecAbsOrigin");
  o.lifeState = FirstOffset({"CBaseEntity"}, "m_lifeState");
  o.eyeAngles = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_angEyeAngles");
  o.flashAlpha = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_flFlashMaxAlpha");
  o.flashDuration = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_flFlashDuration");
  o.takesDamage = FirstOffset({"CBaseEntity"}, "m_bTakesDamage");
  o.spawnEnabled = FirstOffset({"SpawnPoint", "CInfoPlayerCounterterrorist"}, "m_bEnabled");
  o.spawnPriority = FirstOffset({"SpawnPoint", "CInfoPlayerCounterterrorist"}, "m_iPriority");
  o.health = FirstOffset({"CBaseEntity"}, "m_iHealth");
  o.beamWidth = FirstOffset({"CBeam"}, "m_fWidth");
  o.beamEnd = FirstOffset({"CBeam"}, "m_vecEndPos");
  o.renderColor = FirstOffset({"CBaseModelEntity"}, "m_clrRender");
  o.thrower = FirstOffset({"CBaseGrenade"}, "m_hThrower");
  o.initPos = FirstOffset({"CBaseCSGrenadeProjectile"}, "m_vInitialPosition");
  o.initVel = FirstOffset({"CBaseCSGrenadeProjectile"}, "m_vInitialVelocity");
  o.isInc = FirstOffset({"CMolotovProjectile"}, "m_bIsIncGrenade");
  o.absVelocity = FirstOffset({"CBaseEntity"}, "m_vecAbsVelocity");
  o.absRotation = FirstOffset({"CGameSceneNode"}, "m_angAbsRotation");
  o.ctrlConnected = FirstOffset({"CBasePlayerController", "CCSPlayerController"}, "m_iConnected");
  Log("practice: schema pawn=%d body=%d node=%d origin=%d life=%d eyes=%d flash=%d/%d takesdamage=%d spawn=%d/%d "
      "nade thrower=%d initial=%d/%d inc=%d",
      o.ctrlPawn, o.bodyComponent, o.sceneNode, o.absOrigin, o.lifeState, o.eyeAngles, o.flashAlpha, o.flashDuration,
      o.takesDamage, o.spawnEnabled, o.spawnPriority, o.thrower, o.initPos, o.initVel, o.isInc);
  return o;
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}
template <typename T>
void Wr(void* base, int off, T v) {
  std::memcpy(static_cast<unsigned char*>(base) + off, &v, sizeof(T));
}

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

bool Finite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < 65536.f &&
         std::fabs(v.y) < 65536.f && std::fabs(v.z) < 65536.f;
}

void* PawnForSlot(int slot) {
  const Offsets& o = Off();
  if (slot < 0 || o.ctrlPawn < 0) return nullptr;
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return nullptr;
  const char* cls = g_api->entity_classname(g_api->self, ctrl);
  if (!cls || std::strcmp(cls, "cs_player_controller") != 0) return nullptr;
  return g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, o.ctrlPawn));
}

bool Alive(void* pawn) {
  const Offsets& o = Off();
  return pawn && (o.lifeState < 0 || Rd<uint8_t>(pawn, o.lifeState) == 0);
}

bool OriginOf(void* ent, Vec3* out) {
  const Offsets& o = Off();
  if (!ent || o.bodyComponent < 0 || o.sceneNode < 0 || o.absOrigin < 0) return false;
  void* body = Rd<void*>(ent, o.bodyComponent);
  if (!body) return false;
  void* node = Rd<void*>(body, o.sceneNode);
  if (!node) return false;
  *out = Rd<Vec3>(node, o.absOrigin);
  return Finite(*out);
}

struct Spot {
  Vec3 pos;
  Vec3 ang;  // pitch yaw roll (informational: CS2 cannot set another player's view)
  bool set = false;
};

bool SpotOfSlot(int slot, Spot* out) {
  void* pawn = PawnForSlot(slot);
  if (!Alive(pawn) || !OriginOf(pawn, &out->pos)) return false;
  const Offsets& o = Off();
  out->ang = o.eyeAngles >= 0 ? Rd<Vec3>(pawn, o.eyeAngles) : Vec3{};
  out->set = true;
  return true;
}

bool Teleport(int slot, const Vec3& p) {
  if (!RU_API_HAS(g_api, entity_set_abs_origin) || !g_api->entity_set_abs_origin) return false;
  void* pawn = PawnForSlot(slot);
  if (!Alive(pawn)) return false;
  const float xyz[3] = {p.x, p.y, p.z};
  return g_api->entity_set_abs_origin(g_api->self, pawn, xyz) == 1;
}

constexpr const char* kNoTeleport =
    "cannot move you: teleport is unavailable on this server build (ru selftest: CBaseEntity::SetAbsOrigin).";

std::string Fmt(const Vec3& p) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f", p.x, p.y, p.z);
  return buf;
}

// ---- players --------------------------------------------------------------------------------------

// What the engine has in player slot `slot` (practice_rules.h SlotEngineState).
practice::SlotEngineState EngineSlotState(int slot) {
  if (slot < 0) return practice::SlotEngineState::kEmpty;
  if (g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return practice::SlotEngineState::kUnknown;
  const Offsets& o = Off();
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  const char* cls = ctrl ? g_api->entity_classname(g_api->self, ctrl) : nullptr;
  const bool isController = cls && std::strcmp(cls, "cs_player_controller") == 0;
  return practice::SlotStateFrom(true, isController, isController && o.ctrlConnected >= 0,
                                 isController && o.ctrlConnected >= 0 ? Rd<int>(ctrl, o.ctrlConnected) : 0);
}

// A connected player by slot (humans: engine slot; bots: their log <N>, which is the slot in CS2).
// The core's registry can still list a player that left (a kicked bot); the engine has the last
// word (EngineSlotState), so a stale entry is never picked.
bool PlayerBySlot(int slot, ru_player* out) {
  if (!practice::SlotEntryUsable(EngineSlotState(slot))) return false;
  struct Ctx {
    int slot;
    ru_player* out;
    bool found;
  } c{slot, out, false};
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        auto* c = static_cast<Ctx*>(u);
        if ((p->slot >= 0 ? p->slot : p->userid) != c->slot) return 1;
        if (!p->connected) return 1;
        std::memcpy(c->out, p, std::min<size_t>(sizeof(ru_player), p->struct_size));
        c->found = true;
        return 0;
      },
      &c);
  return c.found;
}

int TeamOfSlot(int slot) {
  ru_player p{};
  p.struct_size = sizeof(p);
  return PlayerBySlot(slot, &p) ? p.team : 0;
}

std::set<int> BotUserids() {
  std::set<int> out;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        if (p->is_bot && p->userid >= 0) static_cast<std::set<int>*>(u)->insert(p->userid);
        return 1;
      },
      &out);
  return out;
}

// ---- state (game thread only) ---------------------------------------------------------------------

std::string g_map;
std::map<uint64_t, std::map<std::string, Spot>> g_saved;  // player id -> name -> spot
std::unordered_map<uint64_t, Spot> g_lastThrow;          // grenade_thrown position
std::unordered_map<uint64_t, Spot> g_beforeTeleport;     // where .loadpos / .spawn took them from
std::unordered_set<int> g_noflash;                        // slots
std::unordered_set<int> g_god;                            // slots (m_bTakesDamage false)
bool g_buddhaOff = false;                                 // .god fallback toggled buddha off
bool g_touched = false;                                   // any of the above set this session
std::unordered_map<uint64_t, ThrowHistory> g_throws;      // player id -> grenade history (ME numbering)
std::vector<uint32_t> g_spawnBeams;                       // .showspawns beams (entity handles)
void RemoveSpawnBeams();                                  // .hidespawns; also on leaving practice

struct DelayedCmd {
  double due = 0;
  std::string cmd;  // a console command, or "" = rethrow `nade` with `slot` as the thrower
  Throw nade;
  int slot = -1;
};
std::vector<DelayedCmd> g_delayed;  // .delay'd rethrows

// Projectiles already looked at (CHandles): recorded into a history, or spawned by this plugin.
std::unordered_set<uint32_t> g_projSeen;
std::unordered_map<uint32_t, Launch> g_projOurs;  // debug=1: our rethrows, logged against what they got
int g_projHigh = 0;              // highest entity index seen by a scan this map (the next scan goes +2048)
int g_scanBurst = 0;             // ticks left of every-tick scanning after grenade_thrown
unsigned g_scanTick = 0;         // otherwise every 8th tick
double g_serverRethrowUntil = 0; // sv_rethrow_last_grenade's projectile is not a new throw

struct StopWatch {
  uint64_t id = 0;
  double start = 0;
  double nextDraw = 0;
};
std::unordered_map<int, StopWatch> g_timers;  // slot -> .timer

// Server-wide toggles (ru_api cannot read a cvar): the values prac.cfg sets, flipped by the commands.
struct Toggles {
  bool impacts = true;  // sv_showimpacts 1
  bool traj = true;     // sv_grenade_trajectory_prac_pipreview true
  int solid = 2;        // mp_solid_teammates 2
};
Toggles g_toggles;

bool g_dryRun = false;    // .dryrun round under way
bool g_dryArmed = false;  // its freeze time ended: the next round_end ends it

struct PendingBot {
  int callerSlot = -1;
  Vec3 pos;
  std::set<int> before;  // bot userids when the bot was requested
  double deadline = 0;
};
std::vector<PendingBot> g_pendingBots;
GrenadeDamage g_grenadeDamage;  // HE / fire damage summed per throw (feedback)

// Humans by SteamID64, bots by a pseudo id from their slot (never a real SteamID).
uint64_t PlayerId(int slot, uint64_t steamid64) { return steamid64 ? steamid64 : 0xB0B0000000000000ull | static_cast<uint32_t>(slot); }

void Cmd(const char* c) { g_api->server_command(g_api->self, c); }
void Cmd(const std::string& c) { Cmd(c.c_str()); }
void ChatAll(const std::string& msg) { g_api->chat_all(g_api->self, msg.c_str(), 0); }

void Reply(int slot, uint64_t steamid64, const std::string& msg) {
  Log("practice: slot %d: %s", slot, msg.c_str());
  if (steamid64 != 0 && slot >= 0) g_api->chat_to_slot(g_api->self, slot, (" \x04[ReadyUp]\x01 " + msg).c_str());
}

void ClearState(bool restorePawns) {
  if (restorePawns) {
    const Offsets& o = Off();
    for (int slot : g_god) {
      if (void* pawn = PawnForSlot(slot); pawn && o.takesDamage >= 0) Wr<bool>(pawn, o.takesDamage, true);
    }
    for (int slot : g_noflash) {
      if (void* pawn = PawnForSlot(slot); pawn && o.flashAlpha >= 0) {
        Wr<float>(pawn, o.flashAlpha, 255.f);
        g_api->entity_mark_changed(g_api->self, pawn);
      }
    }
  }
  g_saved.clear();
  g_lastThrow.clear();
  g_beforeTeleport.clear();
  g_noflash.clear();
  g_god.clear();
  g_pendingBots.clear();
  g_grenadeDamage.Clear();
  g_buddhaOff = false;
  g_touched = false;
  g_throws.clear();
  g_delayed.clear();
  g_projSeen.clear();
  g_projOurs.clear();
  g_scanBurst = 0;
  g_serverRethrowUntil = 0;
  g_timers.clear();
  g_toggles = Toggles{};
  g_dryRun = false;
  g_dryArmed = false;
  if (restorePawns) RemoveSpawnBeams();  // the entities of this map; a new map has none
  g_spawnBeams.clear();
}

// ---- mode ---------------------------------------------------------------------------------------

// Set when the home map's settings went in for this map load (practice_set home_cvars, below).
bool g_homeCvarsApplied = false;

bool Enter(const char** why) {
  if (MatchOwnsMode()) {
    if (Match()->set_practice(1) != 1) {
      if (why) {
        *why = RuMode() == "external" ? "another plugin's mode is on (deathmatch: .ru dm off first)"
                                      : "a match is loaded (.ru match end first)";
      }
      return false;
    }
  } else {
    g_standaloneActive = true;
  }
  g_manualOff = false;
  g_toggles = Toggles{};
  g_dryRun = g_dryArmed = false;
  Cmd("exec ReadyUp/prac.cfg");
  g_homeCvarsApplied = false;  // prac.cfg may set them back: the home map's settings again
  // Respawn everyone so they get prac.cfg's grenade set and weapons now, not on their next death.
  Cmd("mp_restartgame 1");
  Log("practice: on (%s)", MatchOwnsMode() ? "match plugin mode" : "standalone");
  return true;
}

void Leave() {
  if (MatchOwnsMode()) (void)Match()->set_practice(0);
  g_standaloneActive = false;
  Cmd("exec ReadyUp/idle.cfg");
  // Everyone respawns with the normal loadout: the practice grenades and rifles go.
  Cmd("mp_restartgame 1");
  ClearState(/*restorePawns=*/true);
  Log("practice: off");
}

// ---- .dryrun (ME) ------------------------------------------------------------------------------

// ME's defaults when no dryrun.cfg exists (ExecUnpracCommands + ExecDryRunCFG), without the
// match-only keys (halftime, overtime, timeouts, backups): one round with normal rules.
constexpr const char* kDryRunCvars[] = {
    // sv_cheats last: sv_infinite_ammo and buddha are cheat cvars.
    "sv_grenade_trajectory_prac_pipreview false; sv_grenade_trajectory_prac_trailtime 0; sv_showimpacts 0; "
    "sv_infinite_ammo 0; buddha 0; mp_teammates_are_enemies false; sv_cheats false",
    "mp_ct_default_grenades \"\"; mp_t_default_grenades \"\"; mp_ct_default_primary \"\"; mp_t_default_primary \"\"; "
    "mp_ct_default_secondary weapon_hkp2000; mp_t_default_secondary weapon_glock",
    "mp_death_drop_defuser 1; mp_death_drop_taser 1; mp_drop_knife_enable 0; mp_death_drop_grenade 2; mp_death_drop_gun 1; "
    "ammo_grenade_limit_default 1; ammo_grenade_limit_flashbang 2; ammo_grenade_limit_total 4; mp_defuser_allocation 0; "
    "mp_free_armor 0",
    "mp_buy_anywhere 0; mp_buytime 20; mp_freezetime 6; mp_roundtime 1.92; mp_roundtime_defuse 1.92; "
    "mp_roundtime_hostage 1.92; mp_startmoney 16000; mp_maxmoney 16000; mp_afterroundmoney 0",
    "mp_respawn_on_death_ct 0; mp_respawn_on_death_t 0; mp_solid_teammates 1; mp_ignore_round_win_conditions 0; "
    "mp_round_restart_delay 5; mp_c4timer 40; mp_give_player_c4 1; mp_friendlyfire 1; mp_forcecamera 1",
};

void StartDryRun() {
  // ME: bots kicked, bot placement and noflash dropped. God mode goes too (a normal round).
  Cmd("bot_kick");
  g_pendingBots.clear();
  const Offsets& o = Off();
  for (int s : g_god) {
    if (void* pawn = PawnForSlot(s); pawn && o.takesDamage >= 0) Wr<bool>(pawn, o.takesDamage, true);
  }
  for (int s : g_noflash) {
    if (void* pawn = PawnForSlot(s); pawn && o.flashAlpha >= 0) {
      Wr<float>(pawn, o.flashAlpha, 255.f);
      g_api->entity_mark_changed(g_api->self, pawn);
    }
  }
  g_god.clear();
  g_noflash.clear();
  g_buddhaOff = false;
  g_delayed.clear();
  for (const char* c : kDryRunCvars) Cmd(c);
  Cmd("exec ReadyUp/dryrun.cfg");  // optional overrides; a missing file only logs a console line
  Cmd("mp_restartgame 1");
  g_dryRun = true;
  g_dryArmed = false;
  g_touched = true;
  Log("practice: dry run started");
  ChatAll("Ready Up: dry run: one round with normal money, freeze time and round time. .dryrun or .prac goes back to "
          "practice.");
}

void EndDryRun(const char* why) {
  g_dryRun = g_dryArmed = false;
  g_toggles = Toggles{};
  Cmd("exec ReadyUp/prac.cfg");
  Cmd("mp_restartgame 1");
  Log("practice: dry run over (%s)", why);
  ChatAll(std::string("Ready Up: ") + why + ": back to practice.");
}

// ---- tools --------------------------------------------------------------------------------------

struct SpawnPt {
  int index;
  int priority;
  Vec3 pos;
};

std::vector<SpawnPt> ListSpawns(int team) {
  std::vector<SpawnPt> out;
  const Offsets& o = Off();
  const char* want = team == 3 ? "info_player_counterterrorist" : "info_player_terrorist";
  // Every entity slot: server-only entities (spawn points) sit above the 16384 networked ones.
  for (int i = 0; i < 32768; ++i) {
    void* e = g_api->entity_by_index(g_api->self, i);
    if (!e) continue;
    const char* cls = g_api->entity_classname(g_api->self, e);
    if (!cls || std::strcmp(cls, want) != 0) continue;
    if (o.spawnEnabled >= 0 && !Rd<bool>(e, o.spawnEnabled)) continue;
    SpawnPt s{i, o.spawnPriority >= 0 ? Rd<int32_t>(e, o.spawnPriority) : 0, {}};
    if (!OriginOf(e, &s.pos)) continue;
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const SpawnPt& x, const SpawnPt& y) {
    return x.priority != y.priority ? x.priority < y.priority : x.index < y.index;
  });
  return out;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ru_api 1.13 feature check ("change_team", "teleport", "entity_create"): the practice gamedata
// verified on this CS2 build.
bool EngineFeature(const char* f) {
  return RU_API_HAS(g_api, engine_feature_available) && g_api->engine_feature_available &&
         g_api->engine_feature_available(g_api->self, f) == 1;
}

bool g_viewSet = false;  // the last TeleportRemembering also set the view (AngleHint says nothing)

// With `ang` (pitch yaw roll) and ru_api 1.13 player_teleport: moved there looking that way and
// stopped (ME's Teleport(pos, angle, zero velocity)). Otherwise only moved (entity_set_abs_origin).
bool TeleportRemembering(int slot, uint64_t id, const Vec3& to, const Vec3f* ang = nullptr) {
  Spot here;
  const bool had = SpotOfSlot(slot, &here);
  g_viewSet = false;
  bool moved = false;
  if (ang && std::isfinite(ang->x) && std::isfinite(ang->y) && EngineFeature("teleport")) {
    const float xyz[3] = {to.x, to.y, to.z}, pyr[3] = {ang->x, ang->y, 0.f}, still[3] = {0.f, 0.f, 0.f};
    moved = g_viewSet = g_api->player_teleport(g_api->self, slot, xyz, pyr, still) == 1;
  }
  if (!moved && !Teleport(slot, to)) return false;
  if (had) g_beforeTeleport[id] = here;
  g_touched = true;
  return true;
}

// Refusal reply, or "" when tools may run.
std::string ToolsRefusal() {
  if (!IsActive()) return "only works in practice mode (.prac).";
  if (!ToolsAllowed(true, Ruleset())) return "not available under the valve ruleset.";
  return {};
}

// Without player_teleport (a build where the practice gamedata did not verify) the server cannot set
// the view: tell them the setang to type (sv_cheats is on in practice).
std::string AngleHint(const Vec3f& a) {
  if (g_viewSet) return ".";
  if (!std::isfinite(a.x) || !std::isfinite(a.y) || (a.x == 0 && a.y == 0)) return ".";
  char buf[96];
  std::snprintf(buf, sizeof(buf), " (view: setang %.2f %.2f 0 in your console).", a.x, a.y);
  return buf;
}

// ---- per-player rethrow ---------------------------------------------------------------------------

uint32_t SpawnTypeOf(const Throw& t) {
  if (t.kind == "smoke") return RU_GRENADE_SMOKE;
  if (t.kind == "flash") return RU_GRENADE_FLASH;
  if (t.kind == "hegrenade") return RU_GRENADE_HE;
  if (t.kind == "molotov") return t.launch.incendiary ? RU_GRENADE_INCENDIARY : RU_GRENADE_MOLOTOV;
  if (t.kind == "decoy") return RU_GRENADE_DECOY;
  return 0;
}

std::string KindName(const Throw& t) {
  if (t.kind == "hegrenade") return "HE grenade";
  if (t.kind == "molotov" && t.launch.incendiary) return "incendiary";
  return t.kind;
}

// "" when `t` can be thrown again, else why not.
std::string RethrowRefusal(const Throw& t) {
  if (!t.launch.set) return "its launch was not recorded (the projectile was not seen)";
  const uint32_t type = SpawnTypeOf(t);
  if (!type || !RU_API_HAS(g_api, grenade_spawn_available) || !g_api->grenade_spawn ||
      !g_api->grenade_spawn_available(g_api->self, type)) {
    return std::string(t.kind == "molotov" && t.launch.incendiary ? "spawning an " : "spawning a ") + KindName(t) +
           " is not available on this CS2 build";
  }
  return {};
}

// Throws `t` again from its recorded launch, `slot` as the thrower (kills, damage, blinds and the
// team are theirs; nobody's when they have no pawn any more). True when the projectile exists.
bool SpawnThrow(const Throw& t, int slot) {
  if (!RethrowRefusal(t).empty()) return false;
  ru_grenade_spawn s{};
  s.struct_size = sizeof(s);
  s.type = SpawnTypeOf(t);
  const Launch& l = t.launch;
  s.origin[0] = l.origin.x, s.origin[1] = l.origin.y, s.origin[2] = l.origin.z;
  s.velocity[0] = l.velocity.x, s.velocity[1] = l.velocity.y, s.velocity[2] = l.velocity.z;
  s.angles[0] = l.angles.x, s.angles[1] = l.angles.y, s.angles[2] = l.angles.z;
  s.angular_velocity[0] = 600.f;  // what the game's own throw passes (CS2 ignores it today)
  s.owner_slot = PawnForSlot(slot) ? slot : -1;
  void* e = g_api->grenade_spawn(g_api->self, &s);
  if (!e) return false;
  const uint32_t h = g_api->entity_handle_of(g_api->self, e);
  g_projSeen.insert(h);
  if (g_api->debug_enabled(g_api->self)) g_projOurs[h] = l;
  Log("practice: slot %d rethrows their %s from %s vel %s (thrower slot %d)", slot, KindName(t).c_str(),
      Fmt(Vec3{l.origin.x, l.origin.y, l.origin.z}).c_str(), Fmt(Vec3{l.velocity.x, l.velocity.y, l.velocity.z}).c_str(),
      s.owner_slot);
  g_touched = true;
  return true;
}

// Throws `t` (`what`: "your last smoke", "grenade no. 3") now, or after its .delay. False (and the
// reply said why) when it cannot be.
bool Rethrow(int slot, uint64_t steamid64, const Throw& t, const std::string& what) {
  if (const std::string why = RethrowRefusal(t); !why.empty()) {
    Reply(slot, steamid64, "Cannot rethrow " + what + ": " + why + ".");
    return false;
  }
  if (t.delay > 0) {
    DelayedCmd d;
    d.due = Now() + t.delay;
    d.nade = t;
    d.slot = slot;
    g_delayed.push_back(std::move(d));
    g_touched = true;
    Reply(slot, steamid64, "Rethrowing " + what + " in " + FormatTimerSeconds(t.delay) + "s.");
    return true;
  }
  if (!SpawnThrow(t, slot)) {
    Reply(slot, steamid64, "Cannot rethrow " + what + ": the server refused the grenade (see the server log).");
    return false;
  }
  Reply(slot, steamid64, "Rethrowing " + what + ".");
  return true;
}

// The old server-wide rethrow, for a throw whose launch is unknown or whose type cannot be spawned.
void ServerRethrow(int slot, uint64_t steamid64, float delay, const std::string& why) {
  const std::string note = "per-player rethrow unavailable (" + why + "), so rethrowing the last grenade thrown on the server";
  g_serverRethrowUntil = Now() + delay + 1.0;
  if (delay > 0) {
    DelayedCmd d;
    d.due = Now() + delay;
    d.cmd = "sv_rethrow_last_grenade";
    g_delayed.push_back(std::move(d));
    g_touched = true;
    return Reply(slot, steamid64, note + " in " + FormatTimerSeconds(delay) + "s.");
  }
  Cmd("sv_rethrow_last_grenade");
  Reply(slot, steamid64, note + ".");
}

// ---- projectiles -> launch data in the history ------------------------------------------------------

bool NonZero(const Vec3& v) { return v.x != 0.f || v.y != 0.f || v.z != 0.f; }

// How projectile `e` left the hand. The m_vInitial* fields are set when the projectile is created
// (the client draws its trajectory from them), so they hold however late the scan sees it; without
// them the projectile's current origin / velocity (a tick or so into the flight).
Launch ReadLaunch(void* e, const std::string& kind) {
  const Offsets& o = Off();
  Launch l;
  Vec3 origin, vel, ang;
  const Vec3 initPos = o.initPos >= 0 ? Rd<Vec3>(e, o.initPos) : Vec3{};
  if (Finite(initPos) && NonZero(initPos)) origin = initPos;
  else if (!OriginOf(e, &origin)) return l;
  const Vec3 initVel = o.initVel >= 0 ? Rd<Vec3>(e, o.initVel) : Vec3{};
  if (Finite(initVel) && NonZero(initVel)) vel = initVel;
  else if (o.absVelocity >= 0) vel = Rd<Vec3>(e, o.absVelocity);
  if (!Finite(vel)) return l;
  if (o.absRotation >= 0 && o.bodyComponent >= 0 && o.sceneNode >= 0) {
    if (void* body = Rd<void*>(e, o.bodyComponent)) {
      if (void* node = Rd<void*>(body, o.sceneNode)) ang = Rd<Vec3>(node, o.absRotation);
    }
    if (!Finite(ang)) ang = Vec3{};
  }
  l.set = true;
  l.origin = {origin.x, origin.y, origin.z};
  l.velocity = {vel.x, vel.y, vel.z};
  l.angles = {ang.x, ang.y, ang.z};
  l.incendiary = kind == "molotov" && o.isInc >= 0 && Rd<bool>(e, o.isInc);
  return l;
}

// A player's projectile: the launch of their grenade_thrown entry, or (nobody threw it: a bot's
// hello_nade, a plugin's grenade_spawn) a history entry of its own.
void RecordLaunch(int slot, const std::string& kind, const Launch& l, double now) {
  ru_player p{};
  p.struct_size = sizeof(p);
  const uint64_t id = PlayerId(slot, PlayerBySlot(slot, &p) && !p.is_bot ? p.steamid64 : 0);
  ThrowHistory& h = g_throws[id];
  const Offsets& o = Off();
  Throw* awaiting = h.AwaitingLaunch(kind, now - 3.0);
  if (awaiting) {
    const bool incFromEvent = awaiting->launch.incendiary;
    awaiting->launch = l;
    if (o.isInc < 0) awaiting->launch.incendiary = incFromEvent;  // no schema field: the event's weapon said
  } else {
    if (now < g_serverRethrowUntil) return;  // sv_rethrow_last_grenade's copy of an older throw
    Throw t;
    Spot s;
    if (SpotOfSlot(slot, &s)) {
      t.pos = {s.pos.x, s.pos.y, s.pos.z};
      t.ang = {s.ang.x, s.ang.y, s.ang.z};
    } else {
      t.pos = l.origin;
    }
    t.kind = kind;
    t.at = now;
    t.launch = l;
    h.Add(t);
    g_touched = true;
  }
  if (g_api->debug_enabled(g_api->self)) {
    Log("practice: slot %d %s launch %s vel %s (%s, history %zu)", slot, kind.c_str(),
        Fmt(Vec3{l.origin.x, l.origin.y, l.origin.z}).c_str(), Fmt(Vec3{l.velocity.x, l.velocity.y, l.velocity.z}).c_str(),
        awaiting ? "its grenade_thrown" : "no grenade_thrown: entry of its own", h.Count());
  }
}

bool EndsWith(const char* s, const char* suffix) {
  const size_t n = std::strlen(s), m = std::strlen(suffix);
  return n >= m && std::strcmp(s + n - m, suffix) == 0;
}

// Every grenade projectile a player owns that no scan has seen yet. m_hThrower may be set a tick
// after the projectile appears: those are looked at again on the next scan.
void ScanProjectiles(double now) {
  const Offsets& o = Off();
  if (o.thrower < 0 || o.ctrlPawn < 0) return;
  std::unordered_map<uint32_t, int> pawns;  // pawn CHandle -> slot
  for (int slot = 0; slot < 64; ++slot) {
    void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
    if (!ctrl) continue;
    const char* cls = g_api->entity_classname(g_api->self, ctrl);
    if (cls && std::strcmp(cls, "cs_player_controller") == 0) pawns[Rd<uint32_t>(ctrl, o.ctrlPawn)] = slot;
  }
  if (g_projSeen.size() > 256) {
    for (auto it = g_projSeen.begin(); it != g_projSeen.end();) {
      it = g_api->entity_from_handle(g_api->self, *it) ? std::next(it) : g_projSeen.erase(it);
    }
  }
  // Projectiles are networked (index < 16384). The first scan of a map looks at every index, later
  // ones up to the highest index seen + 2048.
  const bool full = g_projHigh == 0;
  const auto t0 = std::chrono::steady_clock::now();
  int entities = 0;
  const int last = full ? 16383 : std::min(16383, g_projHigh + 2048);
  for (int i = 65; i <= last; ++i) {
    void* e = g_api->entity_by_index(g_api->self, i);
    if (!e) continue;
    ++entities;
    if (i > g_projHigh) g_projHigh = i;
    const char* cls = g_api->entity_classname(g_api->self, e);
    if (!cls || !EndsWith(cls, "_projectile")) continue;
    const std::string kind = ProjectileKind(cls);
    if (kind.empty()) continue;
    const uint32_t h = g_api->entity_handle_of(g_api->self, e);
    if (h == 0xFFFFFFFFu) continue;
    const auto pawn = pawns.find(Rd<uint32_t>(e, o.thrower));
    if (auto ours = g_projOurs.find(h); ours != g_projOurs.end()) {
      // debug=1: what one of our rethrows got (once its thrower is set), next to what it was given.
      if (pawn == pawns.end()) continue;
      const Launch got = ReadLaunch(e, kind);
      const Launch& want = ours->second;
      Log("practice: rethrown %s: given %s vel %s, got %s vel %s, thrower slot %d", kind.c_str(),
          Fmt(Vec3{want.origin.x, want.origin.y, want.origin.z}).c_str(),
          Fmt(Vec3{want.velocity.x, want.velocity.y, want.velocity.z}).c_str(),
          Fmt(Vec3{got.origin.x, got.origin.y, got.origin.z}).c_str(),
          Fmt(Vec3{got.velocity.x, got.velocity.y, got.velocity.z}).c_str(), pawn->second);
      g_projOurs.erase(ours);
      continue;
    }
    if (g_projSeen.count(h) || pawn == pawns.end()) continue;
    g_projSeen.insert(h);
    const Launch l = ReadLaunch(e, kind);
    if (l.set) RecordLaunch(pawn->second, kind, l, now);
  }
  if (full && g_api->debug_enabled(g_api->self)) {
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    Log("practice: projectile scan: %d entities up to index %d in %.0f us", entities, g_projHigh, us);
  }
}

// .last .lastindex .delay .throwidx and the typed rethrows. True when `cmd` was one of them.
bool RunHistoryTool(int slot, uint64_t steamid64, uint64_t id, const std::string& cmd,
                    const std::vector<std::string>& args) {
  ThrowHistory* h = g_throws.count(id) ? &g_throws[id] : nullptr;
  const size_t count = h ? h->Count() : 0;
  if (cmd == ".last") {
    const Throw* t = h ? h->Last() : nullptr;
    if (!t) return Reply(slot, steamid64, "You have not thrown any nade yet!"), true;
    if (!TeleportRemembering(slot, id, Vec3{t->pos.x, t->pos.y, t->pos.z}, &t->ang)) return Reply(slot, steamid64, kNoTeleport), true;
    Reply(slot, steamid64, "Teleported to your last grenade throw" + AngleHint(t->ang));
    return true;
  }
  if (cmd == ".lastindex") {
    if (!count) return Reply(slot, steamid64, "You have not thrown any nade yet!"), true;
    Reply(slot, steamid64, "Index of last thrown grenade: " + std::to_string(count));
    return true;
  }
  if (cmd == ".delay") {
    float secs = 0;
    if (args.size() < 2) return Reply(slot, steamid64, "Usage: .delay <delay_in_seconds>"), true;
    if (!ParseDelaySeconds(args[1], &secs)) {
      return Reply(slot, steamid64, "Usage: .delay <delay_in_seconds> (more than 0, at most 60)"), true;
    }
    if (!count) return Reply(slot, steamid64, "You have not thrown any nade yet!"), true;
    h->Last()->delay = secs;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Delay of %.2fs set for grenade of index: %zu.", secs, count);
    Reply(slot, steamid64, buf);
    return true;
  }
  if (cmd == ".throwidx" || cmd == ".throwindex") {
    if (args.size() < 2) {
      Reply(slot, steamid64,
            "Usage: .throwindex <number> (You've thrown " + std::to_string(count) + " grenades till now)");
      return true;
    }
    for (size_t i = 1; i < args.size(); ++i) {
      int n = 0;
      if (!ParsePositiveInt(args[i], &n)) {
        Reply(slot, steamid64, args[i] + " is not a valid non-negative number for .throwindex command.");
        continue;
      }
      if (!count) return Reply(slot, steamid64, "You have not thrown any nade yet!"), true;
      const Throw* t = h->At(n);
      if (!t) {
        Reply(slot, steamid64, "Your grenade history only goes from 1 to " + std::to_string(count) + "!");
        continue;
      }
      // ME throws every index given, each after its own .delay.
      (void)Rethrow(slot, steamid64, *t, "grenade no. " + std::to_string(n) + " (" + KindName(*t) + ")");
    }
    return true;
  }
  if (const std::string kind = TypedRethrowKind(cmd); !kind.empty()) {
    const Throw* t = h ? h->LastOfKind(kind) : nullptr;
    if (!t) return Reply(slot, steamid64, "You have not thrown any " + kind + " yet!"), true;
    (void)Rethrow(slot, steamid64, *t, "your last " + KindName(*t));
    return true;
  }
  return false;
}

// .impacts .traj/.pip .solid .break .timer. True when `cmd` was one of them.
bool RunToggleTool(int slot, uint64_t steamid64, const std::string& cmd) {
  if (cmd == ".impacts") {
    g_toggles.impacts = !g_toggles.impacts;
    Cmd(g_toggles.impacts ? "sv_showimpacts 1" : "sv_showimpacts 0");
    g_touched = true;
    Reply(slot, steamid64, std::string("sv_showimpacts is now set to ") + (g_toggles.impacts ? "1" : "0"));
    return true;
  }
  if (cmd == ".traj" || cmd == ".pip") {
    g_toggles.traj = !g_toggles.traj;
    Cmd(g_toggles.traj ? "sv_grenade_trajectory_prac_pipreview true" : "sv_grenade_trajectory_prac_pipreview false");
    g_touched = true;
    Reply(slot, steamid64,
          std::string("sv_grenade_trajectory_prac_pipreview is now set to ") + (g_toggles.traj ? "true" : "false"));
    return true;
  }
  if (cmd == ".solid") {
    g_toggles.solid = NextSolidValue(g_toggles.solid);
    Cmd("mp_solid_teammates " + std::to_string(g_toggles.solid));
    g_touched = true;
    Reply(slot, steamid64, "mp_solid_teammates is now set to " + std::to_string(g_toggles.solid));
    return true;
  }
  if (cmd == ".break") {
    if (!RU_API_HAS(g_api, entity_remove) || !g_api->entity_remove) {
      return Reply(slot, steamid64, "cannot break anything: entity removal is unavailable on this server build."), true;
    }
    // func_breakable(_surf), and the prop_dynamic ones a Break input would break: health > 0 and
    // taking damage (Mirage's vents and windows; decorative props have health 0).
    const Offsets& o = Off();
    int n = 0;
    for (int i = 65; i < 32768; ++i) {
      void* e = g_api->entity_by_index(g_api->self, i);
      if (!e) continue;
      const char* cls = g_api->entity_classname(g_api->self, e);
      if (!cls) continue;
      bool breakable = std::strcmp(cls, "func_breakable") == 0 || std::strcmp(cls, "func_breakable_surf") == 0;
      if (!breakable && std::strcmp(cls, "prop_dynamic") == 0 && o.health >= 0 && o.takesDamage >= 0) {
        breakable = Rd<int32_t>(e, o.health) > 0 && Rd<bool>(e, o.takesDamage);
      }
      if (breakable) n += g_api->entity_remove(g_api->self, e) == 1 ? 1 : 0;
    }
    Log("practice: .break removed %d entities", n);
    Reply(slot, steamid64, n ? "Broke " + std::to_string(n) + " breakables (windows, vents, ...)." : "Nothing left to break.");
    return true;
  }
  if (cmd == ".timer") {
    const uint64_t id = PlayerId(slot, steamid64);
    auto it = g_timers.find(slot);
    if (it != g_timers.end() && it->second.id == id) {
      const std::string res = FormatTimerSeconds(Now() - it->second.start);
      g_timers.erase(it);
      if (steamid64) {
        const std::string html = "<font color='#ffffff'>Timer: " + res + "s</font>";
        if (RU_API_HAS(g_api, center_html_to_slot_prio) && g_api->center_html_to_slot_prio) {
          (void)g_api->center_html_to_slot_prio(g_api->self, slot, html.c_str(), 3, RU_HTML_PRIO_NOTICE);
        }
      }
      Reply(slot, steamid64, "Timer stopped! Result: " + res + "s");
      return true;
    }
    g_timers[slot] = StopWatch{id, Now(), 0};
    g_touched = true;
    Reply(slot, steamid64, "Timer started! Use .timer to stop it.");
    return true;
  }
  return false;
}

// The .timer panels, ~10 times a second (ME: every 0.1 s).
void DrawTimers(double now) {
  if (g_timers.empty() || !RU_API_HAS(g_api, center_html_to_slot_prio) || !g_api->center_html_to_slot_prio) return;
  for (auto it = g_timers.begin(); it != g_timers.end();) {
    ru_player p{};
    p.struct_size = sizeof(p);
    if (!PlayerBySlot(it->first, &p) || PlayerId(it->first, p.is_bot ? 0 : p.steamid64) != it->second.id) {
      it = g_timers.erase(it);  // left, or the slot is someone else now
      continue;
    }
    if (now >= it->second.nextDraw && !p.is_bot) {
      it->second.nextDraw = now + 0.1;
      const std::string html = "<font color='#ffffff'>Timer: " + FormatTimerSeconds(Now() - it->second.start) + "s</font>";
      (void)g_api->center_html_to_slot_prio(g_api->self, it->first, html.c_str(), 1, RU_HTML_PRIO_NOTICE);
    }
    ++it;
  }
}

// The competitive spawns of `team` (ME: the lowest priority among the enabled ones).
std::vector<SpawnPt> CompetitiveSpawns(int team) {
  auto all = ListSpawns(team);
  if (all.empty()) return all;
  const int minPrio = all.front().priority;  // ListSpawns sorts by priority
  std::vector<SpawnPt> out;
  for (const auto& s : all) {
    if (s.priority == minPrio) out.push_back(s);
  }
  return out;
}

void RemoveSpawnBeams() {
  if (!g_spawnBeams.empty() && RU_API_HAS(g_api, entity_remove) && g_api->entity_remove) {
    for (uint32_t h : g_spawnBeams) {
      if (void* e = g_api->entity_from_handle(g_api->self, h)) (void)g_api->entity_remove(g_api->self, e);
    }
  }
  g_spawnBeams.clear();
}

struct Rgba {
  uint8_t r, g, b, a;
};

// ME's ShowSpawnBeam: a 5 wide beam from the spawn point 100 units up, "dying" life state (so it is
// not a live entity for anything that counts them), in the team's color.
bool SpawnBeam(const Vec3& at, Rgba color) {
  const Offsets& o = Off();
  if (o.beamWidth < 0 || o.beamEnd < 0 || o.renderColor < 0 || o.lifeState < 0) return false;
  void* beam = g_api->entity_create(g_api->self, "beam");
  if (!beam) return false;
  Wr<uint8_t>(beam, o.lifeState, 1);
  Wr<float>(beam, o.beamWidth, 5.f);
  Wr<Rgba>(beam, o.renderColor, color);
  Wr<Vec3>(beam, o.beamEnd, Vec3{at.x, at.y, at.z + 100.f});
  const float xyz[3] = {at.x, at.y, at.z};
  (void)g_api->entity_set_abs_origin(g_api->self, beam, xyz);
  if (g_api->entity_spawn(g_api->self, beam) != 1) {
    (void)g_api->entity_remove(g_api->self, beam);
    return false;
  }
  g_spawnBeams.push_back(g_api->entity_handle_of(g_api->self, beam));
  return true;
}

// .showspawns / .hidespawns (ME). True when `cmd` was one of them.
bool RunSpawnMarkers(int slot, uint64_t steamid64, const std::string& cmd) {
  if (cmd != ".showspawns" && cmd != ".hidespawns") return false;
  const bool had = !g_spawnBeams.empty();
  RemoveSpawnBeams();
  if (cmd == ".hidespawns") return Reply(slot, steamid64, had ? "Spawn markers hidden." : "No spawn markers shown."), true;
  const auto ct = CompetitiveSpawns(3), t = CompetitiveSpawns(2);
  if (!EngineFeature("entity_create")) {
    Reply(slot, steamid64, "spawn markers are not available on this CS2 build (entity creation did not verify). This map has " +
                               std::to_string(ct.size()) + " CT and " + std::to_string(t.size()) +
                               " competitive T spawns: .ctspawn N / .tspawn N / .bestspawn go there.");
    return true;
  }
  int n = 0;
  for (const auto& s : ct) n += SpawnBeam(s.pos, Rgba{0, 0, 255, 255}) ? 1 : 0;
  for (const auto& s : t) n += SpawnBeam(s.pos, Rgba{255, 165, 0, 255}) ? 1 : 0;
  g_touched = true;
  Log("practice: .showspawns: %d beams (%zu CT, %zu T spawns)", n, ct.size(), t.size());
  if (n == 0) return Reply(slot, steamid64, "could not place spawn markers (see the server log)."), true;
  Reply(slot, steamid64, "Showing " + std::to_string(ct.size()) + " CT (blue) and " + std::to_string(t.size()) +
                             " T (orange) spawns. .hidespawns hides them.");
  return true;
}

// .fas / .watchme (ME): every other human to the spectators (bots stay, like ME).
bool RunWatchMe(int slot, uint64_t steamid64, const std::string& cmd) {
  if (cmd != ".fas" && cmd != ".watchme") return false;
  if (!EngineFeature("change_team")) {
    return Reply(slot, steamid64, "moving players is not available on this CS2 build (team change did not verify)."), true;
  }
  struct Ctx {
    int self;
    std::vector<int> slots;
  } c{slot, {}};
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        auto* c = static_cast<Ctx*>(u);
        if (!p->is_bot && p->slot >= 0 && p->slot != c->self && p->team != RU_TEAM_SPECTATOR) c->slots.push_back(p->slot);
        return 1;
      },
      &c);
  int moved = 0;
  for (int s : c.slots) moved += g_api->player_change_team(g_api->self, s, RU_TEAM_SPECTATOR) == 1 ? 1 : 0;
  Log("practice: %s by slot %d: %d of %zu players to the spectators", cmd.c_str(), slot, moved, c.slots.size());
  Reply(slot, steamid64, moved ? "Moved " + std::to_string(moved) + " player" + (moved == 1 ? "" : "s") + " to the spectators."
                               : "Nobody else to move.");
  return true;
}

// .bestspawn .worstspawn (+ct / t). True when `cmd` was one of them.
bool RunSpawnTool(int slot, uint64_t steamid64, uint64_t id, const std::string& cmd) {
  if (RunSpawnMarkers(slot, steamid64, cmd)) return true;
  if (RunWatchMe(slot, steamid64, cmd)) return true;
  const bool best = cmd.rfind(".best", 0) == 0;
  if (!best && cmd.rfind(".worst", 0) != 0) return false;
  const std::string rest = cmd.substr(best ? 5 : 6);
  if (rest != "spawn" && rest != "ctspawn" && rest != "tspawn") return false;
  const int team = rest == "ctspawn" ? 3 : rest == "tspawn" ? 2 : TeamOfSlot(slot);
  if (team != 2 && team != 3) return Reply(slot, steamid64, "join CT or T first (or use .bestctspawn / .besttspawn)."), true;
  const char* side = team == 3 ? "CT" : "T";
  Spot here;
  if (!SpotOfSlot(slot, &here)) return Reply(slot, steamid64, "cannot read your position (are you alive?)."), true;
  const auto spawns = CompetitiveSpawns(team);
  if (spawns.empty()) return Reply(slot, steamid64, std::string("no ") + side + " spawn points found on this map."), true;
  std::vector<Vec3f> pts;
  for (const auto& s : spawns) pts.push_back({s.pos.x, s.pos.y, s.pos.z});
  const Vec3f from{here.pos.x, here.pos.y, here.pos.z};
  const int i = best ? ClosestIndex(pts, from) : FarthestIndex(pts, from);
  const Vec3 to = spawns[static_cast<size_t>(i)].pos;
  if (!TeleportRemembering(slot, id, to)) return Reply(slot, steamid64, kNoTeleport), true;
  Reply(slot, steamid64, std::string(best ? "closest " : "farthest ") + side + " spawn (" + std::to_string(i + 1) + "/" +
                             std::to_string(spawns.size()) + ", " + Fmt(to) + ").");
  return true;
}

// ---- lineup library (ME .savenade ...; practice_lineups.h) -----------------------------------------

LineupBook g_lineups;
std::string g_lineupsMap;    // map whose file g_lineups holds ("" = not read yet)
bool g_globalNades = false;  // .globalnades (admin): save / delete the global lineups

std::string LineupsDir() {
  const char* d = g_api->data_dir(g_api->self);
  return std::string(d ? d : ".") + "/lineups";
}

std::string CurrentMap() {
  const char* m = RU_API_HAS(g_api, current_map) && g_api->current_map ? g_api->current_map(g_api->self) : nullptr;
  return m && *m ? m : "unknown";
}

std::string LineupsPath(const std::string& map) { return LineupsDir() + "/" + MapFileStem(map) + ".json"; }

// The book of the current map, read on first use per map.
LineupBook& Lineups() {
  const std::string map = CurrentMap();
  if (g_lineupsMap == map) return g_lineups;
  g_lineupsMap = map;
  g_lineups = LineupBook{};
  readyup::json_store::Json doc;
  std::string note;
  const auto r = readyup::json_store::Load(LineupsPath(map), 1, &doc, &note);
  if (r == readyup::json_store::LoadResult::Ok) {
    int skipped = 0;
    g_lineups.FromJson(doc, &skipped);
    Log("practice: lineups: %zu on %s%s", g_lineups.Count(), map.c_str(),
        skipped ? (" (" + std::to_string(skipped) + " malformed entries skipped)").c_str() : "");
  } else if (r == readyup::json_store::LoadResult::Corrupt) {
    Log("practice: lineups: %s", note.c_str());
  }
  return g_lineups;
}

// Lineup files are written off the game thread (json_store fsyncs the file and the directory,
// which took 150 ms once on the test box): one writer thread, files in the order they changed.
class LineupWriter {
 public:
  void Start() {
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
  }
  // Writes what is queued, then joins (unload: the thread must be gone before the image closes).
  void Stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void Queue(std::string dir, std::string path, readyup::json_store::Json doc) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      queue_.push_back(Job{std::move(dir), std::move(path), std::move(doc)});
    }
    cv_.notify_all();
  }

 private:
  struct Job {
    std::string dir, path;
    readyup::json_store::Json doc;
  };
  void Run() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) return;  // stop_ and drained
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      std::string err;
      if (!readyup::json_store::MakeDirs(job.dir, &err) || !readyup::json_store::Save(job.path, job.doc, 1, &err)) {
        Log("practice: lineups: cannot write %s: %s", job.path.c_str(), err.c_str());
      }
    }
  }
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool stop_ = false;
};
// Created at load and deleted at unload after Stop(). A process that exits without unloading the
// plugin never runs its destructor: destroying a condition variable the thread still waits on
// blocks forever in glibc (the process would hang on exit).
LineupWriter* g_lineupWriter = nullptr;

void SaveLineups() {
  if (g_lineupWriter) g_lineupWriter->Queue(LineupsDir(), LineupsPath(g_lineupsMap), g_lineups.ToJson());
}

// ME keys lineups by SteamID64; bots (ru practice as) by their pseudo id.
std::string OwnerKey(uint64_t id) { return std::to_string(id); }

std::string JoinFrom(const std::vector<std::string>& args, size_t from) {
  std::string out;
  for (size_t i = from; i < args.size(); ++i) out += (out.empty() ? "" : " ") + args[i];
  return out;
}

// True when `cmd` was a lineup command.
bool RunLineupTool(int slot, uint64_t steamid64, uint64_t id, const std::string& cmd,
                   const std::vector<std::string>& args) {
  const bool save = cmd == ".savenade" || cmd == ".sn";
  const bool load = cmd == ".loadnade" || cmd == ".ln";
  const bool list = cmd == ".listnades" || cmd == ".lin";
  const bool del = cmd == ".deletenade" || cmd == ".delnade" || cmd == ".dn";
  const bool imp = cmd == ".importnade" || cmd == ".in";
  if (!save && !load && !list && !del && !imp) return false;
  LineupBook& book = Lineups();
  const std::string mine = OwnerKey(id);
  const std::string arg1 = args.size() > 1 ? args[1] : std::string();
  constexpr const char* kBadName = "lineup names are 1-32 letters, digits, _ - or .";

  if (save) {
    if (arg1.empty()) return Reply(slot, steamid64, "Usage: .savenade <name> [description]"), true;
    if (!ValidLineupName(arg1)) return Reply(slot, steamid64, kBadName), true;
    Spot s;
    if (!SpotOfSlot(slot, &s)) return Reply(slot, steamid64, "cannot read your position (are you alive?)."), true;
    Lineup l;
    l.name = arg1;
    l.pos = {s.pos.x, s.pos.y, s.pos.z};
    l.ang = {s.ang.x, s.ang.y, s.ang.z};
    if (auto h = g_throws.find(id); h != g_throws.end() && h->second.Last()) l.kind = h->second.Last()->kind;
    l.desc = JoinFrom(args, 2).substr(0, 128);
    const std::string owner = g_globalNades ? std::string(kGlobalOwner) : mine;
    if (!book.Add(owner, l)) {
      return Reply(slot, steamid64, "Lineup already exists! Please use a different name or use .delnade <nade>"), true;
    }
    SaveLineups();
    Reply(slot, steamid64, "Lineup " + l.name + " saved successfully!" + (g_globalNades ? " (global)" : "") +
                               " Code: " + ExportCode(l));
    return true;
  }
  if (load) {
    if (arg1.empty()) return Reply(slot, steamid64, "Nade not found! Usage: .loadnade <name>"), true;
    std::string from;
    const Lineup* l = book.Load(mine, JoinFrom(args, 1), &from);
    if (!l) return Reply(slot, steamid64, "Nade " + arg1 + " not found!"), true;
    const Lineup copy = *l;
    if (!TeleportRemembering(slot, id, Vec3{copy.pos.x, copy.pos.y, copy.pos.z}, &copy.ang)) {
      return Reply(slot, steamid64, kNoTeleport), true;
    }
    Reply(slot, steamid64, "Lineup " + copy.name + " loaded successfully" + (copy.kind.empty() ? "" : " [" + copy.kind + "]") +
                               AngleHint(copy.ang) + (copy.kind.empty() ? "" : " Take your " + copy.kind + "."));
    if (!copy.desc.empty()) Reply(slot, steamid64, "Description: " + copy.desc);
    return true;
  }
  if (list) {
    const std::string filter = arg1;
    Reply(slot, steamid64, "-----All Saved Lineups for " + g_lineupsMap + "-----");
    int shown = 0;
    for (const std::string& owner : {std::string(kGlobalOwner), mine}) {
      for (const Lineup* l : book.List(owner, filter)) {
        if (++shown > 40) break;
        Reply(slot, steamid64, "[" + (l->kind.empty() ? std::string("?") : l->kind) + "] .loadnade " + l->name +
                                   (owner == kGlobalOwner ? " (global)" : ""));
      }
    }
    if (shown == 0) Reply(slot, steamid64, "No saved lineups" + (filter.empty() ? "" : " matching \"" + filter + "\"") + ".");
    if (shown > 40) Reply(slot, steamid64, "... more; add a filter: .listnades <filter>");
    return true;
  }
  if (del) {
    if (arg1.empty()) return Reply(slot, steamid64, "Usage: .delnade <name>"), true;
    const std::string owner = g_globalNades ? std::string(kGlobalOwner) : mine;
    if (!book.Remove(owner, arg1)) return Reply(slot, steamid64, "Lineup " + arg1 + " not found!"), true;
    SaveLineups();
    Reply(slot, steamid64, "Lineup " + arg1 + " deleted successfully.");
    return true;
  }
  // .importnade <name> x y z pitch yaw roll (ME: always into the caller's own lineups)
  if (arg1.empty()) return Reply(slot, steamid64, "Usage: .importnade <code>"), true;
  Lineup l;
  if (!ParseImportCode(JoinFrom(args, 1), &l.name, &l.pos, &l.ang)) {
    return Reply(slot, steamid64, "Invalid code format. Please provide a valid code with name, pos, and ang."), true;
  }
  if (!book.Add(mine, l)) {
    return Reply(slot, steamid64, "Lineup " + l.name + " already exists! Please use a different name or use .delnade <nade>"),
           true;
  }
  SaveLineups();
  Reply(slot, steamid64, "Lineup " + l.name + " imported and saved successfully.");
  return true;
}

void RunTool(int slot, uint64_t steamid64, const std::vector<std::string>& args) {
  const std::string cmd = Lower(args[0]);
  if (const std::string why = ToolsRefusal(); !why.empty()) return Reply(slot, steamid64, cmd + " " + why);
  const uint64_t id = PlayerId(slot, steamid64);
  const std::string name = args.size() > 1 ? args[1].substr(0, 32) : std::string("default");

  if (cmd == ".rethrow" || cmd == ".rt" || cmd == ".throw") {
    auto h = g_throws.find(id);
    const Throw* t = h != g_throws.end() ? h->second.Last() : nullptr;
    if (!t) return Reply(slot, steamid64, "You have not thrown any nade yet!");
    if (const std::string why = RethrowRefusal(*t); !why.empty()) return ServerRethrow(slot, steamid64, t->delay, why);
    (void)Rethrow(slot, steamid64, *t, "your last " + KindName(*t));
    return;
  }
  if (RunHistoryTool(slot, steamid64, id, cmd, args)) return;
  if (RunToggleTool(slot, steamid64, cmd)) return;
  if (RunSpawnTool(slot, steamid64, id, cmd)) return;
  if (RunLineupTool(slot, steamid64, id, cmd, args)) return;
  if (cmd == ".clear") {
    // ent_remove_all works from the server console; ent_fire needs a client of its own there.
    for (const char* c : {"ent_remove_all smokegrenade_projectile", "ent_remove_all molotov_projectile",
                          "ent_remove_all inferno", "ent_remove_all decoy_projectile"}) {
      Cmd(c);
    }
    return Reply(slot, steamid64, "smokes, molotovs and decoys cleared.");
  }
  if (cmd == ".savepos") {
    Spot s;
    if (!SpotOfSlot(slot, &s)) return Reply(slot, steamid64, "cannot read your position (are you alive?).");
    g_saved[id][name] = s;
    g_touched = true;
    return Reply(slot, steamid64, "position \"" + name + "\" saved (" + Fmt(s.pos) + ").");
  }
  if (cmd == ".loadpos") {
    auto pit = g_saved.find(id);
    if (pit == g_saved.end() || !pit->second.count(name)) {
      std::string have;
      if (pit != g_saved.end()) {
        for (const auto& kv : pit->second) have += (have.empty() ? "" : ", ") + kv.first;
      }
      return Reply(slot, steamid64,
                   "no saved position \"" + name + "\"" + (have.empty() ? " (.savepos [name] first)." : " (saved: " + have + ")."));
    }
    const Spot s = pit->second[name];
    const Vec3f view{s.ang.x, s.ang.y, s.ang.z};
    if (!TeleportRemembering(slot, id, s.pos, &view)) return Reply(slot, steamid64, kNoTeleport);
    return Reply(slot, steamid64, "loaded position \"" + name + "\" (" + Fmt(s.pos) +
                                      (g_viewSet ? ")." : "; view angle not restored)."));
  }
  if (cmd == ".back" && args.size() > 1) {
    // ME: .back N = to throw N of your grenade history.
    int n = 0;
    if (!ParsePositiveInt(args[1], &n)) {
      return Reply(slot, steamid64,
                   "Invalid value for .back command. Please specify a valid non-negative number. Usage: .back <number>");
    }
    const ThrowHistory* h = g_throws.count(id) ? &g_throws[id] : nullptr;
    if (!h || h->Count() == 0) return Reply(slot, steamid64, "You have not thrown any nade yet!");
    const Throw* t = h->At(n);
    if (!t) return Reply(slot, steamid64, "Your grenade history only goes from 1 to " + std::to_string(h->Count()) + "!");
    if (!TeleportRemembering(slot, id, Vec3{t->pos.x, t->pos.y, t->pos.z}, &t->ang)) return Reply(slot, steamid64, kNoTeleport);
    return Reply(slot, steamid64, "Teleported to grenade of history position: " + std::to_string(n) + "/" +
                                      std::to_string(h->Count()) + AngleHint(t->ang));
  }
  if (cmd == ".back") {
    const Spot* s = nullptr;
    const char* what = "";
    if (auto t = g_lastThrow.find(id); t != g_lastThrow.end()) {
      s = &t->second;
      what = "your last grenade throw";
    } else if (auto b = g_beforeTeleport.find(id); b != g_beforeTeleport.end()) {
      s = &b->second;
      what = "where you were before the last teleport";
    }
    if (!s) return Reply(slot, steamid64, "nothing to go back to (throw a grenade or use .loadpos / .spawn first).");
    const Vec3 to = s->pos;
    if (!Teleport(slot, to)) return Reply(slot, steamid64, kNoTeleport);
    return Reply(slot, steamid64, std::string("back to ") + what + " (" + Fmt(to) + ").");
  }
  if (cmd == ".noflash") {
    const Offsets& o = Off();
    void* pawn = PawnForSlot(slot);
    if (o.flashAlpha < 0 || !pawn) return Reply(slot, steamid64, "noflash is unavailable (pawn / schema field not found).");
    const bool on = !g_noflash.count(slot);
    if (on) g_noflash.insert(slot);
    else g_noflash.erase(slot);
    Wr<float>(pawn, o.flashAlpha, on ? 0.f : 255.f);
    g_api->entity_mark_changed(g_api->self, pawn);
    g_touched = true;
    return Reply(slot, steamid64, on ? "noflash on." : "noflash off.");
  }
  if (cmd == ".god") {
    const Offsets& o = Off();
    void* pawn = PawnForSlot(slot);
    if (o.takesDamage >= 0 && pawn) {
      const bool on = !g_god.count(slot);
      if (on) g_god.insert(slot);
      else g_god.erase(slot);
      Wr<bool>(pawn, o.takesDamage, !on);
      g_touched = true;
      return Reply(slot, steamid64, on ? "god mode on (you take no damage)." : "god mode off.");
    }
    // Fallback: buddha (server-wide, prac.cfg turns it on).
    g_buddhaOff = !g_buddhaOff;
    Cmd(g_buddhaOff ? "buddha 0" : "buddha 1");
    g_touched = true;
    return Reply(slot, steamid64, g_buddhaOff ? "buddha off for everyone." : "buddha on for everyone (nobody dies).");
  }
  if (cmd == ".spawn" || cmd == ".ctspawn" || cmd == ".tspawn") {
    const int team = cmd == ".ctspawn" ? 3 : cmd == ".tspawn" ? 2 : TeamOfSlot(slot);
    if (team != 2 && team != 3) return Reply(slot, steamid64, "join CT or T first (or use .ctspawn / .tspawn).");
    const int n = args.size() > 1 ? std::atoi(args[1].c_str()) : 1;
    const auto spawns = ListSpawns(team);
    const char* side = team == 3 ? "CT" : "T";
    if (spawns.empty()) return Reply(slot, steamid64, std::string("no ") + side + " spawn points found on this map.");
    if (n < 1 || n > static_cast<int>(spawns.size())) {
      return Reply(slot, steamid64, std::string(side) + " spawns: 1-" + std::to_string(spawns.size()) + ".");
    }
    const Vec3 to = spawns[static_cast<size_t>(n - 1)].pos;
    if (!TeleportRemembering(slot, id, to)) return Reply(slot, steamid64, kNoTeleport);
    return Reply(slot, steamid64, std::string(side) + " spawn " + std::to_string(n) + "/" +
                                      std::to_string(spawns.size()) + " (" + Fmt(to) + ").");
  }
}

void RunBot(int slot, uint64_t steamid64, const std::string& cmd) {
  if (const std::string why = ToolsRefusal(); !why.empty()) return Reply(slot, steamid64, cmd + " " + why);
  if (cmd == ".nobots") {
    Cmd("bot_kick");
    g_pendingBots.clear();
    ChatAll("Ready Up: bots removed.");
    return;
  }
  // One bot on the other team (CT player -> T bot, else CT bot), placed where the caller stands.
  const int tn = slot >= 0 ? TeamOfSlot(slot) : 0;
  const bool crouch = (cmd == ".cbot" || cmd == ".crouchbot" || cmd == ".crouchboost");
  if (crouch) Cmd("bot_crouch 1");
  if (tn == 3) {
    Cmd("bot_join_team T");
    Cmd("bot_add_t");
  } else {
    Cmd("bot_join_team CT");
    Cmd("bot_add_ct");
  }
  Cmd("bot_stop 1");
  Cmd("bot_freeze 1");
  Cmd("bot_zombie 1");
  if (crouch) Cmd("bot_crouch 0");  // future bots are not forced to crouch
  Spot s;
  if (slot >= 0 && SpotOfSlot(slot, &s)) {
    PendingBot p;
    p.callerSlot = slot;
    p.pos = s.pos;
    p.before = BotUserids();
    p.deadline = Now() + 5.0;
    g_pendingBots.push_back(std::move(p));
    g_touched = true;
  }
  ChatAll(crouch ? "Ready Up: crouch bot added." : "Ready Up: bot added.");
}

// ---- commands ------------------------------------------------------------------------------------

// <data_dir>/always.txt ("1" / "0"), written by readyup.practice.v1 set_always (the platform's
// practice.set {always}); it wins over the cfg while it exists.
std::string AlwaysFile() {
  const char* d = g_api->data_dir(g_api->self);
  return std::string(d ? d : ".") + "/always.txt";
}
int AlwaysStored() {  // -1 = none
  FILE* f = std::fopen(AlwaysFile().c_str(), "r");
  if (!f) return -1;
  const int c = std::fgetc(f);
  std::fclose(f);
  return c == '1' ? 1 : c == '0' ? 0 : -1;
}
int g_alwaysStored = -2;  // -2 = not read yet

// always=1 in cfg/ReadyUp/practice.cfg or readyup.cfg [practice], unless always.txt says otherwise.
bool AlwaysOn() {
  if (g_alwaysStored == -2) g_alwaysStored = AlwaysStored();
  if (g_alwaysStored >= 0) return g_alwaysStored == 1;
  char b[16] = {};
  return g_api->config_get(g_api->self, "always", b, sizeof(b)) > 0 && ParseBool(b, false);
}

bool StoreAlways(bool on) {
  const std::string path = AlwaysFile(), tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return false;
  const bool ok = std::fputs(on ? "1\n" : "0\n", f) >= 0;
  if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  g_alwaysStored = on ? 1 : 0;
  return true;
}
double g_nextAlwaysCheck = 0;

// A dedicated practice server (always=1) goes back to its home map, the essentials default map
// for "practice" (`ru map default practice <map>`, the platform's practice.set {home_map}; a skin
// tryout server's hub map), whenever nobody is on it. A workshop download takes a while: asked
// again only after kHomeRetrySeconds.
double g_homeAskedAt = -1e9;
constexpr double kHomeRetrySeconds = 180.0;

bool AnyHumanOn() {
  bool any = false;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        if (p->is_bot) return 1;
        *static_cast<bool*>(u) = true;
        return 0;
      },
      &any);
  return any;
}

// Settings for the home map (practice.set {home_cvars}), applied once per map load while on it.
std::vector<std::string> g_homeCvars;
bool g_homeCvarsRead = false;
std::string HomeCvarsFile() {
  const char* d = g_api->data_dir(g_api->self);
  return std::string(d ? d : ".") + "/home_cvars.txt";
}
const std::vector<std::string>& HomeCvars() {
  if (!g_homeCvarsRead) {
    g_homeCvarsRead = true;
    if (FILE* f = std::fopen(HomeCvarsFile().c_str(), "r")) {
      char b[128];
      while (std::fgets(b, sizeof(b), f) && static_cast<int>(g_homeCvars.size()) < kMaxHomeCvars) {
        std::string line = b;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (ValidHomeCvar(line)) g_homeCvars.push_back(line);
      }
      std::fclose(f);
    }
  }
  return g_homeCvars;
}
bool StoreHomeCvars(const std::vector<std::string>& lines) {
  const std::string path = HomeCvarsFile(), tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return false;
  bool ok = true;
  for (const auto& l : lines) ok = ok && std::fputs((l + "\n").c_str(), f) >= 0;
  if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  g_homeCvars = lines;
  g_homeCvarsRead = true;
  g_homeCvarsApplied = false;
  return true;
}

// On the home map: its settings, once per load (and again after practice's cfg).
void ApplyHomeCvars() {
  if (g_homeCvarsApplied || HomeCvars().empty()) return;
  const auto* e = static_cast<const ru_essentials_v1*>(
      g_api->get_interface(g_api->self, RU_ESSENTIALS_IFACE_NAME, RU_ESSENTIALS_IFACE_VERSION));
  if (!e || !e->default_map) return;
  const std::string home = e->default_map("practice");
  const char* cur = g_api->current_map(g_api->self);
  if (home.empty() || !cur || !*cur || !readyup::mapnames::EntryMatchesLoaded(home, cur)) return;
  g_homeCvarsApplied = true;
  for (const auto& l : HomeCvars()) Cmd(l);
  Log("practice: home map %s: %zu setting(s) applied", cur, HomeCvars().size());
}

void GoHomeIfEmpty(double now) {
  if (!AlwaysOn() || now - g_homeAskedAt < kHomeRetrySeconds) return;
  const auto* e = static_cast<const ru_essentials_v1*>(
      g_api->get_interface(g_api->self, RU_ESSENTIALS_IFACE_NAME, RU_ESSENTIALS_IFACE_VERSION));
  if (!e || !e->default_map || !e->load_map) return;
  const std::string home = e->default_map("practice");
  if (home.empty()) return;
  const char* cur = g_api->current_map(g_api->self);
  if (cur && *cur && readyup::mapnames::EntryMatchesLoaded(home, cur)) return;
  if (AnyHumanOn()) return;
  g_homeAskedAt = now;
  // The engine names a workshop map by its bsp only: remember which id this load is for.
  if (readyup::mapnames::MapRef ref; readyup::mapnames::ParseEntry(home, &ref) && !ref.workshop_id.empty()) {
    readyup::mapnames::NoteWorkshopLoad(ref.workshop_id);
  }
  Log("practice: nobody on %s: back to the home map %s", cur && *cur ? cur : "?", home.c_str());
  if (!e->load_map(home.c_str())) Log("practice: the home map %s could not be loaded", home.c_str());
}

std::vector<std::string> Args(const ru_command_ctx* c, int from) {
  std::vector<std::string> out;
  for (int i = from; i < c->argc; ++i) out.emplace_back(c->argv[i] ? c->argv[i] : "");
  return out;
}

int SenderSlot(const ru_command_ctx* c) {
  return c->slot >= 0 ? c->slot : (c->steamid64 ? g_api->slot_for_steamid(g_api->self, c->steamid64) : -1);
}

void OnToolChat(void*, const ru_command_ctx* c) {
  const auto args = Args(c, 0);
  if (args.empty()) return;
  const int slot = SenderSlot(c);
  if (slot < 0) return Log("practice: %s from %s: slot unknown", args[0].c_str(), c->name ? c->name : "?");
  if (IsBotCommand(args[0])) RunBot(slot, c->steamid64, Lower(args[0]));
  else RunTool(slot, c->steamid64, args);
}

bool Toggle(bool on, std::string* reply) {
  if (on == IsActive()) {
    *reply = on ? "practice mode is already on." : "practice mode is already off.";
    return true;
  }
  if (!on) {
    g_manualOff = true;
    Leave();
    *reply = "practice mode disabled.";
    return true;
  }
  const char* why = "";
  if (!Enter(&why)) {
    *reply = std::string("practice mode refused: ") + why + ".";
    return false;
  }
  *reply = "practice mode enabled.";
  return true;
}

void OnPracChat(void*, const ru_command_ctx* c) {
  const int slot = SenderSlot(c);
  if (g_api->is_admin(g_api->self, c->steamid64) != 1) {
    if (slot >= 0) g_api->chat_to_slot(g_api->self, slot, " \x04[ReadyUp]\x01 not authorized");
    return;
  }
  if (g_dryRun && IsActive()) return EndDryRun("dry run stopped");  // ME: .prac during a dry run = practice again
  std::string reply;
  (void)Toggle(!IsActive(), &reply);
  ChatAll("Ready Up: " + reply);
}

// `.dry` / `.dryrun` (ME, admin): a normal round inside practice; again = back to practice.
void OnDryChat(void*, const ru_command_ctx* c) {
  const int slot = SenderSlot(c);
  auto reply = [&](const std::string& m) {
    if (slot >= 0) g_api->chat_to_slot(g_api->self, slot, (" \x04[ReadyUp]\x01 " + m).c_str());
  };
  if (g_api->is_admin(g_api->self, c->steamid64) != 1) return reply("not authorized");
  if (!IsActive()) return reply("Dryrun can only be started in practice mode!");
  if (!ToolsAllowed(true, Ruleset())) return reply("not available under the valve ruleset.");
  if (g_dryRun) return EndDryRun("dry run stopped");
  StartDryRun();
}

// `.globalnades` (ME, admin): .savenade / .deletenade work on the global lineups.
void OnGlobalNadesChat(void*, const ru_command_ctx* c) {
  const int slot = SenderSlot(c);
  if (g_api->is_admin(g_api->self, c->steamid64) != 1) {
    if (slot >= 0) g_api->chat_to_slot(g_api->self, slot, " \x04[ReadyUp]\x01 not authorized");
    return;
  }
  g_globalNades = !g_globalNades;
  Log("practice: .globalnades %s", g_globalNades ? "on" : "off");
  if (slot >= 0) {
    g_api->chat_to_slot(g_api->self, slot,
                        (std::string(" \x04[ReadyUp]\x01 Saving/Loading Lineups Globally is now ") +
                         (g_globalNades ? "Enabled" : "Disabled") + "!")
                            .c_str());
  }
}

// `.exitprac` (MatchZy name): practice off, never on (`.prac` toggles).
void OnExitPracChat(void*, const ru_command_ctx* c) {
  const int slot = SenderSlot(c);
  if (g_api->is_admin(g_api->self, c->steamid64) != 1) {
    if (slot >= 0) g_api->chat_to_slot(g_api->self, slot, " \x04[ReadyUp]\x01 not authorized");
    return;
  }
  std::string reply;
  (void)Toggle(false, &reply);
  ChatAll("Ready Up: " + reply);
}

// `.match` (Get5 / MatchZy name for leaving practice): the same as `.exitprac` while practice is on,
// silent otherwise so a player typing it outside practice is not told off.
void OnMatchChat(void*, const ru_command_ctx* c) {
  if (!IsActive()) return;
  OnExitPracChat(nullptr, c);
}

// `ru practice on|off|status` and `ru practice as <slot> <.command> [args]`.
void OnRu(void*, const ru_command_ctx* c) {
  const std::string sub = c->argc >= 3 && c->argv[2] ? Lower(c->argv[2]) : "";
  auto reply = [&](const std::string& m) {
    if (c->is_console) Log("%s", m.c_str());
    else if (c->slot >= 0) g_api->chat_to_slot(g_api->self, c->slot, (" \x04[ReadyUp]\x01 " + m).c_str());
  };
  if (sub.empty() || sub == "help") {
    for (const char* l : {".ru practice on|off: practice mode (admin; also .prac)", ".ru practice status",
                          ".ru practice dryrun: a dry run round, or back to practice (admin; also .dryrun)",
                          "ru practice as <slot> <.command>: run a practice tool as that player (console)"}) {
      reply(l);
    }
    return;
  }
  if (sub == "status") {
    reply(std::string("practice ") + (IsActive() ? (g_dryRun ? "on (dry run)" : "on") : "off") +
          (MatchOwnsMode() ? " (match plugin mode)" : " (standalone)") + ", always=" + (AlwaysOn() ? "1" : "0"));
    return;
  }
  if (!c->is_console && g_api->is_admin(g_api->self, c->steamid64) != 1) return reply("not authorized");
  if (sub == "on" || sub == "off") {
    std::string r;
    (void)Toggle(sub == "on", &r);
    return reply(r);
  }
  if (sub == "dryrun") {
    if (!IsActive()) return reply("Dryrun can only be started in practice mode!");
    if (g_dryRun) {
      EndDryRun("dry run stopped");
      return reply("dry run stopped.");
    }
    StartDryRun();
    return reply("dry run started.");
  }
  if (sub == "as") {
    if (!c->is_console) return reply(".ru practice as is console / RCON only.");
    if (c->argc < 5) return reply("usage: ru practice as <slot> <.command> [args]");
    const int slot = std::atoi(c->argv[3]);
    ru_player p{};
    p.struct_size = sizeof(p);
    if (!PlayerBySlot(slot, &p)) return reply("ru practice as: no player in slot " + std::to_string(slot));
    const auto args = Args(c, 4);
    const std::string cmd = Lower(args[0]);
    Log("ru practice as: slot %d (%s%s) runs %s", slot, p.name, p.is_bot ? ", bot" : "", cmd.c_str());
    if (IsToolCommand(cmd)) RunTool(slot, p.is_bot ? 0 : p.steamid64, args);
    else if (IsBotCommand(cmd)) RunBot(slot, p.is_bot ? 0 : p.steamid64, cmd);
    else reply("ru practice as: " + cmd + " is not a practice command");
    return;
  }
  reply("unknown command. Type .ru help practice for the list.");
}

// ---- feedback: flashes and damage in chat (practice_feedback.h) -------------------------------------

// feedback=0 in cfg/ReadyUp/practice.cfg turns the lines off.
// Read at most once every 5 s (player_hurt can fire many times a tick with fire); never under the
// valve ruleset.
bool FeedbackOn() {
  static bool on = true;
  static double at = -1e9;
  const double now = Now();
  if (now - at >= 5.0) {
    at = now;
    char b[16] = {};
    on = g_api->config_get(g_api->self, "feedback", b, sizeof(b)) <= 0 || ParseBool(b, true);
  }
  return on && !g_dryRun && ToolsAllowed(true, Ruleset());
}

std::string NameOfSlot(int slot) {
  ru_player p{};
  p.struct_size = sizeof(p);
  return slot >= 0 && PlayerBySlot(slot, &p) ? std::string(p.name) : std::string();
}

void FeedbackHurt(const ru_game_event* ev) {
  const ru_api* a = A();
  const int victim = a->ev_get_player_slot(a->self, ev, "userid");
  const int attacker = a->ev_get_player_slot(a->self, ev, "attacker");
  const int dmg = a->ev_get_int(a->self, ev, "dmg_health", 0);
  if (victim < 0 || dmg <= 0) return;
  const std::string weapon = a->ev_get_string(a->self, ev, "weapon", "");
  const std::string vn = NameOfSlot(victim), an = NameOfSlot(attacker);
  if (IsSummedWeapon(weapon)) {
    g_grenadeDamage.Add(Now(), an, weapon, vn, dmg);
    return;
  }
  ChatAll(FormatHit(an, vn, dmg, a->ev_get_int(a->self, ev, "dmg_armor", 0), a->ev_get_int(a->self, ev, "hitgroup", 0),
                    weapon, a->ev_get_int(a->self, ev, "health", 0)));
}

void FeedbackBlind(const ru_game_event* ev) {
  const ru_api* a = A();
  const int victim = a->ev_get_player_slot(a->self, ev, "userid");
  const double secs = a->ev_get_float(a->self, ev, "blind_duration", 0.0);
  if (victim < 0 || secs < 0.1) return;
  ChatAll(FormatBlind(NameOfSlot(victim), NameOfSlot(a->ev_get_player_slot(a->self, ev, "attacker")), secs));
}

// ---- engine callbacks ---------------------------------------------------------------------------

void OnGameEvent(void*, const char* evName, const ru_game_event* ev) {
  if (!evName || !ev || !IsActive()) return;
  const ru_api* a = A();
  if (std::strcmp(evName, "grenade_thrown") == 0) {
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0) return;
    Spot s;
    if (!SpotOfSlot(slot, &s)) return;
    ru_player p{};
    p.struct_size = sizeof(p);
    const uint64_t id = PlayerId(slot, PlayerBySlot(slot, &p) && !p.is_bot ? p.steamid64 : 0);
    g_lastThrow[id] = s;
    Throw t;
    t.pos = {s.pos.x, s.pos.y, s.pos.z};
    t.ang = {s.ang.x, s.ang.y, s.ang.z};
    const std::string weapon = a->ev_get_string(a->self, ev, "weapon", "");
    t.kind = GrenadeKind(weapon);
    t.at = Now();
    t.launch.incendiary = weapon.find("incgrenade") != std::string::npos;  // until the projectile says
    g_throws[id].Add(t);
    g_touched = true;
    g_scanBurst = 8;  // its projectile shows up over the next ticks (ScanProjectiles)
  } else if (std::strcmp(evName, "round_freeze_end") == 0) {
    if (g_dryRun) g_dryArmed = true;
  } else if (std::strcmp(evName, "round_end") == 0) {
    if (g_dryRun && g_dryArmed) EndDryRun("the dry run round is over");
  } else if (std::strcmp(evName, "player_hurt") == 0) {
    if (FeedbackOn()) FeedbackHurt(ev);
  } else if (std::strcmp(evName, "player_blind") == 0) {
    if (FeedbackOn()) FeedbackBlind(ev);
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0 || !g_noflash.count(slot)) return;
    void* pawn = a->ev_get_player_pawn(a->self, ev, "userid");
    const Offsets& o = Off();
    if (!pawn) return;
    if (o.flashAlpha >= 0) Wr<float>(pawn, o.flashAlpha, 0.f);
    if (o.flashDuration >= 0) Wr<float>(pawn, o.flashDuration, 0.f);
    a->entity_mark_changed(a->self, pawn);
  } else if (std::strcmp(evName, "player_spawn") == 0) {
    // A new pawn (respawn) starts with the default values: apply the toggles again.
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0 || (!g_god.count(slot) && !g_noflash.count(slot))) return;
    void* pawn = a->ev_get_player_pawn(a->self, ev, "userid");
    const Offsets& o = Off();
    if (!pawn) return;
    if (g_god.count(slot) && o.takesDamage >= 0) Wr<bool>(pawn, o.takesDamage, false);
    if (g_noflash.count(slot) && o.flashAlpha >= 0) {
      Wr<float>(pawn, o.flashAlpha, 0.f);
      a->entity_mark_changed(a->self, pawn);
    }
  }
}

void OnMapStart(void*, const ru_event*) {
  g_homeCvarsApplied = false;
  // Binds a workshop id to the map it loaded (EntryMatchesLoaded for the home map).
  if (const char* cur = g_api->current_map(g_api->self); cur && *cur) readyup::mapnames::NoteMapLoaded(cur);
  // New map: new entities and pawns. A standalone practice server keeps practice on (the map load
  // may have run other cfgs); with the match plugin, always=1 re-enters from the tick.
  g_off = Offsets{};
  ClearState(/*restorePawns=*/false);
  g_projHigh = 0;
  g_manualOff = false;
  if (!MatchOwnsMode() && g_standaloneActive) Cmd("exec ReadyUp/prac.cfg");
}


void OnTick(void*, const ru_tick_info* t) {
  if (t->now >= g_nextAlwaysCheck) {
    g_nextAlwaysCheck = t->now + 2.0;
    if (!g_manualOff && ShouldAutoEnter(AlwaysOn(), IsActive(), RuMode())) {
      const char* why = "";
      if (Enter(&why)) Log("practice: always=1: practice mode switched on");
    }
    GoHomeIfEmpty(t->now);
    ApplyHomeCvars();
  }
  if (!IsActive()) {
    if (g_touched) {
      ClearState(/*restorePawns=*/true);
      Log("practice: left practice mode; positions and toggles cleared");
    }
    return;
  }
  for (const auto& l : g_grenadeDamage.Flush(t->now, 1.0)) ChatAll(l);
  if (!g_delayed.empty()) {
    const double now = Now();
    std::vector<DelayedCmd> due;
    for (auto it = g_delayed.begin(); it != g_delayed.end();) {
      if (now >= it->due) {
        due.push_back(std::move(*it));
        it = g_delayed.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto& d : due) {
      if (!d.cmd.empty()) Cmd(d.cmd);
      else if (!SpawnThrow(d.nade, d.slot)) Log("practice: delayed rethrow for slot %d refused", d.slot);
    }
  }
  if (g_scanBurst > 0) {
    --g_scanBurst;
    ScanProjectiles(Now());
  } else if ((++g_scanTick & 7u) == 0) {
    ScanProjectiles(Now());
  }
  DrawTimers(Now());
  if (g_pendingBots.empty()) return;
  const auto bots = BotUserids();
  for (auto it = g_pendingBots.begin(); it != g_pendingBots.end();) {
    bool done = false;
    for (int b : bots) {
      if (it->before.count(b)) continue;
      if (!Alive(PawnForSlot(b))) continue;
      const bool moved = Teleport(b, it->pos);
      Log("practice: bot slot %d %s slot %d's position (%s)", b, moved ? "placed at" : "could not be moved to",
          it->callerSlot, Fmt(it->pos).c_str());
      it->before.insert(b);
      done = true;
      break;
    }
    if (done) {
      it = g_pendingBots.erase(it);
    } else if (t->now > it->deadline) {
      Log("practice: new bot did not spawn within 5s; left at its spawn");
      it = g_pendingBots.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- readyup.practice.v1 + selftest ----------------------------------------------------------------

int IfaceActive() { return IsActive() ? 1 : 0; }
int IfaceSetActive(int on, const char** why) {
  std::string r;
  const bool ok = Toggle(on != 0, &r);
  if (!ok && why) {
    *why = RuMode() == "external" ? "another plugin's mode is on (deathmatch: .ru dm off first)"
                                  : "a match is loaded (.ru match end first)";
  }
  return ok ? 1 : 0;
}
// readyup.help.practice: the `.help` line (essentials); off, how to turn it on.
const char* HelpLineNow() { return IfaceActive() ? HelpLine() : "Practice: .prac to start practice mode"; }
const ru_help_v1 g_help = {sizeof(ru_help_v1), &HelpLineNow};

const char* IfaceHelp() { return HelpLine(); }
int IfaceDryRun() { return IsActive() && g_dryRun ? 1 : 0; }
int IfaceAlways() { return AlwaysOn() ? 1 : 0; }
int IfaceSetAlways(int on) {
  if (!StoreAlways(on != 0)) return 0;
  Log("practice: always=%d (set by the platform)", on ? 1 : 0);
  if (on) {
    g_manualOff = false;
    g_nextAlwaysCheck = 0;  // the next tick switches practice on when nothing blocks it
  }
  return 1;
}
int IfaceSetHomeCvars(const char* const* lines, uint32_t n) {
  if (n > static_cast<uint32_t>(kMaxHomeCvars)) return 0;
  std::vector<std::string> out;
  for (uint32_t i = 0; i < n; ++i) {
    const std::string l = lines && lines[i] ? lines[i] : "";
    if (!ValidHomeCvar(l)) return 0;
    out.push_back(l);
  }
  if (!StoreHomeCvars(out)) return 0;
  Log("practice: %zu home map setting(s) (set by the platform)", out.size());
  return 1;
}
const ru_practice_v1 g_iface = {sizeof(ru_practice_v1), &IfaceActive, &IfaceSetActive, &IfaceHelp,
                                &IfaceDryRun,           &IfaceAlways, &IfaceSetAlways, &IfaceSetHomeCvars};

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  std::string d = std::string(IsActive() ? "on" : "off") + (MatchOwnsMode() ? " (match plugin mode)" : " (standalone)");
  add(ctx, "INFO", "practice", d.c_str());
  add(ctx, RU_API_HAS(g_api, entity_set_abs_origin) && g_api->entity_set_abs_origin ? "OK" : "WARN", "practice teleport",
      "ru_api entity_set_abs_origin (.loadpos / .spawn / bot placement)");
  add(ctx, RU_API_HAS(g_api, grenade_spawn) && g_api->grenade_spawn ? "OK" : "WARN", "practice rethrow",
      "ru_api grenade_spawn (per-player .rethrow / .throwidx; else the server-wide sv_rethrow_last_grenade)");
  scenarios::Selftest(add, ctx);
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace practice

using namespace practice;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,  // needs API 1.2; teleport (1.3 entity_set_abs_origin) is checked with RU_API_HAS
      "practice",
      PRACTICE_VERSION,
      "Ready Up",
      "practice mode + tools (.prac, .savepos/.loadpos, .spawn, .rethrow, .bot, ...)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, register_ru_subcommand)) return 1;
  g_api = api;
  g_off = Offsets{};
  ClearState(false);
  g_nextAlwaysCheck = 0;
  g_manualOff = false;
  g_standaloneActive = false;
  g_lineups = LineupBook{};
  g_lineupsMap.clear();
  g_globalNades = false;
  g_lineupWriter = new LineupWriter;
  g_lineupWriter->Start();
  (void)api->stash_get(api->self, "standalone_active", &g_standaloneActive, sizeof(g_standaloneActive));
  if (!api->register_chat_command(api->self, ".prac", &OnPracChat, nullptr) ||
      !api->register_chat_command(api->self, ".tactics", &OnPracChat, nullptr) ||
      !api->register_chat_command(api->self, ".exitprac", &OnExitPracChat, nullptr) ||
      !api->register_chat_command(api->self, ".match", &OnMatchChat, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not register .prac (another plugin owns it)");
  }
  for (const char* c :
       {".rethrow", ".rt", ".throw", ".savepos", ".loadpos", ".back", ".clear", ".noflash", ".god", ".spawn", ".ctspawn",
        ".tspawn", ".bot", ".cbot", ".crouchbot", ".boost", ".crouchboost", ".nobots",
        // ME extras
        ".last", ".lastindex", ".throwidx", ".throwindex", ".delay", ".throwsmoke", ".rethrowsmoke", ".throwflash",
        ".rethrowflash", ".throwgrenade", ".rethrowgrenade", ".thrownade", ".rethrownade", ".throwmolotov",
        ".rethrowmolotov", ".throwdecoy", ".rethrowdecoy", ".impacts", ".traj", ".pip", ".solid", ".break", ".timer",
        ".bestspawn", ".worstspawn", ".bestctspawn", ".worstctspawn", ".besttspawn", ".worsttspawn", ".showspawns",
        ".hidespawns",
        // lineup library
        ".savenade", ".sn", ".loadnade", ".ln", ".listnades", ".lin", ".deletenade", ".delnade", ".dn", ".importnade",
        ".in"}) {
    if (!api->register_chat_command(api->self, c, &OnToolChat, nullptr)) ru_logf(api, RU_LOG_WARN, "could not register %s", c);
  }
  for (const char* c : {".fas", ".watchme"}) {  // ME: everyone else to the spectators (ru_api 1.13)
    if (!api->register_chat_command(api->self, c, &OnToolChat, nullptr)) ru_logf(api, RU_LOG_WARN, "could not register %s", c);
  }
  if (!api->register_chat_command(api->self, ".globalnades", &OnGlobalNadesChat, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not register .globalnades");
  }
  for (const char* c : {".dry", ".dryrun"}) {
    if (!api->register_chat_command(api->self, c, &OnDryChat, nullptr)) ru_logf(api, RU_LOG_WARN, "could not register %s", c);
  }
  api->register_ru_subcommand(api->self, "practice", &OnRu, nullptr);
  for (const char* e : {"grenade_thrown", "player_blind", "player_spawn", "player_hurt", "round_freeze_end", "round_end"}) {
    if (!api->subscribe_game_event(api->self, e, &OnGameEvent, nullptr)) ru_logf(api, RU_LOG_WARN, "could not subscribe to %s", e);
  }
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_HELP_IFACE_PREFIX "practice", RU_HELP_IFACE_VERSION,
                         const_cast<ru_help_v1*>(&g_help));
  api->provide_interface(api->self, RU_PRACTICE_IFACE_NAME, RU_PRACTICE_IFACE_VERSION, const_cast<ru_practice_v1*>(&g_iface));
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "practice", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  scenarios::Load(api, {[] { return IsActive(); }, [] { return ToolsRefusal(); },
                        [](void* e) { g_projSeen.insert(g_api->entity_handle_of(g_api->self, e)); }});
  ru_logf(api, RU_LOG_INFO, "loaded " PRACTICE_VERSION);
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Hot reload keeps a standalone practice mode on; toggles are restored on the pawns.
  g_api->stash_put(g_api->self, "standalone_active", &g_standaloneActive, sizeof(g_standaloneActive));
  scenarios::Unload();
  ClearState(/*restorePawns=*/true);
  if (g_lineupWriter) {  // pending lineup files are written first
    g_lineupWriter->Stop();
    delete g_lineupWriter;
    g_lineupWriter = nullptr;
  }
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
