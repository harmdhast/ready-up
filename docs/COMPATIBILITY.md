# Running Ready Up next to Metamod and CounterStrikeSharp

> Looking for how each CS2 update is checked (compat.json, the README badge, the poller)? See
> [CS2-COMPAT.md](CS2-COMPAT.md).

**Short version: run Ready Up on its own.** Ready Up is its own plugin platform: the core, the
match flow and anything else you need go on top of it as Ready Up plugins. You don't need
Metamod or CounterStrikeSharp for anything Ready Up does.

If you have to keep Metamod or CounterStrikeSharp on the same server for something else, it
works: see [the answers](#answers). Never run two match plugins (Ready Up's match plugin and
MatchZy, Get5 or MatchZy Enhanced) on one server.

Everything below was tested on a real CS2 server. It isn't guesswork.

## What was tested

| Part | Version |
|---|---|
| CS2 | 1.41.8.4 (Linux dedicated, `linuxsteamrt64`) |
| Ready Up | master `8605aac`, plus the fixes from this change |
| Metamod:Source | 2.0.0-dev+1469 (`fa6f80e`) |
| CounterStrikeSharp | v1.0.375 (`751eb0c`), .NET 10 runtime |
| MatchZy Enhanced (the MatchZy fork) | 2.0.0 |

Each case was a fresh server boot. For each one we captured the console, ran `meta list`,
`css_plugins list`, `ru selftest` and `curl localhost:<port+7>/status`, then ran the bot live
test (`scripts/livetest/run.sh`). That test plays a whole bot-only match: load, knife round,
side pick, going live, 4 rounds with halftime, map end, postgame and back to idle. No human
joined, so anything that needs a player typing in chat is worked out from the code, and the
text below says when that's the case.

## Results

The load order is set in `gameinfo.gi` (`SearchPaths`). "MM" is `Game csgo/addons/metamod`,
"RU" is `Game csgo/readyup`.

| | Setup | Load order | Before this change | With this change |
|---|---|---|---|---|
| 0 | Ready Up only | RU, csgo | selftest PASS | selftest PASS 74/74 |
| A | Metamod, no Metamod plugins | MM, RU, csgo | **Ready Up turned itself off**: every signature "matched 0 times" | selftest PASS 74/74, live test **PASS** |
| B | Metamod + CounterStrikeSharp, no CSSharp plugins | MM, RU, csgo | **CounterStrikeSharp failed to load** ("Cannot hook a null vtable") | both load, selftest PASS 74/74, live test **PASS** |
| C | B + the AT CS2 plugin (MatchZy fork) | MM, RU, csgo | not tested (B already failed) | both load, bot live test **PASS**, but the two plugins get in each other's way (below). **Not supported.** |
| D | Wrong order | RU, MM, csgo | no crash and no recursion, but **Metamod and CSSharp silently don't load** | same, and Ready Up now logs a warning saying so |
| F | B, but CSSharp's gamedata is broken (like after a CS2 update) | MM, RU, csgo | not tested | **the server crashes** in `counterstrikesharp.so`. It crashes the same way without Ready Up. |

### A: Metamod + Ready Up

Before the fix, Metamod loaded first, as it should, and Ready Up loaded Valve's library. But
Ready Up then searched for its signatures in the wrong `libserver.so`: Metamod's, which also
sits in a `bin/linuxsteamrt64/` folder and was loaded first. So every function came back
unresolved and Ready Up switched itself off:

```
[ReadyUp] engine-surface: Host_Say UNRESOLVED: signature matched 0 times (want 1)
[ReadyUp] sigtest FAIL UTIL_ClientPrintAll: signature matched 0 times (want 1)
[ReadyUp] Ready Up disabled: sigtest failed. Server will run without Ready Up hooks.
[ReadyUp] status: could not write .../csgo/readyup/readyup/status.json ...
```

(The last line is a second bug from the same cause. Under Metamod the loader reports our path
as `game/bin/linuxsteamrt64/../../csgo/readyup/bin/linuxsteamrt64//libserver.so`, and the
trailing `//` threw off the "go up three folders to `csgo`" step.)

With the fix:

```
Metamod:Source version 2.0.0-dev+1469   Loaded As: GameDLL (gameinfo.txt)
[ReadyUp] load-order: Metamod:Source is loaded ahead of Ready Up (supported order: Metamod -> Ready Up -> Valve). ...
[ReadyUp] client-command: hooked ISource2GameClients::ClientCommand vtbl[17] (verified)
[ReadyUp] gameframe: hooked GameFrame vtbl[19] (verified)
[ReadyUp] hostsay: hooked Host_Say at 0x7f52a2c70fc0
[ReadyUp] selftest: PASS 74/74
LIVETEST PASS   (idle -> match_warmup -> knife -> pick -> match_live -> postgame -> idle)
```

### B: Metamod + CounterStrikeSharp + Ready Up

Before the fix, CounterStrikeSharp refused to load:

