#!/usr/bin/env python3
"""Fleet step-3 live test: a mock platform assigns a bot match to the readyup-test server.

  scripts/livetest/fleet_livetest.py [--play-out] [--no-offline] [--save-examples DIR]

Runs on the cs2 box as sivert (like livetest.py, whose console helpers it uses):

  1. starts scripts/livetest/fleet_mock_platform.py in Docker (python:3.12-slim, host network,
     127.0.0.1:18095-18097); it validates every frame against plugins/fleet/protocol/v1,
  2. points fleet.so at it (csgo/cfg/ReadyUp/fleet.cfg: url, insecure_dev, a one-time code,
     offline_pause_minutes=1) and reloads it: enroll + hello,
  3. match.assign (epoch 2, empty rosters, knife, 4 rounds): cmd.result ok, state.snapshot, bots
     play (empty roster = everyone ready), phase events warmup -> knife -> side_pick -> live,
     round_start / round_end (RoundSummary) / backup (inline round backup) / patches,
  4. fencing: a second match.assign (busy), a cmd with epoch 1 (stale_epoch), another match_id
     (not_assigned), match.update with a wrong base (conflict + snapshot) and a right one (ok, rev 2),
  5. cmd pause / unpause (acks + event.pause), exec (root, audit id echoed; refused without root),
  6. D12: the mock refuses connections for 75 s -> auto-pause at 60 s offline, the spooled
     event.pause {type: offline} arrives after the reconnect, cmd unpause,
  7. round backups arrive inline (sha256 checked) and are restored with cmd restore_round (local
     file, then the inline copy): rounds_voided + match_restored; a wrong sha256 is refused,
  8. cmd end_match (or --play-out: map_result with MapStats + series_end), match.unassign,
  9. failover resume (FLEET.md §11.3): match.assign epoch+1 with `resume` {map 2 of 3, the round
     backup captured in step 7 (or --resume-backup FILE from an earlier run), series 1-0, map 1's
     result}: bad sha256 -> checksum, bad map -> invalid_config; then phase restoring, the go-live,
     rounds_voided {resume} + match_restored {resume} + CS2 `Loaded server checkpoint`, a
     `restored` snapshot (map 2, series 1-0, paused), unpause -> the next round is the backup's,
  10. admins cache version: admins.set rev N -> state.snapshot.admins_rev = N and, after the
     offline reconnect, hello.admins_rev = N,
  11. checks: no schema errors, cmd.result per command (ref = its id), live_rev +1 per message,
     and the snapshots equal the state rebuilt from the assign snapshot + every patch.

  --resume-only runs 1-2 and 9 only (with --resume-backup); --save-backup FILE keeps step 7's
  backup for such a later run.

Afterwards the server is standalone again: fleet.cfg and fleet.so's data dir are moved to
<target>/.fleet-livetest/<time>/, fleet.so reloaded, `ru mode idle`, `ru mode scrim`, bot_quota back.
Exit 0 PASS, 1 FAIL, 2 no verdict (server busy / down, or the mock platform could not start).

Self-contained: the mock is the only "platform" (loopback only, free ports picked per run unless
--http-port / --ws-port / --ctl-port are given), the enrollment code is a made-up constant and the
token a random one the mock mints per run; nothing leaves the box. The mock runs in Docker when
this user can use it (--mock docker), otherwise as a local process with websockets + jsonschema
pip-installed into a cache dir (--mock local; $LIVETEST_PYDEPS, default
~/.cache/readyup-livetest/pydeps-<python>): the CI runner is not in the docker group.

CI (.github/workflows/cs2-dynamic.yml, `Live: fleet`): `--ssh '' --target <tmp> --game-dir
$CS2_CI_DIR --boot`, like scripts/livetest/run.sh. With COMPAT_STEP_EVENTS=1 the
checks are reported to the compatibility page grouped by phase (enroll, assign, fencing, live,
pause, backups, offline, end, resume, frame stream), nested under COMPAT_STEP_PARENT, and
GITHUB_STEP_SUMMARY gets the table of checks.
"""
import argparse
import json
import os
import shlex
import socket
import subprocess
import sys
import time
import urllib.request
from typing import Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from livetest import STATE_RE, KV_RE, CRASH_RE, Server, StepEvents, gh_escape, log  # noqa: E402

MOCK_NAME = "ru-fleet-mock"
# A throwaway enrollment code: the mock accepts any code and mints a random token per run.
CODE = "RUE-7F3K-9QX2-LM4D-P8TW"
MOCK_DEPS = ["websockets>=13", "jsonschema>=4.18"]


def free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def merge_patch(target, patch):
    if not isinstance(patch, dict):
        return patch
    if not isinstance(target, dict):
        target = {}
    out = dict(target)
    for k, v in patch.items():
        if v is None:
            out.pop(k, None)
        else:
            out[k] = merge_patch(out.get(k), v)
    return out


