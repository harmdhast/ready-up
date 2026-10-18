# Ready Up vs MatchZy Enhanced: parity matrix

This document lists what Ready Up must do before it can replace MatchZy Enhanced (formerly MatchZy Enhanced) on Auto Tournament servers.

**Decision (Sivert, 2026-09; replaces the earlier "no adapter, 1:1 RCON + webhook" plan):** Ready Up is fleet-first. The [fleet link](FLEET.md) (a WebSocket with `match.assign`, `match.update`, `cmd`, `server.config` and the `MatchState` stream) **replaces** the per-server RCON + webhook contract. The platform no longer sends `ru_loadmatch_url`, `ru_server_id`, `ru_bootstrap_*`, `ru_remote_log_*`, `ru_report_*`, `ru_tournament_status`, `ru_addplayer` or `css_*` commands to Ready Up, and none of them exist in the code. Each has a fleet equivalent, listed in the "How" column below. This file stays the **feature** checklist: what a tournament needs from the plugin, and how far Ready Up gets.

Audited against Ready Up `master` on 2026-09-29 (after #108, #109 and #110; the summary is counted from the rows below).

Status: **done** = the feature works, **partial** = it exists with a gap, **missing** = not implemented. Rows the fleet makes unnecessary are in [Not needed](#not-needed-fleet-replaces) and are not counted.
Stability: **stable** = proven on real servers over many matches, **tested** = covered by unit, integration or CI tests, **untested** = no automated coverage found, or an open in-game check.
Effort (remaining work only): **S** < 1 day, **M** 1–3 days, **L** > 3 days.
Engine column: **none** = logic, HTTP or built-in console commands/cvars only. **events** = engine game events Ready Up already hooks. **schema read/write** = a schema field. **fragile** = needs new signatures, offsets or entity writes; avoid where possible.

---

## Summary

Parity is (done + 0.5 × partial) / rows. Stable is the number of rows marked stable.

| Section | Rows | Done | Partial | Missing | Parity | Stable |
|---|---|---|---|---|---|---|
| 1. Link, enrollment and server setup | 9 | 9 | 0 | 0 | 100.0% | 0 |
| 2. Per-server settings | 14 | 14 | 0 | 0 | 100.0% | 1 |
| 3. Match load and lifecycle | 13 | 13 | 0 | 0 | 100.0% | 0 |
| 4. Status | 3 | 3 | 0 | 0 | 100.0% | 1 |
| 5. Demos | 5 | 5 | 0 | 0 | 100.0% | 0 |
| 6. Events and reports | 7 | 7 | 0 | 0 | 100.0% | 0 |
| 7. Player-facing and admin features | 27 | 27 | 0 | 0 | 100.0% | 3 |
| 8. Player stats | 7 | 7 | 0 | 0 | 100.0% | 0 |
| 9. ME features not previously listed | 16 | 16 | 0 | 0 | 100.0% | 0 |
| **Total** | **101** | **101** | **0** | **0** | **100.0%** | **5** (5%) |

Stable rows: minimum ready, engine version/status, `.ready`, knife / `.stay` / `.switch`, simulation.

## Milestones

- **M1: the platform drives one match end-to-end over the fleet link.** Blockers on the platform side: message schemas, gateway inbound/outbound, state store, driver and allocation, the normalizer, minimal admin commands, demo auth. On the Ready Up side: demo config over the link, a fleet live test, install through `install.sh`, one human Bo1 and one Bo3 with a restore.
- **M2: full parity and stability.** The remaining partials and missing rows below, practice extras, CI modes including a fleet path (there is no fleet CI yet).
- **M3: cutover.** Releases per the release plan (there is no GitHub release yet, so csm cannot install Ready Up; this is intentional until then), version check, csm installs Ready Up and the host agent, a mixed pool, the RCON path retired.

---

## 1. Link, enrollment and server setup

The platform used to send these as RCON commands (`ru_server_id`, `ru_bootstrap_*`, `ru_remote_log_*`, `ru_report_*`). The fleet link replaces them ([FLEET.md](FLEET.md) §3–§6).

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Auth and URL (token + endpoint) | Enrollment token, then a WSS link; credentials persisted (`fleet_client.cpp`, `fleet_store.cpp`) | done | tested | none | – |
| Server identity and bootstrap commands | Enrollment `install_id`; `server.config` and `admins.set` messages | done | tested | none | – |
| Remote log URL + auth header | Events go over the WSS link; no per-channel URL or header | done | tested | none | – |
| Retry queue that survives a restart | Disk spool (`fleet_spool.*`), replayed with seq/ack after a reconnect | done | tested | none | – |
| Match report (live match page) | `MatchState` via `state.patch` / `state.snapshot`, cmd `snapshot_now`, and the local `/status` endpoint | done | tested | none | – |
| `server.config` fields applied (all of FLEET.md §7.5) | Match plugin (`fleetstate::PlanServerConfig`, `OnServerConfig` in `fleet_bridge.cpp`): `hostname_format`, `scrim_knife`, `scrim_when_idle`, `chat_prefix` / `admin_chat_prefix` (server settings `ru_chat_prefix` / `ru_admin_chat_prefix`; the chat prefix goes to the core with ru_api 1.10 `set_core_setting`), `series_end_kick_delay.*`, `demo.path` / `name_format`, `warmup.*`; fleet.so: `offline_pause_minutes`, `status_http.token` (`server-config.json`) | done | tested | none | – |
| Server drain / undrain | `server.drain` / `server.undrain` in `fleet_bridge.cpp`: availability `draining`, `match.assign` refused as `busy`; in memory only | done | untested | none | – |
| `server.selftest` / `hello.selftest` | The core's latest selftest (`ru_api` 1.11 `selftest_summary`): `hello.selftest` carries it, `server.selftest` (reliable, proposed schema) goes out when the outcome changes (`fleet_plugin.cpp`) | done | tested | none | – |
| `server.cs2_update_required` | Steam UpToDateCheck (appid 730) every 30 min on a worker thread, once per required version (`cs2_update_check.h`) | done | tested | none | – |

## 2. Per-server settings

These arrived as bootstrap `commands[]` (`ru_<setting> <value>`). Now they are Ready Up server settings, set by `settings.set`, `server.config`, `.ru settings` or `readyup.cfg`.

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Minimum ready required | Setting `.readyrequired` / `settings.set`; match `min_players_to_ready`, fleet `rules.ready.min_per_team`. 0 = a full team (`server_settings.h`, `match_rules.h`) | done | stable | none | – |
| Allow force ready | `allow_force_ready` (match config / `readyup.cfg`) | done | tested | none | – |
| Pause after restore | `pause_after_restore` (state.json); match key / fleet `rules.pause.pause_after_restore` wins | done | tested | none | – |
| `.stop` command available / no damage | `stop_command_available`, `stop_command_no_damage`, `stop_vote_seconds` (votes.cpp); off under the valve ruleset | done | tested | none | – |
| Whitelist enabled default | Setting `.whitelist`; match `whitelist` / fleet `rules.whitelist` wins | done | tested | none | – |
| Kick when no match loaded | Server setting, default 0 | done | tested | none | – |
| Playout enabled default | Setting `.playout`; match `playout` / fleet `rules.playout` wins | done | tested | none | – |
| Roster display names | Roster players play under their match config names (`team1/2.players {steamid64: name}`, fleet `players[].name`): controller `m_iszPlayerName` + entity changed, put back within a second after the engine resets it (`roster_names.h`). Server setting `roster_names_enabled` (default 1); match `roster_names` / fleet `rules.roster_names` wins | done | tested (logic) | none | – |
| Reset cvars on series end | Server setting (default 1). The warmup cvars reset and the match's own `cvars{}` go back to their pre-match values, read at match load with `ru_api` 1.11 `cvar_query` and kept across a reload or restart (`cvar_snapshot.h`) | done | tested | none | – |
| Tactical pause via `.pause` | Server setting `use_pause_command_for_tactical_pause` (default 0) | done | tested | none | – |
| Autostart mode | Server setting `scrim_when_idle` (`ru_scrim_when_idle`, default 1; `server.config.scrim_when_idle`): off = an idle server stays idle when players join, until `.ru mode scrim` or a match (`scrim_flow.cpp`) | done | tested | none | – |
| Hostname format | Setting; `server.config.hostname_format` too. `{TEAM1}` `{TEAM2}` `{MATCH_ID}` `{MAP}` … | done | tested | none | – |
| Demo path and name format | `ru_demo_path`, `ru_demo_name_format` (`demo_recorder.h`); `server.config.demo.path` / `name_format` | done | tested | none | – |
| Series-end kick delays | Console settings; `server.config.series_end_kick_delay` (`match_end.h`) | done | tested | none | – |
| Catalog toggles (roundknife, playout, whitelist, settings, readyrequired) | cmd `settings.set`; `.ru settings show\|set\|default` | done | tested | none | – |

## 3. Match load and lifecycle

Replaces `ru_loadmatch_url`, `ru_addplayer`, `ru_removeplayer` and the `css_*` aliases the allocator called.

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Load a match from the platform | `match.assign` (resume variant too); failover resume in `fleet_bridge.cpp` | done | tested | none | – |
| Roster / team-name changes at runtime (`addplayer`, `removeplayer`, team names) | `match.update` ops (`fleet_state.cpp`) | done | tested | none | – |
| End and reset (`css_endmatch`, `css_restart`) | cmd `end_match`, then `match.unassign` | done | tested | none | – |
| Start / pause / unpause (`css_start`, `css_pause` family) | cmd `start` (`force` for the valve ruleset), `pause`, `unpause` | done | tested | none | – |
| Change map (`css_map`) | cmd `change_map` | done | untested | none | – |
| Swap teams (`css_switch`) | cmd `swap_teams` | done | untested | none | – |
| Round restore (`css_restore`) | cmd `restore_round`; inline backups over the WS (`event.backup`, `match_restored`) | done | tested | none | – |
| Practice on/off (`css_prac`, `css_exitprac`) | cmd `practice.set` | done | untested | none | – |
| Admin say (`css_asay`) | cmd `say` with `as_admin` | done | untested | none | – |
| Reload admins | `admins.set`; `ru_admins_url` + `.ru admins` locally | done | tested | none | – |
| Engine commands (`mp_restartgame`, …) | cmd `exec` (root admins only, validated) | done | tested | none | – |
| Force ready, kick, restart map, whitelist, plugins (fleet extras) | cmds `force_ready`, `kick`, `restart_map`, `whitelist.set`, `plugins.set` | done | tested | none | – |
| Load a match without the platform | Local `ru match load <url>` and MAT `.json` files still work | done | tested | none | – |

## 4. Status

Replaces the `ru_tournament_*` convars read over RCON.

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Tournament status / match / updated | `server.availability` + `MatchState` `phase`, `match_id`, `rev` | done | tested | none | – |
| Server configured / health | `hello` versions and `ping` health | done | tested | none | – |
| Engine `version` / `status` | engine; CS2 build in `hello` (`cs2_version.cpp`) | done | stable | none | – |

## 5. Demos

Recording is done. In fleet mode demos stream to the platform over the link (FLEET.md §12.2); without a platform they are kept and HTTP-uploaded as before.

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Recording per map | `tv_record` per map, name format (`demo_recorder.h`) | done | tested | none | – |
| GOTV gate for the valve ruleset | `EsportsGoLiveAllowed`; `start {force: true}` overrides | done | tested | schema read | – |
| Demo events (recording start/stop, upload start/success/fail/ended) | fleet `event.demo`; webhook payloads still exist locally | done | tested | none | – |
| Upload in fleet mode | Streamed while recording: fleet.so tails the `.dem` (`fleet_demo.h`), `demo.chunk` on the link's lowest-priority lane, resume from the platform's `demo.ack` offset, `demo.end {size, sha256}`, local file deleted only after the platform confirmed it (`demo_keep_hours`). Never HTTP-uploaded; without a fleet link nothing changes. The platform receiver (`demo.*.json`) is platform work | done | tested | none | – |
| Per-match `rules.demo.record` / `.upload` | MAT `demo_record` / `demo_upload`: record wins over `ru_demo_recording_enabled`; upload false = no stream, no HTTP upload (`fleet_state.cpp`) | done | tested | none | – |

## 6. Events and reports

The platform normalizer converts the fleet payloads; the AT webhook shapes stay for the local webhook only.

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Series start / end | `event.phase`; `series_end` winner object in the fleet `series_end` payload | done | tested | none | – |
| Round end / map result (full player stats) | fleet `event.round_end`, `event.map_result`, stats from `match_stats.h` | done | tested | none | – |
| Backup loaded | `event.backup`, `match_restored` | done | tested | none | – |
| Pause / unpause | `event.pause`, `MatchState` pause block | done | tested | none | – |
| Side pick | `event.phase`; `rules.knife.side_pick_seconds` | done | tested | none | – |
| Admin call | fleet `event.admin_called` | done | tested | none | – |
| `map_number` base | Fleet and webhook are 1-based, AT and the platform 0-based; the platform normalizer converts | done | tested | none | – |

## 7. Player-facing and admin features

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| `.ready` / `.unready` | `ru_router.cpp`; ready HUD (`ready_hud.cpp`) | done | stable | none | – |
| `.forceready` | `match_features.cpp`; cmd `force_ready` | done | tested | none | – |
| `.start`, `.restart`, `.endmatch`, `.forcepause`, `.forceunpause` | Chat shortcuts of `.ru match start\|restart\|end\|pause\|unpause` | done | tested | none | – |
| `.pause`, `.tech`, `.tac`, limits | `match_features.*`; typed pause types incl. `halftime`, `auto_5v5` | done | tested | none | – |
| Knife round, `.stay`, `.switch`, `.swap`, `.ct`, `.t` | `knife_tracker.cpp`, `ApplyKnifeSideChoiceLocked` | done | stable | none | – |
| `.gg` vote | votes.cpp; off under the valve ruleset | done | tested | none | – |
| Forfeit when a team leaves | `forfeit_after_seconds` (default 240) | done | tested | none | – |
| Auto-ready | Server setting; match `autoready` / fleet `rules.ready.autoready` | done | tested | none | – |
| `.stop` restore vote | votes.cpp | done | tested | none | – |
| Round restore `.restore <n>` | `round_restore.*`, one path with `.stop` and fleet `restore_round` | done | tested | none | – |
| Remote backup (`ru_remote_backup_url`, `ru_loadbackup_url`) | Replaced by inline backups over the WS + `restore_round` | done | tested | none | – |
| Crash / restart recovery | `match_recovery.*`, `state.json`; the live map's stats continue | done | tested | none | – |
| Whitelist, team enforcement | `EnforceWhitelistLocked`, `MaybeForceRosterTeamsLocked` | done | tested | none | – |
| Team names in game | `mp_teamname_1/2`, `mp_teamflag_1/2` | done | tested | none | – |
| Coaches | CS2 coach slot (`coach.h`); the in-game check is still open | done | tested | schema write | – |
| Match-scoped admins, `.help`, `.ru version` | `match_config_parser.cpp`, `.ru` router | done | tested | none | – |
| Workshop maps in `maplist` | `map_names.cpp` | done | tested | none | – |
| Wingman | `wingman.h`; `.coach` is not refused in wingman | done | untested | none | – |
| Simulation mode | `simulation.h`; `scripts/livetest --simulation` (CI) | done | stable | none | – |
| Damage report | Native: `damage_report.h`, `damage_ledger.h`, `damage_votes_test` | done | tested | events | – |
| Practice: `.prac`, `.bot`, `.boost`, `.spawn`, `.savepos`, `.rethrow`, `.god`, `.clear`, `.noflash` | `plugins/practice` | done | tested | none | – |
| Practice `.match` | `.match` = `.exitprac` while practice is on (admins) | done | untested | none | – |
| `.ruversion` for players | Public aliases of `.ru version`: `.ruversion`, `.version` (ME's chat name); console `ru_version` (`CoreChatAliasToRu` / `CoreConsoleAliasToRu`, `ru_help_test`) | done | tested | none | – |
| `.map` / `.reloadmap` aliases | essentials: `.map <name, workshop id or link> [force]`, `.reloadmap [force]` (admins) | done | untested | none | – |
| `.rcon` | essentials: `.rcon <command>` for `admins.json` admins only (the server's own "root" list; not match or platform admins, who have the platform's root-only cmd `exec`), logged with who ran it; refuses `quit` / `exit` / `_restart` / `restart` / `killserver` / `shutdown`, `sv_setsteamaccount`, `rcon_password`, `rcon`, `readyup_license_key`, `alias` and the fleet link's commands in any `;` part (`RconRefusal`, `essentials_rules` test). ME's reply "Command sent successfully!" | done | tested | none | – |
| Min spectators to ready (`min_spectators_to_ready`) | Match spectators type `.ready`; that many are needed before go-live | done | tested | none | – |
| Skins | Ready Up extra, not in the default release | done | tested | none | – |

## 8. Player stats

The whole AT `PlayerStats` set is computed in `match_stats.h` (tested in `tests/match_flow_test.cpp`). The old note that stats are lost on a crash is stale: `match_recovery` / `state.json` continue them. Still true: an admin's raw `mp_restartgame` during a live map keeps the rounds before it.

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Kills, deaths, assists, flash assists, team kills, suicides, knife kills | Game event `player_death` | done | tested | events | – |
| Damage, utility damage | `player_hurt` (capped, no team damage) | done | tested | events | – |
| Flashes, plants, defuses, MVP, score | `player_blind`, `bomb_*`, `round_mvp`, `m_iScore` | done | tested | events | – |
| 1k–5k, 1vN, first kills / deaths, trades, KAST | `StatsAccumulator`; KAST as percent | done | tested | events | – |
| Stats rewind on round restore, continue after a crash | `stats::RewindTo`, `match_recovery.*` | done | tested | none | – |
| Match stats as one JSON line (`get_match_stats`) | `ru_match_stats [matchid]`: one console line `match_stats {...}` with the current map's stats in the round_end / map_result team shape (`team1` / `team2` with `players[{steamid, name, stats}]`, AT `PlayerStats` incl. `kast` %), which is what the platform parses (`matchEventHandler.ts` reads `stats.kills`, `damage`, `rounds_played`, `kast`, `mvp`, ...); the platform itself never called `get_match_stats` (it takes stats from the events). ME's per-match DB dump `{match, maps, players}` has no Ready Up equivalent (no database, D13): another match id gets ME's "No stats found" (`at::MatchStatsJson`, `match_flow_test`) | done | tested | none | – |
| Reload config (`at_reload_config`) | `.ru reload` / `ru reload` re-reads readyup.cfg (plugins re-read their own settings files when they change); aliases `ru_reload_config` (console) and `.reload_config` (chat, admin) like ME's `matchzy_reload_config` / `.reload_config`. Ready Up has no `config.cfg` to exec: its settings are readyup.cfg + `state.json` | done | tested | none | – |

## 9. ME features not previously listed

Features of the previous (ME) plugin that the first version of this file did not cover.

| Feature | How (fleet message / Ready Up command) | Status | Stability | Engine | Effort |
|---|---|---|---|---|---|
| Practice grenade history: `.last`, `.back N`, `.lastindex`, `.delay`, `.throw` (alias of `.rethrow`) | Per-player history from `grenade_thrown` (position, eye angles, kind; ME numbering, max 100) plus each throw's launch from its projectile (row below). `.delay` makes every rethrow of that grenade wait. `practice_tools.h` | done | tested | events | – |
| Per-player rethrow: `.rethrow` / `.rt` / `.throw`, `.throwidx N [M ...]` / `.throwindex`, typed `.rethrow{smoke,flash,nade,grenade,molotov,decoy}` / `.throw{...}` | Your own grenade again, from where it left your hand with the same velocity, you as the thrower (damage, blinds, team), after its `.delay`: ru_api `grenade_spawn` (1.12, row below). The launch comes from the projectile, not `grenade_thrown` (which has neither): a scan every 8th tick (every tick for 8 ticks after a throw) reads `m_vInitialPosition` / `m_vInitialVelocity` of each player-owned `*_projectile` (`m_hThrower`) and fills that player's pending history entry; a projectile nobody threw (a bot's `hello_nade`) gets an entry of its own, the plugin's own rethrows and `.scen` replays none. `.throwidx` throws every index given; ME's messages. `.rethrow` falls back to the server-wide `sv_rethrow_last_grenade` when the launch was not seen or the type cannot be spawned on this build. No engine surface beyond 1.12, schema reads only. In-game: bot throws rethrown from the recorded point with the recorded velocity (read back from the new projectile), thrower credited | done | tested | schema read/write | – |
| Grenade projectile spawning (engine side of per-player rethrow and scenario replays) | ru_api `grenade_spawn` / `grenade_spawn_available` (1.12): smoke, flash, HE, molotov, incendiary, decoy with position, velocity and thrower (damage / blind credit, team). The five `C*Projectile::Create` functions (what CS2's own point_script `SpawnGrenadeProjectile` calls; flash included, ME used CreateEntityByName) are the practice gamedata fragment `engine-surface.practice.json`, each verified with anchors (its classname + that call site). A CS2 update that breaks one fails the practice component in compat-report and turns only that type off. `.scen` replays throw their recorded utility with it | done | tested | fragile | – |
| Lineup library: `.savenade` / `.sn`, `.loadnade` / `.ln`, `.listnades` / `.lin`, `.importnade` / `.in`, `.deletenade` / `.delnade` / `.dn`, `.globalnades` | Per map in `<plugin data dir>/lineups/<map>.json` (owner = SteamID64 or `default` for the global ones, like ME's savednades.json; written by a writer thread). Position, eye angles, grenade kind (of your last throw), description; ME's nearest-name load (Dice, with a 0.25 floor) and import code. `.loadnade` puts you there looking the saved way (ru_api 1.13 `player_teleport`, `CCSPlayerPawn::Teleport` from the practice gamedata; without it the reply gives the `setang`). One difference: ME also switched to the grenade with a client `slotN` command, which a server cannot send; the reply says which grenade to take. `practice_lineups.h` | done | tested | fragile | – |
| `.bestspawn` / `.worstspawn`, `.bestctspawn` / `.worstctspawn`, `.besttspawn` / `.worsttspawn` | Teleport to the closest / farthest competitive spawn (lowest priority) of the team (`entity_set_abs_origin`) | done | tested | schema read/write | – |
| `.showspawns` / `.hidespawns` | ME's markers: a `beam` from each competitive spawn 100 units up, CT blue / T orange, removed by `.hidespawns`, on leaving practice and with the map. ru_api 1.13 `entity_create` + `entity_spawn` (`UTIL_CreateEntityByName` / `CBaseEntity::DispatchSpawn` in the practice gamedata, verified on 1.41.8.4 / 1.41.8.5); without them the reply gives the spawn counts and points at `.spawn N`. Not seen in game yet | done | untested | fragile | – |
| `.impacts`, `.traj` / `.pip`, `.solid`, `.break`, `.timer` | ME cvar toggles (`sv_showimpacts`, `sv_grenade_trajectory_prac_pipreview`, `mp_solid_teammates` 1/2) tracked from prac.cfg's values (`ru_api` has no cvar read); `.break` removes `func_breakable(_surf)` and damageable `prop_dynamic` with health > 0 (`entity_remove`, no break effect); `.timer` stopwatch in the center panel. Private replies | done | tested | schema read/write | – |
| `.fas` / `.watchme` (everyone else to spectators) | Practice: every other human to the spectators (bots stay, like ME) with ru_api 1.13 `player_change_team` (`CCSPlayerController::ChangeTeam`, vtable slot checked); "not available on this CS2 build" without it. Not seen in game yet | done | untested | fragile | – |
| `.dry` / `.dryrun` | Admin, practice only: `bot_kick`, ME's dry-run defaults (+ optional `cfg/ReadyUp/dryrun.cfg`), `mp_restartgame`; the round's end (or `.dryrun` / `.prac` again) brings prac.cfg back. `readyup.practice.v1` `dry_run` lets the match plugin stop suppressing that round's end. Also `ru practice dryrun` | done | tested | events | – |
| `.noblind` | Done as `.noflash` | done | tested | none | – |
| `.spec` | Moves the player to spectators unless they are on a loaded match's roster (they would be put back). ru_api 1.13 `player_change_team`; before it `.spec` always answered "could not move you" (the old `ForceJoinTeamForSlot` stub, still off for roster enforcement) | done | untested | fragile | – |
| `.rk` alias | Alias of `.roundknife` (admins) | done | tested | none | – |
| Warmup settings (`at_warmup_*`) | Done as `ru_warmup_*` (`match_console.cpp`) | done | tested | none | – |
| Chat reminders | Done differently: ready HUD, go-live cards | done | tested | none | – |
| Admins URL etc. (`at_admins_url`) | Done as `ru_*` | done | tested | none | – |
| `at_version` | `ru_version` on the console, `.version` / `.ruversion` in chat (= `ru version`: "Ready Up <build>"); see `.ruversion` in §7 | done | tested | none | – |

## Not needed (fleet replaces)

Rows from the old RCON + webhook contract. Not counted in the summary.

| Old item | Why |
|---|---|
| RCON reply strings (`queued_match=`, `cleared_queued_match=`, `successfully`, `"…" = "…"`) | Fleet replies are `cmd.result` and `match.assign` acks. |
| `ru_clear_event_queue`, `ru_get_pending_events` | The spool belongs to the link. |
| Queue semantics and `ru_clear_queued_match` | Queueing moves to the platform (FLEET.md §7.1). |
| `ru_loadmatch <file>` | `match.assign` carries the config. |
| `get5_check_auths` | Ready Up always checks SteamIDs. |
| `ru_config_scope` | One install per enrollment. |
| `css_skipveto`, `map_picked`, `map_vetoed` | The veto runs in the browser. |
| `_next_match` (`ru_tournament_next_match`) | No queue in the plugin. |
| `test_event` / `.testevent` | `hello` and `ping` cover the connection test. |
| `team_ready`, `all_players_ready` | `MatchState` ready counters. |
| `pause_requested`, `player_stats_update` | AT never sent them. |
| Heartbeat (`ru_heartbeat_url`) | The WSS `ping`. |
| Stats DB (SQLite/MySQL) | The platform stores stats from events. |
| `sm_pause` / `sm_unpause`, `get5_status`, `get5_web_available` | Not used. |
| `at_check_for_updates`, `css_sleep`, safe auto-updater | Handled by csm. |
| In-plugin map veto | See above. |

## Extras (not in the AT plugin)

`player_gg`, `match_forfeit`, `recover_requested` (webhook events the platform ignores), `.admin` (call an admin), the go-live card, the ready HUD, Skins, the Midas gold addon, Deathmatch, Whitelist and Essentials plugins.

## Remaining work

1. Platform side of demo streaming: the `demo.begin` / `demo.chunk` / `demo.end` receiver answering `demo.ack` (FLEET.md §12.2).
2. Fleet path in CI. In progress.
3. In-game checks of the ru_api 1.13 users: `.fas` / `.watchme`, `.spec`, `.showspawns` beams, `.loadnade` / `.last` view angles.
