# Fleet protocol schemas (server channel, v1)

## Step 1: copied from the platform

Copy of the normative JSON Schemas from the platform repo (docs/FLEET.md D18):
`Auto-Tournament/auto-tournament`, `api/src/integrations/cs2/fleet/protocol/v1/**/*.json`,
branch `feat/fleet-step1` at commit 4649814 (PR #386). Do not edit these here; copy them again when
the platform changes them:

    cp -r <auto-tournament>/api/src/integrations/cs2/fleet/protocol/v1/{envelope,defs}.json \
          <auto-tournament>/api/src/integrations/cs2/fleet/protocol/v1/{messages,http} plugins/fleet/protocol/v1/

Files: `envelope.json`, `defs.json`, `http/enroll.{request,response}.json`,
`messages/{hello,welcome,ping,pong,ack,error,server.config,auth.rotate,auth.rotated}.json`.

## Step 3: proposed here — the platform must adopt them

**Status: proposed — platform must adopt.** Build-order step 3 (FLEET.md §7-§10, §12.3, §13) is
implemented on the Ready Up side first. These schemas describe exactly what Ready Up sends and
accepts; they are written in the platform's style (draft 2020-12, `$id` under
`https://auto-tournament.dev/fleet/v1/`, refs into `defs.json`) so they can be copied into
`api/src/integrations/cs2/fleet/protocol/v1/` unchanged. Once the platform has them, the platform
repo is the source again (D18) and this section goes away. What the platform has to build to
consume them: [`docs/fleet-step3-platform-notes.md`](../../../docs/fleet-step3-platform-notes.md).

| File | Direction | Delivery | Answer |
|---|---|---|---|
| `match.defs.json` | shared `$defs`: MatchState, rules, assign config, InlineBackup, RoundSummary, MapStats, ... | | |
| `messages/match.assign.json` | platform → server | reliable | `cmd.result` |
| `messages/match.update.json` | platform → server | reliable | `cmd.result` (`rev` = config_rev) |
| `messages/match.unassign.json` | platform → server | reliable | `cmd.result` |
| `messages/cmd.json` | platform → server | reliable | `cmd.result` |
| `messages/state.request.json` | platform → server | ephemeral | `state.snapshot` |
| `messages/cmd.result.json` | server → platform | reliable (critical) | |
| `messages/state.snapshot.json` | server → platform | ephemeral | |
| `messages/state.patch.json` | server → platform | reliable | |
| `messages/server.availability.json` | server → platform | reliable | |
| `messages/event.<name>.json` (22) | server → platform | reliable | |

Events: `player_connect`, `player_disconnect`, `player_team`, `player_ready`, `player_unready`,
`phase`, `knife_result`, `side_picked`, `round_start`, `round_end`, `backup`, `pause`, `halftime`,
`overtime`, `rounds_voided`, `map_result`, `series_end`, `demo`, `match_restored`, `forfeit`, `gg`,
`admin_called` (and `error`, reserved).

Differences from the FLEET.md text, decided while implementing:

- `match.assign` has an optional `config_rev` (default 1): the base for `match.update`.
- `state.patch` is new: MatchState changes that are no event (connections, ready counts, scores,
  names after `match.update`). It shares the `live_rev` sequence with `event.*`: every one of them
  bumps `rev` by exactly 1, so "apply when `rev == stored + 1`" holds for both.
- `state.snapshot.reason` adds `assign` (sent right after the assignment was acked).
- `cmd.audit_id` (optional): the platform's audit row id; the server logs it and echoes it in
  `cmd.result.audit_id` (D10, `exec`).