```
CSSharp: Copying bytes from disk for .../game/bin/linuxsteamrt64/../../csgo/readyup/bin/linuxsteamrt64//libserver.so
CSSharp: Failed to find signature for 'Host_Say'
CSSharp: Failed to find signature for 'CEntityIOOutput_FireOutputInternal'
[META] Failed to load plugin addons/counterstrikesharp/...: Failed to initialize CounterStrikeSharp hooks: Cannot hook a null vtable
meta list:  [01] <FAILED> CounterStrikeSharp (v1.0.375 @ 751eb0c)
```

The cause is the same mix-up, on CounterStrikeSharp's side. CSSharp lists every loaded
`.../bin/linuxsteamrt64/*.so` that exports `CreateInterface` and picks the first
`libserver.so` in that list. That was Ready Up's shim. It also does a bare
`dlopen("libserver.so")`, which found the shim through its `DT_SONAME`. With the same CSSharp
setup and no Ready Up, CSSharp loads fine. The fix:

- The shim has no `DT_SONAME` any more, so `dlopen("libserver.so")` finds Valve's library.
- The shim exports `CreateInterface` with protected visibility. The engine and Metamod still
  find it with `dlsym`, but CSSharp's module list skips symbols that aren't default
  visibility, so it skips the shim.

With the fix:

```
CSSharp: CounterStrikeSharp.API Loaded Successfully.
CSSharp: Hooks added.
meta list:  [01] CounterStrikeSharp (v1.0.375 @ 751eb0c) by Roflmuffin
[ReadyUp] selftest: PASS 74/74
LIVETEST PASS
```

### C: adding the AT CS2 plugin (MatchZy fork): two match plugins

Both load, and Ready Up's bot-only match ran from start to finish. That only means neither
one crashes. Two match plugins still fight over the same server:

- **`.r` / `.ready` are handled by both.** The AT plugin reads chat from the `player_chat`
  game event. Ready Up sees the chat line earlier, in `Host_Say` before the original, and by
  default (`consume_ready_chat=0`) lets it through, so the engine runs it and the event fires. One
  `.r` then counts as ready in both plugins. The same goes for `.pause` / `.unpause` and the
  other commands they both have. During the test, the AT plugin sat in its own ready phase
  the whole time (`readyAvailable=True, isMatchSetup=False`), so a player's `.r` would have
  counted. (Worked out from the code, because no human joined. With `consume_ready_chat=1`,
  Ready Up swallows `.r` in `Host_Say` before the original, so the `player_chat` event does not
  fire and the AT plugin would not see it.)
- **Cvars get overwritten.** Every time Ready Up ran a config (warmup, knife, live), the AT
  plugin reset cheats and timescale. This happened 8 times in one match:
  `[Auto Tournament] [SimulationMode] Enforcing sv_cheats 0 and host_timescale 1 for non-simulation match.`
  It also runs its own `warmup.cfg` on every map start.
- **Two sets of events and webhooks.** The AT plugin kept posting `server_health` to its own
  platform URL all through the match. A real match would be reported twice, by two plugins
  that disagree about its state.
- **Chat duplicates.** Both plugins print their own ready and status messages.
- **Round ends.** Ready Up's `TerminateRound` detour drops round ends while Ready Up is in
  warmup or practice. A `TerminateRound` call from the AT plugin during Ready Up's warmup
  goes through that same detour and gets dropped too.

**Never run two match plugins.** Pick one: Ready Up's match plugin or the AT CS2 plugin.

### D: wrong order (Ready Up above Metamod)

This doesn't crash and doesn't recurse any more. An older loader chained into the next
`libserver.so` in the search path, which is why this order used to recurse. The current
loader always opens Valve's library directly. So with Ready Up listed first, the engine loads
Ready Up, Ready Up loads Valve's library, and **Metamod never loads**. Neither does
CounterStrikeSharp or any Metamod plugin, and nothing said so:

```
meta list
Unknown command 'meta'!
Unknown command 'css_plugins'!
[ReadyUp] selftest: PASS 73/73 [1 pending]
```

Ready Up now says so at startup:

```
[ReadyUp] load-order: WARNING: Metamod (Game csgo/addons/metamod) is listed BELOW csgo/readyup in gameinfo.gi: the engine loads Ready Up first, so Metamod and everything it loads (CounterStrikeSharp, Metamod plugins) never start. Put Ready Up's line directly below Metamod's. Fix: python3 <csgo>/readyup/tools/patch_gameinfo.py <csgo>/gameinfo.gi
```

`install.sh` and `readyup/tools/patch_gameinfo.py` already put Ready Up directly below Metamod,
and move it there if it's above (see `tests/test_patch_gameinfo.py`). Ready Up also has a
guard for **two copies of Ready Up in one process**, for example two `csgo/readyup`-style
lines where one copy ends up loading the other. The first copy records itself in the
environment (`READYUP_SHIM_INSTANCE=<pid> <path>`). A second copy logs
`Ready Up disabled: another copy is already active in this process`, installs no hooks, and
only passes calls through to Valve's library. The unit tests are in
`tests/load_order_test.cpp`.

### E: hooks that overlap

What each one patches, and what we saw in the running process (`/proc/<pid>/mem`, scenario B):

