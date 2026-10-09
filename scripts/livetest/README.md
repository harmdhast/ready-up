# Live test

`scripts/livetest/run.sh` plays a short bot-only match (or, with `--scrim`, a bot-only
scrim) on the readyup-test CS2 server and checks Ready Up's state changes in the
console log. Nobody has to join.

```bash
scripts/livetest/run.sh                    # on the cs2 box, as sivert: match mode
scripts/livetest/run.sh --scrim            # bots-only scrim: warmup -> countdown -> knife -> pick -> live
scripts/livetest/run.sh --side switch      # pick sides from the console instead of the timeout
scripts/livetest/run.sh --forfeit          # match + pauses + team-left forfeit (see below)
scripts/livetest/run.sh --simulation       # the match as a simulation: Ready Up brings the bots
scripts/livetest/run.sh --out /tmp/lt      # keep console-capture.log + result.json
```

Exit codes: `0` pass, `1` fail, `2` no verdict: the server is busy (a human is
connected), down, or was restarted by someone else during the run. It prints a table of steps at the end (and writes it to
`$GITHUB_STEP_SUMMARY` when that is set).

## What it does

It uses the server console only: `tmux send-keys` to type commands, and the
`console.log` that the tmux pane is piped into to read results.

### Match mode (default)

| Step | Driven by | Checked by |
|---|---|---|
| preflight | - | tmux session `ru-test` exists (waits up to 5 min if a deploy is restarting it), no human connected since the last boot |
| selftest | `ru selftest` | `[ReadyUp] selftest: PASS` |
| reset | `ru mode idle`, `bot_kick` | `state: mode=idle ... match=none` |
| match load | `ru match load http://127.0.0.1:<port>/match.json`, served by the script: empty roster, `map_sides: ["knife"]`, `maxRounds: 4`, no overtime, short round cvars | `match-load: match context set ... slug=livetest` |
| warmup | `bot_quota 4` (the load kicks bots) | `state: mode=match_warmup warmup=1` |
| bots | - | `state: ... ct=0+2 t=0+2` |
| countdown | - | **SKIP**: only scrims have a countdown (see `--scrim`) |
| knife | automatic (empty roster counts as all ready) | `knife: starting`, `state: mode=match_knife knife=starting`, then `knife=running` |
| knife winner | the bots fight | `knife: winner=...`, `state: ... knife=pick` |
| pick | `auto`: bots-only winner, sides stay after 3 s. `--side stay/switch`: `ru match side ...` | `knife: side picked by ...`, `state: mode=match_warmup golive=pending` |
| match_live | - | `state: mode=match_live` |
| rounds | the bots play | `Team "..." triggered "SFUI_Notice_..." (CT "a") (T "b")`, one round per step |
| halftime | `mp_halftime 1` | a bot changes side after round `maxRounds/2` (falls back to the score swapping) |
| map end | - | `Game Over: ...` |
| postgame | - | `state: mode=postgame` with the match still loaded, before any `idle` |
| idle | - | after the series-end kick delay Ready Up unloads the match: `state: mode=idle match=none` |

It fails right away if `mode` leaves `match_live` before the final round, if the log
shows a crash, or if the tmux session dies. Each step has its own timeout.

### Scrim mode (`--scrim`)

A scrim (no match config) normally needs a ready human on CT/T. The core dev flag
`dev_bots_scrim` lets bots alone run one: with no humans on CT/T and bots on both sides,
Ready Up enters scrim warmup, counts the bots as everyone ready, and skips the empty-scrim
timeout. The test turns it on from the console for the run only
(`ru_dev_bots_scrim 1`) and back to the readyup.cfg value (`ru_dev_bots_scrim cfg`) in
cleanup, before `ru mode scrim`, so the restored bots do not start a scrim afterwards.

| Step | Driven by | Checked by |
|---|---|---|
| selftest, reset | as in match mode | as in match mode |
| dev flag | `ru_dev_bots_scrim 1` | `dev_bots_scrim: 1 (...)` |
| scrim warmup | `ru mode scrim`, `bot_quota 4` | `state: mode=scrim_warmup ... dev_bots_scrim=1` |
| bots | - | `state: ... ct=0+2 t=0+2` |
| countdown | automatic (bots count as ready) | `state: mode=scrim_warmup ... countdown=N` |
| scrim created | countdown expires | `state: ... match=scrim:<id>` |
| knife, winner, pick, match_live | as in match mode | as in match mode |

It stops once the scrim is live (a scrim is MR12, too long for a test) and cleans up
with `ru mode idle`. Scrims enter `postgame` at map end like matches do (for
`restart_delay - 1` seconds, no kick); match mode checks that path.