class Mock:
    """The mock platform (fleet_mock_platform.py): in Docker, or as a local process."""

    def __init__(self, a):
        self.a = a
        for k in ("http_port", "ws_port", "ctl_port"):
            if not getattr(a, k):
                setattr(a, k, free_port())
        self.base = f"http://127.0.0.1:{a.ctl_port}"
        self.how = a.mock
        self.proc: Optional[subprocess.Popen] = None
        self.log_path = ""
        self.name = f"{MOCK_NAME}-{a.ctl_port}"

    @staticmethod
    def docker_usable() -> bool:
        try:
            return subprocess.run(["docker", "info"], capture_output=True, timeout=20).returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            return False

    def args(self, schemas: str) -> list:
        return ["--http", str(self.a.http_port), "--ws", str(self.a.ws_port), "--ctl", str(self.a.ctl_port),
                "--schemas", schemas]

    def local_deps(self) -> str:
        """A dir with websockets + jsonschema (pip install --target), reused across runs."""
        d = os.environ.get("LIVETEST_PYDEPS") or os.path.expanduser(
            f"~/.cache/readyup-livetest/pydeps-{sys.version_info[0]}.{sys.version_info[1]}")
        probe = [sys.executable, "-c", "import websockets.asyncio.server, jsonschema, referencing"]
        env = dict(os.environ, PYTHONPATH=d)
        if subprocess.run(probe, env=env, capture_output=True).returncode == 0:
            return d
        log(f"installing the mock platform's Python deps into {d} ...")
        r = subprocess.run([sys.executable, "-m", "pip", "install", "-q", "--disable-pip-version-check",
                            "--no-warn-script-location", "--upgrade", "--target", d] + MOCK_DEPS,
                           capture_output=True, text=True, timeout=600)
        if r.returncode != 0 or subprocess.run(probe, env=env, capture_output=True).returncode != 0:
            raise RuntimeError("pip install of the mock's deps failed: " + (r.stdout + r.stderr)[-600:])
        return d

    def start(self) -> None:
        if self.how == "auto":
            self.how = "docker" if self.docker_usable() else "local"
        log(f"mock platform ({self.how}): enroll :{self.a.http_port} ws :{self.a.ws_port} ctl :{self.a.ctl_port}")
        if self.how == "docker":
            subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)
            cmd = ("pip install -q --disable-pip-version-check --root-user-action=ignore websockets jsonschema && "
                   "exec python -u /mock.py " + " ".join(self.args("/proto/v1")))
            subprocess.run(["docker", "run", "-d", "--rm", "--name", self.name, "--network", "host",
                            "-v", f"{REPO}/plugins/fleet/protocol:/proto:ro",
                            "-v", f"{HERE}/fleet_mock_platform.py:/mock.py:ro",
                            "python:3.12-slim", "sh", "-c", cmd], check=True, capture_output=True)
        else:
            deps = self.local_deps()
            self.log_path = os.path.join(self.a.out or os.environ.get("RUNNER_TEMP") or "/tmp",
                                         f"fleet-mock-{self.a.ctl_port}.log")
            os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
            env = dict(os.environ, PYTHONPATH=deps, PYTHONUNBUFFERED="1")
            with open(self.log_path, "w") as fh:
                self.proc = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "fleet_mock_platform.py")] +
                                             self.args(os.path.join(REPO, "plugins", "fleet", "protocol", "v1")),
                                             stdout=fh, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        deadline = time.time() + 180
        while time.time() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                break
            try:
                self.get("/status")
                return
            except Exception:  # noqa: BLE001
                time.sleep(1)
        raise RuntimeError("mock platform did not start: " + self.logs()[-1500:])

    def logs(self) -> str:
        if self.how == "docker":
            r = subprocess.run(["docker", "logs", "--tail", "60", self.name], capture_output=True, text=True)
            return r.stdout + r.stderr
        try:
            with open(self.log_path, errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    def stop(self) -> None:
        if self.how == "docker":
            subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)
        elif self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def get(self, path: str):
        with urllib.request.urlopen(self.base + path, timeout=10) as r:
            return json.loads(r.read())

    def post(self, path: str, body: dict):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())

    def send(self, type_: str, payload: dict, epoch: Optional[int] = None) -> str:
        body = {"type": type_, "payload": payload}
        if epoch:
            body["epoch"] = epoch
        return self.post("/send", body)["id"]