| Target | Ready Up | Metamod / CounterStrikeSharp | What happens |
|---|---|---|---|
| `ISource2Server::GameFrame` (vtbl[19]) | writes the vtable slot at load | SourceHook writes the same slot afterwards | Chained: SourceHook's thunk is outermost and calls Ready Up's hook, which calls Valve's function. The slot pointed into SourceHook's anonymous memory, not Valve's function. Live test passes. |
| `ISource2GameClients::ClientCommand` (vtbl[17]) | same | same (CSSharp command listeners) | Chained the same way. CSSharp's pre-hooks run **before** Ready Up, so a CSSharp plugin that returns `Handled` for `say` hides the line from Ready Up where chat still reaches `ClientCommand`. |
| `Host_Say` | funchook detour at load (5-byte `jmp`) | CSSharp's KHook detour (`Host_SayH`) | **CSSharp's hook is quietly missing.** Without Ready Up, the start of `Host_Say` is `e9 .. .. .. .. 90` into CSSharp's trampoline. With Ready Up, it's `e9 ...` into Ready Up's `libserver.so`, and neither Ready Up's trampoline nor its detour was patched a second time. CSSharp finds `Host_Say` fine (it scans a clean copy read from disk) and logs `Hooks added.`, but its detour isn't on the function. So CSSharp's chat handling that depends on `Host_Say` (chat triggers, silent `/` commands) should be treated as broken next to Ready Up. `AddCommandListener("say")` and the `player_chat` event are not affected, except for lines Ready Up swallows (`RU_CMD_HIDE`, `consume_ru_chat`, prefix relays). |
| `CCSGameRules::TerminateRound` | funchook detour, installed only when needed | CSSharp only calls it (`TerminateRound()` API) | Calls go through Ready Up's detour. While Ready Up holds round ends back (warmup/practice), a plugin's `TerminateRound()` does nothing. |

Neither side patches the other's code, so there's no "both overwrote the same prologue"
crash. The risk is behaviour: one hook quietly missing, and round ends dropped.

### F: does Ready Up still keep working after a CS2 update?

Ready Up's own signatures live in `gamedata/engine-surface.json`, not in CSSharp's gamedata,
and a broken Ready Up signature only switches off that one feature. **That protection stops
at Ready Up.** We broke CounterStrikeSharp's gamedata the way a CS2 update does (every Linux
signature made to miss). CSSharp then crashed the whole server while loading:

```
CSSharp: Failed to find signature for 'Host_Say'
CSSharp: Failed to find signature for 'CEntitySystem_AddEntityIOEvent'
CSSharp: Failed to find address for IGameSystem_InitAllSystems_pFirst
[ReadyUp] signal=SIGSEGV (11) addr=0x3 ... counterstrikesharp.so(+0x1d4dcf) ... metamod.2.cs2.so ...
```

Without Ready Up the same crash happens. Ready Up can't protect a server from another
plugin's crash. **With CounterStrikeSharp installed, a CS2 update can take the server down
until CSSharp ships new gamedata, Ready Up or not.** On its own, Ready Up keeps the server up
and turns off only what broke.

### Also seen

- With Metamod listed first, the first writable search path is `csgo/addons/metamod`, so
  CS2's round backup files (`backup_round*.txt`, `readyup_backup_*`) get written there
  instead of `csgo/readyup/`. Restoring still works, because CS2 looks through all the search
  paths, but the files end up in Metamod's folder.

## Answers

**Can I run Ready Up with Metamod?**
Yes. Metamod's `Game csgo/addons/metamod` line goes first and Ready Up's goes directly below
it. The installer does this for you. It needs a Ready Up build with the fixes above: older
builds switched themselves off under Metamod.

**Can I run it with CounterStrikeSharp?**
It loads and it doesn't crash, but we don't recommend it. CSSharp's `Host_Say` hook is lost
next to Ready Up, `TerminateRound()` does nothing while Ready Up is in warmup, and a CS2
update that breaks CSSharp's gamedata crashes the whole server. Only do it if a CSSharp
plugin you can't live without has no Ready Up version, and test the chat features that
plugin uses.

**Can I run it with MatchZy, Get5 or MatchZy Enhanced?**
No. Those are match plugins, and so is Ready Up. Two of them handle the same `.r`, overwrite
each other's cvars and report every match twice. Use Ready Up's match plugin instead. It
uses the same commands and event names.

**What if I need a CounterStrikeSharp plugin?**
Port it to the Ready Up plugin API: a small, versioned C API (chat and console commands, game
and log events, center-screen HTML, server commands, player lookup) that doesn't break when
CS2 updates, because only the core touches the engine. Start with
[`plugins/hello`](../plugins/hello), then read [ARCHITECTURE.md](ARCHITECTURE.md) (plugin API)
and [`core/include/readyup/plugin_api.h`](../core/include/readyup/plugin_api.h). If the API is
missing something you need, open an issue.

**What if I put Ready Up above Metamod by mistake?**
Nothing crashes, but Metamod and everything it loads don't start. Ready Up logs a
`load-order: WARNING` line with the command that fixes it:
`python3 game/csgo/readyup/tools/patch_gameinfo.py game/csgo/gameinfo.gi`.