### Forfeit mode (`--forfeit`)

The match flow up to `match_live` with rules in the match config (`max_tech_pauses_per_team 1`,
`tech_pause_max_seconds 8`, `forfeit_after_seconds 20`, `mp_team_timeout_max 1`,
`mp_team_timeout_time 5`, 30 rounds, no auto team balance), then:

| Step | Driven by | Checked by |
|---|---|---|
| team names | `mp_teamname_1`, `mp_teamname_2` | `LiveTestA` / `LiveTestB` (swapped after a knife `switch`); also in match mode |
| tech pause | `ru match tech team1` | `pause: technical by team1`, then `pause: ended (technical pause time is up)` |
| tech limit | `ru match tech team1` again | `pause: technical refused for team1 (1/1 used)` |
| tactical timeout | `ru match tac team2` (`timeout_terrorist_start`) | `pause: tactical timeout by team2`, then `pause: ended (tactical timeout over)` |
| forfeit countdown | `bot_quota_mode normal`, `bot_join_team T`, `bot_kick "<name>"` per CT bot | `forfeit: team1 has nobody connected` |
| cancel | `bot_add_ct` | `forfeit: team1 is back` |
| forfeit | the CT bots kicked again, 20 s | `forfeit: team1 (team_absent) forfeits map 1`, then postgame and idle |

A `bot_kick` sent right after a bot joined (or at a round restart) is sometimes ignored, so both
countdown steps kick again every 6 s while Ready Up's `state:` line still shows a CT bot.
Cleanup also restores `bot_join_team any`, `mp_autoteambalance 1` and the old `bot_quota_mode`.

### Esports mode (`--ruleset valve`)

The Valve ruleset (docs/ESPORTS-MODE.md) with a few overrides (`freezetime 5`,
`spectators_max 8`, `default_models`, `overtime.startmoney 12500`). Needs
`cfg/ReadyUp/esports_live.cfg` on the server (`scripts/dev-deploy.sh --cfg esports_live.cfg --cfg
esports_override.cfg --cfg live.cfg`).

| Step | Driven by | Checked by |
|---|---|---|
| knife refused | `ru match load` of a valve config with `map_sides: ["knife"]` | `match-load[..]: error: ... knife ... allow_knife` |
| match load | a valve config, `map_sides: ["team1_ct"]`, the overrides | `esports: ruleset=valve (match config) go-live cfg=ReadyUp/esports_live.cfg differs=freezetime,overtime.startmoney,spectators_max,default_models` |
| live | automatic (empty roster) | `mode=match_live` without a knife round |
| bots back | `bot_quota 4` (esports_live.cfg sets `bot_quota 0` + `bot_kick`) | bots on both sides |
| cvars | every Valve exception cvar of `esports_live.cfg` section 2, the Premier values and the overrides, queried from the console | each value (e.g. `mp_freezetime 5`, `mp_team_timeout_time 31`, `mp_weapons_allow_zeus 5`, `mp_spectators_max 8`, `sv_matchpause_auto_5v5 1`, `mp_maxrounds` = the match cvar) |
| `ru match rules` | `ru match rules` | `rules: differs from valve: freezetime 20->5 (override); ...` |
| skins inert | `skins_status` | `skins inert (valve ruleset)` (SKIP without skins.so) |
| default models | bot spawns | `esports: default_models: slot N (CT) -> agents/models/...` for CT and T |
| selftest | `ru selftest` with the match loaded | PASS |

Cleanup also sends `mp_unpause_match`, `sv_matchpause_auto_5v5 0` and `mp_halftime_pausematch 0`.

### Simulation (`--simulation`)

The match flow above, but the match config has `simulation: true`, `simulation_timescale`
(`--timescale`, applied by Ready Up while the map is live) and a made-up roster of
`--bots-per-side` players per team (SteamID64s `7656119899000010N` / `...20N`, names `SimA1`,
`SimB1`, ...). The test adds no bots: Ready Up does (plugins/match/readyup/simulation.h), one per
roster player, and each bot plays as its player and readies up by itself. The bots step also
waits for `simulation: bot <name><N> plays <player> (...)` for every roster player. After the
match (or `ru mode idle` in cleanup) Ready Up kicks the bots and puts `bot_quota 0`,
`bot_join_team any` and `bot_join_after_player 1` back. CI (`cs2-dynamic.yml`) runs the match
this way. Not with `--scrim`, `--forfeit` or `--ruleset valve`.

Ready Up turns on game logging (`log on`) at startup: the knife winner, round ends and the
bots' sides come from game log lines.

### Knife round time

