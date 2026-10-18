#!/usr/bin/env python3
"""Ready Up live regression test: a bot-only match or scrim on a real CS2 server.

Drives the readyup-test server through its tmux console and checks Ready Up's
state transitions from the console log. See scripts/livetest/README.md.

Match flow (default, all driven from the server console, no human needed):
  preflight   tmux session up, nobody (human) connected
  selftest    `ru selftest` must print `selftest: PASS`
  reset       `ru mode idle`, kick bots
  load        `ru match load http://127.0.0.1:<port>/match.json` (served by this
              script): empty roster, map_sides=[knife], maxRounds=4, no OT
  bots        bot_quota 4 -> 2 CT + 2 T bots in the `state:` line
  knife       match_knife (starting -> running), knife winner, pick
  pick        bots-only winner -> sides stay after 3s (or `ru match side ...`)
  live        mode=match_live
  rounds      round ends 1..2, halftime side swap, rounds 3.., `Game Over`
  postgame    mode=postgame (match still loaded) after the final round
  idle        Ready Up clears the match: mode=idle match=none

Simulation (--simulation): the same match flow, but the match config has `simulation: true` and a
roster of --bots-per-side players per team, and Ready Up brings the bots itself (simulation.h): one
bot per roster player, each plays as its player (`simulation: bot X<N> plays <name>`), readies up by
itself; `--timescale` goes into the config as `simulation_timescale`. Ready Up removes the bots when
the match is gone.

Scrim flow (--scrim): selftest, reset, `ru_dev_bots_scrim 1` (core dev flag), bots
only -> scrim_warmup -> countdown -> scrim created -> knife -> pick -> match_live.
The flag goes back to `cfg` in cleanup.

Forfeit flow (--forfeit): the match flow up to match_live (with pause / forfeit rules in
the match config), then the team names, a technical pause that unpauses by itself, the
per-team technical pause limit, a tactical timeout, and the team-left forfeit: CT bots
kicked -> countdown -> one bot back (cancelled) -> kicked again -> forfeit -> postgame -> idle.

Esports flow (--ruleset valve, docs/ESPORTS-MODE.md): a match config with map_sides knife is
refused; a match with "ruleset": "valve" + overrides loads (the esports: line names what differs),
goes live without a knife round (esports_live.cfg), bots come back, then every Valve exception cvar
(read from cfg/ReadyUp/esports_live.cfg), the Premier values and the overrides are queried from the
console; `ru match rules`, `skins_status` (inert), default_models (CT + T) and `ru selftest` are checked.

Compatibility page (CI, .github/workflows/cs2-dynamic.yml): with COMPAT_STEP_EVENTS=1 every step
is reported as it starts and ends through `scripts/ci/compat-report.py step` (nested under the
step COMPAT_STEP_PARENT, e.g. live-match), from a background thread, at most one POST every
COMPAT_STEP_INTERVAL seconds (default 5); the last state is always sent at the end. Reporting
never slows down or fails the test. In GitHub Actions each FAIL step is also a ::error:: annotation.

Exit codes: 0 PASS, 1 FAIL, 2 server busy / unavailable, or restarted by someone
else mid-test (no verdict).
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import queue
import re
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
STATE_RE = re.compile(r"\[ReadyUp\] state: (.*?) reason=(\S+)")
KV_RE = re.compile(r"(\w+)=(\S+)")
SELFTEST_RE = re.compile(r"\[ReadyUp\] selftest: (PASS|FAIL)\b(.*)")
LOADED_RE = re.compile(r"match-load\[\d+\]: match context set: matchid=(\d+) slug=(\S+)")
LOAD_ERR_RE = re.compile(r"match-load\[\d+\]: error: (.*)")
KNIFE_START_RE = re.compile(r"\[ReadyUp\] knife: starting knife round")
KNIFE_RUN_RE = re.compile(r"\[ReadyUp\] knife: round started")
KNIFE_WIN_RE = re.compile(r"\[ReadyUp\] knife: winner=(\S+) \((team\d)\)(.*)")
KNIFE_PICK_RE = re.compile(r"\[ReadyUp\] knife: side picked by (.+?): (\S+) \((.*?)\) -> (\S+);")
KNIFE_SHORT_RE = re.compile(
    r"\[ReadyUp\] knife: (?:dev flag on|simulation) and no humans on CT/T - knife round time (\S+) min")
SIM_PLAYS_RE = re.compile(r"\[ReadyUp\] simulation: bot (.*?)<(-?\d+)> plays (.+?) \((\d+), team(\d)\)")
DEV_SCRIM_RE = re.compile(r"\[ReadyUp\] dev_bots_scrim: ([01]) \(")
ROUND_END_RE = re.compile(r'Team "(CT|TERRORIST)" triggered "(SFUI_Notice_\w+)" \(CT "(\d+)"\) \(T "(\d+)"\)')
GAME_OVER_RE = re.compile(r"Game Over: (.*)")
BOT_SIDE_RE = re.compile(r'"([^"<]+)<\d+><BOT><(CT|TERRORIST)>"')
# A bot leaving. Its own pattern (not LEFT_RE): the tmux pane can put typed console input such as
# `bot_kick "Name"` in front of the log line on the same row, and LEFT_RE's name would then start at
# that first quote.
BOT_LEFT_RE = re.compile(r'"([^"<]+)<\d+><BOT><[^>]*>" disconnected')
BOT_SWITCH_RE = re.compile(r'"([^"<]+)<\d+><BOT>" switched from team <(CT|TERRORIST)> to <(CT|TERRORIST)>')
BOT_QUOTA_RE = re.compile(r'"?bot_quota"?\s*=\s*"?(\d+)')
CRASH_RE = re.compile(r"\[ReadyUp\] signal=SIG|Segmentation fault|\bAborted\b \(core dumped\)")
PLAYER_RE = r'"(?P<name>.*?)<(?P<uid>\d+)><(?P<sid>[^>]*)><[^>]*>"'
ENTERED_RE = re.compile(PLAYER_RE + r" entered the game")
LEFT_RE = re.compile(PLAYER_RE + r" disconnected")
BOOT_MARKERS = ("Loaded real libserver.so", "player server started")
TEAMNAME_RE = re.compile(r'"?mp_teamname_([12])"?\s*(?:=|is)\s*"?([^"\r\n]*)')
BOT_JOIN_RE = re.compile(r'"([^"<]+)<\d+><BOT>(?:<[^>]*>)?" switched from team <[^>]*> to <(CT|TERRORIST)>')
QUOTA_MODE_RE = re.compile(r'"?bot_quota_mode"?\s*=\s*"?(\w+)')
PAUSE_RE = re.compile(r"\[ReadyUp\] pause: (.*)")
FORFEIT_RE = re.compile(r"\[ReadyUp\] forfeit: (.*)")

ESPORTS_RE = re.compile(r"\[ReadyUp\] esports: ruleset=(\S+) \((.*?)\) go-live cfg=(\S+) differs=(\S+)")
GOTV_REFUSED_RE = re.compile(r"\[ReadyUp\] esports: gotv=down: go-live refused")
DEFAULT_MODEL_RE = re.compile(r"\[ReadyUp\] esports: default_models: slot (-?\d+) \((CT|T)\) -> (\S+)")
DEFAULT_MODEL_FAIL_RE = re.compile(r"\[ReadyUp\] esports: default_models: entity_set_model failed")
RULES_DIFF_RE = re.compile(r"\[ReadyUp\] rules: (differs from \S+:.*|no differences from \S+)")
SKINS_STATUS_RE = re.compile(r"status: version \S+; (skins inert \([^)]*\); )?loadouts")
UNKNOWN_CMD_RE = re.compile(r"Unknown command '([a-z0-9_]+)'!")
CVAR_RE = re.compile(r'^\s*"?([a-z][a-z0-9_]+)"?\s*=\s*"?([^"\s]*)')

MATCH_SLUG = "livetest"

# Postgame -> idle: Ready Up clears the match at the series-end kick, restart_delay - 1 +
# series_end_kick_delay (90 s default since #234) after Game Over. With the CS2 default tv_delay 10
# and a demo: restart_delay 35 s -> 124 s. A tv_delay 105 match (ruleset valve) needs 130 - 1 + 90.
IDLE_AFTER_SERIES_TIMEOUT = 180

# --ruleset valve: overrides under test (on top of Valve's preset) and what differs because of them.
VALVE_OVERRIDES = {"freezetime": 5, "spectators_max": 8, "default_models": True, "overtime": {"startmoney": 12500}}
VALVE_DIFFERS = "freezetime,overtime.startmoney,spectators_max,default_models"
# Premier values esports_live.cfg does not list itself (they come from gamemode_competitive[_tmm].cfg)
# plus the overrides; the Valve exception list is read from cfg/ReadyUp/esports_live.cfg.
VALVE_EXTRA_CVARS = {
    "mp_freezetime": "5",             # override (Premier 20)
    "mp_spectators_max": "8",         # override (Valve 10)
    "mp_overtime_startmoney": "12500",  # override (engine default 10000)
    "mp_team_timeout_time": "31",
    "mp_team_timeout_max": "3",
    "mp_team_timeout_ot_add_once": "1",
    "mp_weapons_allow_zeus": "5",
    "mp_respawn_immunitytime": "-1",
    "mp_technical_timeout_per_team": "1",
    "mp_technical_timeout_duration_s": "120",
    "tv_delay": "105",
    "mp_overtime_enable": "1",
}

# Rulebook cvars CS2 1.41.8 does not have ("Unknown command" when esports_live.cfg sets them and when
# queried). The cfg keeps them (Valve's list); the test fails if any other cvar goes missing.
VALVE_ABSENT = {"sv_maxusrcmdprocessticks", "sv_max_dropped_packets_to_process", "sv_damage_print_enable",
                "sv_occlude_players", "sv_force_transmit_players", "sv_force_transmit_ents", "sv_holiday_mode"}


def valve_exception_cvars() -> dict:
    """Section 2 of cfg/ReadyUp/esports_live.cfg (Valve's exception list): cvar -> value."""
    out: dict = {}
    section = 0
    with open(os.path.join(REPO_ROOT, "cfg", "ReadyUp", "esports_live.cfg")) as fh:
        for raw in fh:
            line = raw.strip()
            if line.startswith("// 2)"):
                section = 2
            elif line.startswith("// 3)"):
                break
            if section != 2 or not line or line.startswith("//"):
                continue
            parts = line.split("//", 1)[0].split()
            if len(parts) == 2 and parts[0] != "log":
                out[parts[0]] = parts[1]
    return out


def cvar_equal(got: str, want: str) -> bool:
    norm = {"true": "1", "false": "0"}
    g, w = norm.get(got.lower(), got), norm.get(want.lower(), want)
    if g == w:
        return True
    if got.lower() == "true" and w not in ("0", ""):
        return True  # a bool cvar on this build (mp_logmoney 2 reads back as true)
    try:
        return float(g) == float(w)
    except ValueError:
        return False


def log(msg: str) -> None:
    print(f"[livetest {time.strftime('%H:%M:%S')}] {msg}", flush=True)


# --------------------------------------------------------------------------- server access


class Server:
    """Console access to the test server: tmux send-keys + the console log."""

    def __init__(self, ssh: str, target: str, session: str):
        self.ssh = ssh
        self.target = target.rstrip("/")
        self.session = session
        self.log_path = f"{self.target}/console.log"

    def _argv(self, remote_cmd: str) -> list[str]:
        if not self.ssh:
            return ["bash", "-c", remote_cmd]
        return ["ssh", "-o", "BatchMode=yes", self.ssh, remote_cmd]

    def run(self, remote_cmd: str, check: bool = False, timeout: int = 60) -> subprocess.CompletedProcess:
        return subprocess.run(self._argv(remote_cmd), capture_output=True, text=True, errors="replace",
                              check=check, timeout=timeout)

    def session_alive(self) -> bool:
        return self.run(f"tmux has-session -t {shlex.quote(self.session)}").returncode == 0

    def send(self, cmd: str) -> None:
        log(f"console> {cmd}")
        s = shlex.quote(self.session)
        self.run(f"tmux send-keys -t {s} -l {shlex.quote(cmd)} && tmux send-keys -t {s} Enter", check=True)

    def log_size(self) -> int:
        r = self.run(f"stat -c %s {shlex.quote(self.log_path)}", check=True)
        return int(r.stdout.strip())

    def follow(self, start_offset: int) -> "LogFollower":
        return LogFollower(self._argv(f"exec tail -c +{start_offset + 1} -F {shlex.quote(self.log_path)}"))

    def connected_humans(self) -> list[str]:
        """Humans that entered the game since the last server boot and have not disconnected."""
        pats = "|".join(re.escape(m) for m in BOOT_MARKERS) + "|entered the game|disconnected"
        r = self.run(f"grep -a -E {shlex.quote(pats)} {shlex.quote(self.log_path)} | tail -n 5000")
        present: dict[tuple[str, str], str] = {}
        for raw in r.stdout.splitlines():
            line = ANSI_RE.sub("", raw)
            if any(m in line for m in BOOT_MARKERS):
                present.clear()
                continue
            m = ENTERED_RE.search(line)
            if m and m.group("sid") != "BOT":
                present[(m.group("uid"), m.group("sid"))] = m.group("name")
                continue
            m = LEFT_RE.search(line)
            if m:
                present.pop((m.group("uid"), m.group("sid")), None)
        return sorted(present.values())

    def boot(self, timeout: int) -> bool:
        """Start the tmux session (same recipe as scripts/dev-deploy.sh --restart)."""
        t, s, lg = shlex.quote(self.target), shlex.quote(self.session), shlex.quote(self.log_path)
        script = (f"touch {lg}; start=$(( $(stat -c %s {lg}) + 1 )); "
                  f"tmux new-session -d -s {s} -x 250 -y 50 {t}/run.sh && "
                  f"tmux pipe-pane -t {s} -o 'cat >> {self.log_path}'; "
                  f"for i in $(seq {timeout}); do "
                  f"  tail -c +$start {lg} | grep -a -q 'player server started' && exit 0; "
                  f"  tmux has-session -t {s} 2>/dev/null || exit 1; sleep 1; done; exit 1")
        return self.run(script, timeout=timeout + 30).returncode == 0


class LogFollower:
    def __init__(self, argv: list[str]):
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                                     errors="replace", bufsize=1)
        self.q: "queue.Queue[str]" = queue.Queue()
        self.t = threading.Thread(target=self._pump, daemon=True)
        self.t.start()

    def _pump(self) -> None:
        assert self.proc.stdout
        for raw in self.proc.stdout:
            self.q.put(ANSI_RE.sub("", raw).replace("\r", "").rstrip("\n"))

    def lines(self, wait: float) -> list[str]:
        out: list[str] = []
        try:
            out.append(self.q.get(timeout=wait))
            while True:
                out.append(self.q.get_nowait())
        except queue.Empty:
            pass
        return out

    def close(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# --------------------------------------------------------------------------- match config server


def serve_match_json(bind: str, doc: dict) -> tuple[http.server.HTTPServer, int]:
    body = json.dumps(doc).encode()

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            if self.path.split("?")[0] != "/match.json":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            log("http: " + fmt % args)

    srv = http.server.HTTPServer((bind, 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def sim_roster(team: int, per_side: int) -> dict:
    """--simulation: made-up roster players (individual-account SteamID64 format), per team."""
    return {str(76561198990000000 + team * 100 + i): f"Sim{'AB'[team - 1]}{i}" for i in range(1, per_side + 1)}


def match_doc(map_name: str, max_rounds: int, forfeit: bool = False, simulation: Optional[dict] = None) -> dict:
    matchid = int(time.time() * 1000)
    doc = {
        "id": matchid,
        "slug": MATCH_SLUG,
        "config": {
            "matchid": matchid,
            "num_maps": 1,
            "maplist": [map_name],
            "map_sides": ["knife"],
            "maxRounds": max_rounds,
            "overtimeMode": "disabled",
            "knifeDecisionSeconds": 5,
            "team1": {"name": "LiveTestA", "players": {}},
            "team2": {"name": "LiveTestB", "players": {}},
            # Applied after live.cfg on the knife -> live path (modes.cpp ApplyMatchCvarsLocked).
            "cvars": {
                "mp_maxrounds": max_rounds,
                "mp_overtime_enable": 0,
                "mp_halftime": 1,
                "mp_halftime_duration": 3,
                "mp_freezetime": 1,
                "mp_roundtime": 1,
                "mp_roundtime_defuse": 1,
                "mp_roundtime_hostage": 1,
                "mp_round_restart_delay": 2,
                "mp_c4timer": 20,
                "mp_win_panel_display_time": 1,
                "mp_team_timeout_max": 0,
            },
        },
    }
    if simulation:
        # Ready Up adds one bot per roster player and readies them up (simulation.h).
        cfg = doc["config"]
        cfg["simulation"] = True
        cfg["simulation_timescale"] = simulation["timescale"]
        cfg["team1"]["players"] = sim_roster(1, simulation["per_side"])
        cfg["team2"]["players"] = sim_roster(2, simulation["per_side"])
    if forfeit:
        cfg = doc["config"]
        # Rules under test (match_rules.h): 1 technical pause per team, 8 s auto-unpause,
        # both teams unpause, 20 s forfeit; 1 tactical timeout of 5 s. Long map (the forfeit
        # ends it) and no auto team balance (it would move a T bot to the empty CT side).
        cfg.update({"max_tech_pauses_per_team": 1, "tech_pause_max_seconds": 8,
                    "both_teams_unpause_required": True, "forfeit_after_seconds": 20, "maxRounds": 30})
        cfg["cvars"].update({"mp_maxrounds": 30, "mp_team_timeout_max": 1, "mp_team_timeout_time": 5,
                             "mp_autoteambalance": 0, "mp_limitteams": 0})
    return doc


def valve_doc(map_name: str, max_rounds: int, knife: bool = False) -> dict:
    """ruleset valve: sides from the "veto" (team1_ct), overrides on top of Valve's preset, and only
    match cvars that no Valve rule covers (short rounds for bots)."""
    doc = match_doc(map_name, max_rounds)
    cfg = doc["config"]
    cfg.pop("overtimeMode", None)
    cfg.update({"ruleset": "valve", "overrides": VALVE_OVERRIDES, "map_sides": ["knife" if knife else "team1_ct"]})
    cfg["cvars"] = {"mp_maxrounds": max_rounds, "mp_roundtime": 1, "mp_roundtime_defuse": 1, "mp_roundtime_hostage": 1,
                    "mp_round_restart_delay": 2, "mp_c4timer": 20, "mp_halftime_duration": 3,
                    "mp_win_panel_display_time": 1}
    if knife:
        doc["id"] = cfg["matchid"] = cfg["matchid"] + 1
    return doc


# --------------------------------------------------------------------------- facts + steps


@dataclass
class Facts:
    seq: int = 0
    states: list = field(default_factory=list)  # (seq, fields, reason)
    modes: list = field(default_factory=list)  # deduped mode sequence
    selftest: Optional[tuple] = None
    selftest_after: int = 0
    loaded: Optional[tuple] = None
    load_error: Optional[str] = None
    knife_start: int = 0
    knife_run: int = 0
    knife_win: Optional[tuple] = None
    knife_pick: Optional[tuple] = None
    live_seq: int = 0
    round_ends: list = field(default_factory=list)  # (seq, total, ct, t, notice)
    bot_sides: dict = field(default_factory=dict)  # name -> [(seq, side)] (side changes only)
    bot_switches: list = field(default_factory=list)  # (seq, name, from, to) CT<->T only
    game_over: Optional[tuple] = None
    crash: Optional[str] = None
    restarted: Optional[str] = None
    bot_quota: Optional[int] = None
    notes: list = field(default_factory=list)
    armed: bool = False  # scrim mode: a scrim may go live (there is no match-load line)
    dev_scrim: Optional[tuple] = None  # (seq, "0"|"1") reply to ru_dev_bots_scrim
    knife_short: Optional[str] = None  # dev-flag knife round time (min)
    sim_plays: dict = field(default_factory=dict)  # roster steamid64 -> (seq, bot name) (--simulation)
    our_matchid: Optional[str] = None  # set once our match loaded
    teamnames: dict = field(default_factory=dict)  # "1"/"2" -> (seq, value)
    pauses: list = field(default_factory=list)  # (seq, text) `pause:` lines
    forfeits: list = field(default_factory=list)  # (seq, text) `forfeit:` lines
    bot_quota_mode: Optional[str] = None
    bots_gone: set = field(default_factory=set)  # bot names that disconnected
    esports: list = field(default_factory=list)  # (seq, ruleset, source, cfg, differs) at match load
    gotv_refused: int = 0  # seq of the last "esports: gotv=down: go-live refused" line
    default_models: list = field(default_factory=list)  # (seq, slot, side, model)
    default_model_fail: Optional[str] = None
    rules_diff: list = field(default_factory=list)  # (seq, text) `ru match rules` difference line
    skins_status: list = field(default_factory=list)  # (seq, "skins inert (...); " or "")
    cvars: dict = field(default_factory=dict)  # name -> (seq, value) from console cvar queries
    unknown_cmds: dict = field(default_factory=dict)  # name -> seq of "Unknown command 'name'!"

    def last_state(self) -> Optional[dict]:
        return self.states[-1][1] if self.states else None

    def feed(self, line: str) -> None:
        self.seq += 1
        s = self.seq
        if (m := STATE_RE.search(line)):
            fields = dict(KV_RE.findall(m.group(1)))
            self.states.append((s, fields, m.group(2)))
            mode = fields.get("mode", "?")
            label = mode + (f"/knife={fields['knife']}" if "knife" in fields else "") + \
                (" golive=pending" if fields.get("golive") == "pending" else "") + \
                (" countdown" if "countdown" in fields else "")
            if not self.modes or self.modes[-1] != label:
                self.modes.append(label)
            if mode == "match_live" and not self.live_seq and (self.loaded or self.armed):
                self.live_seq = s
            return
        if (m := DEV_SCRIM_RE.search(line)):
            self.dev_scrim = (s, m.group(1))
            return
        if (m := PAUSE_RE.search(line)):
            self.pauses.append((s, m.group(1).strip()))
            return
        if (m := FORFEIT_RE.search(line)):
            self.forfeits.append((s, m.group(1).strip()))
            return
        if (m := ESPORTS_RE.search(line)):
            self.esports.append((s, m.group(1), m.group(2), m.group(3), m.group(4)))
            return
        if GOTV_REFUSED_RE.search(line):
            self.gotv_refused = s
            return
        if (m := DEFAULT_MODEL_RE.search(line)):
            self.default_models.append((s, m.group(1), m.group(2), m.group(3)))
            return
        if DEFAULT_MODEL_FAIL_RE.search(line):
            self.default_model_fail = line.strip()
            return
        if (m := RULES_DIFF_RE.search(line)):
            self.rules_diff.append((s, m.group(1)))
            return
        if (m := SKINS_STATUS_RE.search(line)):
            self.skins_status.append((s, m.group(1) or ""))
            return
        if (m := UNKNOWN_CMD_RE.search(line)):
            self.unknown_cmds[m.group(1)] = s
        if "[ReadyUp]" not in line and (m := CVAR_RE.match(line)):
            self.cvars[m.group(1)] = (s, m.group(2))
        if "[ReadyUp]" not in line and (m := TEAMNAME_RE.search(line)):
            self.teamnames[m.group(1)] = (s, m.group(2).strip())
        if (m := KNIFE_SHORT_RE.search(line)):
            self.knife_short = m.group(1)
            return
        if (m := SIM_PLAYS_RE.search(line)):
            if m.group(4) != "0":
                self.sim_plays[m.group(4)] = (s, m.group(1))
            return
        if (m := SELFTEST_RE.search(line)) and s > self.selftest_after and not self.selftest:
            self.selftest = (m.group(1), m.group(2).strip(), line.strip())
            return
        if (m := LOADED_RE.search(line)):
            if self.our_matchid and self.our_matchid != m.group(1):
                # Someone else (a concurrent test on the shared server) replaced our match.
                self.restarted = self.restarted or f"another match was loaded mid-test: {line.strip()}"
            self.loaded = (s, m.group(1), m.group(2))
            return
        if (m := LOAD_ERR_RE.search(line)):
            self.load_error = m.group(1)
            return
        if KNIFE_START_RE.search(line):
            self.knife_start = self.knife_start or s
            return
        if KNIFE_RUN_RE.search(line):
            self.knife_run = self.knife_run or s
            return
        if (m := KNIFE_WIN_RE.search(line)):
            self.knife_win = (s, m.group(1), m.group(2), m.group(3).strip())
            return
        if (m := KNIFE_PICK_RE.search(line)):
            self.knife_pick = (s, m.group(1), m.group(2), m.group(4))
            return
        # "player server started" alone is also printed by host_workshop_map (a workshop map starts
        # a new server host state); only the core's own load line means the process restarted.
        if BOOT_MARKERS[0] in line:
            self.restarted = self.restarted or line.strip()
        if CRASH_RE.search(line):
            self.crash = line.strip()
        if (m := BOT_QUOTA_RE.search(line)) and "bot_quota" in line and self.bot_quota is None:
            self.bot_quota = int(m.group(1))
        if (m := BOT_SWITCH_RE.search(line)):
            self.bot_switches.append((s, m.group(1), m.group(2), m.group(3)))
        if (m := BOT_JOIN_RE.search(line)):
            hist = self.bot_sides.setdefault(m.group(1), [])
            if not hist or hist[-1][1] != m.group(2):
                hist.append((s, m.group(2)))
            self.bots_gone.discard(m.group(1))
        if (m := QUOTA_MODE_RE.search(line)) and "[ReadyUp]" not in line and self.bot_quota_mode is None:
            self.bot_quota_mode = m.group(1)
        if (m := BOT_LEFT_RE.search(line)):
            self.bots_gone.add(m.group(1))
        for name, side in BOT_SIDE_RE.findall(line):
            hist = self.bot_sides.setdefault(name, [])
            if not hist or hist[-1][1] != side:
                hist.append((s, side))
        if self.live_seq and (m := ROUND_END_RE.search(line)):
            ct, t = int(m.group(3)), int(m.group(4))
            total = ct + t
            if not self.round_ends or self.round_ends[-1][1] != total:
                self.round_ends.append((s, total, ct, t, m.group(2)))
            return
        if self.live_seq and (m := GAME_OVER_RE.search(line)):
            self.game_over = (s, m.group(1).strip())

    def states_since(self, seq: int):
        return [(s, f, r) for (s, f, r) in self.states if s > seq]

    def halftime_swap(self, half: int) -> Optional[str]:
        """Evidence that sides swapped after round `half` (bot side flip or score swap)."""
        ends = {total: (s, ct, t) for (s, total, ct, t, _) in self.round_ends}
        if half not in ends:
            return None
        half_seq = ends[half][0]
        flipped = [f"{name} {a}->{b}" for (s, name, a, b) in self.bot_switches if s > half_seq]
        if not flipped:
            # Fallback: a bot's side in log headers before vs after the halftime round end.
            for name, hist in self.bot_sides.items():
                before = [side for (s, side) in hist if s <= half_seq]
                after = [side for (s, side) in hist if s > half_seq]
                if before and after and after[-1] != before[-1]:
                    flipped.append(f"{name} {before[-1]}->{after[-1]}")
        if flipped:
            return "bot side flip: " + ", ".join(sorted(flipped))
        if half + 1 in ends:
            _, ct1, t1 = ends[half]
            _, ct2, t2 = ends[half + 1]
            if ct1 != t1 and ct2 >= t1 and t2 >= ct1 and not (ct2 >= ct1 and t2 >= t1):
                return f"score swap: CT {ct1}:T {t1} -> CT {ct2}:T {t2}"
        return None


class StepEvents:
    """Reports steps to the compatibility page (compat-report.py step) from a background thread,
    in order, batching whatever queued up while the previous call ran. Off unless
    COMPAT_STEP_EVENTS=1. Never raises into the test."""

    def __init__(self, parent: str, interval: float):
        self.parent = parent
        self.interval = interval
        self.q: "queue.Queue[Optional[dict]]" = queue.Queue()
        self.script = os.path.join(REPO_ROOT, "scripts", "ci", "compat-report.py")
        self.thread = threading.Thread(target=self._work, name="compat-steps", daemon=True)
        self.thread.start()

    @classmethod
    def from_env(cls) -> "Optional[StepEvents]":
        if os.environ.get("COMPAT_STEP_EVENTS", "") != "1":
            return None
        parent = os.environ.get("COMPAT_STEP_PARENT", "") or "livetest"
        try:
            interval = float(os.environ.get("COMPAT_STEP_INTERVAL", "5"))
        except ValueError:
            interval = 5.0
        return cls(parent, interval)

    def step_id(self, index: int) -> str:
        return f"{self.parent}.{index:02d}"

    def send(self, index: int, name: str, status: str, detail: str = "") -> None:
        upd = {"id": self.step_id(index), "name": name, "stage": "live", "parent": self.parent, "status": status}
        if detail:
            upd["detail"] = detail
        self.q.put(upd)

    def _call(self, *extra: str) -> None:
        try:
            r = subprocess.run([sys.executable, self.script, "step", *extra], cwd=REPO_ROOT,
                               capture_output=True, text=True, timeout=40)
            for line in (r.stdout + r.stderr).splitlines():
                if "warning" in line or r.returncode:
                    log(f"compat step: {line}")
        except Exception as e:  # noqa: BLE001 - reporting must never break the test
            log(f"compat step: {type(e).__name__}")

    def _work(self) -> None:
        stop = False
        while not stop:
            batch = [self.q.get()]
            while True:
                try:
                    batch.append(self.q.get_nowait())
                except queue.Empty:
                    break
            if None in batch:
                stop = True
            updates = [u for u in batch if u is not None]
            if updates:
                self._call("--updates", json.dumps(updates), "--min-interval", str(self.interval))
            if stop:
                self._call("--flush")

    def close(self, timeout: float = 60) -> None:
        self.q.put(None)
        self.thread.join(timeout)


def gh_escape(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


@dataclass
class Step:
    name: str
    timeout: float
    check: Callable[[Facts], "bool | str"]  # True = pass, str = hard failure reason
    action: Optional[Callable[[], None]] = None
    detail: Callable[[Facts], str] = lambda f: ""
    status: str = "PEND"
    elapsed: float = 0.0
    info: str = ""


class Runner:
    def __init__(self, args):
        self.a = args
        self.srv = Server(args.ssh, args.target, args.session)
        self.f = Facts()
        self.follower: Optional[LogFollower] = None
        self.http: Optional[http.server.HTTPServer] = None
        self.results: list[Step] = []
        self.captured: list[str] = []
        self.match_loaded = False
        self.touched = False
        self.timescale_set = False
        self.flag_set = False
        self.quota_mode_set = False
        self.events: Optional[StepEvents] = None
        self.step_index: dict[int, int] = {}  # id(Step) -> 1-based position, for the step events

    # ---- plumbing
    def pump(self, wait: float = 0.5) -> None:
        assert self.follower
        for line in self.follower.lines(wait):
            self.captured.append(line)
            self.f.feed(line)

    def emit(self, st: Step, status: str, detail: str = "") -> None:
        if self.events and id(st) in self.step_index:
            self.events.send(self.step_index[id(st)], st.name, status, detail)

    def run_step(self, st: Step) -> bool:
        log(f"step {st.name} (timeout {st.timeout:.0f}s)")
        self.emit(st, "running")
        t0 = time.time()
        if st.action:
            st.action()
        last_alive_check = t0
        while True:
            self.pump(0.5)
            res = st.check(self.f)
            if self.f.crash:
                res = f"crash in log: {self.f.crash}"
            elif self.f.restarted:
                res = "server restarted during the test (another deploy?) - result not valid"
            now = time.time()
            if res is not True and now - last_alive_check > 10:
                last_alive_check = now
                if not self.srv.session_alive():
                    res = f"tmux session {self.a.session} died"
            if res is True:
                st.status = "PASS"
            elif isinstance(res, str) and res.startswith("SKIP"):
                st.status, st.info = "SKIP", res[5:].strip()
            elif isinstance(res, str) and res:
                st.status, st.info = "FAIL", res
            elif now - t0 > st.timeout:
                st.status, st.info = "FAIL", f"timeout after {st.timeout:.0f}s"
            if st.status != "PEND":
                st.elapsed = now - t0
                d = st.detail(self.f)
                if d:
                    st.info = (st.info + "; " if st.info else "") + d
                log(f"step {st.name}: {st.status} {st.info}")
                self.emit(st, st.status.lower(), st.info)
                self.results.append(st)
                return st.status in ("PASS", "SKIP")

    # ---- scenario
    def steps(self) -> list[Step]:
        f = self.f
        a = self.a
        half = a.max_rounds // 2
        mark: dict[str, int] = {}

        def since(key: str) -> int:
            return mark.get(key, 0)

        def state_where(pred, after_key: str):
            for s, fl, _ in f.states_since(since(after_key)):
                if pred(fl):
                    return fl
            return None

        def act_selftest():
            f.selftest_after = f.seq
            self.srv.send("ru selftest")

        def chk_selftest(_f):
            if not f.selftest:
                return False
            return True if f.selftest[0] == "PASS" else f"selftest: {f.selftest[0]} {f.selftest[1]}"

        def act_reset():
            mark["reset"] = f.seq
            self.touched = True
            self.srv.send("bot_quota")  # echo current value so it can be restored
            self.srv.send("ru mode idle")
            self.srv.send("bot_kick")
            self.srv.send("ru match state")

        def chk_reset(_f):
            return state_where(lambda fl: fl.get("mode") == "idle" and fl.get("match") == "none", "reset") is not None

        def act_load():
            mark["load"] = f.seq
            cur = f.last_state() or {}
            map_name = a.map or cur.get("map") or "de_dust2"
            if map_name == "?":
                map_name = "de_dust2"
            sim = {"per_side": a.bots_per_side, "timescale": a.timescale} if a.simulation else None
            doc = match_doc(map_name, a.max_rounds, forfeit=a.forfeit, simulation=sim)
            self.http, port = serve_match_json(a.http_bind, doc)
            url = f"http://{a.http_host}:{port}/match.json"
            log(f"serving match {doc['config']['matchid']} on {url} (map {map_name})")
            self.srv.send(f"ru match load {url}")

        def chk_load(_f):
            if f.load_error:
                return f"match load error: {f.load_error}"
            if not f.loaded or f.loaded[0] <= since("load"):
                return False
            if f.loaded[2] != MATCH_SLUG:
                return f"loaded unexpected slug {f.loaded[2]}"
            self.match_loaded = True
            f.our_matchid = f.loaded[1]
            return True

        def act_bots():
            if a.simulation:
                log("simulation: Ready Up adds the bots itself")
                return
            # The match load kicks bots and sets bot_quota 0; bring 2 per side back.
            time.sleep(0.3)
            self.srv.send("bot_join_after_player 0")  # bots join without a human on the server (empty CI server)
            self.srv.send(f"bot_quota {a.bots_per_side * 2}")

        def chk_warmup(_f):
            if state_where(lambda fl: fl.get("mode") == "match_warmup" and fl.get("warmup") == "1",
                           "load") is not None:
                return True
            # With bots filling both teams the flow can pass warmup before a state line
            # shows it (no countdown in the match flow, see chk_countdown): a knife or
            # live state since the load means warmup was entered and left.
            later = state_where(lambda fl: fl.get("mode") in ("match_knife", "match_live"), "load")
            if later is not None:
                f.warmup_note = f"passed through (next seen: {later.get('mode')})"
                return True
            return False

        def bots_ok(fl):
            # bot_quota counts differ with bot_quota_mode; require >= N bots and no humans per side.
            def side_ok(v):
                h, _, b = (v or "").partition("+")
                return h == "0" and b.isdigit() and int(b) >= a.bots_per_side
            return side_ok(fl.get("ct")) and side_ok(fl.get("t"))

        def chk_bots(_f):
            if a.simulation and len(f.sim_plays) < 2 * a.bots_per_side:
                return False  # every roster player needs its bot
            return any(bots_ok(fl) for _, fl, _ in f.states_since(since("load")))

        def bots_detail(f_):
            d = next((f"ct={fl.get('ct')} t={fl.get('t')}" for _, fl, _ in reversed(f_.states)), "")
            if a.simulation:
                d += f"; simulation: {len(f_.sim_plays)}/{2 * a.bots_per_side} roster players have a bot"
            return d

        def chk_countdown(_f):
            return ("SKIP: scrim-only; the match flow goes straight to the knife round. "
                    "Covered by `run.sh --scrim`")

        def chk_knife_start(_f):
            if f.live_seq:
                return "went live without a knife round"
            return state_where(lambda fl: fl.get("mode") == "match_knife", "load") is not None and f.knife_start > 0

        def chk_knife_run(_f):
            return f.knife_run > 0 and state_where(lambda fl: fl.get("knife") == "running", "load") is not None

        def chk_knife_end(_f):
            return f.knife_win is not None and state_where(lambda fl: fl.get("knife") == "pick", "load") is not None

        def act_pick():
            if a.side != "auto":
                self.srv.send(f"ru match side {a.side}")

        def chk_pick(_f):
            if not f.knife_pick:
                return False
            if a.side == "auto" and f.knife_pick[1] != "timeout":
                return f"side picked by {f.knife_pick[1]}, expected the bots-only timeout"
            want = {"auto": "stay", "stay": "stay", "switch": "mp_swapteams"}.get(a.side)
            if want and f.knife_pick[3] != want:
                return f"pick result {f.knife_pick[3]}, expected {want}"
            return state_where(lambda fl: fl.get("mode") == "match_warmup" and fl.get("golive") == "pending",
                               "load") is not None

        def chk_live(_f):
            return f.live_seq > 0

        def act_timescale():
            if a.simulation:
                return  # simulation_timescale in the match config; Ready Up applies it while live
            if a.timescale and a.timescale != 1:
                self.srv.send("sv_cheats 1")
                self.srv.send(f"host_timescale {a.timescale}")
                self.timescale_set = True

        def final_round_seq() -> int:
            win = a.max_rounds // 2 + 1
            for (s, total, ct, t, _) in f.round_ends:
                if ct >= win or t >= win or total >= a.max_rounds:
                    return s
            return 0

        def no_early_exit():
            if f.game_over or final_round_seq():
                return None
            for _, fl, _ in f.states_since(f.live_seq):
                if fl.get("mode") != "match_live":
                    return f"left match_live early: mode={fl.get('mode')}"
            return None

        def chk_round(n):
            def _c(_f):
                if (e := no_early_exit()):
                    return e
                return any(total >= n for (_, total, _, _, _) in f.round_ends)
            return _c

        def chk_half(_f):
            if (e := no_early_exit()):
                return e
            return f.halftime_swap(half) is not None

        def chk_game_over(_f):
            if (e := no_early_exit()):
                return e
            return f.game_over is not None

        def chk_postgame(_f):
            # Postgame is entered at the final round end (same frame as its log line); the
            # state: line follows on the next tick. The match stays loaded until the reset.
            if not final_round_seq():
                return False
            for s, fl, _ in f.states_since(f.live_seq):
                if fl.get("mode") == "postgame":
                    if fl.get("match") in (None, "none"):
                        return "postgame without a match loaded"
                    mark["postgame"] = s
                    return True
                if fl.get("mode") == "idle":
                    return "went to idle without postgame"
            return False

        def chk_postgame_any(_f):
            for s, fl, _ in f.states_since(f.live_seq):
                if fl.get("mode") == "postgame":
                    if fl.get("match") in (None, "none"):
                        return "postgame without a match loaded"
                    mark["postgame"] = s
                    return True
                if fl.get("mode") == "idle":
                    return "went to idle without postgame"
            return False

        def chk_idle_after_postgame(_f):
            for _, fl, _ in f.states_since(since("postgame")):
                if fl.get("mode") == "idle" and fl.get("match") == "none":
                    return True
                if fl.get("mode") not in ("postgame", "idle"):
                    return f"left postgame for {fl.get('mode')}, expected idle"
            return False

        def rounds_detail(_f):
            return " ".join(f"r{total}={ct}:{t}" for (_, total, ct, t, _) in f.round_ends)

        def knife_detail(f_):
            if not f_.knife_win:
                return ""
            short = f" [dev knife time {f_.knife_short} min]" if f_.knife_short else ""
            return (f"winner={f_.knife_win[1]} ({f_.knife_win[2]}) {f_.knife_win[3]}"[:140]) + short

        knife_steps = [
            Step("knife: match_knife starting", 60, chk_knife_start),
            Step("knife: round running", 60, chk_knife_run),
            Step("knife: winner decided", 240, chk_knife_end, None, knife_detail),
            Step(f"pick ({a.side}) -> golive pending", 30, chk_pick, act_pick,
                 lambda f_: f"by {f_.knife_pick[1]}: {f_.knife_pick[2]} -> {f_.knife_pick[3]}" if f_.knife_pick else ""),
            Step("match_live", 30, chk_live, None),
        ]

        if a.scrim:
            return self.scrim_steps(f, a, mark, since, state_where, bots_ok, act_selftest, chk_selftest,
                                    act_reset, chk_reset, knife_steps)
        if a.ruleset == "valve":
            return self.valve_steps(f, a, mark, since, bots_ok, act_selftest, chk_selftest, act_reset, chk_reset,
                                    chk_warmup, act_bots)

        # mp_teamname_1 = the team starting on CT (team1 unless the knife pick switched).
        def act_teamnames():
            mark["names"] = f.seq
            time.sleep(1.0)
            self.srv.send("mp_teamname_1")
            self.srv.send("mp_teamname_2")

        def chk_teamnames(_f):
            got = {k: v for k, (sq, v) in f.teamnames.items() if sq > since("names")}
            if "1" not in got or "2" not in got:
                return False
            switched = f.knife_pick is not None and f.knife_pick[3] == "mp_swapteams"
            want = ("LiveTestB", "LiveTestA") if switched else ("LiveTestA", "LiveTestB")
            if (got["1"], got["2"]) != want:
                return f"mp_teamname_1/2 = {got['1']!r}/{got['2']!r}, expected {want[0]!r}/{want[1]!r}"
            return True

        teamname_step = Step("team names (mp_teamname_1/2)", 20, chk_teamnames, act_teamnames,
                             lambda f_: " / ".join(f"{k}={v[1]}" for k, v in sorted(f_.teamnames.items())))

        steps = [
            Step("selftest", 45, chk_selftest, act_selftest,
                 lambda f_: f_.selftest[2].split("] ", 1)[-1] if f_.selftest else ""),
            Step("reset (idle, no match)", 30, chk_reset, act_reset),
            Step("match load (ru match load)", 30, chk_load, act_load,
                 lambda f_: f"matchid={f_.loaded[1]} slug={f_.loaded[2]}" if f_.loaded else ""),
            Step("warmup (match_warmup)", 60 if a.simulation else 20, chk_warmup, act_bots,
                 lambda f_: getattr(f_, "warmup_note", "")),
            Step(f"bots (>= {a.bots_per_side} per side, no humans)", 60, chk_bots, None, bots_detail),
            Step("countdown", 1, chk_countdown),
        ] + knife_steps + [teamname_step]
        if a.forfeit:
            return steps + self.forfeit_steps(f, mark, since, chk_postgame_any, chk_idle_after_postgame)
        steps.append(Step("round 1 ends", a.round_timeout, chk_round(1), act_timescale, rounds_detail))
        for n in range(2, half + 1):
            steps.append(Step(f"round {n} ends", a.round_timeout, chk_round(n), None, rounds_detail))
        steps.append(Step(f"halftime swap after round {half}", a.round_timeout + 30, chk_half, None,
                          lambda f_: f_.halftime_swap(half) or ""))
        steps.append(Step(f"round {half + 1} ends", a.round_timeout, chk_round(half + 1), None, rounds_detail))
        steps.append(Step("map end (Game Over)", a.round_timeout * max(1, a.max_rounds - half - 1) + 30,
                          chk_game_over, None,
                          lambda f_: (f_.game_over[1] + " | " + rounds_detail(f_)) if f_.game_over else rounds_detail(f_)))
        steps.append(Step("postgame (mode=postgame, match loaded)", 30, chk_postgame))
        steps.append(Step("idle: match cleared (idle, match=none)", IDLE_AFTER_SERIES_TIMEOUT, chk_idle_after_postgame))
        return steps

    def forfeit_steps(self, f, mark, since, chk_postgame_any, chk_idle_after_postgame) -> list[Step]:
        """Pauses (technical limit + auto-unpause, tactical timeout) and the team-left forfeit."""
        a = self.a

        def line_after(lines, key, pred):
            return next((t for (sq, t) in lines if sq > since(key) and pred(t)), None)

        def send_marked(key, *cmds):
            def _act():
                mark[key] = f.seq
                for c in cmds:
                    self.srv.send(c)
            return _act

        def chk_pause(key, want_start, want_end=None, refuse=None):
            def _c(_f):
                if refuse and line_after(f.pauses, key, lambda t: t.startswith(refuse)):
                    return True
                if refuse:
                    if line_after(f.pauses, key, lambda t: t.startswith("technical by")):
                        return "a technical pause over the limit was accepted"
                    return False
                if not line_after(f.pauses, key, lambda t: t.startswith(want_start)):
                    return False
                return want_end is None or line_after(f.pauses, key, lambda t: t.startswith(want_end)) is not None
            return _c

        def pause_detail(key):
            return lambda f_: " | ".join(t for (sq, t) in f_.pauses if sq > since(key))[:200]

        def forfeit_line(key, prefix):
            return line_after(f.forfeits, key, lambda t: prefix in t)

        def chk_ff(key, prefix, bad=None):
            def _c(_f):
                if bad and forfeit_line(key, bad):
                    return f"unexpected: {forfeit_line(key, bad)}"
                return forfeit_line(key, prefix) is not None
            return _c

        def ff_detail(key):
            return lambda f_: " | ".join(t for (sq, t) in f_.forfeits if sq > since(key))[:200]

        def ct_bots():
            return sorted(n for n, hist in f.bot_sides.items() if hist and hist[-1][1] == "CT" and n not in f.bots_gone)

        kicked_at: dict = {}

        def act_kick(key):
            def _act():
                mark[key] = f.seq
                self.quota_mode_set = True
                self.srv.send("bot_quota_mode")  # echo, restored in cleanup
                self.srv.send("mp_autoteambalance 0")
                self.srv.send("bot_quota_mode normal")
                self.srv.send("bot_join_team T")
                names = ct_bots()
                log(f"CT bots: {names}")
                for n in names:
                    self.srv.send(f'bot_kick "{n}"')
                kicked_at[key] = time.time()
            return _act

        def ct_bots_on_server() -> int:
            """CT bots in Ready Up's newest state line (ct=<humans>+<bots>), -1 if unknown."""
            last = f.last_state()
            try:
                return int(((last or {}).get("ct") or "").split("+")[1])
            except (IndexError, ValueError):
                return -1

        def chk_kicked(key, prefix):
            """chk_ff, but kicks again every 6 s while a CT bot is still on the server: a bot_kick
            sent while a bot is just joining (or at a round restart) is sometimes ignored."""
            base = chk_ff(key, prefix)

            def _c(_f):
                res = base(_f)
                if res is not False:
                    return res
                if time.time() - kicked_at.get(key, 0) > 6 and ct_bots_on_server() != 0:
                    names = ct_bots()
                    log(f"CT bots still on the server ({ct_bots_on_server()}): kicking {names} again")
                    for n in names:
                        self.srv.send(f'bot_kick "{n}"')
                    kicked_at[key] = time.time()
                return False
            return _c
        return [
            Step("tech pause (ru match tech team1) -> auto-unpause after 8s", 150,
                 chk_pause("tech1", "technical by team1", "ended (technical pause time is up)"),
                 send_marked("tech1", "ru match tech team1"), pause_detail("tech1")),
            Step("tech pause limit (2nd for team1 refused)", 20,
                 chk_pause("tech2", "", refuse="technical refused for team1"),
                 send_marked("tech2", "ru match tech team1"), pause_detail("tech2")),
            Step("tactical timeout (ru match tac team2) -> ends at freeze end", 150,
                 chk_pause("tac", "tactical timeout by team2", "ended (tactical timeout over)"),
                 send_marked("tac", "ru match tac team2"), pause_detail("tac")),
            Step("forfeit countdown (CT bots kicked)", 30, chk_kicked("ff1", "team1 has nobody connected"),
                 act_kick("ff1"), ff_detail("ff1")),
            Step("forfeit cancelled (a CT bot back)", 30, chk_ff("ff2", "team1 is back", bad="forfeits map"),
                 send_marked("ff2", "bot_add_ct"), ff_detail("ff2")),
            Step("the CT bot is seen", 20, lambda _f: bool(ct_bots()), None, lambda _f: ", ".join(ct_bots())),
            Step("forfeit countdown again (kicked)", 30, chk_kicked("ff3", "team1 has nobody connected"),
                 act_kick("ff3"), ff_detail("ff3")),
            Step("forfeit: team1 forfeits after 20s", 60, chk_ff("ff3", "team1 (team_absent) forfeits map"), None,
                 ff_detail("ff3")),
            Step("postgame (forfeit map result)", 30, chk_postgame_any),
            Step("idle: match cleared (idle, match=none)", IDLE_AFTER_SERIES_TIMEOUT, chk_idle_after_postgame),
        ]

    def valve_steps(self, f, a, mark, since, bots_ok, act_selftest, chk_selftest, act_reset, chk_reset,
                    chk_warmup, act_bots) -> list[Step]:
        """--ruleset valve (docs/ESPORTS-MODE.md): knife refused, go-live execs esports_live.cfg, every
        Valve exception cvar + the overrides after go-live, `ru match rules`, skins inert, default models."""
        t0: dict = {}

        def serve(doc):
            if self.http:
                self.http.shutdown()
            self.http, port = serve_match_json(a.http_bind, doc)
            url = f"http://{a.http_host}:{port}/match.json"
            log(f"serving match {doc['config']['matchid']} on {url} (ruleset valve)")
            self.srv.send(f"ru match load {url}")

        def map_name():
            cur = f.last_state() or {}
            name = a.map or cur.get("map") or "de_dust2"
            return "de_dust2" if name == "?" else name

        def act_knife():
            mark["knife_load"] = f.seq
            f.load_error = None
            serve(valve_doc(map_name(), a.max_rounds, knife=True))

        def chk_knife(_f):
            if f.loaded and f.loaded[0] > since("knife_load"):
                return "a knife match loaded under ruleset valve"
            if not f.load_error:
                return False
            if "knife" in f.load_error and "allow_knife" in f.load_error:
                return True
            return f"unexpected load error: {f.load_error}"

        def act_load():
            mark["load"] = f.seq
            f.load_error = None
            serve(valve_doc(map_name(), a.max_rounds))

        def chk_load(_f):
            if f.load_error:
                return f"match load error: {f.load_error}"
            if not f.loaded or f.loaded[0] <= since("load"):
                return False
            if f.loaded[2] != MATCH_SLUG:
                return f"loaded unexpected slug {f.loaded[2]}"
            es = [e for e in f.esports if e[0] > since("load")]
            if not es:
                return False
            _, rs, src, cfg, differs = es[-1]
            self.match_loaded = True
            f.our_matchid = f.loaded[1]
            if (rs, src, cfg, differs) != ("valve", "match config", "ReadyUp/esports_live.cfg", VALVE_DIFFERS):
                return f"esports line: ruleset={rs} ({src}) cfg={cfg} differs={differs}; want valve / {VALVE_DIFFERS}"
            return True

        def chk_live(_f):
            if f.knife_start > since("load"):
                return "a knife round started under ruleset valve"
            if f.gotv_refused > since("load"):
                return "go-live refused: GOTV is off on the test server (valve needs tv_enable 1 before the map loads)"
            return f.live_seq > since("load")

        def act_bots_again():
            # esports_live.cfg: bot_quota 0 + bot_kick (Premier's base cfg fills bots).
            mark["bots2"] = f.seq
            time.sleep(2.0)
            self.srv.send("bot_join_after_player 0")  # bots join without a human on the server (empty CI server)
            self.srv.send(f"bot_quota {a.bots_per_side * 2}")
            time.sleep(8.0)
            self.srv.send("ru match state")  # a fresh state: line even when the bot count did not change

        def chk_bots_again(_f):
            return any(bots_ok(fl) for _, fl, _ in f.states_since(since("bots2")))

        want = dict(valve_exception_cvars())
        want.update(VALVE_EXTRA_CVARS)
        want["mp_maxrounds"] = str(a.max_rounds)  # match cvars run after the cfg
        names = sorted(want)

        def act_cvars():
            mark["cvars"] = f.seq
            # Short batches: the console input line is cut at ~255 bytes.
            for i in range(0, len(names), 6):
                self.srv.send("; ".join(names[i:i + 6]))

        def cvar_report():
            got = {k: v for k, (sq, v) in f.cvars.items() if sq > since("cvars") and k in want}
            unknown = sorted(k for k, sq in f.unknown_cmds.items() if sq > since("cvars") and k in want)
            missing = [k for k in names if k not in got and k not in unknown]
            bad = [f"{k}={got[k]} (want {want[k]})" for k in names if k in got and not cvar_equal(got[k], want[k])]
            # Rulebook cvars this CS2 build does not have are expected (VALVE_ABSENT); any other
            # "Unknown command" means CS2 or the rulebook renamed one.
            bad += [f"{k} unknown to this CS2 build" for k in unknown if k not in VALVE_ABSENT]
            return got, missing, bad, unknown

        def chk_cvars(_f):
            got, missing, bad, _ = cvar_report()
            if missing:
                return False
            return True if not bad else "cvars differ: " + ", ".join(bad)

        def cvars_detail(_f):
            got, missing, bad, unknown = cvar_report()
            s = f"{len(got)}/{len(names)} cvars as expected" if not bad else f"{len(got)}/{len(names)} cvars replied"
            if unknown:
                s += "; not on this CS2 build: " + ", ".join(unknown)
            if missing:
                s += "; no reply: " + ", ".join(missing[:8])
            return s + (f"; {len(bad)} wrong" if bad else "")

        def act_rules():
            mark["rules"] = f.seq
            self.srv.send("ru match rules")

        def chk_rules(_f):
            lines = [t for (sq, t) in f.rules_diff if sq > since("rules")]
            if not lines:
                return False
            t = lines[-1]
            for frag in ("freezetime 20->5 (override)", "spectators_max 10->8 (override)",
                         "overtime.startmoney 10000->12500 (override)", "default_models false->true (override)"):
                if frag not in t:
                    return f"`ru match rules` misses {frag!r}: {t}"
            return True

        def act_skins():
            mark["skins"] = f.seq
            t0["skins"] = time.time()
            self.srv.send("skins_status")

        def chk_skins(_f):
            st = [x for (sq, x) in f.skins_status if sq > since("skins")]
            if st:
                return True if "valve" in st[-1] else f"skins_status does not say inert: {st[-1]!r}"
            if time.time() - t0.get("skins", time.time()) > 8:
                return "SKIP: no skins_status reply (skins plugin not loaded on this server)"
            return False

        def chk_models(_f):
            if f.default_model_fail:
                return f.default_model_fail
            dm = [d for d in f.default_models if d[0] > since("load")]
            sides = {d[2] for d in dm}
            if {"CT", "T"} <= sides:
                bad = [d for d in dm if not d[3].startswith("agents/models/")]
                return True if not bad else f"unexpected model {bad[0][3]}"
            return False

        def act_selftest2():
            f.selftest = None
            f.selftest_after = f.seq
            self.srv.send("ru selftest")

        return [
            Step("selftest", 45, chk_selftest, act_selftest,
                 lambda f_: f_.selftest[2].split("] ", 1)[-1] if f_.selftest else ""),
            Step("reset (idle, no match)", 30, chk_reset, act_reset),
            Step("valve: map_sides knife refused (no allow_knife)", 30, chk_knife, act_knife,
                 lambda f_: (f_.load_error or "")[:160]),
            Step("valve: match load (ruleset + overrides)", 30, chk_load, act_load,
                 lambda f_: " ".join(str(x) for x in (f_.esports[-1][1:] if f_.esports else ()))),
            # Empty roster: everyone is ready at once, so the match may go live before the bots join.
            Step("warmup (match_warmup)", 20, chk_warmup, act_bots),
            Step("valve: straight to match_live (no knife round)", 90, chk_live),
            Step("valve: bots back (esports_live.cfg kicks them)", 60, chk_bots_again, act_bots_again),
            Step(f"valve: {len(names)} cvars after go-live (Valve exceptions, Premier, overrides)", 30, chk_cvars,
                 act_cvars, cvars_detail),
            Step("valve: `ru match rules` differs from valve", 15, chk_rules, act_rules,
                 lambda f_: (f_.rules_diff[-1][1] if f_.rules_diff else "")[:200]),
            Step("valve: skins plugin inert", 15, chk_skins, act_skins,
                 lambda f_: (f_.skins_status[-1][1] if f_.skins_status else "").strip()),
            Step("valve: default_models (CT + T reset to default agents)", 120, chk_models, None,
                 lambda f_: ", ".join(sorted({f"{d[2]}={d[3]}" for d in f_.default_models}))[:200]),
            Step("selftest (valve match loaded)", 45, chk_selftest, act_selftest2,
                 lambda f_: f_.selftest[2].split("] ", 1)[-1] if f_.selftest else ""),
        ]

    def scrim_steps(self, f, a, mark, since, state_where, bots_ok, act_selftest, chk_selftest,
                    act_reset, chk_reset, knife_steps) -> list[Step]:
        """Bots-only scrim: dev_bots_scrim on, bots on both sides, no humans, no match config."""

        def act_flag():
            self.flag_set = True
            self.srv.send("ru_dev_bots_scrim 1")

        def chk_flag(_f):
            if not f.dev_scrim:
                return False
            return True if f.dev_scrim[1] == "1" else "ru_dev_bots_scrim 1 did not turn the flag on"

        def act_scrim():
            mark["load"] = f.seq
            f.armed = True
            self.srv.send("ru mode scrim")  # `ru mode idle` (reset) turned auto scrim warmup off
            time.sleep(0.3)
            self.srv.send("bot_join_after_player 0")  # bots join without a human on the server (empty CI server)
            self.srv.send(f"bot_quota {a.bots_per_side * 2}")

        def chk_scrim_warmup(_f):
            fl = state_where(lambda fl: fl.get("mode") == "scrim_warmup", "load")
            if fl is None:
                return False
            if fl.get("dev_bots_scrim") != "1":
                return "state: line does not show dev_bots_scrim=1"
            return True

        def chk_bots(_f):
            return any(bots_ok(fl) for _, fl, _ in f.states_since(since("load")))

        def chk_countdown(_f):
            return state_where(lambda fl: fl.get("mode") == "scrim_warmup" and "countdown" in fl, "load") is not None

        def chk_created(_f):
            fl = state_where(lambda fl: (fl.get("match") or "").startswith("scrim:"), "load")
            if fl is None:
                return False
            return True

        return [
            Step("selftest", 45, chk_selftest, act_selftest,
                 lambda f_: f_.selftest[2].split("] ", 1)[-1] if f_.selftest else ""),
            Step("reset (idle, no match)", 30, chk_reset, act_reset),
            Step("dev flag (ru_dev_bots_scrim 1)", 15, chk_flag, act_flag),
            Step("scrim warmup (bots only, dev_bots_scrim=1)", 60, chk_scrim_warmup, act_scrim),
            Step(f"bots (>= {a.bots_per_side} per side, no humans)", 60, chk_bots, None,
                 lambda f_: next((f"ct={fl.get('ct')} t={fl.get('t')}" for _, fl, _ in reversed(f_.states)), "")),
            Step("countdown (all ready -> 5s)", 30, chk_countdown),
            Step("scrim created (match=scrim:...)", 30, chk_created, None,
                 lambda f_: next((f"match={fl.get('match')}" for _, fl, _ in reversed(f_.states)
                                  if (fl.get("match") or "").startswith("scrim:")), "")),
        ] + knife_steps

    # ---- main
    def preflight(self) -> Optional[str]:
        deadline = time.time() + self.a.wait_session
        while not self.srv.session_alive():
            if time.time() > deadline:
                if self.a.boot:
                    log(f"tmux session {self.a.session} missing; booting the server (--boot)")
                    if self.srv.boot(240):
                        break
                    return "server did not boot"
                return f"tmux session {self.a.session} missing (server down or mid-restart)"
            log(f"tmux session {self.a.session} missing; waiting (another deploy restarting?)")
            time.sleep(10)
        humans = self.srv.connected_humans()
        if humans and not self.a.force:
            return "humans connected: " + ", ".join(humans)
        return None

    def cleanup(self) -> None:
        try:
            if self.timescale_set:
                self.srv.send("host_timescale 1")
                self.srv.send("sv_cheats 0")
            if self.touched:
                self.srv.send("ru mode idle")    # clears the match context + persisted match
                if self.a.simulation:
                    self.pump(2.0)  # Ready Up removes its simulation bots first (bot_quota 0 + bot_kick)
                if self.flag_set:
                    # Before `ru mode scrim`: with the flag on, the restored bots would start a scrim.
                    self.srv.send("ru_dev_bots_scrim cfg")
                if self.a.ruleset == "valve":
                    # Engine pauses the valve ruleset turns on (auto 5v5, halftime) must not outlive
                    # the test; ReadyUp/live.cfg resets them for the next default match too.
                    self.srv.send("mp_unpause_match")
                    self.srv.send("sv_matchpause_auto_5v5 0")
                    self.srv.send("mp_halftime_pausematch 0")
                if self.a.forfeit:
                    self.srv.send("bot_join_team any")
                    self.srv.send("mp_autoteambalance 1")
                    if self.quota_mode_set:
                        self.srv.send(f"bot_quota_mode {self.f.bot_quota_mode or 'competitive'}")
                self.srv.send("ru mode scrim")   # re-enable auto scrim warmup (ru mode idle turns it off)
                quota = self.f.bot_quota if self.f.bot_quota is not None else self.a.restore_bot_quota
                self.srv.send("bot_join_after_player 1")  # CS2 default, set to 0 while testing
                self.srv.send(f"bot_quota {quota}")
                self.pump(2.0)
        except Exception as e:  # noqa: BLE001
            log(f"cleanup error: {e}")
        if self.http:
            self.http.shutdown()
        if self.follower:
            self.follower.close()

    def report(self, verdict: str) -> None:
        rows = [("STEP", "RESULT", "TIME", "DETAIL")]
        for st in self.results:
            rows.append((st.name, st.status, f"{st.elapsed:5.1f}s", st.info))
        w0 = max(len(r[0]) for r in rows)
        print()
        print("=" * 100)
        for r in rows:
            print(f"{r[0]:<{w0}}  {r[1]:<6} {r[2]:>7}  {r[3]}")
        print("=" * 100)
        print("mode sequence: " + " -> ".join(self.f.modes))
        print(f"LIVETEST {verdict}")
        if os.environ.get("GITHUB_ACTIONS") == "true":
            what = "scrim" if self.a.scrim else "forfeit" if self.a.forfeit else "match"
            for st in self.results:
                if st.status == "FAIL":
                    print(f"::error title=livetest {what}::{gh_escape(st.name + ': ' + st.info)}")
        summ = os.environ.get("GITHUB_STEP_SUMMARY")
        if summ:
            with open(summ, "a") as fh:
                fh.write(f"## Ready Up live test: {verdict}\n\n| step | result | time | detail |\n|---|---|---|---|\n")
                for st in self.results:
                    fh.write(f"| {st.name} | {st.status} | {st.elapsed:.1f}s | {st.info.replace('|', '/')} |\n")
                fh.write("\nMode sequence: `" + " -> ".join(self.f.modes) + "`\n")
        if self.a.out:
            os.makedirs(self.a.out, exist_ok=True)
            with open(os.path.join(self.a.out, "console-capture.log"), "w") as fh:
                fh.write("\n".join(self.captured) + "\n")
            with open(os.path.join(self.a.out, "result.json"), "w") as fh:
                json.dump({"verdict": verdict, "modes": self.f.modes,
                           "steps": [{"name": s.name, "status": s.status, "elapsed": round(s.elapsed, 1),
                                      "detail": s.info} for s in self.results]}, fh, indent=2)

    def main(self) -> int:
        why = self.preflight()
        if why:
            log(f"BUSY/UNAVAILABLE: {why}")
            print(f"LIVETEST SKIPPED ({why})")
            return 2
        self.follower = self.srv.follow(self.srv.log_size())
        failed = False
        deadline = time.time() + self.a.total_timeout
        self.events = StepEvents.from_env()
        try:
            steps = self.steps()
            for i, st in enumerate(steps, 1):
                self.step_index[id(st)] = i
                self.emit(st, "queued")
            for st in steps:
                if failed:
                    st.status = "SKIP"
                    self.emit(st, "skip")
                    self.results.append(st)
                    continue
                if time.time() > deadline:
                    st.status, st.info = "FAIL", "total timeout"
                    self.emit(st, "fail", st.info)
                    self.results.append(st)
                    failed = True
                    continue
                if not self.run_step(st):
                    failed = True
        except KeyboardInterrupt:
            failed = True
            log("interrupted")
        finally:
            self.cleanup()
            if self.events:
                self.events.close()
        if failed and self.f.restarted:
            self.report("SKIPPED (server restarted mid-test)")
            return 2
        verdict = "FAIL" if failed else "PASS"
        self.report(verdict)
        return 1 if failed else 0


def parse_args(argv=None):
    env = os.environ.get
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ssh", default=env("RU_SSH", "cs2servermanager@localhost"),
                   help="ssh destination owning the server ('' = run locally)")
    p.add_argument("--target", default=env("RU_TARGET", "/home/cs2servermanager/readyup-test"))
    p.add_argument("--session", default=env("RU_SESSION", "ru-test"))
    p.add_argument("--http-bind", default=env("LIVETEST_HTTP_BIND", "127.0.0.1"))
    p.add_argument("--http-host", default=env("LIVETEST_HTTP_HOST", "127.0.0.1"),
                   help="host the CS2 server uses to fetch match.json from this script")
    p.add_argument("--map", default=env("LIVETEST_MAP", ""), help="default: the current map")
    p.add_argument("--scrim", action="store_true", default=env("LIVETEST_MODE", "match") == "scrim",
                   help="bots-only scrim (dev_bots_scrim): warmup -> countdown -> knife -> pick -> live")
    p.add_argument("--forfeit", action="store_true", default=env("LIVETEST_MODE", "match") == "forfeit",
                   help="match mode + pauses (technical limit / auto-unpause, tactical) + team-left forfeit")
    p.add_argument("--ruleset", choices=["default", "valve"], default=env("LIVETEST_RULESET", "default"),
                   help="valve: esports ruleset (docs/ESPORTS-MODE.md): knife refused, every Valve exception cvar "
                        "and the overrides checked after go-live, `ru match rules`, skins inert, default_models")
    p.add_argument("--max-rounds", type=int, default=int(env("LIVETEST_MAX_ROUNDS", "4")))
    p.add_argument("--bots-per-side", type=int, default=int(env("LIVETEST_BOTS_PER_SIDE", "2")))
    p.add_argument("--side", choices=["auto", "stay", "switch"], default=env("LIVETEST_SIDE", "auto"),
                   help="knife pick: auto = bots-only timeout (stay); stay/switch = `ru match side ...` from console")
    p.add_argument("--timescale", type=float, default=float(env("LIVETEST_TIMESCALE", "1")),
                   help="host_timescale during match_live only (needs sv_cheats 1; reverted after); "
                        "with --simulation the match config's simulation_timescale")
    p.add_argument("--simulation", action="store_true", default=env("LIVETEST_SIMULATION", "0") == "1",
                   help="match config with simulation: true and a roster; Ready Up adds and readies the bots")
    p.add_argument("--round-timeout", type=float, default=float(env("LIVETEST_ROUND_TIMEOUT", "200")))
    p.add_argument("--total-timeout", type=float, default=float(env("LIVETEST_TOTAL_TIMEOUT", "1500")))
    p.add_argument("--wait-session", type=float, default=float(env("LIVETEST_WAIT_SESSION", "300")),
                   help="how long to wait for a missing tmux session (deploy mid-restart)")
    p.add_argument("--boot", action="store_true", help="start the server if the session is still missing")
    p.add_argument("--force", action="store_true", help="run even if humans are connected")
    p.add_argument("--restore-bot-quota", type=int, default=2)
    p.add_argument("--out", default=env("LIVETEST_OUT", ""), help="write console-capture.log + result.json here")
    a = p.parse_args(argv)
    if a.max_rounds < 2 or a.max_rounds % 2:
        p.error("--max-rounds must be even and >= 2")
    if a.ruleset == "valve" and (a.scrim or a.forfeit):
        p.error("--ruleset valve runs its own flow; not with --scrim or --forfeit")
    if a.forfeit and a.scrim:
        p.error("--forfeit and --scrim cannot be combined")
    if a.simulation and (a.scrim or a.forfeit or a.ruleset == "valve"):
        p.error("--simulation runs the default match flow only (no --scrim, --forfeit or --ruleset valve)")
    return a


if __name__ == "__main__":
    sys.exit(Runner(parse_args()).main())
