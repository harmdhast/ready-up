# Fork: custom

`custom` is upstream `master` plus the feature and fix branches below, merged with `git merge --no-ff`.
Each branch also exists on its own, so it can go upstream as a separate PR.

## Versioning

- Tags are the upstream release `custom` is based on plus `.custom.N` (`v0.1.0-beta.16.custom.1` for `v0.1.0-beta.16`; `vX.Y.Z-custom.N` on a stable `vX.Y.Z`). The dot keeps SemVer ordering numeric (`beta.100.custom.1` > `beta.99.custom.1`).
- Push a tag to build a fork release. A custom version is always a pre-release, never latest.
- Servers install it with `READYUP_REPO=harmdhast/ready-up`:

```bash
curl -fsSL https://raw.githubusercontent.com/harmdhast/ready-up/<tag>/install.sh | READYUP_REPO=harmdhast/ready-up bash -s -- --version <tag>
```

- The license line date comes from the `vX.Y.0` tag in the fork: push upstream tags to the fork or the build date is used.

## Workflow

- New work: branch from `upstream/master`, merge into `custom` with `git merge --no-ff`.
- Branch from `custom` only when the work needs custom-only code; rebase it onto upstream before proposing it.
- Sync upstream: `git merge upstream/master` into `custom`. Never rebase `custom`.
- `git config rerere.enabled true` replays conflict resolutions; review them before committing.

## Branches in custom

Based on upstream `master` 8f3ecf5 (v0.1.0-beta.77 is 0b7ce93). Conflicts are between branches. Merge order is the table order.