- `cmd` names add the aliases `restart_round` (= `restore_round`) and `end` (= `end_match`).
- The inline round backup event is `event.backup` (FLEET.md §8.1); `InlineBackup.round` is the round
  the backup starts (CS2's `…roundNN.txt` holds NN rounds played, so `round` = NN + 1).
- `series_end` from `cmd end_match` carries `forced: true` and `reason`.
- `match.assign.resume` (failover, §11.3) is `match.defs.json#/$defs/resume`: `map_number`,
  `round`, `backup` (InlineBackup, single part) or `backup_ref {file?, sha256?}` (a file the server
  has) or neither (the server's own file), `series_score`, `maps` (earlier results), `sides`,
  `score`, `map_stats`, `state`. Extra answers: `checksum`, `no_backup`, `unsupported` (multi-part).
  `rounds_voided.reason` adds `resume`, `match_restored.data` adds `resume` and `from_epoch`,
  `state.snapshot.reason` adds `restored`.
- `mapName` also accepts `ws:<id>`; a name of digits / `ws:<id>` / `workshop/<id>[/name]` (or a
  `workshop_id`) is a Steam Workshop map (`host_workshop_map`).

## Demo streaming: proposed here — the platform must adopt them

**Status: proposed — platform must adopt.** FLEET.md §12.2: demos of platform matches stream over
the link while they record (capability `demo.stream.v1`). Examples: `examples/v1/demo.*.json`.

| File | Direction | Delivery | Answer |
|---|---|---|---|
| `messages/demo.begin.json` | server → platform | ephemeral (normal lane) | `demo.ack` (stored offset) |
| `messages/demo.chunk.json` | server → platform | ephemeral (lowest-priority lane) | `demo.ack` (stored offset, or `gap`) |
| `messages/demo.end.json` | server → platform | ephemeral (lowest-priority lane, resent every 10 s) | `demo.ack` (`complete: true` or `checksum`) |
| `messages/demo.ack.json` | platform → server | ephemeral | |

## D13 (no Postgres): proposed here — the platform must adopt them

**Status: proposed — platform must adopt.** Ready Up dropped Postgres (FLEET.md D13); in fleet mode
admins and skins loadouts come over the link. Same style as the step 3 schemas. What the platform
has to build: [`docs/fleet-step3-platform-notes.md`](../../../docs/fleet-step3-platform-notes.md)
§10.

| File | Direction | Delivery | Answer |
|---|---|---|---|
| `messages/admins.set.json` | platform → server | reliable | ack only |
| `messages/skins.loadout.json` | platform → server | reliable | ack only |
| `messages/skins.invalidate.json` | platform → server | reliable | ack only |
| `messages/skins.stattrak.json` | server → platform | reliable (critical) | |

Examples: `examples/v1/{admins.set,skins.loadout,skins.invalidate,skins.stattrak}.json`.

`hello` (a copied step-1 schema) gets one proposed optional member, `admins_rev`: the rev of the
admins.set list the server has cached (absent = none). `state.snapshot.admins_rev` carries the
same value (0 = none). The platform sends `admins.set` after `welcome` only when its rev differs.

## Core selftest: proposed here — the platform must adopt it

**Status: proposed — platform must adopt.** `hello.selftest` (already in the copied step-1 schema,
optional) is now filled from the core's latest selftest (ru_api 1.11 `selftest_summary`), and
`messages/server.selftest.json` (server → platform, reliable) carries the same object whenever
the outcome changes (pass/fail, counts, failing checks; a re-run with the same outcome is not
sent). Examples: `examples/v1/{hello.selftest,server.selftest}.json`.

## Console logs: proposed here — the platform must adopt them

**Status: proposed — platform must adopt.** FLEET.md §7.6: a live console stream of a fleet server
(capability `logs.v1`).

| File | Direction | Delivery | Answer |
|---|---|---|---|
| `messages/logs.start.json` | platform → server | ephemeral | `logs.lines` (backlog first) |
| `messages/logs.stop.json` | platform → server | ephemeral | |
| `messages/logs.lines.json` | server → platform | ephemeral | |

Examples: `examples/v1/logs.{start,stop,lines}.json`.

## Tests

- `fleet_protocol` (`tests/fleet_protocol_test.cpp`): every schema loads and every example frame in
  `examples/v1/` validates (envelope + payload); broken copies do not. `examples/v1/live.*.json`
  are frames Ready Up really sent in the live test.
- `fleet_integration`: every frame fleet.so sends is validated against these schemas
  (`tests/schema_check.cpp`), so a schema that no longer matches what Ready Up produces fails CI.
- `scripts/livetest/fleet_livetest.py`: a Python mock platform (`fleet_mock_platform.py`, Docker)
  validates every frame of a real bot match with `jsonschema` (draft 2020-12).
