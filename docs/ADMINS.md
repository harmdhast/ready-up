# Admins

Ready Up has no database (docs/FLEET.md D13). Who counts as an admin:

- the per-match `admins` of the loaded match config,
- the MAT admin list (`ru_admins_url`), if configured,
- `admins.json` of the essentials plugin (`plugins/essentials/admins.json`), always,
- **fleet mode** (`[fleet] url` set): also the platform's fleet-wide list.

## `admins.json`

`game/csgo/readyup/plugins/essentials/admins.json` (the essentials plugin; on its first load it copies an
older `plugins/match/admins.json` there)

```json
{
  "version": 1,
  "admins": [
    { "steamid64": "76561198000000000", "name": "alice" }
  ]
}
```

`ru admins add|remove` write it for you. You can also edit it by hand: Ready Up re-reads it
within 30 s when it changes. Write it atomically (to a temp file, then rename). A file that is
not valid JSON or has no `"version"` is moved to `admins.json.corrupt-<time>`, logged, and Ready
Up starts with no admins from the file. The server console can always add admins again.

Each server has its own file. To share admins between servers, copy the file, point them at the
same MAT admin list (`ru_admins_url`), or use fleet mode.

## Fleet mode: the platform's list

In fleet mode the platform sends the whole admin list (`admins.set`, D5). Ready Up caches it in
`game/csgo/readyup/plugins/match/fleet-admins.json`, so a server that starts while the platform
is unreachable still knows its admins. `admins.json` still counts on top of it: a local admin
(the server owner) stays an admin even when the platform sends no list or a list without them.
`ru admins list|add|remove` work on `admins.json`; the platform's list is edited on the platform.

## Commands

`ru` commands are main commands with subcommands: `.ru <command> <subcommand> [args]` in chat,
`ru <command> <subcommand> [args]` on the server console / RCON, or `ru ...` in a player's own
game console (like Metamod's `meta` / CounterStrikeSharp's `css_` commands: same admin checks,
nothing shows in chat). `.ru help` lists the main commands, one chat line each; `.ru help
<command>` (or just `.ru <command>`) lists its subcommands. An unknown command answers "unknown
command, type .ru help". Help, state and errors go to the sender only; actions that concern
everyone (a pause, a map change) are announced to all.

