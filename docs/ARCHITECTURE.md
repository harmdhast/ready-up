# Ready Up architecture: core + plugins

Ready Up is a fake `libserver.so`. `gameinfo.gi` lists `Game csgo/readyup` first, so the
engine loads our shim, which `dlopen`s Valve's real `libserver.so` and forwards its exports.
Up to now everything (engine plumbing, match flow, skins) was one monolithic shim. This
document describes the split:

| Piece | Ships as | Owns |
|---|---|---|
| **readyup-core** | `csgo/readyup/bin/linuxsteamrt64/libserver.so` | every piece of engine surface, plus the plugin host |
| **readyup-match** | `csgo/readyup/plugins/match.so` | ready-up, scrim, knife, pause, match config, webhooks, JSON persistence, admins, practice |
| **readyup-skins** | `csgo/readyup/plugins/skins.so` | weapon paints, knives, gloves, agents. **Not in the default release** (servers running skin changers risk GSLT bans) |
| **readyup-fleet** | `csgo/readyup/plugins/fleet.so` | the Auto Tournament platform link ([FLEET.md](FLEET.md)): enrollment, WebSocket, spool; `readyup.fleet.v1` for other plugins |
| example: **hello** | `csgo/readyup/plugins/hello.so` | `.hello`, a 10 s tick heartbeat, event logging. Dev only, never shipped |

The model is Metamod's. The core is the only thing that knows about signatures, offsets,
vtable indices, RTTI, hooks and SDK struct layouts. Plugins only call a versioned table of C
function pointers. The goals:

1. **CS2 updates touch the core only.** When Valve moves a function, `gamedata/engine-surface.json`
   and core code change. Plugins built against the same API version keep working without a rebuild.
2. **Hot reload.** `ru plugin reload match` swaps the match logic without restarting the server.
   This is the main speed-up for development.
3. **Separate shipping.** Skins is its own `.so`. Leaving it out of a release means removing one file.

