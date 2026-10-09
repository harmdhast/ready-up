# Center HUD (CS2 center panel)

Ready Up draws per-player HTML with the `show_survival_respawn_status` game
event (`loc_token` = HTML). Tested in game on CS2 1.41.8.3 with `.ru hud test 1-7`; `.ru hud test 8-11` measure the panel (rows, width, image sizes).

## Refresh rules

- The panel stays up for a few hundred ms; the event's `duration` does not keep it open.
- Each send replays the panel's fade-in, so a panel resent every frame blinks. Tested on CS2
  1.41.9.0: every frame and 50 ms blink, 100 ms shows the fade-in cut off, 250 ms pulses, 500 ms
  opens and closes.
- The match plugin sets `CCSGameRules::m_bGameRestart` every tick while no restart is pending
  (`hud_steady=1`, default; workaround by Poggu, as in girlglock/CS2FlashingHtmlHudFix). With it,
  a panel resent every frame stays steady. `mp_restartgame` still works: the flag is left to the
  engine while `m_flRestartRoundTime` is after the current round's start.
- Steady display: resend every frame (`hud_tick_ms=0`, `hud_resend_ms=0`,
  `hud_duration_s=1`, the defaults) with `hud_steady=1`. The welcome card and the go-live card
  use the same every-frame cadence.
- An `<img>` in a panel that is resent every frame flashes, because the image is
  reloaded on each send. Only use images in one-off sends.

## One panel per player

CS2 has a single center panel per client, so the core decides who owns it (plugin API 1.6,
`center_html_to_slot_prio` / `center_html_all_prio`). Each send carries a priority, highest wins:

| Priority | Level | Panels |
|---|---|---|
| 90 | `RU_HTML_PRIO_ALERT` | map change card, Workshop download bar, `.admin` call card (admins only); later: pause called |
| 80 | `RU_HTML_PRIO_MENU` | a menu the player opened (the planned WASD `.ru` menu) |
| 70 | `RU_HTML_PRIO_NOTICE` | welcome card, go-live card, vote prompts, `.ru hud test`, `.ru hud anim`, deathmatch winner card |
| 50 | `RU_HTML_PRIO_HUD` | ready HUD, knife panel, live / pause status panel, deathmatch leaderboard (every `hud_interval_ms`, [DEATHMATCH.md](DEATHMATCH.md)) (and every old call) |
| 10 | `RU_HTML_PRIO_INFO` | idle / background information |

While a plugin's panel is up (its `seconds` have not run out), another
plugin's lower send is refused (-1); it goes through once that panel expires, so a HUD that
re-sends comes back by itself. Within the match plugin the welcome card holds the ready HUD back
itself (by SteamID or slot); an `.admin` call card holds back the HUD, the welcome and the go-live
card for that admin.

## Measuring the redraw rate

`.ru hud anim [hz] [seconds]` (admin, to you only) redraws an ease-out progress bar with a frame
counter `hz` times a second (1-64, default 64) for `seconds` (1-30, default 10). Record the screen
and count the distinct frame numbers in one second: that is how fast the client really redraws
the panel, and the ceiling for animations.

## What renders

| Works | Does not work |
|---|---|
| `class='fontSize-s|sm|m|l|xl|xxl'`, `fontWeight-Bold`, `<b>` | `<i>`, `<u>` (rendered as plain text) |
| `<font color='#hex'>`, named colors (`red`) | `<span style=...>` |
| `<img src='https://...png'>` (public URL) | SVG images; `width`/`height` attributes (PNG draws at native size) |
| Raw UTF-8 (✔ ✖ ★ • →), decimal and hex entities | Named entities (`&check;`, `&star;`, `&rarr;`) |

Logos must be hosted as a PNG already scaled to the display size.

## When the ready panel is shown

The ready list shows in scrim warmup and in match warmup. `ru_warmup_enable 0` does not just hide
it: it turns off ready-up for loaded matches (no panel, no knife round, live on the next round
start). To hide the panel but keep ready-up, set `ready_hud=0` in `readyup.cfg` (chat reminders
come back).

## Knife panel and chat

- After the knife round starts, the KNIFE ROUND panel stays up for `hud_knife_hold_s` seconds (default 10), also while the round is running.
- While the panel reaches players, flow updates (ready, countdown, knife start, knife winner, side pick) are shown only in the panel, not in chat. Replies that explain a refused command, admin actions and the LIVE line still go to chat. With `ready_hud=0`, or if a panel send fails, the chat messages come back.

## Live panel (pauses, forfeit)

While a map is live the panel is only shown when there is something to wait for
(`plugins/match/readyup/match_features.h`), with the same every-frame resend and no images:

- TACTICAL TIMEOUT (`.tac`) with the team, until the timeout ends (`round_freeze_end`).
- TECHNICAL PAUSE with the team, "pausing at freeze time" until it takes effect, then the
  auto-unpause countdown (`tech_pause_max_seconds`) and who has typed `.unpause`.
- PAUSED BY ADMIN (only `.fup` ends it).
- `<team> LEFT` with the forfeit countdown (`forfeit_after_seconds`) while a team has nobody
  connected. Chat also announces the start, 120/60/30/10 s, a cancel and the forfeit.

## Go-live card

When a map goes live (all ready, the knife winners' `.stay` / `.switch` / `.ct` / `.t`, a scrim going
live, `ru match start`), everyone gets a card for `golive_card_seconds` (readyup.cfg, default 10,
0 = off): **LIVE · GO GO GO**, the team names with their sides (`Team A (CT) vs Team B (T)`; not in
scrims) and the commands that work: `.p` / `.pause` / `.tech`, `.up` / `.unpause`, `.tac`,
`.admin [message]`. The go-live restart and CS2's "Match started" announcement wipe the panel, so
the card starts `welcome_round_delay_ms` (5 s) after the go-live round start, like the welcome
card, and the same HTML is re-sent every frame (`hud_resend_ms` / `hud_duration_s`, like the ready
HUD, see "Refresh rules") at `RU_HTML_PRIO_NOTICE` for the full `golive_card_seconds`. It ends when
the go-live round's freeze time ends at the latest (the defaults, 5 s + 10 s, fit in `live.cfg`'s
18 s `mp_freezetime`; a shorter freeze time cuts the card short), and early when a pause or a
forfeit countdown needs the panel (the live panel above takes over). With `debug=1` (readyup.cfg), the log
shows when the card was armed, shown, skipped for a player (welcome / `.admin` card, `.ru hud anim`,
a higher panel) and why it ended, with the number of sends. Chat keeps the
single "LIVE!" line; if center HTML is unavailable, one chat line lists the commands instead.
