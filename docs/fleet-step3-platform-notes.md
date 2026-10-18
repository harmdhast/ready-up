# Fleet step 3: what the platform must build

Ready Up implements build-order step 3 of [`FLEET.md`](FLEET.md) (§19.4 item 3: `match.assign` →
events → normalizer, one match end to end on the fleet driver) on its side: `plugins/match`
(`fleet_bridge.cpp`, `fleet_state.cpp`) over `readyup.fleet.v1`. The platform side
(`Auto-Tournament/auto-tournament`) does not exist yet. This file is the task for it: everything the
platform has to implement to consume what Ready Up now sends and to drive it. Section numbers
(§) refer to `FLEET.md`.

The wire contract is the set of JSON Schemas in
[`plugins/fleet/protocol/v1`](../plugins/fleet/protocol/v1) marked **proposed** in its
[README](../plugins/fleet/protocol/README.md). Real frames Ready Up sent during the live test are in
[`plugins/fleet/protocol/examples/v1`](../plugins/fleet/protocol/examples/v1) (`live.*.json`).

## 1. Adopt the schemas (D18)

- Copy `match.defs.json` and `messages/{match.*,cmd,cmd.result,state.*,server.availability,event.*}.json`
  into `api/src/integrations/cs2/fleet/protocol/v1/`. Nothing else changes (`envelope.json`,
  `defs.json` are untouched).
- `index.ts`: `ajv.addSchema(matchDefs)` next to `defs`, and add every new type to
  `FLEET_MESSAGE_SCHEMAS`. `types.ts`: add the types to `FleetMessageType` and TS types for the
  payloads (or generate them from the schemas).
- `tests/api/fleet-protocol.spec.ts`: validate the files in `examples/v1` (copy them too).
- Then Ready Up copies them back from the platform (its README's "copy" command) and the
  "proposed" section there is deleted.

## 2. Gateway (`/api/fleet/ws`)

- Accept and ack the new reliable server types (`cmd.result`, `state.patch`, `event.*`,
  `server.availability`); persist them in `cs2_fleet_events` (unique on stream id + seq) before
  acking. Critical ones (§6.5): `event.round_end`, `event.backup`, `event.map_result`,
  `event.series_end`, `event.demo`, `event.rounds_voided`, `event.match_restored`,
  `event.forfeit`, `cmd.result`.
- `state.snapshot` and `state.request` are ephemeral (no seq, never replayed).
- Platform → server messages (`match.assign`, `match.update`, `match.unassign`, `cmd`) are
  reliable, go into the outbound stream table and are replayed after a reconnect until acked.
  Put the assignment's `epoch` in the **envelope** of every match-scoped message (Ready Up also
  reads `payload.epoch`).
- Every one of those gets **exactly one** `cmd.result` whose envelope `ref` is the message id and
  whose `epoch` is its epoch. Correlate on `ref`. A replayed message that was already done is
  answered again (`ok`) without effect, so retries are safe.
- `cmd.expires_at` (unix ms, 0 = never): an expired command is answered `status: "expired"`.

## 3. Match state store (§9, §19.2 #3)

Table `match_live_state (match_id, epoch, server_id, live_rev, config_rev, state jsonb, updated_at)`.

- `state.snapshot` (reasons `assign`, `hello`, `request`, `reset`, `periodic`): replace the stored
  state when `state.epoch` is the current epoch; `periodic` / `hello` are also a drift check
  (log a mismatch against the stored state, then take the snapshot). `state: null` = idle server.
