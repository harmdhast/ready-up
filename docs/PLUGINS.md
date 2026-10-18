# Plugins

Ready Up is a core plus plugins, like Metamod and its plugins. The core (`libserver.so`) is the
only part that touches the engine; each feature is its own plugin (`csgo/readyup/plugins/<name>.so`)
that talks to the core through the versioned C API in
[`core/include/readyup/plugin_api.h`](../core/include/readyup/plugin_api.h). Every plugin ships as its own
zip and can be installed, hot reloaded, or turned off (`ru plugin disable <name>`) on its own; the
bundles below are just zips with several of them. The in-house plugins use nothing but that public
API, so they double as examples for your own.

Plugins cooperate through named interfaces (`provide_interface` / `get_interface`): the match plugin
publishes `readyup.match.v1` (mode, ruleset, map stats; `set_external_mode` lets another plugin such as deathmatch take the server), the practice plugin `readyup.practice.v1`, the whitelist
plugin `readyup.whitelist.v1` (its list, and other plugins' guests: the tv plugin's casters), the tv plugin `readyup.tv.v1` (GOTV state, `tv.set`), the skins plugin `readyup.skins.v1` (paint one weapon; Midas uses it), the essentials plugin `readyup.essentials.v1` (default map per mode, map loads). The match plugin owns the match phase; the others read it and stand
down while a match is loaded or live, and under the valve ruleset. None of them needs another to be
loaded: each checks for the interface and works on its own (a practice-only server is core + practice).

Everything lives in this repo. The core is always installed; plugins are separate `.so` files you add or leave out, and they hot reload without restarting the server (`ru plugin reload <name>`). `ru plugin list` shows them; `ru plugin disable <name>` / `enable <name>` turns one off or on and keeps it that way after a restart.

<details>
<summary><b>Core</b> (<code>core/</code>): loader, engine layer, plugin host</summary>

<br />

Loads into CS2, owns every engine touchpoint (`gamedata/engine-surface.json`), and gives plugins a small, versioned C API: chat and console commands, game and log events, center-screen HTML, server commands, player and team lookup. Also ships `ru selftest` and the local status endpoint. It runs on its own too: without plugins it checks the engine and serves `/status`, but there is no match flow.

</details>

<details>
<summary><b>Match</b> (<code>plugins/match</code>): ready-up, knife, pauses</summary>

<br />

The match flow: scrim ready-up with a center-screen panel, knife round and side pick, a go-live card with the commands, pauses, `.admin [message]` to call an admin, admins, match configs, webhooks for the Auto Tournament platform, GOTV demos and per-map stats. Ships as `plugins/match.so` in both bundles. `ru plugin reload match` swaps in a new build without dropping a loaded match: ready states, scores and the knife round carry over.

</details>

<details>
<summary><b>Essentials</b> (<code>plugins/essentials</code>): admins and map commands</summary>

<br />