Status: the split is done ([migration plan](#migration-plan) steps 0-4, and step 5's "drop
libpq/libcurl from the core link"). The core only owns engine-facing infrastructure; the match
flow is `plugins/match` (`match.so`), skins is `plugins/skins`, both talk to the core through
API v1.2 only. The core runs on its own without any plugin (engine checks, `ru selftest`, the
status endpoint with `summary.match_plugin: "none"`).

---

## 1. The boundary

The rule: **if it depends on a byte pattern, an offset, a vtable index, a struct layout, a
hook or an engine interface, it is core.** Everything else is policy and belongs in a plugin.

The core owns:

- **Loading.** The `libserver.so` shim, forwarding exports and `CreateInterface`, `real_server`.
- **Engine surface.** Resolving `engine-surface.json` (signatures + identity anchors), sigtest,
  fail-closed "disabled" mode, and `readyup_sigcheck`.
- **Hooks** (funchook detours / vtable / GOT patches): GameFrame, Host_Say, ClientCommand,
  CCommandBuffer::AddText, TerminateRound, the logging listener.
- **RTTI-verified interfaces**: the game event manager, the legacy per-client listener, the entity system.
- **Game events**: registering the listener and turning events into normalized lifecycle events.
- **Threads and commands**: the GameFrame thread queue (`post_to_game_thread`, per-tick
  callbacks) and server command enqueue.
- **Command routing**: chat and console commands (dedupe across the three chat paths; core
  commands reserved).
- **Logs**: the log-line listener and log-derived lifecycle events (the fallback when engine events are down).
- **Schema and entities**: field offsets by name, entity lookup and iteration, and entity
  primitives (attribute by name, subclass, model, bodygroup, network-state-changed).
- **Output**: chat to all or to one slot (`UTIL_ClientPrintAll` / `ClientPrint`), center HTML (broadcast or per client).
- **Players**: the identity and team registry (slot ↔ SteamID64 ↔ name ↔ team, bots).
- **Infrastructure**: config (`readyup.cfg` core keys, plus per-plugin config lookup), logging, the crash handler.
- **Admin checks**: the permission check (`is_admin`). Who counts as an admin comes from a
  provider; the match plugin registers the MAT / admins.json / platform-backed one (without it, only the console is).
- **Status endpoint** (`/health /status /stream /metrics /selftest`, docs/FLEET.md §17): the
  HTTP server, versions, selftest and health are the core's; the match part of `/status`
  (summary, MatchState, `update_safe`) comes from the match plugin through `readyup.match.v1`
  (`core/include/readyup/match_iface.h`), the platform part from `readyup.fleet.v1`.
- **Selftest**: engine surface, hooks, schema, events, plugin host, features; plugins add their
  own lines through `readyup.selftest.<name>` (`core/include/readyup/selftest_iface.h`).

Plugins own **policy**. They decide what happens on a round start, what `.r` means, which
webhook to send and which skin to apply. They get engine access only through `ru_api`.

## 2. The C ABI (`core/include/readyup/plugin_api.h`)

That header is the whole contract. Plugins include it and nothing from the core's sources.

### What a plugin exports

```c
READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void);
READYUP_PLUGIN_EXPORT int  readyup_plugin_load(const ru_api* api, uint32_t core_api_version);
READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void);
```

- `readyup_plugin_info` is called right after `dlopen` and before anything else. It returns
  `{struct_size, api_version, name, version, author, description}`. `name` must equal the
  file name (`hello` ↔ `hello.so`). The core rejects the plugin on an API major mismatch, or
  if the plugin needs a newer minor than the core has.
- `readyup_plugin_load` runs on the game thread. The plugin registers its commands, ticks and
  subscriptions here and keeps `api`, which stays valid until unload returns. Returning
  non-zero aborts the load: the core removes anything already registered and `dlclose`s.
- `readyup_plugin_unload` runs on the game thread at a safe point (see §4).

### What the core gives it (v1.0, implemented)

Every function takes `api->self` (the plugin's `ru_plugin*`) first. The core uses it to
attribute every registration to its owner.

| Member | Thread | Purpose |
|---|---|---|
| `struct_size`, `api_version`, `core_version`, `self` | – | versioning and identity |
| `log(self, level, msg)` | any | console log line `[ReadyUp] plugin[name]: ...` (`ru_logf` is a printf helper in the header) |
| `server_command(self, cmd)` | game | queue a server console command (the core's `EnqueueServerCommand`) |
| `chat_all(self, msg, flags)` | game | chat to everyone, with the Ready Up prefix (or `RU_CHAT_RAW`) |
| `chat_to_slot(self, slot, msg)` | game | chat to one player (`ClientPrint`) |
| `register_chat_command(self, ".name", fn, user)` | game | chat command. Core commands (`.ru`, `.r`, `.pause`, …) are reserved. Only real players (SteamID ≠ 0) trigger it |
| `register_console_command(self, "name", fn, user)` | game | server console / RCON command. `ru` and every core command win over plugin commands |
| `on_tick(self, fn, user)` | game | once per simulating GameFrame, after the core's frame work |
| `subscribe(self, type, fn, user)` | game | lifecycle events (below). `RU_EVENT_ANY` for all |
| `unregister(self, handle)` | game | remove any of the above (safe from inside that callback) |
| `post_to_game_thread(self, fn, user)` | **any** | hand work from a plugin's worker thread back to the game thread. Dropped if the plugin unloads first |
| `slot_for_steamid(self, steamid64)` | game | engine slot of a connected player, or -1 |
| `data_dir(self)` | game | `csgo/readyup/plugins/<name>/` for plugin data and config |

**Lifecycle events** (`ru_event`) arrive already normalized. They come from engine game
events once the core's listener is live, and from server log lines until then (`source` says
which). A plugin never has to reconcile the two sources.

| Event | Fields |
|---|---|
| `RU_EVENT_MAP_START` | `map` |
| `RU_EVENT_MATCH_START` | CS2 `Match_Start` (warmup end, `mp_restartgame`) |
| `RU_EVENT_ROUND_START` | `round` |
| `RU_EVENT_ROUND_END` | `winner` (2 = T, 3 = CT), `reason`, `round`, scores when known |
| `RU_EVENT_PLAYER_CONNECT` / `DISCONNECT` | `slot`, `steamid64`, `name` (+`reason`) |
| `RU_EVENT_PLAYER_TEAM` | `slot`, `steamid64`, `name`, `team`, `old_team` |

Every event also carries `map` (the current map).

### v1.1 (implemented)

Appended to `ru_api` in this order; `plugin_api.h` has the exact signatures and comments.
Bookkeeping members live in `plugin_loader.cpp`; engine-facing ones in `plugin_engine_api.cpp`.
Everything is game-thread only except where noted.

| Group | Members | Notes |
|---|---|---|
| Output | `center_html_to_slot`, `center_html_all` | per-client center HTML (never a broadcast event); `_all` loops over connected humans |
| Players | `get_player`, `get_player_by_steamid`, `for_each_player` | from the core's human/bot registry; `ru_player` is caller-owned with an inline `name[128]` |
| Raw engine events | `subscribe_game_event(name)`, `ev_get_int/float/uint64/string/player_slot/player_controller/player_pawn` | **synchronous** on the game thread inside the engine's dispatch, before the core takes its events lock. The core adds its listener for every subscribed name (re-checked when the set changes and every 2 s) |
| Log lines | `subscribe_log_line` | queued copies; fed from `ObserveLifecycleLogLine`, which both the in-process listener and the file-tail fallback go through |
| Schema / entities | `schema_offset`, `entity_system_status`, `entity_by_index`, `entity_from_handle`, `entity_handle_of`, `entity_classname`, `entity_mark_changed`, `econ_attr_set_by_name`, `entity_change_subclass`, `entity_set_model`, `entity_set_bodygroup_by_name`; later `entity_set_abs_origin` (1.3) and `entity_remove` (1.5, UTIL_Remove) | backed by `schema.*` and `entity.*`; each returns 0/NULL when its engine function did not resolve |
| Match control | `set_round_termination_suppressed`, `set_chat_name_prefix` | see below for `terminate_round` |
| Admins | `is_admin` (**any thread**, may block), `set_admin_provider` | the core's `IsReadyUpAdmin` asks the provider first, so core checks follow it too. Providers run on the caller's thread under a shared lock that unload takes exclusively |
| Config | `config_get`, `debug_enabled` (any thread), `config_dir` (any thread) | `config_get` reads `csgo/cfg/ReadyUp/<plugin>.cfg` (top-level keys or a `[<plugin>]` section), then the `[<plugin>]` section of `readyup.cfg`. The core's own parser now ignores everything after the first `[section]` line |
| Plugin-to-plugin | `provide_interface`, `get_interface` | one provider per name, removed on unload; look it up again in each callback |
| Reload state | `stash_put`, `stash_get` | byte blobs keyed by (plugin, key), kept in core memory across unload/load, max 1 MiB each |

Deviations from the original plan:

- **`terminate_round` is not in v1.1.** The core only has the TerminateRound *suppression*
  detour; calling TerminateRound needs the CCSGameRules pointer and a verified ABI, which the
  engine surface does not have. `set_round_termination_suppressed` covers what match uses today.
- **`set_chat_name_prefix`** is keyed by SteamID64 and feeds the existing relay (the core
  re-sends `"<prefix> <name>: <msg>"` and swallows the original), not a real name swap.
- **`RU_CMD_HIDE` / `register_chat_command_ex`** and `RouteChatCommand` carrying the sender slot
  were not in v1.1; they are in v1.2 (below), ahead of the match move.

`plugins/hello` requires 1.0 and uses every 1.1 group it can behind `RU_API_HAS` (stash load
counter, `greeting` from config, `player_death`, log-line count, `readyup.hello.v1`), and the
host test checks each of them, including that nothing is delivered after unload.

### v1.2 (implemented)

What moving the match flow out of the core needed. Appended to `ru_api` in this order (plus two
appended struct fields); bookkeeping members in `plugin_loader.cpp`, `feature_state` in
`plugin_engine_api.cpp`.

| Member | Thread | Purpose |
|---|---|---|
| `log_untagged(self, level, msg)` | any | `[ReadyUp] <msg>` without the `plugin[name]: ` tag, for log formats tools already parse (`state:`, `knife:`) |
| `register_chat_command_ex(self, name, flags, fn, user)` | game | chat command with `RU_CMD_HIDE`: the core swallows the sender's line in its Host_Say hook, before the engine prints it |
| `register_console_command_ex(self, name, flags, fn, user)` | game | console command with `RU_CMD_OBSERVE`: the plugin sees the line, the engine still runs it (e.g. `tv_delay 5`). Observers never conflict |
| `register_ru_subcommand(self, name, fn, user)` | game | `ru <name> ...` (console / RCON) and `.ru <name> ...` (chat) both reach fn; argv[0] is `ru` / `.ru`. The core's subcommands (`help`, `plugin`, `version`, `selftest`, `sigtest`, `reload`, `status_http`, and `license` since 1.9) are reserved |
| `on_frame(self, fn, user)` | game | every GameFrame, simulating or not (`ru_tick_info.simulating`), after `on_tick`. For timers that must fire while the server does not simulate |
| `feature_state(self, name)` | game | 1 on / 0 pending / -1 off for a core feature (`knife`, `ready_hud`, ...) or dependency (`fn:X`, `cmdbuf`, `loglistener`, `eventmgr`, ...), plus `events_live` (engine events drive the round lifecycle) |
| `current_map(self)` | game | the map of the last `RU_EVENT_MAP_START`, e.g. for a plugin that loads mid-map |

Struct fields: `ru_tick_info.simulating`, `ru_player.userid` (the log `<N>`, what `kickid` takes).

**Sender slot.** Chat commands now carry the sender's slot from the Host_Say hook's controller (it
used to be looked up again from the SteamID when the command was delivered). The hook routes every
plugin-owned chat command, not only `.ru` / `.r`; the log listener's copy of the line is deduped.
The sender's identity (SteamID / name per slot) still comes from the game log (`log on`):
`GetSlotIdentity` is filled by the log listener. Since CS2 1.41.9.0 chat no longer reaches
ClientCommand as `say`; the ClientCommand hook runs the same shared handler only on builds that
still send chat there.

**Still not in the API: `terminate_round`.** Calling `CCSGameRules::TerminateRound` needs the
game rules pointer and a verified calling convention for the float/reason arguments; neither is
in the engine surface, and a wrong guess crashes the server mid-match. Match keeps using
`set_round_termination_suppressed` (the detour that returns early), which is all it needs.

Plugin lines in `ru selftest` are not an API member: a plugin publishes the
`readyup.selftest.<name>` interface (`core/include/readyup/selftest_iface.h`).

`plugins/hello` (still requiring 1.0) now also registers `ru hello`, a hidden `.hellohide`, an
`sv_cheats` observer and an `on_frame` counter; the host test checks each one, including that
they disappear on unload.

### v1.9 (implemented)

| Member | Thread | Purpose |
|---|---|---|
| `license_player_line(self, buf, len)` | any | `Licensed to <licensee>` when `readyup_show_license 1` is set and the server's license key is valid and names a licensee; -1 otherwise (the default). The match plugin adds it to `.help`. Informational only |

`license` joins the reserved core `ru` subcommands.

### v1.10 (implemented)

| Member | Thread | Purpose |
|---|---|---|
| `set_core_setting(self, key, value)` | game | Runtime value of a core readyup.cfg setting that a platform pushes (FLEET.md §7.5 `server.config`): `chat_prefix` (`<Color>` tokens, max 64 bytes) and `status_http_token` (16..200 printable bytes; `status.json` is rewritten). Wins over readyup.cfg until cleared with `""`; not saved by the core (the plugin that set it restores it: match for `chat_prefix`, fleet for the token). Never logs the value |

### v1.11 (implemented)

Both members live in `plugin_loader.cpp` and touch no engine surface of their own (not in
`needs.json` `"api"` or `compat-report.py` `API_SURFACE`).

| Member | Thread | Purpose |
|---|---|---|
| `cvar_query(self, name, fn, user)` | game | Reads a cvar's current value. The core queues the bare name as a console command (what typing it in the console does) and answers from the `<name> = <value>` line the engine prints, seen by the logging listener; `Unknown command '<name>'!` or no answer within 3 s gives value NULL. fn runs on the game thread (normally the next frame). A query queued before a `<name> <value>` command reads the value before it. Needs `cmdbuf` and `loglistener` (`feature_state`); name is `[A-Za-z0-9_.]`, at most 256 pending |
| `selftest_summary(self, buf, len)` | any | The latest core selftest as JSON (`pass`, `passed`, `total`, `pending`, `failures`, `summary`, `ran_at`), the object the status endpoint shows; -1 before the first run |

Why asynchronous: a synchronous read needs `ICvar::FindConVar` / `GetConVarData` (vtable slots in
libtier0 plus the `ConVarData` value layout), new engine surface that moves with CS2 updates. The
console answer uses the command buffer and log listener the core depends on already. The match
plugin uses `cvar_query` for `reset_cvars_on_series_end` (`plugins/match/readyup/cvar_snapshot.h`),
fleet.so uses `selftest_summary` for `hello.selftest` / `server.selftest`, and `plugins/hello`
shows both (`hello_cvar <name>`, `hello_selftest`).

### v1.12 (implemented)

Grenade projectiles. Engine side in `core/src/readyup/grenades.cpp` (type table and request checks
in `grenade_spec.h`, unit-tested by `tests/grenade_spec_test.cpp`), members in `plugin_engine_api.cpp`.

| Member | Thread | Purpose |
|---|---|---|
| `grenade_spawn(self, spec)` | game | Spawns one projectile from a `ru_grenade_spawn` and returns the entity (NULL + a log line on refusal). `spec`: `struct_size`, `type` (`RU_GRENADE_SMOKE/FLASH/HE/MOLOTOV/INCENDIARY/DECOY`), `origin[3]`, `angles[3]`, `velocity[3]` (units/s), `angular_velocity[3]` (passed on; CS2 ignores it today), `owner_slot` (-1 = no thrower). The thrower is that player's pawn: kills, damage and blinds are credited to it and the projectile takes its team. Refused: unknown type, non-finite or off-map origin (±32768), speed components ≥ 10000, an `owner_slot` without a pawn, no map, or the type unavailable |
| `grenade_spawn_available(self, type)` | game | 1 if `grenade_spawn` can spawn `type` on this build. Check it first and tell the player "not available on this CS2 build" |

How it works: CS2's own projectile factories, `CSmokeGrenadeProjectile::Create`,
`CFlashbangProjectile::Create`, `CHEGrenadeProjectile::Create`, `CMolotovProjectile::Create`
(molotov item 46 and incendiary 48) and `CDecoyProjectile::Create`, called exactly the way CS2's
point_script `SpawnGrenadeProjectile()` calls them: `(origin, angles, velocity, angular velocity,
thrower pawn, item definition index)`, plus the team for smoke. The projectile then behaves like a
thrown one: it flies and bounces, HE / flash go off after their fuse, smoke / decoy once they stop,
molotov / incendiary on landing, and the game fires the usual `*_detonate`, `inferno_startburn`,
`player_blind` and `player_hurt` events with the thrower as `userid` / `attacker`.

The signatures live in the practice gamedata fragment (`gamedata/engine-surface.practice.json`,
shipped with `practice.so` next to the core's `engine-surface.json`). Each must match exactly once
and pass two anchors: `string_ref` of its projectile classname, and `caller_string`
`SpawnGrenadeProjectile` (a call from the script binding). The signatures cover the prologue's
argument moves, so a changed calling convention stops matching instead of being called wrongly.
None of them is in a plugin's `needs.json` `"surface"` (`API_SURFACE` maps both members to `[]`):
a CS2 update that breaks one fails the `practice` component in compat-report and turns that
grenade type off (`grenade_spawn_available` 0), it never disables a plugin. Without the fragment
(a server without the practice component) every type is unavailable.

Not in 1.12: a fuse / detonate-time override (throw later instead: schedule the call from
`on_tick`), and player view angles (standing a player where a grenade was thrown from is
`entity_set_abs_origin` on the pawn; 1.13 `player_teleport` also turns their view). Users:
`.scen` replays (`plugins/practice/practice_scenarios.cpp` `SpawnGrenade`) throw each recorded
grenade from its recorded spawn point and velocity, the practice rethrows (`.rethrow`, `.throwidx`,
`.rethrowsmoke`, ...) throw a player's own grenade again from the launch read off its projectile
(`m_vInitialPosition` / `m_vInitialVelocity`, `practice_plugin.cpp` `ScanProjectiles`), and `plugins/hello` has
`hello_nade <type> <slot> [speed]` (throws from that player's eyes the way they look).

```c
ru_grenade_spawn s = {0};
s.struct_size = sizeof(s);
s.type = RU_GRENADE_SMOKE;
s.origin[0] = x; s.origin[1] = y; s.origin[2] = z;  /* where the projectile starts */
s.velocity[0] = vx; s.velocity[1] = vy; s.velocity[2] = vz;
s.owner_slot = slot;                                /* or -1 */
if (!RU_API_HAS(api, grenade_spawn_available) || !api->grenade_spawn_available(api->self, s.type)) {
  /* "not available on this CS2 build" */
} else if (!api->grenade_spawn(api->self, &s)) {
  /* refused: the reason is in the server log */
}
```

### v1.13 (implemented)

Team changes, pawn teleports with view angles, and creating entities. Engine side in
`core/src/readyup/practice_engine.cpp` (feature table and request checks in
`practice_engine_spec.h`, unit-tested by `tests/practice_engine_spec_test.cpp`), members in
`plugin_engine_api.cpp`.

| Member | Thread | Purpose |
|---|---|---|
| `engine_feature_available(self, feature)` | game | 1 if `feature` works on this build: `"change_team"`, `"teleport"`, `"entity_create"` (create + spawn). Check it first and tell the player "not available on this CS2 build" |
| `player_change_team(self, slot, team)` | game | The player (bots too) joins `RU_TEAM_SPECTATOR` / `_T` / `_CT` through `CCSPlayerController::ChangeTeam`, what `jointeam` ends in (a live player on a team dies). 1 = done |
| `player_teleport(self, slot, origin, angles, velocity)` | game | `CCSPlayerPawn::Teleport` on the slot's live pawn: position, **view angles** (pitch / yaw / roll: how a plugin sets where a player looks) and velocity; a NULL part is left alone. Refused: bad slot, dead, non-finite / off-map values (±32768), pitch outside -90..90, speed components ≥ 10000 |
| `entity_create(self, classname)` | game | `UTIL_CreateEntityByName`: a new entity (`[a-z0-9_]`, not players / controllers / game rules / world), not spawned yet. Write its fields (`schema_offset`, `entity_set_abs_origin`), then `entity_spawn` it; remove it with `entity_remove` |
| `entity_spawn(self, entity)` | game | `CBaseEntity::DispatchSpawn(entity, NULL)` for an entity from `entity_create` (its handle must round-trip; not index 0..64) |

The four functions are in the practice gamedata fragment next to the grenade factories, each with
two anchors: `ChangeTeam` has `string_ref` `CSForceTeamThink` and `callee_string` `player_team`
(the event it fires); `UTIL_CreateEntityByName` / `DispatchSpawn` have `caller_string`s of the
entities their callers create (`chicken`, `info_deathmatch_spawn`, `info_hostage_rescue_zone_hint`);
`CCSPlayerPawn::Teleport` has no string of its own, so it has two `mov_disp` field reads. The two
virtuals are also `vtable_indices` entries (`CCSPlayerController::ChangeTeam` [104],
`CCSPlayerPawn::Teleport` [164], the slots CounterStrikeSharp uses), with `rtti` entries for both
classes: the RTTI-located vtable's slot must equal the verified function, and before every call the
controller's / pawn's vptr must be that vtable. As with grenades nothing here is a plugin's hard
need (`API_SURFACE` maps the members to the entity system only): a break turns that feature off
(`engine_feature_available` 0) and fails the `practice` component in compat-report. Verified on
CS2 1.41.8.4 and 1.41.8.5 (`readyup_sigcheck`).

Users: practice `.fas` / `.watchme` (everyone else to the spectators), `.showspawns` /
`.hidespawns` (a `beam` per competitive spawn: `m_fWidth`, `m_vecEndPos`, `m_clrRender`,
`m_lifeState` written before `entity_spawn`), the view angle of `.loadnade`, `.last`, `.back N`
and `.loadpos`; match `.spec`.

### v1.14 (implemented)

Respawning one player. Same engine module and feature table as 1.13 (`practice_engine.cpp`,
`practice_engine_spec.h`), member in `plugin_engine_api.cpp`.

| Member | Thread | Purpose |
|---|---|---|
| `player_respawn(self, slot)` | game | The dead player in `slot` (bots too) on T / CT respawns on their player pawn: `CBasePlayerController::SetPawn(pawn, true, false, false, false)` then `CCSPlayerController::Respawn`, as CounterStrikeSharp's `Respawn()`. 1 = respawned, 2 = already alive (nothing done), 0 = refused (bad slot, not on T / CT, no pawn) or unavailable. Feature `"respawn"` in `engine_feature_available` |

The entries are the core's (`engine-surface.json`), since match warmup needs them without the
practice component. `CCSPlayerController::Respawn` [276] is an anchors-only `vtable_indices` entry
(no byte signature): two `mov_disp` field reads and a `string_ref` of the network state-change
message; the call goes to the verified slot target after the controller's vptr check. SetPawn is a
signature (CounterStrikeSharp's) with two `mov_disp` and a `caller_string` `player_disconnect`.
Respawn works on the controller's current pawn, which is the observer pawn after a death, hence
SetPawn first. Both are optional (`API_SURFACE` maps `player_respawn` to the entity system only):
a break turns respawn off. The core declares `rtti` `CCSPlayerController` too; a fragment may
repeat an rtti entry with the same typeinfo name and module (the practice one does). Verified on
CS2 1.41.9.0 (`readyup_sigcheck`).

User: match warmup respawns a human who joins T / CT, or whom the roster team lock moved, instead
of restarting the warmup round (`mp_restartgame 1` stays the fallback without the feature or when
it refuses).

### ABI rules

1. **Plain C across the boundary.** No C++ classes, references, STL, `std::string` or
   exceptions. Plugins may use C++ inside but must catch everything before returning into the
   core. The core also catches anything thrown out of a callback and logs it, but a throw
   across the C boundary is a plugin bug.
2. **Fixed-width scalars** (`int`, `uint32_t`, `uint64_t`, `double`) and pointers only. Enum
   values travel as `uint32_t`; `int` is used as the boolean.
3. **No ownership transfer.** Strings and structs passed to a callback live only for that call.
   The core never frees plugin memory and plugins never free core memory. `user` pointers are opaque.
4. **Only `extern "C"` symbols cross.** Plugins export exactly the three entry points (hidden
   visibility for everything else) and import nothing from the core. They link with
   `--no-undefined`, and every core service is a function pointer in `ru_api`.
5. **`-fno-gnu-unique`** for C++ plugins (set by `readyup_add_plugin`). `STB_GNU_UNIQUE`
   symbols make `dlclose` a no-op, and a reload would then silently keep the old code. The
   core checks with `RTLD_NOLOAD` after `dlclose` and warns if the image is still mapped.
6. **Versioning.** `READYUP_PLUGIN_API_VERSION = (MAJOR << 16) | MINOR`.
   - **MINOR**: new members are appended to `ru_api` (or to a callback struct). Nothing is
     removed, reordered or changed in meaning. A plugin built for 1.0 runs on a 1.3 core. A
     plugin that wants a 1.2 member either requires 1.2 in `ru_plugin_info.api_version`
     (older cores refuse it) or checks `RU_API_HAS(api, member)` at runtime and degrades.
   - **MAJOR**: any incompatible change. The core refuses plugins with another major. Bump it
     rarely, and only together with the plugins in this repo.
   - Every struct begins with `uint32_t struct_size`. Readers check it before touching
     trailing fields.
7. **Compatibility policy.** A core release supports its own API major, and every minor up to
   its own. Plugins in this monorepo are always built with the core they ship with.
   Third-party plugins only need to be rebuilt on a major bump.

## 3. Threading rules

- **Every plugin callback runs on the server main thread**, the one that runs
  `ISource2Server::GameFrame`: load, unload, commands, events, ticks and posted tasks.
- **API functions** must be called from that thread (they are, when called from a callback).
  Off-thread calls are rejected and logged. The exceptions are `log` and `post_to_game_thread`,
  which are thread-safe.
- **Chat, console and event producers may run on any thread** (log listener, AddText caller,
  file tail thread). The core only **queues** there; delivery happens in the next GameFrame.
  As a result plugin code never runs inside the AddText detour or (except for raw
  `subscribe_game_event` callbacks, which are synchronous by design) the engine's event dispatch,
  and never while a core mutex is held (no lock-order problems between core and plugin
  locks). The cost is at most one frame of latency (about 15 ms at 64 tick).
- **Frame order**: original GameFrame → core frame work (event listener registration, entity
  system status) → `plugins::Frame`: pending load/unload/reload, then posted tasks, commands,
  events and log lines (FIFO per queue), then `on_tick` if simulating, then `on_frame` (every
  frame). Queues and ops also drain on non-simulating frames.
- **Worker threads** (DB, HTTP) belong to the plugin. It starts them in load, **joins them in
  unload**, and hands results back with `post_to_game_thread`.
- Queues are bounded (1024 commands / events, 4096 tasks). Floods drop the oldest events and
  refuse new commands and tasks instead of growing without limit.

## 4. Hot reload and its safety

`ru plugin list | load <name> | unload <name> | reload <name>` works from the server console,
RCON, and chat (`.ru plugin ...`, admins only). `reload` = unload + load of
`csgo/readyup/plugins/<name>.so` from disk.

**The op is deferred to a safe point.** The command only queues the request. It runs at the
top of `plugins::Frame` in the next GameFrame. At that point no plugin code is on the stack
(`g_depth == 0`), the engine is not inside AddText or event dispatch, and the core holds no
lock. A plugin that runs `server_command("ru plugin reload x")` therefore cannot unload
itself mid-callback.

**Unload sequence:**

1. Mark the plugin `unloading`. The core stops dispatching to it and refuses its `post_to_game_thread`.
2. Call `readyup_plugin_unload()`. The plugin joins its threads and frees its memory.
3. The core removes **every** registration the plugin owns (chat and console commands, ticks,
   subscriptions) and every queued task it posted, whether or not the plugin unregistered them itself.
4. Mark the handle dead, then `dlclose`, then check with `dlopen(RTLD_NOLOAD)` that the image
   is really gone (and warn if not).

**What that guarantees:**

- **No dangling hooks.** Plugins cannot hook anything. Every detour belongs to the core and
  outlives any plugin, so there is never a trampoline into an unmapped image.
- **No stale callbacks.** Queued commands and events are resolved by registration handle when
  they are delivered. Anything queued for the old image before the reload is dropped. It is
  never delivered to the new one, and never into unmapped code. (The host test checks this.)
- **No use-after-free on stale handles.** `ru_plugin` and the `ru_api` table are never freed
  (a few hundred bytes per reload). A plugin thread that wrongly outlives unload finds
  `alive == false`, and its calls become no-ops instead of touching freed memory. Its own
  code, however, is unmapped, which is why joining threads in unload is the one rule the core
  cannot enforce.
- **Failed loads leave nothing behind**: bad exports, version mismatch, name mismatch, or
  `load` returning non-zero.
- **Crash attribution.** While a plugin callback runs, the crash handler prints
  `crashed inside plugin "<name>"` before the backtrace.
- **State is not carried over** unless the plugin uses `stash_put`/`stash_get`. If the
  new file fails to load, the plugin stays unloaded. `dev-deploy.sh` keeps `<name>.so.prev`,
  so `mv` it back and reload.

**Skins-specific cleanup** (for when it becomes a plugin):
- Stop and join the loadout worker thread. Drop the per-player loadout cache and the
  "decorate next frame" lists.
- Never keep `CEntityInstance*` across frames. Keep entity handles and re-resolve them with
  `entity_from_handle`, so nothing points at entities freed while the plugin was unloaded.
- Paint, knife, glove and agent changes already applied stay on live entities. They are
  ordinary entity state and reset on respawn or map change. Unload does not try to revert them.
- The econ and entity primitives stay in the core. The skins signatures should move to a
  gamedata fragment (`engine-surface.skins.json`) that ships **only in the skins zip**. The
  default core then does not even resolve them, and the matching `ru_api` members return 0.

**Match-specific** (`plugins/match/readyup/reload_state.h`): `ru plugin reload match` keeps the
match going. Unload stops and joins every worker thread (webhook sender, store writer, admin
refresh, demo upload: an upload in progress is aborted and restarted by the next image), then
keeps one JSON document in the stash; load restores it before anything runs. What survives:

- the loaded match (full match context), the mode (warmup / knife / live / postgame / scrim /
  practice) and every ready state;
- map number, round, scores (log-derived and the stats model's team1/team2 score), the per-map
  stats model and event totals, halftime / overtime counters, series wins and map results;
- the knife round (phase, winner, pick deadline, deaths / HP so far) and the pause state;
- runtime settings: webhook / heartbeat / admins URLs, match token, `ru_warmup_*`,
  `ru_cfg_exec_enable`, `ru_dev_bots_scrim`, demo settings, series-end kick delays, `.ru mode idle`;
- undelivered webhook events, the demo recording in progress, a pending GOTV-flush stop and the
  pending postgame step (next map / series-end kick / unload), rescheduled with the time left.

Not kept: per-player UI throttles, the scrim countdown (it restarts), the round in progress in
the stats model (starts over empty), and log lines / events of the one frame no image was
loaded. The stash is core memory: a server restart still recovers through the match state persisted
in `state.json` (`local_store`, `match_recovery`). A cold load (no stash) does that restore.

While match is unloaded, round-termination suppression is lifted (unload turns it off) and chat
commands like `.r` are not routed; the core keeps working.

**Hibernation:** reload runs in GameFrame. If an empty server stops producing frames, a
queued reload waits until it wakes. Dev servers should run with `sv_hibernate_when_empty 0`.

## 5. Where the current modules go

Done. Core sources are in `core/src/readyup/` (the `readyup/` subdirectory keeps every
`#include "readyup/..."` unchanged), the shim entry points in `core/src/exports.{cpp,map}`,
vendored code in `third_party/`, the non-engine helpers in `libs/readyup/`, the match flow in
`plugins/match/readyup/` and skins in `plugins/skins/`.

### Core (`core/src/readyup/`)

| Files | Notes |
|---|---|
| `exports.cpp`, `real_server.*`, `path.*`, `disabled.*`, `banner.*`, `version.h`, `cs2_version.*` | shim, loading |
| `engine_surface.*`, `engine_surface_core.*`, `signature_scan.*`, `sigtest.*`, `sdk/*` | engine surface |
| `game_frame_hook.*`, `host_say_hook.*`, `client_command_hook.*`, `round_termination_hook.*`, `console_command.*` | hooks (`console_command` is a probe and can go) |
| `server_game_clients_hook.*` | the ClientCommand hook: runs the shared chat handler for `say` / `say_team` on builds that still send chat there |
| `chat_intercept.*`, `chat_rules.*` | the shared chat handler the Host_Say hook runs before the engine: routes plugin chat commands with the sender slot (`RU_CMD_HIDE` swallows the line) and relays chat name prefixes plugins set (match sets the admin / captain ones); `chat_rules` is the pure decision |
| `command_buffer_hook.*` | the AddText hook, `EnqueueServerCommand`, the core `ru` subcommands (`help`, `plugin`, `sigtest`, `selftest`, `reload`, `status_http`, `version`) and the dispatch of plugin console commands / `ru <sub>` / `RU_CMD_OBSERVE` |
| `game_events.*` | listener install, RTTI, registration (core events + what plugins subscribe to), normalized events, the per-slot controller map, team-for-slot, the human team table's `player_team` source |
| `log_receiver.*` | the file-tail fallback, the line fan-out to plugins and the log-derived normalized events |
| `chat.*`, `chat_colors.h`, `client_print.*`, `center_html.*` | output |
| `schema.*`, `entity.*` (was `skins_engine.*`) | schema and entity primitives (the generic half of skins) |
| `slot_registry.*`, `steamid.*` | identity and team registry |
| `ru_router.*`, `ru_help.*` | routing, dedupe, `.ru` / `.ru plugin` / `.ru selftest` / `.ru reload` / version; everything else goes to the plugin that registered it |
| `config.*` | core keys (debug, banner, chat_prefix, consume_ru_chat, status_http_*); plugins read their own |
| `admin_check.*` | `is_admin`: the plugin admin provider's answer |
| `status_feed.*`, `status_http.*`, `status_server.*` | local status endpoint (JSON / hub in `libs/readyup/status_snapshot.*`) |
| `selftest.*`, `features.*` | `ru selftest`, the feature table plugins query with `feature_state` |
| `logging.*`, `crash_handler.*` | infrastructure |
| `plugin_loader.*`, `plugin_engine_api.cpp` | plugin host, API implementation |

### readyup-match (`plugins/match/`)

**Done (step 4).** `plugins/match/readyup/`:

| Files | What |
|---|---|
| `match_plugin.cpp` | entry points; registers the player chat commands (`.r` & co, `RU_CMD_HIDE` when `consume_ready_chat=1`), the `ru` subcommands, the console settings (`ru_match_token`, `ru_webhook_url`, `ru_warmup_*`, `ru_demo_*`, ...), a `tv_delay` observer, ticks / frames, lifecycle events, log lines, raw engine events, the admin provider, `readyup.match.v1` and `readyup.selftest.match` |
| `host.*`, `engine.h`, `logging.h`, `players.h`, `workers.*`, `config.*` | the adapters: the functions the match flow called in the core (`Print`, `SendToChat`, `EnqueueServerCommand`, `ListHumans`, `FeatureEnabled`, ...) on top of `ru_api`; calls from worker threads are queued to the next frame. `workers` tracks every thread so unload can join them. Settings come from readyup.cfg (top level, as before, then `[match]`) and `cfg/ReadyUp/match.cfg`, re-read when a file changes |
| `modes.*`, `scrim_flow.*`, `pause_state.*`, `match_state.*`, `knife_tracker.*` | the flow: idle / scrim / warmup / knife / live / postgame / practice |
| `match_events.*` | the match half of the old `game_events.cpp`: round lifecycle, halftime / OT swaps, knife round end, per-map stats, damage totals (round events are handled on the frame's tick, after its log lines) |
| `match_log.*` | the match half of `log_receiver`'s `LifecycleImpl`: map number, scores, the log-driven knife round, the log fallback of the round lifecycle, connect / disconnect webhooks |
| `match_router.*`, `match_console.*` | chat / `ru` / console commands (the match parts of `ru_router.cpp` and `command_buffer_hook.cpp`, incl. `ru match load`) |
| `webhook.*`, `match_token.*`, `mat_admins.*`, `admins.*`, `admin_check.*`, `persisted_*.*`, `local_store.*`, `backup_files.*`, `match_recovery.*`, `match_config_parser.*` | platform link, admins (admins.json / the platform's `admins.set`), JSON persistence (`state.json`), boot recovery |
| `match_stats.*`, `match_end*.*`, `demo_*.*`, `game_timers.*` | stats model, map / series end, GOTV demos + upload, game-thread timers (`on_frame`, also on non-simulating frames) |
| `welcome.*`, `ready_hud.*` | the welcome card and the ready HUD (center HTML) |
| `match_status.*` | `readyup.match.v1`: the match part of `/status` |
| `reload_state.*` | what survives `ru plugin reload match` (§4) |

It links libcurl itself (static in release builds); the core does not. There is no database
(docs/FLEET.md D13): `local_store.*` keeps `state.json`, `admins.json` and `fleet-admins.json`
in the plugin data dir, written by one writer thread (atomic replace, versioned).

Behaviour changes that come with the move:

- Match console / chat commands run on the next frame (up to ~15 ms later), never inside the
  engine's AddText or event dispatch.
- The `jointeam` early trigger of the welcome card is gone (the ClientCommand hook only routes
  chat now); the card still shows on the team-switch log line / `player_team` event.
- Admin checks only read memory: `admins.json` is re-read in the background when it changes
  (checked every 30 s).
- Settings can also live in a `[match]` section or `cfg/ReadyUp/match.cfg`.

### readyup-skins (`plugins/skins/`)

**Done (step 3).** `skins_plugin.cpp` (entry points, `skins_status` / `skins_refresh` /
`skins_debug_as` console commands), `loadout.cpp` (was `weapon_paints.cpp`; loadouts from
`loadouts.json` or `skins.loadout`), `apply.cpp`,
`cosmetics.cpp`, `apply_internal.h`, `stattrak.cpp` (the `player_death` StatTrak bump that used
to sit in `game_events.cpp`), `legacy_paint_kits.inc` + `gen_legacy_paint_kits.py`,
`docs/json-contract.md`, `docs/engine-surface.md` (dev seed: `scripts/seed-dev-skins.py`). Gamedata:
`gamedata/engine-surface.skins.json` (below).

How it talks to the core, all through `ru_api` v1.1:

- per tick: `on_tick` drives the same controller/weapon walk as before (entity_by_index,
  entity_from_handle, entity_classname, schema_offset);
- paints / knives / gloves / agents: econ_attr_set_by_name, entity_change_subclass,
  entity_set_model, entity_set_bodygroup_by_name, entity_mark_changed;
- prefetch: raw `player_spawn`, `item_equip`, `item_pickup` (the core no longer listens to the
  last two); StatTrak: raw `player_death`;
- loadouts: `data_dir()/loadouts.json` (+ `stattrak.json`) on one worker thread that unload
  joins; in fleet mode `skins.loadout` / `skins.invalidate` over `readyup.fleet.v1` and
  `skins.stattrak` back (capability `skins.v1`).
- to other plugins: `readyup.skins.v1` (`core/include/readyup/skins_iface.h`): `paint_weapon`
  puts a paint kit on one weapon entity (the midas plugin's gold finish) and keeps the loadout off
  it until handed back with paint kit 0; `active` is 0 while skins is inert.

**Gamedata fragment.** The engine entries only skins uses (the econ/model functions and the
`CEntityInstance::NetworkStateChanged` slot) moved from `engine-surface.json` to
`gamedata/engine-surface.skins.json`. The core merges every `engine-surface.<name>.json` next to
the shim at load (entries may only be added, never overridden; an identical rtti entry is shared); the skins packages ship the
fragment, the others do not. `readyup_sigcheck` / `readyup_hookcheck` take fragments as extra
arguments and `scripts/ci/verify-cs2.sh` passes every fragment in `gamedata/`.
`dev-deploy.sh --plugin skins` installs the fragment too (a new fragment needs `--restart`).

### readyup-deathmatch (`plugins/deathmatch/`)

FFA / team deathmatch on CS2's deathmatch game mode ([DEATHMATCH.md](DEATHMATCH.md)).
`deathmatch_plugin.cpp` (commands, events, tick), `dm_rules.*` (settings, cvars, scoring, win
conditions, leaderboard HTML, weapon rounds, map choice; ctest `deathmatch_rules`). Only `ru_api`
calls that exist already: `server_command` for the cvars / `game_type` / map loads, raw
`player_death`, `center_html_*_prio`, `for_each_player`. Talks to the match plugin through
`readyup.match.v1` `set_external_mode` (v1.5: the match flow's `External` mode) and to essentials
through `readyup.essentials.v1` (`core/include/readyup/essentials_iface.h`: `default_map(mode)`
from `default_maps.json`, `load_map(entry)` with the Workshop download bar).

### Shared non-engine code (`libs/`)

`json_store.*` (versioned JSON files: atomic replace, corrupt files moved aside, flock),
`http_client.*`, `minijson.*`, `steamid.*` and `status_snapshot.*` (JSON value with an
order-keeping parser, merge-patch diff, the status hub). Skins compiles `json_store` and
`status_snapshot` in; match compiles `json_store`, `http_client`, `minijson`, `steamid` and
`status_snapshot`. Each plugin links in
what it needs, statically and with hidden visibility. This is source-level sharing: nothing
crosses the ABI, and none of it touches the engine. The core keeps only `minijson`, `steamid`
and `status_snapshot`, and needs no libcurl.

### Target monorepo layout

```
core/                     libserver.so shim: engine surface, hooks, events, loader, API impl
core/include/readyup/plugin_api.h   public C ABI (plugins include only this + the *_iface.h)
plugins/match/            readyup-match
plugins/skins/            readyup-skins (separately shippable)
plugins/hello/            example plugin
libs/                     shared non-engine code (json store, http, json)
gamedata/                 engine-surface.json (+ engine-surface.skins.json)
third_party/              funchook, distorm (licences kept)
tools/                    sigcheck, plugin host test, generators
scripts/ docs/ cfg/ .github/
```

The top-level CMake builds everything. Each plugin is its own target
(`readyup_add_plugin(<name> ...)` → `readyup_plugin_<name>` → `build/plugins/<name>.so`), so
`cmake --build build --target readyup_plugin_match` rebuilds one plugin without relinking the core.

## 6. Build, release and dev workflow

**Build:**

```bash
./build.sh                                  # build/libserver.so, build/plugins/hello.so, tests
(cd build && ctest --output-on-failure)     # offline plugin host test (load, commands, events, hot reload)
scripts/docker-build.sh                     # Debian 12 image -> build-docker/
BUILD_TARGET=readyup_plugin_hello scripts/docker-build.sh   # one plugin only
build/readyup_sigcheck ~/ru-analysis/libserver.so gamedata/engine-surface.json
```

`tools/plugin_host_test.cpp` links the real `plugin_loader.cpp` against stubs of the few core
functions it calls. It loads `hello.so`, dispatches chat and console commands and events,
ticks, then hot-swaps in a second build of hello (`build/test/hello.so`, version
`1.0.1-reloaded`), reloads it and checks that the new code runs, the old image is unmapped,
and stale queued commands are dropped.

**Installed layout:**

```
csgo/readyup/bin/linuxsteamrt64/libserver.so      core
csgo/readyup/bin/linuxsteamrt64/engine-surface.json
csgo/readyup/plugins/match.so                     every release
csgo/readyup/plugins/skins.so                     skins release only
csgo/readyup/plugins/<name>/                      plugin data dir (optional)
```

Every `*.so` in `plugins/` loads on the first server frame, in name order. To disable one,
rename it (for example `skins.so.off`) or `ru plugin unload` it. `READYUP_PLUGINS=0` disables
all of them. `READYUP_PLUGINS_DIR` overrides the directory (tests).

**Release** (`scripts/package-release.sh`, built and uploaded by CI on every run):

- component zips `ready-up-{core,match,skins,hello}-X.Y.Z-linuxsteamrt64.zip` (match: `match.so`
  plus the `cfg/ReadyUp/*.cfg` templates it execs), which the root `install.sh` mixes;
- bundles `ready-up-essentials-...` (core + match, the default, no skins) and
  `ready-up-full-...` (core + match + skins + the gamedata checkers);
- `SHA256SUMS`.

Every zip carries `readyup/manifests/<component>.json` (its file list). `scripts/ci/check-bundles.sh`
fails CI if core, match or essentials contain skins code or gamedata, if match / essentials /
full lack `match.so`, or if the core zip carries `match.so` or match code, and the `installer` job
runs `tests/installer/test_install.sh` against the built zips. Plugins carry the release version; the API version is separate.
CI runs sigcheck against the core only; plugins have nothing to check.

**Dev loop (no server restart):**

```bash
scripts/dev-deploy.sh --plugin match      # build only readyup_plugin_match, copy match.so to
                                          # readyup-test/game/csgo/readyup/plugins/, then send
                                          # `ru plugin reload match` into the ru-test tmux console
                                          # as cs2servermanager and print the result
scripts/dev-deploy.sh --restart           # core changes still need the full deploy + restart
```

`--plugin` renames the file into place (a running image is never overwritten), keeps
`<name>.so.prev`, and waits up to 15 s for `plugin: reloaded <name>` / `reload <name> failed`
in `console.log`.

## 7. Migration plan

Order matters. The big move must not land while `selftest`, `knife-hud`, `ci` and the parity
docs branches are still open, because every one of them touched `src/readyup/*` and `CMakeLists.txt`.

| Step | What | Effort |
|---|---|---|
| 0 | Plugin host, API v1.0, `hello`, host test, `dev-deploy --plugin` (this branch). Match and skins unchanged. | done |
| 1 | **Done.** One `git mv` commit to the target layout (`src/readyup` → `core/src/readyup`, `src/third_party` → `third_party`, `src/exports.*` → `core/src`, postgres/db_config/http_client/minijson/steamid → `libs/readyup`), with CMake, scripts, CI and docs paths updated. No code changes, so review is trivial and `git log --follow` keeps history. | 0.5 day |
| 2 | **Done.** API v1.1: the additions listed in §2 (players, center HTML, raw engine events, log lines, schema/entity/econ, terminate round, name prefix, admins, config, interfaces, stash) and `RouteChatCommand` carrying the sender slot (Host_Say already knows it). | 2–3 days |
| 3 | **Done.** Extract **skins** to `plugins/skins`: `libs/` for postgres/db_config, `skins_engine` → core `entity.*`, split out the skins gamedata fragment, replace `weapon_paints::GameFrameTick` / `MaybeRefreshAsync` call sites with `on_tick` and event subscriptions. Verify on server-4 (paints, knife, gloves, agents; reload mid-map). Release script: two zips. | 2–3 days |
| 4 | **Done.** Extract **match** to `plugins/match` (§5): `modes`, `webhook`, the match half of `game_events`, `ru_router`, `command_buffer_hook` commands, `log_receiver` lifecycle, scrim, welcome, HUD, admins, Postgres, demos, stats and the match part of the status endpoint (`readyup.match.v1`) are plugin code on API v1.2. `ru plugin reload match` keeps the match (§4); the core runs without match.so. Verified with `scripts/livetest` (match and scrim) on readyup-test, including a reload mid-warmup. | 5–7 days |
| 5 | Cleanup. **Done:** libpq/libcurl dropped from the core link (with step 4); CI builds every zip and runs the host test. Open: a guide for writing third-party plugins. | 1 day |

**Total: roughly 2–2.5 weeks of focused work** after step 1 is unblocked. Steps 3 and 4 are
independent once step 2 lands, so skins can ship as a plugin before match moves.