- `state.patch` and every `event.*` carry `rev` and `patch` (RFC 7386 merge patch on MatchState;
  `null` removes a member, arrays are replaced whole). They share one counter:
  - `rev == stored_rev + 1`: apply `patch`, store `rev`;
  - `rev <= stored_rev`: duplicate, ignore the patch (still process the event's `data` idempotently);
  - `rev > stored_rev + 1`: gap — send `state.request` and hold patches until the snapshot.
  The live test checks this on real frames: applying every patch to the `assign` snapshot gives
  exactly the later snapshots.
- MatchState never contains `null` values; an absent member means "unknown / none"
  (e.g. `teams.team1.side` before the sides are known, `pause.type` when not paused).
- One writer per field (§9.2): the platform owns `match_id`, `epoch`, `config_rev`, team names /
  ids / tags, rosters and roles, spectators, `rules`, the map list and `sides`; the server owns
  everything else. The platform never edits server fields; it sends a `cmd`.

## 4. Fleet driver (§19.1, §19.2 #4)

`ServerDriver.loadMatch` → `match.assign` (schema `messages/match.assign.json`):

- `match_id` = the match slug (`^[A-Za-z0-9_.:-]{1,64}$`), `epoch` ≥ 1, bumped on every
  (re)assignment of that match; keep the highest epoch per match (fencing, §11.4).
- `config_rev` (new, default 1): the platform's config counter, the base for `match.update`.
- `config` from `matchConfig.ts`: `id` (the match's numeric id, unique per platform: slugs
  like `r1m1` repeat across tournaments and would share round backup names), `num_maps`, `maps[] {name, workshop_id?, sides}` (sides from the
  veto: `team1_ct` | `team2_ct` | `knife`; a workshop map is `workshop_id` + its name, or a name
  `ws:<id>` / `workshop/<id>[/<name>]` / all digits: Ready Up loads it with `host_workshop_map`), `team1/team2 {id, name, tag?, captain?, players[]
  {steamid64 (string), name, role: player|sub|coach}}`, `spectators[]`, `admins[]`,
  `password` (generate one per match: printable ASCII, no space, quotes, `\` or `;`, ≤ 64),
  `rules` (§7.1, typed; replaces `maxRounds`, `overtimeMode`, `knifeDecisionSeconds`, `at_*`),
  `cvars` (engine cvars only: `mp_*`, `sv_*`, `tv_*`, `bot_*`; Ready Up drops anything else,
  plus `sv_password`, `rcon_password`, `sv_cheats`).
  Coaches are whitelisted as spectators (not ready-gated). `ready.min_per_team` is reported in
  `MatchState.ready.required_per_team`; the pause / forfeit / gg-vote rules are carried in
  `MatchState.rules` but not enforced by Ready Up yet.
- Answers: `ok`; `rejected` with `invalid_config` (message says which field), `busy` (another
  match or a local `ru match load` match; a finished-but-not-unassigned match does not block),
  `stale_epoch`; with `resume` also `checksum`, `no_backup`, `unsupported` (section 11).
- Show the connect string with the password to the roster and admins only (D9).
- D16: the server ends a running scrim itself (chat notice, non-roster humans kicked after 5 s,
  then load). Nothing to do on the platform except allocating idle-or-scrim servers.
- `match.update` (roster / names / password / rules mid-match): send `base_config_rev` = the
  config_rev the platform last saw acked and `config_rev` = the new one. `rejected: conflict` comes
  with `rev` = the server's config_rev and a `state.snapshot (request)`: rebuild the update on
  that base. `ok` carries `rev` = the new config_rev. All ops apply or none (`invalid_update`).
- `match.unassign {reason, kick_message?}` when the series is finished and stored, when the admin
  cancels / overrides a result, or `superseded` after a move. Ready Up clears `sv_password`, kicks
  (at once with `kick_message`), becomes `available` and fences that epoch: later messages of it
  are `stale_epoch`.
- `server.availability`: `busy` after an assign; `available` after the unassign, or already when
  the finished match was unloaded (ServerReset, `reason: "series_end"`) — turnover can start there.

## 5. Normalizer (`fleet/normalize.ts`, §8.1, §19.1)

| Fleet message | → NormalizedEvent / use |
|---|---|
| `event.phase {from, to, reason}` | `phase.changed`; `to: live` on map N the first time → `map.started`; the first `map.started` → `series.started` |
| `event.player_connect` / `player_disconnect` / `player_ready` / `player_unready` | `presence.changed` |
| `event.player_team` | live page |
| `event.round_start` | live page (round number, score) |
| `event.round_end {round: RoundSummary}` | `score.updated` (`team1_score` / `team2_score` after the round), `player.stats` per `players[]` line |
| `event.halftime` / `event.overtime` / `event.pause` | `phase.changed` |
| `event.knife_result` / `event.side_picked` | live page; fix `maps[n].sides` for a failover (§11.2) |
| `event.map_result` (MatchFlowEvent MapResult with `stats: MapStats`) | `map.result`; `stats` is the source of truth for map totals (§13) |
| `event.series_end` | `series.ended` (`forced: true` = admin `end_match`) |
| `event.rounds_voided {from_round, reason}` | drop stored round stats with `round >= from_round` for that map |
| `event.match_restored` | failover / restore confirmation |
| `event.demo` | turnover (`utils/serverTurnover.ts`); the demo itself streams over the link (`demo.*`, FLEET.md §12.2) |
| `event.forfeit` / `event.gg` | admin notification / result proposal |
| `event.admin_called {call_id, player, message, called_at}` | a player typed `.admin [message]`: notify admins, resolve on the platform (same fields as the `admin_called` webhook, docs/ADMINS.md) |
| `event.chat {player, message, team_only, at}` | a player's chat line (not a command), for the admin match view; at most 5 lines per player per 5 s |
| `event.backup` | backup store (section 7) |

Bots: player ids in the dev-bot range (`PlayerLine.bot = true`) are dropped unless the match is a
simulation (§13).

## 6. Admin commands (§7.4)

`cmd {match_id?, epoch?, name, args, issued_by {user_id, name, root}, expires_at, audit_id?}`.
Match commands (`pause`, `unpause`, `force_ready`, `start`, `restore_round`, `restart_map`,
`end_match`, `change_map`, `swap_teams`) need `match_id` + the current epoch. Error codes to show:
`bad_phase` (e.g. `pause` outside live, `force_ready` / `change_map` / `swap_teams` after warmup),
`already_paused`, `not_paused`, `bad_args`, `not_connected` (kick), `no_backup`, `checksum`,
`forbidden`, `invalid_command`, `unknown_command`, `stale_epoch`, `not_assigned`.

- `exec`: root admins only (D10). Write the audit row first and send its id as `audit_id`; Ready
  Up logs `fleet: exec by <name> (platform:<user_id>, audit <id>): <command>` and echoes
  `audit_id` in `cmd.result`, with `output` = console log lines seen within 0.5 s (≤ 8 KiB,
  best effort).
- `say`: ≤ 190 bytes after control characters are stripped.
- `restore_round {map_number, round, backup?}`: `backup` = an `InlineBackup` from the store
  (single part). Without it the server uses its own file. On success: `event.rounds_voided`,
  `event.match_restored`, the match is paused until an unpause.
- D12: after `offline_pause_minutes` (default 3) without the platform, a live match pauses with
  `event.pause {type: "offline", by: "platform-link"}` (spooled, arrives after the reconnect). Show
  it and offer `cmd unpause`.

## 7. Round backup store (§12.1, §12.3, §19.2 #5)

- `event.backup data: InlineBackup {map_number, round, file, size, sha256, score, encoding:
  "base64", data, part?, parts?}`. `round` = the round the backup starts (1-based). Files above
  384 KiB come in `parts` (same `file` / `sha256`); reassemble, check `size` and `sha256`, store
  `DATA_DIR/backups/<match_id>/map<N>/round<RR>.txt`, index `match_round_backups (match, map,
  round, epoch, server, sha256, size, stored_at)`.
- The same round can arrive again with other content after a restore (the round is replayed):
  keep the newest per (match, map, round) and the sha256 of each.
- The admin's backup picker (§11.2) lists them; `cmd restore_round` sends one back inline.

## 8. Not in Ready Up's step 3 (later steps)

Failover detection and the admin's choice (§11.1, §11.2; the server side of `resume` is done,
section 11), the demo stream receiver (FLEET.md §12.2, schemas `messages/demo.{begin,chunk,end,ack}.json`: store chunks at their offset, answer `demo.ack` with the contiguous stored offset, verify size + sha256 on `demo.end`; `event.demo` already flows), `server.config`, `server.drain`, `hello.state` epoch check → `match.unassign {superseded}` (the server
side is ready: `hello` carries the MatchState with `epoch`), refusing `ru match load` in fleet
mode.

## 9. Checking it end to end

Ready Up's `scripts/livetest/fleet_livetest.py` runs a bot match against a Python mock of this
contract (`fleet_mock_platform.py`). Pointing a real platform at a test server
(`ru fleet enroll <url> <code>`) and replaying the same sequence — assign, fencing checks,
`match.update` CAS, pause / unpause, backups + restore, offline auto-pause, `end_match`, unassign —
is the acceptance test for this task.

## 10. Admins and skins over the link (D13: Ready Up has no Postgres)

Ready Up dropped Postgres (FLEET.md D13, PR "drop Postgres"). Standalone servers keep admins and
skins loadouts in JSON files; in fleet mode they come from the platform. Proposed schemas (same
status as the step 3 ones, [README](../plugins/fleet/protocol/README.md) "D13"):
`messages/admins.set.json`, `messages/skins.loadout.json`, `messages/skins.invalidate.json`,
`messages/skins.stattrak.json`, examples in `examples/v1/`.

- **`admins.set {rev, admins: [{steamid64, name}]}`** (reliable, D5): the whole fleet-wide list.
  Bump `rev` on every change (start at 1) and send it whenever the list changes. After a
  `welcome`, send it only when `hello.admins_rev` (the rev the server has cached in
  `fleet-admins.json`; absent = none) is not your rev. `state.snapshot.admins_rev` reports the
  same value (0 = none), so a snapshot also tells you whether the server has the latest list. The server ignores a lower `rev`, caches the list in `fleet-admins.json` (so an offline
  boot still has admins) and, in fleet mode, uses only this list plus the per-match `admins` of
  the assignment. `ru admins add|remove` answer "Admins are managed on the platform"; `ru admins
  list` shows the platform's list and rev.
- **`skins.loadout {steamid64, rev, items}`** (reliable, D6): only when the deployment has skins
  on **and** the server's `hello.capabilities` has `skins.v1`. Send it when you see
  `event.player_connect` for a player with a loadout, and again when the player edits it (`rev`
  per player, bumped on change). `items.paints[].stattrak` present = StatTrak on, value = count.
  Knives are a defindex (`507` = Karambit, table in `plugins/skins/docs/json-contract.md`).
  The server applies it from the next weapon / spawn; it keeps it in memory only.
- **`skins.invalidate {steamid64}`**: the player removed their loadout (stock items).
- **`skins.stattrak {increments: [{steamid64, defindex, kills}]}`** (server → platform, reliable,
  critical, at most one per 10 s): add `kills` to the stored count of that paint and send the
  new count in the next `skins.loadout` (no need to push one per kill).
- Store: `admins (steamid64 PK, name, updated_at)` + a `rev` counter; `skins_loadouts (steamid64,
  team, defindex, paint, wear, seed, nametag, stattrak_enabled, stattrak_count)` + knife / gloves /
  agents, i.e. the old `readyup_weapon_*` tables. Existing Ready Up installs export theirs with
  `scripts/migrate-postgres-to-json.py`; the resulting `loadouts.json` / `admins.json` are a
  ready import format for the platform.

## 11. Failover resume (`match.assign.resume`, §11.3)

Ready Up accepts `resume` in `match.assign` (schema `match.defs.json#/$defs/resume`, example
`examples/v1/match.assign.resume.json`; frames Ready Up sent in the live test:
`live.event.match_restored.resume.json`, `live.state.snapshot.restored.json`). What the platform sends after the admin confirmed a
failover proposal (§11.2) or a manual move:

- A **new epoch** for the match (`from_epoch` = the failed one; keep the highest per match and
  answer the old server's later `hello` with `match.unassign {superseded}`, §11.4) and the full
  `config` (new `password`). Fix `config.maps[n].sides` (or send `resume.sides`) from
  `event.side_picked`: a resume at round >= 1 of a map still on `knife` is `invalid_config`.
- `map_number` + `round` (the backup's round, 1-based) and the chosen backup: `backup` = the
  stored file as one InlineBackup part (`data` base64 of the whole file; `parts` > 1 is
  `unsupported`) for a move to a spare server; `backup_ref {file?, sha256?}` for a restart in
  place (the server uses its own file; `sha256` = the stored one's, checked); neither = the
  server's own file for that map / round. Without `round` / a backup the map restarts from
  warmup (before the first backup of a map, §11.2).
- The series so far: `series_score` (maps won) and `maps {"<n>": {score, winner}}` for the maps
  before `map_number`, or just your stored `state` (MatchState): its `series.score`,
  `series.maps` (status `done`), `series.maps[n].sides` and `teams.*.score` / `side` are used when
  the explicit fields are absent. `map_stats` (your MapStats of the map, as `state.snapshot`
  carries it) keeps the players' stats; rounds >= `round` are dropped from it.
- Answers: `ok`; `checksum` (inline sha256 / size, or the `backup_ref` sha256), `no_backup` (the
  server has no such file), `invalid_config`, `unsupported`, `busy`, `stale_epoch`. A server that
  still runs (or recovered after a crash) this match accepts the resume and reloads the match.

Then: `state.snapshot {assign}` with `phase: restoring` (and the series restored); players
connect with the new password and ready (or `cmd force_ready` / `start`); at the go-live the
server restores the backup and sends `event.rounds_voided {from_round: round, reason: resume}`,
`event.match_restored {map_number, round, backup_sha256, file, resume: true, from_epoch}` and
`state.snapshot {reason: restored}` (map N, paused unless `rules.pause.pause_after_restore` is
false). Drop your stored round stats `>= round` for that map (as for `restore_round`) and show
the unpause button. The go-live round before the restore is never reported. The server's own
round backups continue under the usual name; the restored file is
`readyup_resume_<id>_map<N>_round<NN>.txt`.

Checked live by `scripts/livetest/fleet_livetest.py` (end of the run, or `--resume-only
--resume-backup FILE` with a backup saved by an earlier `--save-backup FILE`): map 2 of 3 from a
round backup of map 1, series 1-0, map 1's result, knife-decided sides.

## 12. Rulesets: esports mode (`rules.ruleset`, `rules.overrides`)

Ready Up can run a match under Valve's CS Major rulebook ([ESPORTS-MODE.md](ESPORTS-MODE.md)).
Proposed schema: `match.defs.json` `rules.ruleset`, `rules.overrides`, `matchState.ruleset`,
`matchState.effective_rules`.

- **`rules.ruleset`**: `"default"` (Ready Up's cfgs) or `"valve"`. Left out, the server's
  `readyup.cfg` `ruleset=` applies. Under `valve` the server execs `ReadyUp/esports_live.cfg` at
  go-live (Premier defaults + Valve's exception list), refuses maps whose `sides` is `knife`
  (send the veto result: `team1_ct` / `team2_ct`), keeps the skins plugin inert (players' own
  inventories only), counts technical pauses (1 per team, 120 s), pauses at halftime (both
  teams `.unpause`) and ignores `rules.overtime.max_overtimes` / `rules.tiebreak` (overtime is the
  engine's, unlimited MR3 unless `overrides.overtime.limit`).
- **`rules.overrides`**: the organiser's changes on top of the preset, one named key per rule:
  `freezetime`, `tac_timeouts`, `tac_timeout_seconds`, `tech_pauses_per_team`,
  `tech_pause_seconds`, `allow_knife`, `overtime {enabled, maxrounds, startmoney, limit}`, `zeus`,
  `spectators_max`, `halftime_pausematch`, `tv_delay`, `tv_broadcast_url`, `camera_man_steamid`,
  `lan`, `coaches_online`, `default_models`, `cosmetics` (`inventory` | `plugin`). Types and
  ranges are in the schema. An unknown key, a wrong type or a value out of range is refused:
  `cmd.result rejected invalid_config` with the key in the message (`rules.overrides: unknown
  override "freeztime" (known: ...)`). The same check runs on `match.update` `set_rules`.
- Coaches (`role: coach`) are admitted as spectators unless the effective rules say online +
  `coaches_online: false` (Valve: no staff online). `lan: true` admits them.
- **What to show**: `MatchState.ruleset` and `MatchState.effective_rules` =
  `{ruleset, rules: {...effective values...}, differs: ["freezetime", "overtime.startmoney"],
  preset: {freezetime: 20, overtime: {startmoney: 10000}}, source: {freezetime: "override", ...}}`.
  `differs` lists every key whose value is not the preset's, whatever set it: an override, a
  per-match pause key (`source: "match"`) or a raw `cvars` entry that touches a rule's cvar
  (`source: "cvars"`). Show it as "differs from Valve" next to the match; it is in every
  `state.snapshot` and `/status`. `ru match rules` prints the same on the server.
- Prefer overrides over raw `cvars` for anything the table covers: overrides are validated and
  reported, and they go out after the cfg **and** after the match `cvars`.