Server basics kept apart from the match flow, so every kind of server has them: the admins list (`ru admins`, `plugins/essentials/admins.json`) and `.ru map change <name|workshop id|link>` / `reload` / `restart` (refused during a live map unless `force`). Default maps per mode (`ffa`, `tdm`, `practice`, `warmup`, `retakes`, ...): `.ru map default <mode> <map|workshop id|link>` and `.ru map defaults`, saved in `plugins/essentials/default_maps.json`; other plugins read them through `readyup.essentials.v1` (the deathmatch plugin loads its mode's default map). While the server downloads a Workshop map, everyone sees a progress bar in the center of the screen. A practice-only server is core + essentials + practice; an esports server can run core + match alone. In both bundles.

</details>

<details>
<summary><b>Practice</b> (<code>plugins/practice</code>): practice mode and tools</summary>

<br />

`.prac` (admin) switches practice on or off: `cfg/ReadyUp/prac.cfg` (cheats, a full grenade set, infinite ammo) and everyone respawns with it. Tools: `.savepos`/`.loadpos [name]`, `.back`, `.spawn N` / `.ctspawn N` / `.tspawn N`, `.rethrow`, `.clear`, `.noflash`, `.god`, `.bot`/`.cbot`/`.boost`, `.nobots`. Scenarios: `.scen load <id> [player] [start]` puts you where a pro stood in a recorded round while bots replay the other nine ([docs/SCENARIOS.md](SCENARIOS.md); converter in `tools/scenario/`). Runs with the match plugin (which then shows practice as its mode and refuses it while a match is loaded) or without it: `always=1` in `cfg/ReadyUp/practice.cfg` makes a dedicated practice server. Never active under the valve ruleset. In both bundles.

</details>

<details>
<summary><b>Fleet</b> (<code>plugins/fleet</code>): link to the Auto Tournament platform</summary>

<br />

One outbound WebSocket to the platform: enrollment, match assignment and commands, state stream, offline spool ([FLEET.md](FLEET.md)). Ships as `plugins/fleet.so` in both bundles plus a fully commented `cfg/ReadyUp/fleet.cfg`. Without a `url` it loads, logs one line and stays idle, so standalone servers are unaffected.

</details>

<details>
<summary><b>Skins</b> (<code>plugins/skins</code>): optional, not in the default bundle</summary>

<br />

Weapon paints, knives, gloves and agents from `loadouts.json` ([contract](../plugins/skins/docs/json-contract.md)), or from the platform in fleet mode. Skin changers can get a server banned, so this plugin is only in the Full bundle and you add it on purpose. Ships as `plugins/skins.so` plus its gamedata `engine-surface.skins.json`; the core runs without either.

</details>

<details>
<summary><b>Midas</b> (<code>plugins/midas</code>): fun, Full bundle only</summary>

<br />

Weapons picked up by Midas players turn gold, and stay gold when someone else picks them up. Off by default (`cfg/ReadyUp/midas.cfg`, `enabled=1`) and never active under the valve ruleset. Hot reloads with `ru plugin reload midas`.

- **Who**: the players in `midas_steamids`, the players an admin gives it to (`.ru midas give <player>`: part of a name or a SteamID64, again to take it back; `.ru midas take <player>`; `.ru midas` shows who is Midas and why; kept in `plugins/midas/given.txt`), and with `best_player=1` the best player of the map: top ADR or kills (`best_player_stat=adr|kills`) from the match plugin's stats, picked a few ticks after each round start once `best_player_min_rounds` (3) rounds are played, or at the start of every new half (`best_player_when=half`). Scrims only; real matches need `best_player_in_matches=1`. Ties go to the other stat, then fewer deaths, then the current Midas. A player who becomes Midas (or loses it) gets a card in the middle of the screen at their next spawn or round start ("Blessed by Midas"; texts in `midas.cfg`, `cards=0` turns them off); everyone else gets the chat line.
- **Gold**: with the skins plugin loaded, a gold paint kit through its `readyup.skins.v1` interface (`paint_kit=1025`, "Gold Brick", a pattern finish that fits every gun; `paint_wear`, `paint_seed`; any paint kit id works). Knives, grenades and the C4, and every weapon without skins.so or with `finish=tint`, get the render colour `color=255,200,40` instead (a server can't send custom textures). With the readyup_midas Workshop addon, gold models by item (`plugins/midas/models.txt`), also on the grenades a Midas player throws and the bomb they plant.

</details>

<details>
<summary><b>Whitelist</b> (<code>plugins/whitelist</code>): only listed players, Full bundle</summary>

<br />

For practice and scrim servers: `ru whitelist on`, `ru whitelist add <steamid64>`, and anyone else who joins is kicked (admins and bots stay). Stands down while a match is loaded (the match roster decides then). The list is saved in `plugins/whitelist/whitelist.json`.

</details>

<details>
<summary><b>TV</b> (<code>plugins/tv</code>): GOTV broadcast settings, Full bundle</summary>

<br />

Owns the server's GOTV settings so they hold for a whole series: `delay_seconds` (`tv_delay`), `allow_insecure_observers` (`tv_secure_bypass 1` + `sv_deltaticks_enforce 0` together, for observers whose CS2 runs `-insecure`, e.g. HLAE / Lexogrine HUD Manager; GOTV only, the game port stays VAC-secured), `max_clients`, `relay_text_chat`, `relay_voice`. Server defaults in `cfg/ReadyUp/tv.cfg` (all comments: it touches nothing until a key is set) or `.ru tv set <key> <value>`; a platform match's `rules.broadcast` (or `.ru tv match <key> <value>`) wins for that match. Applied again after every map load, `mp_restartgame` and match phase change, and put back when the match ends. Never lowers `tv_delay` below what the match asks for (ruleset valve: 105). `.ru tv caster add <steamid64>`: casters may join the game port as spectators (needs the whitelist plugin; the match plugin lets them in as spectators). `.ru tv` (or `.tv`) shows the GOTV port, delay, clients and whether a password is set. Reports the state to the platform (`hello.tv`, `server.tv`) and takes `cmd tv.set`. See [docs/design/tv-plugin.md](design/tv-plugin.md).

</details>

<details>
<summary><b>Deathmatch</b> (<code>plugins/deathmatch</code>): FFA / team deathmatch, Full bundle</summary>

<br />

`.ru dm ffa [map]` / `.ru dm tdm [map]` (admin) switch the server to CS2's own deathmatch game mode (`game_type 1` / `game_mode 2`, so a map loads: the one given, else the mode's default map from essentials, else the current map again), free for all (`mp_teammates_are_enemies 1`) or team deathmatch (`0`, team kills score for the team). Ready Up's rules on top, in `cfg/ReadyUp/deathmatch.cfg`: first player / team to the kill limit (30 / 100) or the leader after the time limit (10 min) wins, announced in chat and on a winner card, then the next game starts. A small leaderboard (top 5 + your rank) is on every player's screen (`.ru dm hud` hides yours), plus spawn protection, `headshot_only=1` and weapon rounds (every N minutes everyone spawns with the next weapon of a list). `.ru dm status` / `.ru dm top` for everyone, `.ru dm off` (admin) goes back to competitive and the match plugin's idle / scrim. While it is on, the match plugin steps aside (no scrim warmup or ready panel); a match load or `.ru mode idle` ends it. See [docs/DEATHMATCH.md](DEATHMATCH.md).

</details>

<details>
<summary><b>Hello</b> (<code>plugins/hello</code>): example plugin</summary>

<br />

A minimal plugin that registers `.hello` in chat. Start here to write your own.

</details>

Downloads: `ready-up-core`, `ready-up-match`, `ready-up-fleet`, `ready-up-skins`, `ready-up-hello`, `ready-up-midas`, `ready-up-whitelist`, `ready-up-practice`, `ready-up-essentials-plugin`, `ready-up-deathmatch`, `ready-up-addons`, and two bundles: **Essentials** (core + essentials + match + fleet + practice) and **Full** (core + essentials + match + fleet + practice + skins + midas + whitelist + deathmatch + addons + the gamedata checkers). The installer mixes the single components. `fleet` is the link to the Auto Tournament platform; it stays idle until you set a `url` in `cfg/ReadyUp/fleet.cfg` (shipped fully commented out), so it is safe on standalone servers.