class Test:
    def __init__(self, a):
        self.a = a
        self.srv = Server(a.ssh, a.target, a.session)
        self.mock = Mock(a)
        self.frames = []
        self.follower = None
        self.lines = []
        self.results = []
        self.failures = []
        game = (a.game_dir or a.target).rstrip("/")
        self.cfg_path = f"{game}/game/csgo/cfg/ReadyUp/fleet.cfg"
        self.data_dir = f"{game}/game/csgo/readyup/plugins/fleet"
        # admins.set is cached by match.so (fleet-admins.json): kept aside and put back afterwards.
        self.admins_cache = f"{game}/game/csgo/readyup/plugins/match/fleet-admins.json"
        # Compatibility page (COMPAT_STEP_EVENTS=1): one step per phase, not per check (a run has
        # at most 100 steps).
        self.events: Optional[StepEvents] = None
        self.phase_name = ""
        self.phase_no = 0
        self.phase_fail: list = []
        self.phase_n = 0
        self.admins_rev = None
        self.touched = False
        self.match_id = f"lt-{int(time.time())}"
        self.epoch = 2
        self.scrim_flag = False

    # ---- plumbing -------------------------------------------------------------------------
    def pump(self, wait: float = 0.3) -> None:
        if self.follower:
            for ln in self.follower.lines(wait):
                self.lines.append(ln)
                if CRASH_RE.search(ln):
                    raise RuntimeError("crash in the console log: " + ln)
        new = self.mock.get(f"/frames?since={len(self.frames)}")
        self.frames.extend(new)

    def frames_of(self, type_: str, since: int = 0, pred=None):
        return [f for f in self.frames[since:] if f["frame"].get("type") == type_ and (pred is None or pred(f["frame"]))]

    def wait_frame(self, type_: str, since: int, pred=None, timeout: float = 30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(0.3)
            got = self.frames_of(type_, since, pred)
            if got:
                return got[0]["frame"]
        return None

    def wait_line(self, needle: str, since: int, timeout: float) -> Optional[str]:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in self.lines[since:]:
                if needle in ln:
                    return ln
            self.pump(0.5)
        return None

    def step(self, name: str, ok: bool, detail: str = "") -> bool:
        ok = bool(ok)
        detail = detail or ""
        self.results.append((name, "PASS" if ok else "FAIL", detail))
        log(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
        self.phase_n += 1
        if not ok:
            self.failures.append(name)
            self.phase_fail.append(name)
        return ok

    def phase(self, name: str) -> None:
        """Start a group of checks (one step on the compatibility page); ends the previous one."""
        self.end_phase()
        self.phase_name, self.phase_fail, self.phase_n = name, [], 0
        self.phase_no += 1
        if self.events:
            self.events.send(self.phase_no, name, "running")

    def end_phase(self) -> None:
        if not self.phase_name:
            return
        if self.events:
            if self.phase_fail:
                self.events.send(self.phase_no, self.phase_name, "fail", "failed: " + "; ".join(self.phase_fail))
            else:
                self.events.send(self.phase_no, self.phase_name, "pass", f"{self.phase_n} check(s)")
        self.phase_name = ""

    def cmd(self, name: str, args: dict, match: bool = True, epoch: Optional[int] = None, root: bool = False,
            audit: str = "", match_id: Optional[str] = None, timeout: float = 20.0):
        payload = {"name": name, "args": args, "issued_by": {"user_id": "u_lt", "name": "livetest", "root": root},
                   "expires_at": int(time.time() * 1000) + 60000}
        e = None
        if match:
            payload["match_id"] = match_id or self.match_id
            e = epoch if epoch is not None else self.epoch
            payload["epoch"] = e
        if audit:
            payload["audit_id"] = audit
        since = len(self.frames)
        mid = self.mock.send("cmd", payload, e)
        return self.wait_frame("cmd.result", since, lambda f: f.get("ref") == mid, timeout)

    def result_ok(self, res) -> bool:
        return bool(res) and res["payload"]["status"] == "ok"

    def err_code(self, res) -> str:
        return (res or {}).get("payload", {}).get("error", {}).get("code", "") if res else "(no cmd.result)"

    def state_line(self, since: int, pred, timeout: float) -> Optional[dict]:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in self.lines[since:]:
                m = STATE_RE.search(ln)
                if m:
                    kv = dict(KV_RE.findall(m.group(1)))
                    if pred(kv):
                        return kv
            self.pump(0.5)
        return None

    # ---- server config ----------------------------------------------------------------------
    def configure_fleet(self) -> None:
        cfg = (f"url=http://127.0.0.1:{self.a.http_port}\ninsecure_dev=1\nenroll_code={CODE}\n"
               "offline_pause_minutes=1\n")
        q = shlex.quote
        self.srv.run(f"test ! -e {q(self.cfg_path)} || cp -p {q(self.cfg_path)} "
                     f"{q(self.cfg_path)}.pre-livetest; printf %s {q(cfg)} > {q(self.cfg_path)}; "
                     f"test ! -e {q(self.admins_cache)} || cp -p {q(self.admins_cache)} {q(self.admins_cache)}.pre-livetest",
                     check=True)
        self.touched = True

    def unconfigure_fleet(self) -> None:
        trash = f"{self.a.target}/.fleet-livetest/{time.strftime('%Y%m%d-%H%M%S')}"
        q = shlex.quote
        self.srv.run(f"mkdir -p {q(trash)}; test -e {q(self.cfg_path)} && mv {q(self.cfg_path)} {q(trash)}/; "
                     f"test -e {q(self.cfg_path)}.pre-livetest && mv {q(self.cfg_path)}.pre-livetest {q(self.cfg_path)}; "
                     f"test -d {q(self.data_dir)} && mv {q(self.data_dir)} {q(trash)}/fleet-data; "
                     f"test -e {q(self.admins_cache)} && mv {q(self.admins_cache)} {q(trash)}/; "
                     f"test -e {q(self.admins_cache)}.pre-livetest && mv {q(self.admins_cache)}.pre-livetest "
                     f"{q(self.admins_cache)}; true")
        log(f"fleet.cfg and fleet data moved to {trash}")

    # ---- the test ---------------------------------------------------------------------------
    def assign_payload(self, map_name: str) -> dict:
        r = self.a.max_rounds
        return {
            "match_id": self.match_id, "epoch": self.epoch, "config_rev": 1,
            "config": {
                "num_maps": 1,
                "maps": [{"number": 1, "name": map_name, "sides": "knife"}],
                "team1": {"id": "lt_a", "name": "LiveTestA", "players": []},
                "team2": {"id": "lt_b", "name": "LiveTestB", "players": []},
                "password": "",
                "rules": {"max_rounds": r, "overtime": {"enabled": False, "rounds_per_half": 3},
                          "knife": {"side_pick_seconds": 5}, "clinch_series": True},
                "cvars": {"mp_halftime": 1, "mp_halftime_duration": 3, "mp_freezetime": 1, "mp_roundtime": 1,
                          "mp_roundtime_defuse": 1, "mp_roundtime_hostage": 1, "mp_round_restart_delay": 2,
                          "mp_c4timer": 20, "mp_win_panel_display_time": 1, "mp_team_timeout_max": 0},
            },
        }

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

    def run(self) -> int:
        why = self.preflight()
        if why:
            log(f"BUSY/UNAVAILABLE: {why}")
            print(f"FLEET LIVETEST SKIPPED ({why})")
            return 2
        self.follower = self.srv.follow(self.srv.log_size())
        try:
            self.mock.start()
        except (RuntimeError, OSError, subprocess.SubprocessError) as e:
            # The test's own stand-in platform, not Ready Up: no verdict.
            log(f"mock platform: {e}")
            print("FLEET LIVETEST SKIPPED (the mock platform did not start)")
            self.mock.stop()
            self.follower.close()
            return 2
        self.events = StepEvents.from_env()
        try:
            return self.body()
        except Exception as e:  # noqa: BLE001 - a driver error is a failed run, with its table
            log(f"error: {type(e).__name__}: {e}")
            self.step("test driver", False, f"{type(e).__name__}: {e}")
            return self.finish()
        finally:
            self.cleanup()
            if self.events:
                self.events.close()

    def body(self) -> int:
        a = self.a
        # ---- enroll + hello
        self.phase("enroll + hello")
        mark = len(self.lines)
        self.srv.send("ru mode idle")
        self.srv.send("bot_kick")
        self.configure_fleet()
        self.srv.send("ru plugin reload fleet")
        hello = self.wait_frame("hello", 0, timeout=60)
        st = self.mock.get("/status")
        if not self.step("enroll + hello", bool(hello) and st["connected"],
                         f"server {st['server_id']}, capabilities {hello and hello['payload'].get('capabilities')}"):
            return self.finish()
        caps = hello["payload"].get("capabilities", [])
        self.step("match.v1 capability", "match.v1" in caps, ",".join(caps))
        self.step("match.resume.v1 + maps.workshop.v1 capabilities",
                  "match.resume.v1" in caps and "maps.workshop.v1" in caps, ",".join(caps))

        # ---- admins cache version (D13): admins.set rev N -> admins_rev N in snapshots and hellos
        self.phase("admins.set -> admins_rev")
        cached = hello["payload"].get("admins_rev")
        self.admins_rev = (cached or 0) + 1
        self.mock.send("admins.set", {"rev": self.admins_rev, "admins": []})
        res = self.cmd("snapshot_now", {}, match=False)
        snapa = self.wait_frame("state.snapshot", 0, lambda f: f["payload"].get("admins_rev") == self.admins_rev)
        self.step("admins.set rev -> state.snapshot.admins_rev", self.result_ok(res) and bool(snapa),
                  f"hello admins_rev {cached}, sent rev {self.admins_rev}, snapshot admins_rev "
                  f"{snapa and snapa['payload'].get('admins_rev')}")

        # ---- cmd reset and exec without an assignment
        self.phase("idle: cmd reset, exec")
        res = self.cmd("reset", {}, match=False)
        self.step("cmd reset on an idle server -> ok, ended none",
                  self.result_ok(res) and res["payload"].get("result") == {"ended": "none", "match_id": None},
                  json.dumps(res and res["payload"]))
        res = self.cmd("exec", {"command": "echo lt-idle-exec"}, match=False, root=True)
        self.step("cmd exec on an idle server -> ok with its output",
                  self.result_ok(res) and "lt-idle-exec" in (res["payload"].get("output") or ""),
                  self.err_code(res) or "ok")

        # ---- assign
        self.phase("match.assign")
        cur_map = ""
        for ln in reversed(self.lines):
            m = STATE_RE.search(ln)
            if m:
                cur_map = dict(KV_RE.findall(m.group(1))).get("map", "")
                if cur_map:
                    break
        map_name = a.map or cur_map or "de_dust2"
        if a.resume_only:
            bd = self.load_backup()
            if not self.step("backup from an earlier run", bool(bd), a.resume_backup or "(no --resume-backup)"):
                return self.finish()
            self.resume_scenario(bd, map_name)
            return self.finish()
        if a.from_scrim:
            # D16: a bots-only scrim is running when the assignment arrives.
            mark = len(self.lines)
            self.scrim_flag = True
            self.srv.send("ru_dev_bots_scrim 1")
            self.srv.send("ru mode scrim")
            self.srv.send(f"bot_quota {2 * a.bots_per_side}")
            kv = self.state_line(mark, lambda kv: kv.get("match", "").startswith("scrim:"), 120)
            self.step("scrim running before the assignment", bool(kv), kv and f"mode={kv.get('mode')} match={kv.get('match')}")
        mark = len(self.lines)
        since = len(self.frames)
        t_assign = time.time()
        aid = self.mock.send("match.assign", self.assign_payload(map_name), self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == aid)
        self.step("match.assign ack", self.result_ok(res) and res.get("epoch") == self.epoch,
                  json.dumps(res and res["payload"]))
        snap = self.wait_frame("state.snapshot", since, lambda f: f["payload"].get("reason") == "assign")
        self.step("state.snapshot (assign)", bool(snap) and snap["payload"]["state"]["match_id"] == self.match_id
                  and snap.get("epoch") == self.epoch,
                  snap and f"phase={snap['payload']['state']['phase']} live_rev={snap['payload']['state']['live_rev']}")
        if a.from_scrim:
            loaded = self.wait_line(f"fleet: match {self.match_id} epoch {self.epoch} loaded", mark, 20)
            took = time.time() - t_assign
            ho = self.wait_line("hand-over: scrim ended", mark, 1)
            notice = self.wait_line("assigned a tournament match. Thanks for playing!", mark, 1)
            self.step("D16 hand-over: notice, scrim ended, load after 5 s", bool(ho and notice and loaded) and took >= 4.5,
                      f"{ho} | loaded {took:.1f} s after the assign")
            self.srv.send("ru_dev_bots_scrim cfg")
        time.sleep(3)  # the load kicks bots (bot_kick / bot_quota 0); add them after it
        self.srv.send(f"bot_quota {2 * a.bots_per_side}")

        # fencing while it warms up
        self.phase("fencing, match.update, exec")
        since = len(self.frames)
        other = self.assign_payload(map_name)
        other["match_id"] = self.match_id + "-other"
        other["epoch"] = 1
        bid = self.mock.send("match.assign", other, 1)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == bid)
        self.step("second assign -> busy", self.err_code(res) == "busy", self.err_code(res))
        # A match command: fleet.so runs `say` itself (FLEET.md 7.4), so it is not fenced by epoch.
        res = self.cmd("snapshot_now", {}, epoch=1)
        self.step("cmd with a lower epoch -> stale_epoch", self.err_code(res) == "stale_epoch", self.err_code(res))
        res = self.cmd("pause", {"type": "admin"}, match_id="not-this-one")
        self.step("cmd for another match -> not_assigned", self.err_code(res) == "not_assigned", self.err_code(res))
        since = len(self.frames)
        uid = self.mock.send("match.update", {"match_id": self.match_id, "epoch": self.epoch, "base_config_rev": 0,
                                              "config_rev": 2, "ops": [{"op": "rename_team", "team": "team1",
                                                                        "name": "LT Alpha"}]}, self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == uid)
        snapc = self.wait_frame("state.snapshot", since, lambda f: f["payload"].get("reason") == "request")
        self.step("match.update wrong base -> conflict", self.err_code(res) == "conflict" and
                  res["payload"].get("rev") == 1 and bool(snapc), f"{self.err_code(res)} rev={res and res['payload'].get('rev')}")
        since = len(self.frames)
        uid = self.mock.send("match.update", {"match_id": self.match_id, "epoch": self.epoch, "base_config_rev": 1,
                                              "config_rev": 2, "ops": [{"op": "rename_team", "team": "team1",
                                                                        "name": "LT Alpha"}]}, self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == uid)
        deadline = time.time() + 10
        renamed = False
        while time.time() < deadline and not renamed:
            self.pump(0.3)
            renamed = any("LT Alpha" in json.dumps(f["frame"]["payload"].get("patch", {})) for f in self.frames[since:])
        self.step("match.update CAS ok (+ patch)", self.result_ok(res) and res["payload"].get("rev") == 2 and renamed,
                  f"rev={res and res['payload'].get('rev')} renamed_in_patch={renamed}")
        res = self.cmd("exec", {"command": "echo fleet-livetest"}, match=False, root=False, audit="aud_lt_1")
        self.step("exec without root -> forbidden", self.err_code(res) == "forbidden" and
                  res["payload"].get("audit_id") == "aud_lt_1", self.err_code(res))
        res = self.cmd("exec", {"command": "echo fleet-livetest-exec"}, match=False, root=True, audit="aud_lt_2")
        self.step("exec (root) ok + audit id echoed", self.result_ok(res) and res["payload"].get("audit_id") == "aud_lt_2",
                  json.dumps(res and {k: v for k, v in res["payload"].items() if k != "output"}) +
                  f" output={len((res or {}).get('payload', {}).get('output', ''))}B")

        # ---- the match goes live
        self.phase("warmup -> knife -> live, rounds")
        live = self.wait_frame("event.phase", 0, lambda f: f["payload"]["data"]["to"] == "live", timeout=300)
        phases = [f["frame"]["payload"]["data"]["from"] + "->" + f["frame"]["payload"]["data"]["to"]
                  for f in self.frames_of("event.phase")]
        if not self.step("phase events to live", bool(live), " ".join(phases)):
            return self.finish()
        self.step("knife_result + side_picked", bool(self.frames_of("event.knife_result")) and
                  bool(self.frames_of("event.side_picked")),
                  json.dumps([f["frame"]["payload"]["data"] for f in self.frames_of("event.knife_result")]))
        rs = self.wait_frame("event.round_start", 0, timeout=120)
        re1 = self.wait_frame("event.round_end", 0, timeout=a.round_timeout)
        ok = bool(re1) and isinstance(re1["payload"]["data"]["round"].get("players"), list)
        self.step("round_start + round_end (RoundSummary)", bool(rs) and ok,
                  re1 and f"round {re1['payload']['data']['round']['round_number']} winner_team="
                          f"{re1['payload']['data']['round']['winner_team']} "
                          f"players={len(re1['payload']['data']['round']['players'])} rev={re1['payload']['rev']}")
        rnums = [f["frame"]["payload"]["data"]["round"] for f in self.frames_of("event.round_start")]
        self.step("round numbers start at 1 on a fresh match", bool(rnums) and rnums[0] == 1, f"round_start rounds {rnums}")

        # ---- pause / unpause (right after round 1: the match is live)
        self.phase("pause / unpause")
        res = self.cmd("pause", {"type": "admin"})
        pz = self.wait_frame("event.pause", 0, lambda f: f["payload"]["data"]["action"] == "paused")
        self.step("cmd pause -> ok + event.pause", self.result_ok(res) and bool(pz),
                  pz and json.dumps(pz["payload"]["data"]))
        res = self.cmd("pause", {"type": "admin"})
        self.step("pause when paused -> already_paused", self.err_code(res) == "already_paused", self.err_code(res))
        time.sleep(3)
        res = self.cmd("unpause", {})
        up = self.wait_frame("event.pause", 0, lambda f: f["payload"]["data"]["action"] == "unpaused")
        self.step("cmd unpause -> ok + event.pause", self.result_ok(res) and bool(up),
                  up and json.dumps(up["payload"]["data"]))
        res = self.cmd("unpause", {})
        self.step("unpause when not paused -> not_paused", self.err_code(res) == "not_paused", self.err_code(res))

        # ---- round backups: forwarded inline, then restored (local file, then inline)
        self.phase("round backups + restore_round")
        bk = self.wait_frame("event.backup", 0, timeout=a.round_timeout)
        if bk:  # the newest backup so far (a restore then really goes back)
            self.pump(2.0)
            bk = self.frames_of("event.backup")[-1]["frame"]
        bd = bk and bk["payload"]["data"]
        self.step("inline round backup (event.backup)", bool(bk),
                  bk and f"{bd['file']} round {bd['round']} {bd['size']} B sha256 {bd['sha256'][:12]} "
                         f"base64 {len(bd['data'])} chars")
        if bk:
            import base64
            import hashlib
            raw = base64.b64decode(bd["data"])
            self.step("backup sha256 matches its data", hashlib.sha256(raw).hexdigest() == bd["sha256"]
                      and len(raw) == bd["size"], f"{len(raw)} B")
            if a.save_backup:
                with open(a.save_backup, "w") as fh:
                    json.dump(bd, fh)
                log(f"round backup saved to {a.save_backup}")
            for how, args in (("local file", {"map_number": bd["map_number"], "round": bd["round"]}),
                              ("inline", {"map_number": bd["map_number"], "round": bd["round"], "backup": bd})):
                since = len(self.frames)
                mark = len(self.lines)
                res = self.cmd("restore_round", args)
                # Asked mid-round, the restore waits for the round to end and runs at the next
                # round start (DoRestore: weapons come back only then), so give it a round.
                rv = self.wait_frame("event.rounds_voided", since, timeout=a.round_timeout + 30)
                mr = self.wait_frame("event.match_restored", since, timeout=a.round_timeout + 30)
                mr_i = next((w["i"] for w in self.frames if w["frame"] is mr), len(self.frames)) if mr else len(self.frames)
                nxt = self.wait_frame("event.round_start", mr_i, timeout=15)
                self.step(f"round counter after restore ({how})", bool(nxt) and nxt["payload"]["data"]["round"] == bd["round"]
                          and not self.frames_of("event.round_end", mr_i),
                          f"next round_start {nxt and nxt['payload']['data']['round']}, want {bd['round']}; "
                          f"round_end since restore: {len(self.frames_of('event.round_end', mr_i))}")
                self.step(f"restore_round ({how}) -> ok + rounds_voided + match_restored",
                          self.result_ok(res) and bool(rv) and bool(mr),
                          (self.err_code(res) or "ok") + (mr and f" sha256 {mr['payload']['data']['backup_sha256'][:12]}" or ""))
                eng = self.wait_line("Loaded server checkpoint", mark, 10)
                self.step(f"CS2 loaded the backup ({how})", bool(eng), eng or "no `Loaded server checkpoint` line")
                time.sleep(3)
                res = self.cmd("unpause", {})
                self.step(f"unpause after restore ({how})", self.result_ok(res), self.err_code(res) or "ok")
            bad = self.cmd("restore_round", {"map_number": bd["map_number"], "round": bd["round"],
                                             "backup": dict(bd, sha256="0" * 64)})
            self.step("restore with a wrong sha256 -> checksum", self.err_code(bad) == "checksum", self.err_code(bad))

        # ---- D12: platform unreachable -> auto-pause
        if not a.no_offline:
            self.phase("platform offline -> auto-pause")
            mark = len(self.lines)
            self.mock.post("/offline", {"seconds": 75})
            line = self.wait_line("auto-paused match", mark, 120)
            self.step("offline 60 s -> auto-pause", bool(line), line or "no auto-pause line")
            off = self.wait_frame("event.pause", 0, lambda f: f["payload"]["data"].get("type") == "offline"
                                  and f["payload"]["data"]["action"] == "paused", timeout=90)
            self.step("spooled event.pause {offline} after reconnect", bool(off),
                      off and f"seq {off.get('seq')} rev {off['payload']['rev']}")
            res = self.cmd("unpause", {}, timeout=60)
            self.step("platform unpause after reconnect", self.result_ok(res), self.err_code(res) or "ok")
            hellos = self.frames_of("hello")
            last = hellos[-1]["frame"]["payload"] if hellos else {}
            self.step("hello after the reconnect carries admins_rev", len(hellos) >= 2 and
                      last.get("admins_rev") == self.admins_rev,
                      f"{len(hellos)} hello(s), last admins_rev {last.get('admins_rev')} (want {self.admins_rev})")

        # ---- end
        self.phase("end_match + match.unassign")
        if a.play_out:
            mr = self.wait_frame("event.map_result", 0, timeout=a.round_timeout * (a.max_rounds + 2))
            se = self.wait_frame("event.series_end", 0, timeout=120)
            self.step("map_result (MapStats) + series_end", bool(mr) and bool(se),
                      mr and f"{mr['payload']['data']['team1_score']}-{mr['payload']['data']['team2_score']} "
                             f"players={len(mr['payload']['data']['stats']['players'])}")
        elif self.frames_of("event.series_end"):
            self.step("cmd end_match", True, "SKIP: the map already ended on its own (series_end seen)")
        else:
            res = self.cmd("end_match", {"reason": "livetest"})
            se = self.wait_frame("event.series_end", 0)
            self.step("cmd end_match -> ok + event.series_end", self.result_ok(res) and bool(se),
                      se and json.dumps(se["payload"]["data"]))
        since = len(self.frames)
        uid = self.mock.send("match.unassign", {"match_id": self.match_id, "epoch": self.epoch, "reason": "ended"},
                             self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == uid)
        av = self.wait_frame("server.availability", 0, lambda f: f["payload"]["availability"] == "available")
        self.step("match.unassign -> ok + available", self.result_ok(res) and bool(av), self.err_code(res) or "ok")
        res = self.cmd("pause", {"type": "admin"})
        self.step("cmd after unassign -> stale_epoch", self.err_code(res) == "stale_epoch", self.err_code(res))
        res = self.cmd("reset", {}, match=False)
        self.step("cmd reset after unassign -> ended none", self.result_ok(res) and
                  (res["payload"].get("result") or {}).get("ended") == "none", json.dumps(res and res["payload"]))
        if not a.no_resume:
            src = self.load_backup() if a.resume_backup else bd
            if self.step("round backup for the resume", bool(src), (a.resume_backup or "this run's") +
                         (f" round {src['round']} sha256 {src['sha256'][:12]}" if src else "")):
                self.resume_scenario(src, map_name)
        self.pump(2.0)
        return self.finish()

    # ---- failover resume (FLEET.md §11.3) ------------------------------------------------------
    def load_backup(self):
        if not self.a.resume_backup:
            return None
        try:
            with open(self.a.resume_backup) as fh:
                return json.load(fh)
        except (OSError, ValueError) as e:
            log(f"cannot read {self.a.resume_backup}: {e}")
            return None

    def resume_scenario(self, bd: dict, map_name: str) -> None:
        a = self.a
        self.phase("failover resume")
        prev_epoch = self.epoch
        self.epoch += 1
        rnd = bd["round"]
        payload = self.assign_payload(map_name)
        payload["epoch"] = self.epoch
        cfg = payload["config"]
        cfg["num_maps"] = 3
        cfg["maps"] = [{"number": n, "name": map_name, "sides": "team1_ct"} for n in (1, 2, 3)]
        cfg["rules"]["pause"] = {"pause_after_restore": True}
        backup = dict(bd, map_number=2)
        backup.pop("part", None)
        backup.pop("parts", None)
        payload["resume"] = {"from_epoch": prev_epoch, "map_number": 2, "round": rnd, "backup": backup,
                             "series_score": {"team1": 1, "team2": 0},
                             "maps": {"1": {"score": {"team1": 13, "team2": 5}, "winner": "team1"}},
                             "sides": "team1_ct"}

        def assign(p):
            since = len(self.frames)
            aid = self.mock.send("match.assign", p, p["epoch"])
            return since, self.wait_frame("cmd.result", since, lambda f: f.get("ref") == aid)

        bad = json.loads(json.dumps(payload))
        bad["resume"]["backup"]["sha256"] = "0" * 64
        _, res = assign(bad)
        self.step("resume with a wrong sha256 -> checksum", self.err_code(res) == "checksum", self.err_code(res))
        bad = json.loads(json.dumps(payload))
        bad["resume"]["map_number"] = 4
        _, res = assign(bad)
        self.step("resume of a map past num_maps -> invalid_config", self.err_code(res) == "invalid_config",
                  self.err_code(res))

        mark = len(self.lines)
        since, res = assign(payload)
        self.step("match.assign with resume -> ok", self.result_ok(res) and res.get("epoch") == self.epoch,
                  json.dumps(res and res["payload"]))
        snap = self.wait_frame("state.snapshot", since, lambda f: f["payload"].get("reason") == "assign")
        st = snap and snap["payload"]["state"]
        self.step("state.snapshot (assign): phase restoring, series 1-0", bool(st) and st["phase"] == "restoring" and
                  st["series"]["score"] == {"team1": 1, "team2": 0} and st["epoch"] == self.epoch,
                  st and f"phase={st['phase']} series={st['series']['score']} epoch={st['epoch']}")
        line = self.wait_line(f"resumes on map 2 at round {rnd}", mark, 20)
        self.step("Ready Up resumes map 2 at the backup's round", bool(line), line or "no `resumes on map 2` line")
        time.sleep(3)  # the load kicks bots; add them after it
        self.srv.send(f"bot_quota {2 * a.bots_per_side}")

        rv = self.wait_frame("event.rounds_voided", since, lambda f: f["payload"]["data"]["reason"] == "resume",
                             timeout=240)
        mr = self.wait_frame("event.match_restored", since, lambda f: f["payload"]["data"].get("resume") is True)
        mrd = mr and mr["payload"]["data"]
        self.step("go-live -> rounds_voided {resume} + match_restored {resume}", bool(rv) and bool(mr) and
                  mrd["backup_sha256"] == bd["sha256"] and mrd["round"] == rnd and mr["payload"]["map_number"] == 2
                  and mrd.get("from_epoch") == prev_epoch,
                  mrd and f"map {mr['payload']['map_number']} round {mrd['round']} file {mrd.get('file')} "
                          f"sha256 {mrd['backup_sha256'][:12]} from_epoch {mrd.get('from_epoch')}")
        eng = self.wait_line("Loaded server checkpoint", mark, 15)
        self.step("CS2 loaded the resume backup", bool(eng), eng or "no `Loaded server checkpoint` line")
        mr_i = next((w["i"] for w in self.frames if w["frame"] is mr), len(self.frames)) if mr else len(self.frames)
        early = [w["frame"]["type"] for w in self.frames[since:mr_i]
                 if w["frame"].get("type") in ("event.round_start", "event.round_end", "event.backup")]
        rsnap = self.wait_frame("state.snapshot", since, lambda f: f["payload"].get("reason") == "restored")
        rs = rsnap and rsnap["payload"]["state"]
        ok = bool(rs) and rs["series"]["current_map"] == 2 and rs["series"]["score"] == {"team1": 1, "team2": 0} \
            and rs["series"]["maps"]["1"].get("status") == "done" and rs["series"]["maps"]["1"].get("winner") == "team1" \
            and rs["phase"] == "paused" and rs["pause"]["active"]
        self.step("state.snapshot (restored): map 2, series 1-0, map 1 done, paused", ok,
                  rs and f"current_map={rs['series']['current_map']} series={rs['series']['score']} "
                         f"map1={rs['series']['maps']['1'].get('status')}/{rs['series']['maps']['1'].get('winner')} "
                         f"phase={rs['phase']} round={rs['round']['number']} score="
                         f"{rs['teams']['team1']['score']}-{rs['teams']['team2']['score']}")
        self.step("no round events reported before the restore", bool(mr) and not early,
                  f"round events before match_restored: {early}")
        # Everyone readied up to get here: the resume goes live by itself 3 s after the restore.
        up = self.wait_frame("event.pause", mr_i, lambda f: f["payload"]["data"]["action"] == "unpaused", timeout=15)
        nxt = self.wait_frame("event.round_start", mr_i, timeout=30)
        self.step("resume unpauses by itself -> next round is the backup's",
                  bool(up) and bool(nxt) and nxt["payload"]["data"]["round"] == rnd and nxt["payload"]["map_number"] == 2,
                  f"unpaused by {up and up['payload']['data'].get('by')}; next round_start "
                  f"{nxt and nxt['payload']['data']['round']} map {nxt and nxt['payload']['map_number']} "
                  f"(want round {rnd} map 2)")
        res = self.cmd("end_match", {"reason": "livetest-resume"})
        self.step("cmd end_match after the resume", self.result_ok(res), self.err_code(res) or "ok")
        since = len(self.frames)
        uid = self.mock.send("match.unassign", {"match_id": self.match_id, "epoch": self.epoch, "reason": "ended"},
                             self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == uid)
        self.step("match.unassign after the resume", self.result_ok(res), self.err_code(res) or "ok")

    # ---- checks over everything received -----------------------------------------------------
    def verify_stream(self) -> None:
        self.phase("frame stream: schemas, live_rev, patches")
        bad = [(f["frame"].get("type"), f["errors"]) for f in self.frames if f["errors"]]
        if self.a.save_examples:
            # The resume's own frames too (the generic dump below keeps the first of each type).
            os.makedirs(self.a.save_examples, exist_ok=True)
            for f in self.frames:
                fr = f["frame"]
                d = fr.get("payload", {}).get("data", {})
                name = None
                if fr.get("type") == "event.match_restored" and d.get("resume"):
                    name = "live.event.match_restored.resume.json"
                elif fr.get("type") == "state.snapshot" and fr["payload"].get("reason") == "restored":
                    name = "live.state.snapshot.restored.json"
                if name:
                    with open(os.path.join(self.a.save_examples, name), "w") as fh:
                        json.dump(fr, fh, indent=2)
                        fh.write("\n")
        self.step("every frame matches the schemas", not bad, f"{len(self.frames)} frames" if not bad else json.dumps(bad[:5])[:600])
        # live_rev: +1 per state.patch / event.*, and patches rebuild the snapshots.
        state = None
        rev = None
        gaps, mismatches, checked = [], [], 0
        fr_epoch = None
        for f in self.frames:
            fr = f["frame"]
            t = fr.get("type", "")
            p = fr.get("payload", {})
            if t == "state.snapshot" and p.get("state") and p["state"].get("match_id") == self.match_id:
                if state is None or p.get("reason") == "assign" or p["state"].get("epoch") != fr_epoch:
                    fr_epoch = p["state"].get("epoch")
                    state, rev = p["state"], p["state"]["live_rev"]
                    continue
                if p["state"]["live_rev"] == rev:
                    checked += 1
                    if json.dumps(state, sort_keys=True) != json.dumps(p["state"], sort_keys=True):
                        mismatches.append(rev)
            elif (t == "state.patch" or t.startswith("event.")) and state is not None and p.get("match_id") == self.match_id \
                    and fr.get("epoch") == fr_epoch:
                if p["rev"] != rev + 1:
                    gaps.append((rev, p["rev"], t))
                rev = p["rev"]
                state = merge_patch(state, p["patch"])
        self.step("live_rev +1 per message", not gaps and rev is not None, f"last rev {rev}" if not gaps else str(gaps[:5]))
        self.step("patches rebuild the snapshots", not mismatches and checked > 0,
                  f"{checked} snapshot(s) compared" if not mismatches else f"mismatch at rev {mismatches[:5]}")
        # One cmd.result per platform message.
        results = self.frames_of("cmd.result")
        refs = [f["frame"].get("ref") for f in results]
        self.step("one cmd.result per command", len(refs) == len(set(refs)), f"{len(refs)} results")
        types = sorted({f["frame"].get("type") for f in self.frames})
        log("frame types: " + " ".join(types))
        if self.a.save_examples:
            os.makedirs(self.a.save_examples, exist_ok=True)
            seen = set()
            for f in self.frames:
                t = f["frame"].get("type")
                if t in seen or t in ("ping", "hello"):
                    continue
                seen.add(t)
                with open(os.path.join(self.a.save_examples, f"live.{t}.json"), "w") as fh:
                    json.dump(f["frame"], fh, indent=2)
                    fh.write("\n")
            log(f"saved {len(seen)} example frames to {self.a.save_examples}")

    def finish(self) -> int:
        try:
            self.pump(0.5)
        except Exception as e:  # noqa: BLE001
            log(f"pump: {e}")
        self.verify_stream()
        self.end_phase()
        print()
        print("=" * 100)
        for name, status, detail in self.results:
            print(f"{name:<55} {status:<5} {(detail or '')[:200]}")
        print("=" * 100)
        verdict = "FAIL" if self.failures else "PASS"
        print(f"FLEET LIVETEST {verdict}" + (f" ({', '.join(self.failures)})" if self.failures else ""))
        if os.environ.get("GITHUB_ACTIONS") == "true":
            for name, status, detail in self.results:
                if status == "FAIL":
                    print(f"::error title=livetest fleet::{gh_escape(name + ': ' + (detail or ''))}")
        summ = os.environ.get("GITHUB_STEP_SUMMARY")
        if summ:
            with open(summ, "a") as fh:
                fh.write(f"## Ready Up fleet live test: {verdict}\n\n| check | result | detail |\n|---|---|---|\n")
                for name, status, detail in self.results:
                    fh.write(f"| {name} | {status} | {(detail or '').replace('|', '/')[:300]} |\n")
        if self.a.out:
            os.makedirs(self.a.out, exist_ok=True)
            with open(os.path.join(self.a.out, "frames.json"), "w") as fh:
                json.dump(self.frames, fh, indent=1)
            with open(os.path.join(self.a.out, "console.log"), "w") as fh:
                fh.write("\n".join(self.lines) + "\n")
            with open(os.path.join(self.a.out, "mock.log"), "w") as fh:
                fh.write(self.mock.logs())
        return 1 if self.failures else 0

    def cleanup(self) -> None:
        try:
            if self.scrim_flag:
                self.srv.send("ru_dev_bots_scrim cfg")
            if self.touched:
                self.srv.send("ru mode idle")
                self.srv.send("bot_kick")
                self.unconfigure_fleet()
                mark = len(self.lines)
                self.srv.send("ru plugin reload fleet")
                ok = self.wait_line("standalone (no [fleet] url)", mark, 20)
                log("fleet.so standalone again" if ok else "WARNING: fleet.so did not report standalone")
                self.srv.send("ru mode scrim")
                self.srv.send(f"bot_quota {self.a.restore_bot_quota}")
        except Exception as e:  # noqa: BLE001
            log(f"cleanup error: {e}")
        if self.a.out:
            os.makedirs(self.a.out, exist_ok=True)
            with open(os.path.join(self.a.out, "mock.log"), "w") as fh:
                fh.write(self.mock.logs())
        self.mock.stop()
        if self.follower:
            self.follower.close()


def parse_args(argv=None):
    env = os.environ.get
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ssh", default=env("RU_SSH", "cs2servermanager@localhost"))
    p.add_argument("--target", default=env("RU_TARGET", "/home/cs2servermanager/readyup-test"))
    p.add_argument("--session", default=env("RU_SESSION", "ru-test"))
    p.add_argument("--game-dir", default=env("RU_GAME_DIR", ""),
                   help="CS2 install root (contains game/) when it is not --target (CI: $CS2_CI_DIR)")
    p.add_argument("--boot", action="store_true", help="start the server if the session is still missing")
    p.add_argument("--wait-session", type=float, default=float(env("LIVETEST_WAIT_SESSION", "300")),
                   help="how long to wait for a missing tmux session (deploy mid-restart)")
    p.add_argument("--mock", choices=["auto", "docker", "local"], default=env("FLEET_MOCK", "auto"),
                   help="how the mock platform runs: docker (python:3.12-slim), local (deps pip-installed "
                        "into $LIVETEST_PYDEPS), auto = docker when this user can use it")
    p.add_argument("--map", default="")
    p.add_argument("--max-rounds", type=int, default=4)
    p.add_argument("--bots-per-side", type=int, default=2)
    p.add_argument("--round-timeout", type=float, default=200)
    p.add_argument("--http-port", type=int, default=0, help="mock platform ports (loopback); 0 = a free one")
    p.add_argument("--ws-port", type=int, default=0)
    p.add_argument("--ctl-port", type=int, default=0)
    p.add_argument("--play-out", action="store_true", help="play the map to the end instead of cmd end_match")
    p.add_argument("--no-offline", action="store_true", help="skip the 75 s platform outage (D12 auto-pause)")
    p.add_argument("--from-scrim", action="store_true",
                   help="assign while a bots-only scrim runs (dev_bots_scrim): D16 hand-over")
    p.add_argument("--force", action="store_true")
    p.add_argument("--restore-bot-quota", type=int, default=2)
    p.add_argument("--out", default="")
    p.add_argument("--save-examples", default="", help="write the first frame of each type the server sent here")
    p.add_argument("--no-resume", action="store_true", help="skip the failover resume (match.assign resume)")
    p.add_argument("--resume-only", action="store_true",
                   help="only enroll and run the resume scenario (needs --resume-backup)")
    p.add_argument("--resume-backup", default="", help="event.backup data (JSON) from an earlier run for the resume")
    p.add_argument("--save-backup", default="", help="write this run's round backup (JSON) here for a later --resume-only")
    return p.parse_args(argv)


if __name__ == "__main__":
    sys.exit(Test(parse_args()).run())