Bots do not knife each other, so a bots-only knife round always runs to the time
limit. With `dev_bots_ready` or `dev_bots_scrim` on, or a simulated match, and no human on
CT/T, Ready Up sets the knife round time to 0.5 min (`knife: dev flag on and no humans on
CT/T ...` / `knife: simulation and no humans ...`); the knife step shows
`[dev knife time 0.5 min]` when that happened. `live.cfg` restores the real round time before
going live. Servers without dev flags are unchanged.

Afterwards it always puts the server back the way it was: `ru mode idle`, then `ru mode scrim`
(so scrims start again on their own) and the old `bot_quota`. It never restarts the
server. Use `--boot` to start it if the session is still missing after the wait.

Options (also as env vars, see `--help`): `--scrim` (`LIVETEST_MODE=scrim`), `--simulation`
(`LIVETEST_SIMULATION=1`),
`--max-rounds` (even, default 4), `--bots-per-side` (2), `--side auto|stay|switch`,
`--timescale N` (sets `host_timescale` with `sv_cheats 1`, but only once the match is
live, because Ready Up's restart checks use wall-clock time), `--round-timeout`,
`--map`, and `RU_SSH` / `RU_TARGET` / `RU_SESSION` (same as `scripts/dev-deploy.sh`).

## Fleet (platform link) test

`scripts/livetest/fleet_livetest.py` is the step-3 fleet test (docs/FLEET.md): a mock platform
(`fleet_mock_platform.py`, Python `websockets` + `jsonschema`, on 127.0.0.1, free ports per run
unless `--http-port` / `--ws-port` / `--ctl-port` are given) enrolls fleet.so, assigns a bot match, checks every frame against
`plugins/fleet/protocol/v1`, and drives fencing, `match.update`, pause / unpause, `exec`, round
backups + `restore_round`, the offline auto-pause (75 s outage, `offline_pause_minutes=1`),
`end_match` and `match.unassign`, then a failover resume (`match.assign` with `resume`: map 2 of
3 from the run's round backup, series 1-0) and the admins cache version (`admins.set` rev ->
`admins_rev` in snapshots and the next `hello`). It checks that `live_rev` goes up by one per
message and that the patches rebuild the snapshots. Options: `--play-out` (natural map end:
`map_result` with MapStats, `series_end`), `--from-scrim` (assign while a bots-only scrim runs: D16
hand-over), `--no-offline`, `--no-resume`, `--save-backup FILE` / `--resume-only --resume-backup
FILE` (resume from a backup of an earlier run), `--map ws:<id>` (a workshop map), `--out DIR`
(frames.json, console.log, mock.log), `--save-examples DIR`.

It writes `csgo/cfg/ReadyUp/fleet.cfg` for the run and afterwards moves it and fleet.so's data dir
(`csgo/readyup/plugins/fleet/`) to `<target>/.fleet-livetest/<time>/` and reloads fleet.so,
so the server is standalone again. Nothing leaves the box: the enrollment code is a made-up
constant and the mock mints a random token per run.

The mock runs in a `python:3.12-slim` container when this user can use Docker (`--mock docker`),
otherwise as a local process with its deps pip-installed once into
`~/.cache/readyup-livetest/pydeps-<python>` (`--mock local`, `$LIVETEST_PYDEPS`); `--mock auto`
(default) picks Docker when usable. If the mock does not start the test exits 2 (no verdict).
Like `run.sh` it takes `--boot` / `--wait-session`, and `--game-dir DIR` when the CS2 install is
not `--target` (CI: the console log is in a temp dir, the install is `$CS2_CI_DIR`).

```bash
flock /tmp/readyup-test.lock -c 'scripts/livetest/fleet_livetest.py --mock local --out /tmp/fleet-lt'
```

## CI

`.github/workflows/cs2-dynamic.yml` (nightly, after CS2 updates, manual dispatch on master) runs
match (`--simulation`), scrim, fleet and forfeit against its own CI server and reports each to
the compatibility page (docs/CS2-COMPAT.md, "Dynamic stage"). fleet and forfeit are advisory until
they have passed 3 runs in a row: a failure keeps their check pending instead of failing it.
`--ruleset valve` is not in CI: it needs GOTV (`tv_enable 1` before the map loads), which the CI
server (and readyup-test) boot without.

`.github/workflows/livetest.yml` (manual dispatch only; input `mode` = match or scrim) targets
readyup-test over `ssh cs2servermanager@localhost` from a runner labelled `readyup-live`. The
only such runner is cs2-dynamic's, which runs as cs2servermanager itself and is meant for the CI
install, so do not dispatch it; run the tests by hand on the cs2 box instead (wrapped in
`flock /tmp/readyup-test.lock`).