| Branch | Summary | Status | Conflicts with other branches | Resolved in custom |
|---|---|---|---|---|
| fix/force-log-on | Game logging is turned on at startup | merged | none | |
| fix/chat-host-say | Chat commands are handled in `Host_Say` (CS2 1.41.9.0) | merged | none | |
| fix/autokick | Team damage kick is off in warmup and live without the cfg files | merged | `match_host_test.cpp` with warmup-reapply, warmup-round-end, warmup-infinite-ammo | Test cases of both sides kept |
| fix/hud-blink | `m_bGameRestart` set so center panels stop blinking | merged | `needs.json`, `test_compat_report.py` with warmup-infinite-ammo | Schema entries of both kept; compat schema total is 27 |
| fix/ready-panel | Ready panel with status header, team counts, who is not ready | merged | none | |
| fix/warmup-reapply | Warmup rules apply again after every map load, also onto the same map | merged | `match_host_test.cpp` with autokick, warmup-round-end, warmup-infinite-ammo; `modes.cpp`, `modes.h` with warmup-join-spawn | Both tests kept (autokick check first); upstream's `ModesOnMapStart` and this branch's `ModesOnMapLoaded` both stay |
| fix/warmup-round-end | A round scored in warmup restarts the game and resends the warmup rules | merged | `modes.cpp` with warmup-reapply, backups-live-only; `match_host_test.cpp` with autokick, warmup-reapply; `match_plugin.cpp`, `match_rules_test.cpp`, `test_compat_report.py` with warmup-infinite-ammo | `State` fields of both kept; scrim and match score tests both kept; the same-map reload test now expects the reset once the map has loaded, since a reload counts as loaded |
| fix/warmup-infinite-ammo | Warmup reserve ammo without `sv_cheats` | merged | `match_host_test.cpp`, `match_rules_test.cpp`, `test_compat_report.py`, `match_plugin.cpp` with warmup-round-end; `match_host_test.cpp` with autokick, warmup-reapply | Both tick calls and both test sets kept; compat schema total updated again by hud-blink |
| fix/reconnect-team | Team lock reads the team from the log header and logs a move once | merged | `CMakeLists.txt`, `modes.cpp`, `match_host_test.cpp` with warmup-join-spawn | upstream #243 moves players by the engine team (`TeamMoveNeeded`, `MoveRetryCap`); the log header parse (`log_header.h`) stays, a move is reported once when the engine or the registry shows it landed |
| fix/warmup-join-spawn | Respawn only the joining player in match warmup (`player_respawn`, ru_api 1.16) | merged | `CMakeLists.txt`, `modes.cpp`, `match_host_test.cpp` with reconnect-team; `modes.cpp`, `modes.h` with warmup-reapply | Both CMake test targets kept; `ModesOnMapLoaded` and `ModesOnHumanJoinedTeam(slot, steamid, team)` both declared; a team-lock move logs "moved" once and calls the respawn only when it landed, "could not move" once when refused; upstream #216 keeps the debounced warmup restart: it is the fallback here when the respawn is refused or unavailable; `player_respawn` at 1.16 changes the ru_api layout, so match.so now declares 1.17 and a core of the older layout refuses it; a custom.* match.so built before the beta.77 sync must not run on this core |
| fix/fleet-replay-before-handlers | Fleet holds match messages until the match plugin can take them | merged | `fleet_integration_test.cpp` with busy-reason, cmd-reset, cmd-add-time | |
| fix/stale-round-backups | A fresh assignment removes round backups an earlier match with the same id left | merged | `fleet_bridge.cpp` with restore-round-zero-backup | |
| fix/restore-round-zero-backup | A failover resume no longer forwards its go-live round backup as round 1 | merged | `fleet_bridge.cpp` with stale-round-backups, backups-live-only; `fleet_state_test.cpp` with assign-match-id; `FLEET.md` with backups-live-only | Stale removal runs first, then the first backup map and round are set |
| fix/backups-live-only | Round backups are written and forwarded only while the match is live | merged | `fleet_bridge.cpp` with restore-round-zero-backup, cmd-reset, cmd-add-time; `FLEET.md` with restore-round-zero-backup; `modes.cpp` with warmup-round-end | `ScanBackups` takes both `firstRound` and `liveSinceMs` and applies both filters; FLEET.md paragraph merged; `wasLive` and `warmupMapReady` both kept |
| feat/assign-match-id | `match.assign` `config.id` names round backups and demos | merged | `fleet_bridge.cpp`, `fleet_state_test.cpp` with resume-backup-chunks, restore-round-zero-backup | Test cases of both kept; the stale-backup prefix uses `AsgMatchId()` instead of the slug hash |
| feat/resume-backup-chunks | Resume backups are received in parts | merged | `fleet_bridge.cpp` with assign-match-id, cmd-reset, cmd-add-time; `fleet_state.*`, `fleet_state_test.cpp`, `fleet_protocol_test.cpp` with busy-reason, cmd-reset, cmd-add-time | `g_backupParts` declared above the `PrepareResumeBackup` comment, which takes the numeric match id |
| feat/busy-reason | Availability reason and detail on hello and `state.snapshot` | merged | `fleet_state.*`, `fleet_state_test.cpp`, `fleet_protocol_test.cpp` with resume-backup-chunks; `fleet_integration_test.cpp` with fleet-replay-before-handlers | `BackupParts` and availability functions side by side; both integration tests and all test cases kept |
| feat/cmd-reset | `cmd reset` ends any match context and frees the server | merged | `fleet_bridge.cpp` with resume-backup-chunks, backups-live-only | Both capabilities advertised; `g_liveSinceMs` kept in `Tick`, the exec reply loop stays where this branch moved it |
| feat/cmd-add-time | `cmd add_time` extends the live round; players in `state.snapshot`; `cmd.add_time.v1` is advertised once the schema resolves, not only at fleet instance start (a late one reaches the platform on the next hello) | merged | `fleet_bridge.cpp` with resume-backup-chunks, backups-live-only; `fleet_state.*`, `fleet_protocol_test.cpp` with resume-backup-chunks | Clean on top of cmd-reset |
| feat/fleet-live-logs | Live console relay: `logs.start` / `logs.stop` / `logs.lines` (capability `logs.v1`) | merged | `CMakeLists.txt`, `fleet_bridge.cpp` with fix/hud-blink, feat/cmd-add-time | Both test targets kept; `Tick` runs the exec replies and the players check, then `SendLogBatches` and the secret refresh |
| feat/fleet-stop-rules | `match.assign` `rules.stop` sets `.stop` (`enabled`, `no_damage`); older servers ignore it | merged | none | |
| fix/resume-full-roster | A failover resume waits in warmup for the full roster to connect and ready (`min_players_to_ready` and autoready ignored); the crash recovery gate too | partly superseded (upstream #231) | `modes.h` (declarations of both kept), `ready_hud.cpp` (header status `FAILOVER` in custom's `status` field instead of the branch's title) | upstream's `ResumeWaiting` / `ResumeMayGoLive` go-live gate runs next to this branch's `FullRosterGate`, which still holds `.forceready`, autoready and the recovery gate to a full team and sets the FAILOVER header |
| fix/backup-file-clock | Round backups are forwarded again: file times compare as Unix ms (libstdc++'s file clock is negative before 2174, so the scan read every match as not live) | merged | none (on top of fix/backups-live-only) | |
| fix/match-end-kick | Everyone (admins included) is kicked when a match ends, is unassigned or moves | superseded (upstream #234, #220) | none | upstream's series-end delay (90 s with a demo upload) and admin kick are taken, the 20 s cap is gone; slots without a SteamID are not kicked: the plugin API has no engine slot list |
| feat/resume-round-sides | Round backups carry their sides; a resume warmup is on the resumed round's sides and score; bot fill and restores follow them | merged | none | |
| fix/golive-win-conditions | No round is credited between the go-live and its restart (a warmup side with nobody on it no longer scores per tick and flips the halves) | merged | none | |
| fix/ghost-after-disconnect | A disconnected player stays out of the player registry when CS2 logs more lines for them (no phantom player after a move or a kick) | superseded (upstream #240) | `CMakeLists.txt` (test blocks of both kept) | upstream #240 (`AcceptHumanLogLine`) covers it; its `slot_registry.*` and tests are upstream's, the departed set is gone |
| fix/warmup-reapply-once | The `Started map` line of a level load no longer resets the map state a second time after its MAP_START (custom-only interplay of upstream `ModesOnMapStart` and fix/warmup-reapply's `ModesOnMapLoaded`); branched from `custom`, merge after fix/warmup-reapply | merged | none | |
| fix/schema-offset-retry | A schema offset that was missing is looked up again on later ticks (warmup score, warmup ammo, hud steady), logged once; branched from `custom` | merged | none | |
| fix/backup-parts-cap | A backup part sent again does not count twice against the 16 MiB cap; `part` and `parts` outside int range are refused instead of wrapping; branched from `custom` | merged | none | |
| fix/cs2-update-check-flag | A throwing CS2 update check no longer leaves the checker marked as running (upstream code) | merged | none | |

## Upstream syncs

- merge/upstream-sync-beta70: `upstream/master` 7632335 (v0.1.0-beta.70) merged. 20 files conflicted. ru_api: upstream 1.14 (`filter_client_command`) and 1.15 (`license_stopped`) kept, `player_respawn` moved from 1.14 to 1.16. Protocol and capability lists are unions. Compat schema total for match is 36.
- merge/upstream-sync-beta77: `upstream/master` 8f3ecf5 (v0.1.0-beta.77, tag on 0b7ce93) merged. 3 files conflicted (`plugin_api.h`, `plugin_engine_api.cpp`, `engine-surface.json`). Upstream took ru_api 1.16 for `license_key` (license check-in), so `player_respawn` moved from 1.16 to 1.17 (`READYUP_PLUGIN_API_VERSION_MINOR` 17; match.so requires 1.17; a core of the 1.16 layout refuses it). Gamedata is upstream's for CS2 1.41.9.2. `event.player_connect` protocol schema gained `address` (the platform's protocol copy must follow). No fork branch superseded. Compat schema total for match stays 36.

## Not in custom

- spike/live-logs-perf: spike.
- test/*: retired integration branches.