**Every `.ru` command is admin-only** (in chat and in a player's own console), except:

- `.ru` and `.ru version` (the Ready Up build), `.ru help` and `.ru list` (the same list: for a
  player who is not an admin, only the commands below),
- `.ru dm status|top|hud` (and `.ru help dm`): deathmatch has no player chat commands.

Anyone else gets a private "not authorized" and nothing runs: the core checks it before the
command reaches the core or a plugin, whatever the plugin itself checks (plugins keep their own
checks too). The server console / RCON always may. Players have their own chat commands for
everything else (`.r`, `.pause`, `.stay` / `.switch`, `.settings`, `.coach`, `.admin`, `.help`,
...). An admin's `.help` points at `.ru help`. The public list is `RuCommandPublic` in
`core/src/readyup/ru_help_text.cpp`; add to it only a command players have no other way to run.

| Command | Who | Does |
|---|---|---|
| `.ru match load <url>` | admin | loads a match config (http/https; the URL is visible in chat) |
| `.ru match start [force]` | admin | force-starts the loaded match (`force`: also under the valve ruleset without GOTV, [ESPORTS-MODE.md](ESPORTS-MODE.md)) |
| `.ru match restart` | admin | the loaded match back to its warmup; everyone readies again |
| `.ru match end` | admin | ends the loaded match (`series_end` winner none) and resets the server |
| `.ru match recover [round]` | admin | asks the platform to recover the match (`recover_requested`) |
| `.ru match restore <round>` | admin | plays `<round>` of the current map again from its start (CS2's round backup); `.restore <round>` in chat. Paused afterwards until both teams `.unpause` (`ru_pause_after_restore 0`: live after 3 s) |
| `.ru match backups` | admin | the loaded match's round backups on this server (`ru_listbackups` on the console) |
| `.ru match pause` / `unpause` | admin | admin pause / unpause (`.fp` / `.fup` in chat) |
| `.ru match tech\|tac team1\|team2` | admin | technical pause / tactical timeout for a team, with its limits |
| `.ru match side stay\|switch\|ct\|t` | admin | knife side pick for the knife winners (players: `.stay` / `.switch`) |
| `.ru match coach <player> team1\|team2\|ct\|t` / `uncoach <player>` | admin | makes a spectator a team's coach / stops it (`<player>`: SteamID, `#userid` or name; players use `.coach ct\|t` / `.uncoach`, [below](#coaches-coach)) |
| `.ru match state` / `rules` | admin | match and mode state / effective rules |
| `.ru match swap` | admin | swaps the teams' sides in warmup (`mp_swapteams`); team1 / team2 stay who they are (`.switch` when no knife pick is pending) |
| `.ru match team1\|team2 <name>` | admin | renames a team of a match loaded with `ru match load` (fleet matches: rename on the platform); the in-game names follow |
| `.ru settings show` | admin | every [match server setting](INSTALL.md#match-server-settings) and where it comes from (players: `.settings`) |
| `.ru settings set <setting> <value>` / `default <setting>` | admin | change one (saved across restarts) / back to readyup.cfg or the default |
| `.admin [message]` | everyone | calls an admin ([below](#calling-an-admin-admin)); not an `ru` command |
| `.ru map change <name\|workshop id\|link> [force]` | admin | `changelevel <name>`, or `host_workshop_map <id>` for `3084291314`, `ws:<id>`, `workshop/<id>[/name]` or a pasted Workshop link (`…/filedetails/?id=3084291314`); refused during a knife round or a live map unless `force` (essentials plugin) |
| `.ru map reload [force]` | admin | loads the current map again (a workshop map by its id) (essentials plugin) |
| `.ru map restart [force]` | admin | restarts the game (`mp_restartgame 1`); a loaded match stays loaded (essentials plugin) |
| `.ru map defaults` / `.ru map default <mode> [<map>\|clear]` | admin | the default map per mode (`ffa`, `tdm`, `practice`, `warmup`, `retakes`, ...), `plugins/essentials/default_maps.json` (essentials plugin) |
| `.ru mode show` | admin | the current mode |
| `.ru mode idle` / `practice` / `scrim` | admin | plain CS2 / practice mode (toggles, `.prac`; needs the practice plugin) / auto scrim warmup back on |
| `.ru practice on\|off\|status` | admin | practice plugin: practice mode |
| `.ru dm ffa\|tdm [map]` / `.ru dm off` | admin | deathmatch plugin: free for all / team deathmatch (CS2's deathmatch game mode; loads the map given, else the mode's default map) / back to competitive; `.ru dm status\|top\|hud`: everyone, the only public plugin `.ru` commands ([DEATHMATCH.md](DEATHMATCH.md)) |
| `.ru admins list` | admin | the admins (essentials plugin) |
| `.ru admins add\|remove <steamid64\|name_fragment>` | admin (standalone) | edit `admins.json` |
| `.ru hud test <1-11>` | admin | a HUD test panel, to you only |
| `.ru whitelist on\|off\|add\|remove\|list\|clear` | admin | whitelist plugin: only listed players may stay (not during a match) |
| `.ru tv [status]` / `.tv` | admin | tv plugin: GOTV port, delay, clients, insecure observers, password set, the profile in use |
| `.ru tv set\|match <key> <value\|default>` | admin | tv plugin: server setting (saved) / this match only. Keys `delay_seconds`, `allow_insecure_observers`, `max_clients`, `relay_text_chat`, `relay_voice` ([design/tv-plugin.md](design/tv-plugin.md)) |
| `.ru tv caster add\|remove <steamid64>` / `list` / `clear`, `.ru tv reapply` | admin | tv plugin: casters who may join as spectators; send every GOTV value again |
| `.ru midas` / `.ru midas give\|take <player>` | admin | midas plugin: who is Midas and why (midas_steamids, given, best player); give Midas to a player (again: takes it back) / take a given Midas back (`<player>`: part of the name or a SteamID64; kept in `plugins/midas/given.txt`) |
| `.ru plugin list\|load\|unload\|reload <name>` | admin | plugins (core); load / unload last until a restart |
| `.ru plugin enable\|disable <name>` | admin | load / unload a plugin and keep it that way after a restart (`csgo/readyup/plugins/plugins.json`) |
| `.ru reload` | admin | reloads `readyup.cfg` (core); also `.reload_config`, and `ru_reload_config` on the console (the old plugin's `matchzy_reload_config` / `at_reload_config`) |
| `.ru selftest` | admin | core |
| `.ru` / `.ru version` / `.ru help` / `.ru list` | everyone | core: the build / the commands you can use. `.ruversion` / `.version` = `.ru version`; `ru_version` on the console (the old plugin's `matchzy_version` / `at_version`) |
| `.rcon <command>` | `admins.json` admins | essentials plugin: runs a server console command (the old plugin's `.rcon` / `css_rcon`). Only the server's own admins (`admins.json`), not a match config's admins or the platform's list (the platform has root-only cmd `exec`). Logged with who ran it. Refused: `quit`, `exit`, `_restart`, `restart`, `killserver`, `shutdown`, `sv_setsteamaccount`, `rcon_password`, `rcon`, `readyup_license_key`, `alias` and the fleet link's commands, in any `;`-separated part |

Chat shortcuts (admins; each one runs the `.ru` command it stands for, with the same checks):

| Shortcut | Runs |
|---|---|
| `.start` / `.forcestart` | `.ru match start` |
| `.restart` / `.rr` | `.ru match restart` (back to warmup; the match stays loaded) |
| `.endmatch` / `.forceend` | `.ru match end` |
| `.team1 <name>` / `.team2 <name>` | `.ru match team1\|team2 <name>` |
| `.switch` / `.swap` | `.ru match swap` when no knife pick is pending (during a pick they stay the knife winners' side choice) |
| `.settings` | `.ru settings show` (anyone) |
| `.readyrequired <n>` | `.ru settings set minimum_ready_required <n>` |
| `.playout` / `.roundknife` / `.whitelist` `[on\|off]` | toggle (or set) `playout_enabled_default` / `knife_enabled_default` / `whitelist_enabled_default` |
| `.asay <message>` | the message in chat with the admin prefix |
| `.prac` / `.exitprac` | practice mode on / off (practice plugin) |

Before this layout the match commands were flat (`ru start`, `ru end`, `ru idle`, `ru state`,
`ru side`, `ru fp`, ...). Those names are gone: use the table above. `ru match load <url>` is
unchanged.

Notes:
- If no admins exist yet, the **first admin must be added from the server console** (or in
  `admins.json`).
- SteamID input is **SteamID64** (decimal) or a connected-player name fragment.

## Coaches (`.coach`)

A coach is a spectator who belongs to a team: CS2's own coach slot (`sv_coaching_enabled`, the
controller's `m_iCoachingTeam`). CS2 counts them as a member of that team for voice and team
chat and lets them spectate only that team (`mp_forcecamera 1`). They follow the team through the
knife `.switch`, halftime and overtime, and never take a player slot: they are not in the ready
gate, the auto_5v5 count or the forfeit check.

- Coaches listed per team in the match config coach that team as soon as they are spectators:
  MAT / get5 `team1` / `team2` `coaches` (`{"<steamid64>": "<name>"}` or `["<steamid64>"]`), the
  fleet role `coach` in a team. The top-level MAT `coaches: [...]` (no team) type `.coach ct|t`.
- `.coach ct|t` (or `.coach` alone for a listed coach) / `.uncoach` in chat. Rostered players
  never coach. Outside scrims only listed coaches can; in scrims anyone who is not playing can.
- Admins: `.ru match coach <player> team1|team2|ct|t` (also someone not in the config; the
  whitelist then lets them stay) and `.ru match uncoach <player>`.
- Unlisted coaches (scrim, admin): at most `coaches_per_team` per team (match config, default 2).
- A coach who joins CT / T is told to go back to Spectators; they coach again once there.
- The ruleset can keep coaches out: `valve` online (`lan` false, `coaches_online` false).
- `.ru match state` lists the coaches.

## Calling an admin (`.admin`)

Any player (roster, spectator, scrim, any mode) can type `.admin [message]` in chat:

- The caller gets a private "admins notified.". Each player can call once every
  `admin_call_cooldown_s` seconds (`readyup.cfg`, default 60, 0 = no cooldown); a call during the
  cooldown only answers "you can call again in Ns".
- Every admin in game gets a private chat line (`ADMIN CALL <name> (<team>, <side>) needs an
  admin: <message>`) and a center card for about 6 s ("ADMIN CALLED", same text) at
  `RU_HTML_PRIO_ALERT` ([HUD.md](HUD.md)).
- The server console logs `admin-call: <name> (<steamid64>, <team>): <message> [<call_id>]`.
- The platform gets an `admin_called` event: through the webhook pipeline (same events URL,
  token and retry queue as `match_paused` & co; the token goes in both `Authorization: Bearer`
  and `X-Auto-Tournament-Token`), and on the fleet link as
  `event.admin_called` (while the server has a platform assignment; `data` = the same fields
  without `event` / `matchid` / `map_number`, schema
  `plugins/fleet/protocol/v1/messages/event.admin_called.json`).

Resolving a call is done on the platform; there is no in-game command for it.

The webhook body (POSTed to `<events url>/<match slug | matchid>`; with no match loaded
`<events url>/unknown`, in a scrim `<events url>/scrim`):

```json
{
  "event": "admin_called",
  "matchid": 4242,
  "map_number": 0,
  "server_id": "srv-eu-1",
  "call_id": "3f2b8c1e-9a4d-4e6f-8b21-7c5d0e9f1a2b",
  "player": { "steamid64": "76561198000000001", "name": "alice", "team": "team1", "side": "ct" },
  "message": "smoke bugged on B site",
  "called_at": "2026-09-25T12:40:12.345Z"
}
```

- `matchid`: the loaded match's id (a number); `-1` in a scrim or with no match loaded.
- `map_number`: the current map of the series, **0-based** (0 = the first map; 0 without a match).
  The other webhook events count maps from 1.
- `server_id`: the fleet server id, only when the server is enrolled in fleet mode.
- `call_id`: unique per call (a version-4 UUID).
- `player.steamid64`: a string. `player.team`: `"team1"` / `"team2"` (the match roster),
  `"spectator"` (on the spectator team) or `null` (scrim player, not on the roster).
  `player.side`: `"ct"` / `"t"` / `null`.
- `message`: what followed `.admin`, trimmed, chat color bytes removed, at most 200 characters;
  may be empty.
- `called_at`: ISO 8601 UTC with milliseconds.

## Upgrading from Postgres

Older versions kept admins in Postgres (`readyup_db.json`). Run the migration once; it copies
admins, persisted settings and skins loadouts into the JSON files and only reads the database:

```bash
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo
# Postgres in a container without psql on the host:
python3 game/csgo/readyup/tools/migrate-postgres-to-json.py --csgo game/csgo --docker readyup-postgres
```

See [INSTALL.md](INSTALL.md#upgrading-from-postgres).
