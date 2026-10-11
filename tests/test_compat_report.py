"""Tests for scripts/ci/compat-report.py.   Run: python3 -m unittest discover -s tests -v"""
import http.server
import importlib.util
import io
import json
import os
import pathlib
import shutil
import tempfile
import textwrap
import threading
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIX = ROOT / "tests" / "fixtures" / "compat"

_spec = importlib.util.spec_from_file_location("compat_report", ROOT / "scripts" / "ci" / "compat-report.py")
cr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cr)

RUN = ["--run-id", "123", "--run-url", "https://example.invalid/run/123", "--trigger", "build_change",
       "--started-at", "2026-09-25T22:00:00Z", "--commit", "7f61b71ee5cecde6a1ec33ac7f9c1f03eff7e038",
       "--version-file", str(ROOT / "VERSION")]
ISO = r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$"


def assert_contract(tc, doc):
    """The exact compat.json contract the platform builds against (docs/CS2-COMPAT.md)."""
    tc.assertEqual(set(doc), {"schema", "cs2", "readyup", "run", "overall", "components", "checked_at"})
    tc.assertEqual(doc["schema"], 1)
    tc.assertEqual(set(doc["cs2"]), {"buildid", "patch"})
    tc.assertIsInstance(doc["cs2"]["buildid"], str)
    tc.assertIsInstance(doc["cs2"]["patch"], str)
    tc.assertEqual(set(doc["readyup"]), {"version", "commit"})
    run = doc["run"]
    tc.assertEqual(set(run), {"id", "url", "trigger", "stage", "state", "started_at", "finished_at"})
    tc.assertIsInstance(run["id"], str)
    tc.assertIn(run["trigger"], ("build_change", "surface_change", "nightly", "release", "manual"))
    tc.assertIn(run["stage"], ("static", "selftest", "live"))
    tc.assertIn(run["state"], ("queued", "checking", "pass", "warn", "fail", "no_verdict"))
    tc.assertRegex(run["started_at"], ISO)
    if run["finished_at"] is not None:
        tc.assertRegex(run["finished_at"], ISO)
    tc.assertIn(doc["overall"], ("pass", "warn", "fail", "checking", "no_verdict"))
    tc.assertRegex(doc["checked_at"], ISO)
    for c in doc["components"]:
        tc.assertEqual(set(c), {"id", "name", "status", "checks"})
        tc.assertIn(c["status"], ("pass", "warn", "fail", "pending", "checking"))
        for k in c["checks"]:
            tc.assertEqual(set(k), {"kind", "status", "passed", "total", "failures"})
            tc.assertIn(k["kind"], ("signature", "rtti", "vtable", "hook_site", "layout", "schema", "event",
                                    "selftest", "livetest"))
            tc.assertIn(k["status"], ("pass", "warn", "fail", "pending"))
            tc.assertIsInstance(k["passed"], int)
            tc.assertIsInstance(k["total"], int)
            tc.assertTrue(all(isinstance(f, str) for f in k["failures"]))


class CompatReportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.gamedata = self.tmp / "gamedata"
        self.gamedata.mkdir()
        for f in ("engine-surface.json", "engine-surface.skins.json"):
            shutil.copy(FIX / f, self.gamedata / f)

    def raw(self, sig="sigcheck-pass.txt", hook="hookcheck-pass.txt", sig_rc=0, hook_rc=0):
        d = self.tmp / "raw"
        d.mkdir(exist_ok=True)
        (d / "sigcheck.txt").write_text((FIX / sig).read_text() if sig else "")
        (d / "hookcheck.txt").write_text((FIX / hook).read_text() if hook else "")
        (d / "rc.env").write_text("SIGCHECK_RC=%d\nHOOKCHECK_RC=%d\n" % (sig_rc, hook_rc))
        return d

    def result(self, plugins_dir=None, **kw):
        """plugins_dir None: no needs.json anywhere (plugins without a manifest)."""
        out = self.tmp / "out"
        if plugins_dir is None:
            plugins_dir = self.tmp / "no-plugins"
            plugins_dir.mkdir(exist_ok=True)
        # A tag build sets GITHUB_REF_TYPE=tag, which drops the "-dev.<sha>" suffix.
        with mock.patch.dict(os.environ, {"GITHUB_REF_TYPE": ""}):
            rc = cr.main(["result", "--raw-dir", str(self.raw(**kw)), "--gamedata", str(self.gamedata),
                          "--build-env", str(FIX / "cs2-build.env"), "--out-dir", str(out),
                          "--plugins-dir", str(plugins_dir)] + RUN)
        self.assertEqual(rc, 0)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        return doc, json.loads((out / "badge.json").read_text())

    @staticmethod
    def comp(doc, cid):
        return next(c for c in doc["components"] if c["id"] == cid)

    @staticmethod
    def check(comp, kind):
        return next(k for k in comp["checks"] if k["kind"] == kind)

    def test_static_pass_is_warn_live_pending(self):
        doc, badge = self.result()
        self.assertEqual(doc["cs2"], {"buildid": "25537370", "patch": "1.41.8.5"})
        self.assertEqual(doc["readyup"]["commit"], "7f61b71ee5cecde6a1ec33ac7f9c1f03eff7e038")
        self.assertTrue(doc["readyup"]["version"].endswith("-dev.7f61b71"))
        self.assertEqual(doc["run"]["stage"], "static")
        self.assertEqual(doc["run"]["state"], "warn")
        self.assertEqual(doc["overall"], "warn")
        self.assertIsNotNone(doc["run"]["finished_at"])
        ids = [c["id"] for c in doc["components"]]
        self.assertEqual(ids, ["core", "skins", "match", "practice", "essentials", "midas", "whitelist", "deathmatch", "fleet"])
        core, skins = self.comp(doc, "core"), self.comp(doc, "skins")
        self.assertEqual((core["name"], core["status"]), ("Core", "pass"))
        self.assertEqual(self.check(core, "signature"), {"kind": "signature", "status": "pass", "passed": 2,
                                                         "total": 2, "failures": []})
        # SKIP (runtime-only module) is not counted
        self.assertEqual((self.check(core, "rtti")["passed"], self.check(core, "rtti")["total"]), (1, 1))
        self.assertEqual(self.check(core, "vtable")["total"], 1)
        self.assertEqual(self.check(core, "hook_site")["total"], 1)
        self.assertEqual(self.check(core, "layout")["total"], 1)
        self.assertEqual(self.check(core, "selftest")["status"], "pending")
        self.assertEqual(skins["status"], "pass")
        self.assertEqual(self.check(skins, "signature")["total"], 2)
        self.assertEqual(self.check(skins, "vtable")["total"], 1)
        for cid in cr.RUNTIME_ONLY:
            c = self.comp(doc, cid)
            self.assertEqual(c["status"], "pending")
            self.assertEqual([k["kind"] for k in c["checks"]], ["selftest", "livetest"] if cid in ("match", "fleet") else ["selftest"])
            self.assertTrue(all(k["status"] == "pending" for k in c["checks"]))
        self.assertEqual(badge, {"schemaVersion": 1, "label": "CS2 1.41.8.5", "message": "static ok",
                                 "color": "yellow"})

    def test_fragment_failure_fails_only_its_component(self):
        doc, badge = self.result(sig="sigcheck-skins-fail.txt", sig_rc=1)
        self.assertEqual(self.comp(doc, "core")["status"], "pass")
        skins = self.comp(doc, "skins")
        self.assertEqual(skins["status"], "fail")
        sig = self.check(skins, "signature")
        self.assertEqual((sig["status"], sig["passed"], sig["total"]), ("fail", 1, 2))
        self.assertEqual(len(sig["failures"]), 1)
        self.assertTrue(sig["failures"][0].startswith("CBaseModelEntity_SetModel: optional matches=0"))
        self.assertEqual(doc["overall"], "fail")
        self.assertEqual(doc["run"]["state"], "fail")
        self.assertEqual((badge["message"], badge["color"]), ("incompatible", "red"))

    def test_core_rtti_and_hook_failure(self):
        sig = (FIX / "sigcheck-pass.txt").read_text().replace(
            "OK   rtti   CBaseEntity                            11CBaseEntity  name ok; vtable at 0x2a1b3c8",
            "FAIL rtti   CBaseEntity                            11CBaseEntity  class name not in binary")
        hook = (FIX / "hookcheck-pass.txt").read_text().replace(
            "OK   Host_Say                                 rva=0x1c3f0c0 prologue relocates",
            "FAIL Host_Say                                 rva=0x1c3f0c0 funchook_prepare=5: too short")
        d = self.raw()
        (d / "sigcheck.txt").write_text(sig)
        (d / "hookcheck.txt").write_text(hook)
        (d / "rc.env").write_text("SIGCHECK_RC=1\nHOOKCHECK_RC=1\n")
        out = self.tmp / "out2"
        cr.main(["result", "--raw-dir", str(d), "--gamedata", str(self.gamedata), "--build-env",
                 str(FIX / "cs2-build.env"), "--out-dir", str(out), "--plugins-dir", str(self.tmp)] + RUN)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        core = self.comp(doc, "core")
        self.assertEqual(core["status"], "fail")
        self.assertEqual(self.check(core, "rtti")["status"], "fail")
        self.assertEqual(self.check(core, "hook_site")["failures"][0][:9], "Host_Say:")
        self.assertEqual(self.comp(doc, "skins")["status"], "pass")
        self.assertEqual(doc["overall"], "fail")

    def test_abnormal_exit_without_fail_lines_is_a_failure(self):
        doc, badge = self.result(sig=None, hook=None, sig_rc=2, hook_rc=2)
        core = self.comp(doc, "core")
        self.assertEqual(core["status"], "fail")
        self.assertIn("readyup_sigcheck exited 2", self.check(core, "signature")["failures"][0])
        self.assertIn("readyup_hookcheck exited 2", self.check(core, "hook_site")["failures"][0])
        self.assertEqual(badge["color"], "red")

    def test_missing_raw_output_is_not_a_pass(self):
        out = self.tmp / "out3"
        empty = self.tmp / "empty"
        empty.mkdir()
        cr.main(["result", "--raw-dir", str(empty), "--gamedata", str(self.gamedata), "--out-dir", str(out),
                 "--buildid", "1", "--patch", "1.0", "--plugins-dir", str(empty)] + RUN)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        self.assertEqual(doc["overall"], "fail")
        self.assertEqual(doc["cs2"], {"buildid": "1", "patch": "1.0"})

    def test_overall_rules(self):
        passed = {"id": "core", "name": "Core", "status": "pass",
                  "checks": [{"kind": "signature", "status": "pass", "passed": 1, "total": 1, "failures": []}]}
        live_pending = dict(passed, checks=passed["checks"] + [cr.pending_check()])
        pending = {"id": "match", "name": "Match", "status": "pending", "checks": [cr.pending_check()]}
        failed = dict(passed, status="fail")
        self.assertEqual(cr.overall_status([passed]), "pass")
        self.assertEqual(cr.overall_status([live_pending]), "warn")
        self.assertEqual(cr.overall_status([passed, pending]), "warn")
        self.assertEqual(cr.overall_status([passed, pending, failed]), "fail")
        self.assertEqual(cr.component_status(live_pending["checks"]), "pass")
        self.assertEqual(cr.component_status([cr.pending_check()]), "pending")
        self.assertEqual(cr.badge({"overall": "pass", "cs2": {"patch": "1.41.8.5", "buildid": "1"}}),
                         {"schemaVersion": 1, "label": "CS2 1.41.8.5", "message": "compatible",
                          "color": "brightgreen"})

    def test_phase_events(self):
        for state, overall, comp_status in (("queued", "checking", "checking"),
                                            ("checking", "checking", "checking"),
                                            ("no_verdict", "no_verdict", "pending")):
            out = self.tmp / ("%s.json" % state)
            self.assertEqual(cr.main(["phase", state, "--out", str(out), "--gamedata", str(self.gamedata),
                                      "--buildid", "25537371"] + RUN), 0)
            doc = json.loads(out.read_text())
            assert_contract(self, doc)
            self.assertEqual((doc["run"]["state"], doc["overall"]), (state, overall))
            self.assertEqual(doc["cs2"], {"buildid": "25537371", "patch": ""})
            self.assertEqual(len(doc["components"]), 9)
            self.assertTrue(all(c["status"] == comp_status and c["checks"] == [] for c in doc["components"]))
            if state == "no_verdict":
                self.assertRegex(doc["run"]["finished_at"], ISO)
            else:
                self.assertIsNone(doc["run"]["finished_at"])

    def test_real_gamedata_all_ok(self):
        """Every entry of the repo's own gamedata is attributed to core or its fragment."""
        files = cr.surface_files(str(ROOT / "gamedata"))
        self.assertEqual(files[0][0], "core")
        owner, hooked = cr.ownership(files)
        sig, hook = [], []
        for (kind, name), _ in sorted(owner.items()):
            if kind == "signature":
                sig.append("OK   %-45s optional matches=1 rva=0x1  ok" % name)
            elif kind in ("rtti", "vtable", "layout"):
                sig.append("OK   %-6s %-38s ok" % (kind, name))
        hook = ["OK   %-40s rva=0x1 prologue relocates" % n for n in sorted(hooked)]
        comps = cr.build_components("\n".join(sig), "\n".join(hook), 0, 0, files)
        self.assertEqual(cr.overall_status(comps), "warn")
        total = sum(k["total"] for c in comps for k in c["checks"])
        self.assertEqual(total, len(owner))
        for c in comps:
            self.assertIn(c["status"], ("pass", "pending"))

    def test_grenade_fragment_is_practice_and_never_a_hard_need(self):
        """ru_api grenade_spawn (1.12): its Create functions are the practice fragment's, the core's
        type table names exactly those entries, and no plugin lists them in "surface" (a CS2 update
        that breaks one must turn grenade spawning off, not disable a plugin)."""
        files = cr.surface_files(str(ROOT / "gamedata"))
        owner, _ = cr.ownership(files)
        spec = (ROOT / "core" / "src" / "readyup" / "grenade_spec.h").read_text()
        keys = set(__import__("re").findall(r'"(C\w+Projectile_Create)"', spec))
        self.assertEqual(len(keys), 5)
        for k in keys:
            self.assertEqual(owner.get(("signature", k)), "practice", k)
        frag = json.loads((ROOT / "gamedata" / "engine-surface.practice.json").read_text())
        for k in keys:
            fn = frag["functions"][k]
            self.assertFalse(fn["required"], k)
            self.assertNotIn("hook", fn, k)
            types = [a["type"] for a in fn["anchors"]]
            self.assertIn("string_ref", types, k)
            self.assertIn({"type": "caller_string", "string": "SpawnGrenadeProjectile", "window": 768}, fn["anchors"], k)
        self.assertEqual(cr.API_SURFACE["grenade_spawn"], [])
        self.assertEqual(cr.API_SURFACE["grenade_spawn_available"], [])
        for pid, need in cr.load_needs(str(ROOT / "plugins")).items():
            self.assertFalse(keys & set(need.get("surface") or []), pid)
        # A broken grenade signature fails the practice component (with the entry named), not core.
        rows = ["OK   %-45s optional matches=1 rva=0x1  ok" % n for (kind, n) in sorted(owner) if kind == "signature"]
        rows = [r.replace("OK   CHEGrenadeProjectile_Create", "FAIL CHEGrenadeProjectile_Create")
                .replace("matches=1", "matches=0") if "CHEGrenadeProjectile_Create" in r else r for r in rows]
        comps = {c["id"]: c for c in cr.build_components("\n".join(rows), "", 1, 0, files)}
        self.assertEqual(comps["practice"]["status"], "fail")
        self.assertEqual(comps["core"]["status"], "pass")
        sig = next(k for k in comps["practice"]["checks"] if k["kind"] == "signature")
        self.assertTrue(sig["failures"][0].startswith("CHEGrenadeProjectile_Create:"))

    def test_practice_engine_fragment_is_practice_and_never_a_hard_need(self):
        """ru_api 1.13 (player_change_team, player_teleport, entity_create / entity_spawn): the four
        functions and two vtable slots are the practice fragment's, the core's feature table names
        exactly those entries, every function has anchors, and no plugin lists them in "surface"."""
        files = cr.surface_files(str(ROOT / "gamedata"))
        owner, _ = cr.ownership(files)
        spec = (ROOT / "core" / "src" / "readyup" / "practice_engine_spec.h").read_text()
        fns = {"CCSPlayerController_ChangeTeam", "CCSPlayerPawn_Teleport", "UTIL_CreateEntityByName",
               "CBaseEntity_DispatchSpawn"}
        slots = {"CCSPlayerController::ChangeTeam", "CCSPlayerPawn::Teleport"}
        for k in fns | slots:
            self.assertIn('"%s"' % k, spec, k)
        frag = json.loads((ROOT / "gamedata" / "engine-surface.practice.json").read_text())
        for k in fns:
            self.assertEqual(owner.get(("signature", k)), "practice", k)
            fn = frag["functions"][k]
            self.assertFalse(fn["required"], k)
            self.assertNotIn("hook", fn, k)
            self.assertGreaterEqual(len(fn["anchors"]), 2, k)
        for k in slots:
            vt = frag["vtable_indices"][k]
            self.assertIn(vt["function"], fns, k)
            self.assertIn(vt["class"], frag["rtti"], k)
        for m in ("engine_feature_available", "player_change_team", "player_teleport", "entity_create", "entity_spawn"):
            self.assertIn(m, cr.API_SURFACE)
            self.assertFalse(fns & set(cr.API_SURFACE[m]), m)
        for pid, need in cr.load_needs(str(ROOT / "plugins")).items():
            self.assertFalse((fns | slots) & set(need.get("surface") or []), pid)

    def test_respawn_is_core_and_never_a_hard_need(self):
        """ru_api 1.14 player_respawn: SetPawn and the Respawn slot are the core's, optional, named by
        the core's feature table, and no plugin lists them in "surface"."""
        files = cr.surface_files(str(ROOT / "gamedata"))
        owner, _ = cr.ownership(files)
        spec = (ROOT / "core" / "src" / "readyup" / "practice_engine_spec.h").read_text()
        core = json.loads((ROOT / "gamedata" / "engine-surface.json").read_text())
        entries = {"CBasePlayerController_SetPawn", "CCSPlayerController::Respawn"}
        for k in entries:
            self.assertIn('"%s"' % k, spec, k)
        self.assertEqual(owner.get(("signature", "CBasePlayerController_SetPawn")), "core")
        self.assertEqual(owner.get(("rtti", "CCSPlayerController")), "core")
        fn = core["functions"]["CBasePlayerController_SetPawn"]
        self.assertFalse(fn["required"])
        self.assertGreaterEqual(len(fn["anchors"]), 2)
        vt = core["vtable_indices"]["CCSPlayerController::Respawn"]
        self.assertIn(vt["class"], core["rtti"])
        self.assertGreaterEqual(len(vt["anchors"]), 2)
        self.assertIn("player_respawn", cr.API_SURFACE)
        self.assertFalse(entries & set(cr.API_SURFACE["player_respawn"]))
        for pid, need in cr.load_needs(str(ROOT / "plugins")).items():
            self.assertFalse(entries & set(need.get("surface") or []), pid)

    # ---- plugin needs (plugins/<id>/needs.json) ------------------------------------------------

    def test_plugin_static_verdicts_from_needs(self):
        doc, badge = self.result(plugins_dir=ROOT / "plugins")
        self.assertEqual(doc["overall"], "warn")
        for cid in cr.RUNTIME_ONLY + ["skins"]:
            c = self.comp(doc, cid)
            self.assertEqual(c["status"], "pass", cid)
            self.assertEqual(self.check(c, "selftest")["status"], "pending")
        match = self.comp(doc, "match")
        # match needs Host_Say (chat commands) + GameFrame (ticks) + the CCommand layout, among others
        self.assertEqual(self.check(match, "hook_site")["total"], 1)
        self.assertEqual(self.check(match, "vtable")["passed"], 2)
        self.assertEqual(self.check(match, "schema")["status"], "pending")
        self.assertEqual(self.check(match, "schema")["total"], 27)
        self.assertEqual(self.check(match, "event")["status"], "pending")
        self.assertEqual(self.check(match, "livetest")["status"], "pending")
        # whitelist needs no Host_Say (no chat commands)
        self.assertNotIn("hook_site", [k["kind"] for k in self.comp(doc, "whitelist")["checks"]])

    def test_needed_entry_failure_fails_the_plugins_that_need_it(self):
        hook = (FIX / "hookcheck-pass.txt").read_text().replace("OK   Host_Say", "FAIL Host_Say")
        d = self.raw()
        (d / "hookcheck.txt").write_text(hook)
        (d / "rc.env").write_text("SIGCHECK_RC=0\nHOOKCHECK_RC=1\n")
        out = self.tmp / "o"
        cr.main(["result", "--raw-dir", str(d), "--gamedata", str(self.gamedata), "--build-env",
                 str(FIX / "cs2-build.env"), "--out-dir", str(out), "--plugins-dir", str(ROOT / "plugins")] + RUN)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        status = {c["id"]: c["status"] for c in doc["components"]}
        for cid in ("core", "match", "practice", "fleet", "skins", "midas", "essentials"):  # midas / essentials: chat commands
            self.assertEqual(status[cid], "fail", cid)
        for cid in ("whitelist", "deathmatch"):
            self.assertEqual(status[cid], "pass", cid)
        self.assertTrue(self.check(self.comp(doc, "match"), "hook_site")["failures"][0].startswith("Host_Say:"))

    def test_fragment_entry_needed_by_another_plugin(self):
        # CBaseModelEntity_SetModel lives in the skins fragment; match needs it (entity_set_model)
        doc, _ = self.result(plugins_dir=ROOT / "plugins", sig="sigcheck-skins-fail.txt", sig_rc=1)
        status = {c["id"]: c["status"] for c in doc["components"]}
        self.assertEqual((status["core"], status["skins"], status["match"]), ("pass", "fail", "fail"))
        self.assertEqual(status["practice"], "pass")

    def test_repo_needs_are_in_sync_with_the_source(self):
        """The CI drift check: every plugin's needs.json covers its schema_offset / game event /
        engine-facing ru_api use, and names only real engine-surface entries."""
        self.assertEqual(cr.main(["needs-check"]), 0)

    def test_needs_check_catches_drift(self):
        plugins = self.tmp / "plugins"
        src = plugins / "match"
        (src / "tests").mkdir(parents=True)
        (src / "a.cpp").write_text(textwrap.dedent("""
            void Install(const ru_api* api) {
              api->schema_offset(api->self, "CFoo", "m_bar");
              int x = Pick({"CA", "CB"}, "m_both");
              static const char* const kEvents[] = {"round_start", "bomb_planted"};
              for (const char* e : kEvents) api->subscribe_game_event(api->self, e, &On, nullptr);
              for (const char* e : {"player_hurt"}) api->subscribe_game_event(api->self, e, &On, nullptr);
              api->entity_remove(api->self, ent);
              // api->chat_all(api->self, "commented out", 0);
            }
        """))
        (src / "tests" / "t.cpp").write_text('a->schema_offset(a->self, "CTest", "m_only_in_tests");')
        good = {"schema_version": 1, "plugin": "match", "api": ["entity_remove", "schema_offset", "subscribe_game_event"],
                "surface": ["UTIL_Remove", "CSchemaSystem", "CSchemaSystemTypeScope", "CGameEventManager_Init",
                            "CGameEventManager"],
                "schema": ["CFoo.m_bar", "CA.m_both|CB.m_both"], "schema_optional": [],
                "events": ["bomb_planted", "player_hurt", "round_start"]}
        (src / "needs.json").write_text(json.dumps(good))
        gd = ["--gamedata", str(ROOT / "gamedata"), "--plugins-dir", str(plugins)]
        self.assertEqual(cr.main(["needs-check"] + gd), 0)
        bad = dict(good, events=["round_start"], schema=["CA.m_both"], surface=good["surface"][1:] + ["NoSuchEntry"])
        (src / "needs.json").write_text(json.dumps(bad))
        with mock.patch("sys.stdout", new_callable=io.StringIO) as outp:
            self.assertEqual(cr.main(["needs-check"] + gd), 1)
        text = outp.getvalue()
        for want in ("game event bomb_planted", "game event player_hurt", "schema CFoo.m_bar", "schema CB.m_both",
                     "ru_api entity_remove needs engine-surface entry UTIL_Remove", "NoSuchEntry"):
            self.assertIn(want, text)
        self.assertNotIn("m_only_in_tests", text)
        self.assertNotIn("chat_all", text)

    # ---- dynamic stages ------------------------------------------------------------------------

    SELFTEST = textwrap.dedent("""\
        [engine surface]
          OK   fn Host_Say                                   rva=0x1c3f0c0
        [schema]
          OK   schema system                                 ok
        [plugins]
          OK   plugin host                                   api 1.6, dir /x/plugins, 3 loaded: essentials, fleet, match
          OK   match: database                               json store ok
          FAIL fleet: platform link                          handshake refused
          WARN skins: disabled                               missing CEconEntity.m_nFallbackPaintKit after CS2 build 25537370
        [needs]
          OK   need match schema CBaseEntity.m_iTeamNum      0x3cb
          OK   need match schema CBasePlayerController.m_steamID|CCSPlayerController.m_steamID 0x6d0
          WARN need match schema CCSPlayerController.m_iTeamNum|CBaseEntity.m_iTeamNum not found
          WARN need match schema_optional CCSPlayerController.m_iKills not found
          OK   need match event round_start
          WARN need match event round_mvp                     unknown to this CS2 build
          OK   need essentials surface UTIL_ClientPrintAll   resolved
        selftest: FAIL 5/6 (fleet: platform link)
        exit_code: 1
    """)

    def dynamic(self, stage, selftest=SELFTEST, live=(), advisory=()):
        base, _ = self.result(plugins_dir=ROOT / "plugins")
        bp = self.tmp / "base.json"
        bp.write_text(json.dumps(base))
        sp = self.tmp / "readyup_selftest.txt"
        sp.write_text(selftest)
        out = self.tmp / ("dyn-" + stage)
        args = ["dynamic", "--stage", stage, "--base", str(bp), "--selftest", str(sp), "--out-dir", str(out),
                "--gamedata", str(self.gamedata), "--plugins-dir", str(ROOT / "plugins")] + RUN
        for n, rc in live:
            args += ["--livetest", "%s=%d" % (n, rc)]
        for n in advisory:
            args += ["--livetest-advisory", n]
        self.assertEqual(cr.main(args), 0)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        self.assertEqual(doc["run"]["stage"], stage)
        self.assertEqual(doc["cs2"]["buildid"], "25537370")
        return doc

    def test_dynamic_selftest_stage(self):
        doc = self.dynamic("selftest")
        match = self.comp(doc, "match")
        schema = self.check(match, "schema")
        self.assertEqual((schema["status"], schema["passed"], schema["total"]), ("fail", 2, 4))
        self.assertIn("CCSPlayerController.m_iTeamNum|CBaseEntity.m_iTeamNum: not found", schema["failures"][0])
        self.assertEqual(self.check(match, "event")["status"], "warn")
        self.assertEqual(self.check(match, "selftest")["status"], "pass")
        self.assertEqual(self.check(match, "livetest")["status"], "pending")
        self.assertEqual(match["status"], "fail")
        self.assertEqual(self.comp(doc, "fleet")["status"], "fail")
        skins = self.check(self.comp(doc, "skins"), "selftest")
        self.assertEqual(skins["status"], "fail")
        self.assertIn("disabled: missing CEconEntity.m_nFallbackPaintKit", skins["failures"][0])
        self.assertIn("not loaded", self.check(self.comp(doc, "practice"), "selftest")["failures"][0])
        self.assertEqual(self.check(self.comp(doc, "essentials"), "selftest")["status"], "pass")
        core = self.check(self.comp(doc, "core"), "selftest")
        self.assertEqual((core["status"], core["passed"], core["total"]), ("pass", 2, 2))
        self.assertEqual(doc["overall"], "fail")

    def test_dynamic_live_all_green(self):
        ok = "\n".join(l for l in self.SELFTEST.splitlines() if "WARN" not in l and "FAIL" not in l)
        ok = ok.replace("3 loaded: essentials, fleet, match",
                        "8 loaded: deathmatch, essentials, fleet, match, midas, practice, skins, whitelist")
        # every need of every plugin resolved
        lines = []
        for pid, need in cr.load_needs(str(ROOT / "plugins")).items():
            for k in ("schema", "schema_optional"):
                lines += ["  OK   need %s %s %s 0x10" % (pid, k, e) for e in need.get(k) or []]
            lines += ["  OK   need %s event %s" % (pid, e) for e in need.get("events") or []]
        ok = ok.replace("[needs]\n", "[needs]\n" + "\n".join(lines) + "\n").replace("exit_code: 1", "exit_code: 0")
        doc = self.dynamic("live", selftest=ok, live=[("match", 0), ("scrim", 0), ("fleet", 0)])
        for c in doc["components"]:
            self.assertEqual(c["status"], "pass", c)
        self.assertEqual(self.check(self.comp(doc, "match"), "livetest")["passed"], 2)
        self.assertEqual(self.check(self.comp(doc, "fleet"), "livetest")["passed"], 1)
        # the static rtti of CSchemaSystem is runtime-only (SKIP) -> nothing pending remains
        self.assertEqual(doc["overall"], "pass")
        self.assertEqual(cr.badge(doc)["message"], "compatible")

    def test_dynamic_loaded_plugin_with_only_info_lines_passes(self):
        # a loaded plugin whose selftest lines are all INFO (whitelist "off", deathmatch "off") is
        # loaded and fine: pass, not pending forever
        st = self.SELFTEST.replace("3 loaded: essentials, fleet, match", "4 loaded: essentials, fleet, match, whitelist")
        st = st.replace("[needs]\n", "  INFO whitelist: whitelist                         off, 0 player(s)\n[needs]\n")
        doc = self.dynamic("selftest", selftest=st)
        wl = self.check(self.comp(doc, "whitelist"), "selftest")
        self.assertEqual((wl["status"], wl["passed"], wl["total"]), ("pass", 1, 1))
        # a plugin that is not loaded still fails, INFO lines or not
        st = st.replace("4 loaded: essentials, fleet, match, whitelist", "3 loaded: essentials, fleet, match")
        wl = self.check(self.comp(self.dynamic("selftest", selftest=st), "whitelist"), "selftest")
        self.assertEqual(wl["status"], "fail")

    def test_dynamic_livetest_fail_and_no_verdict(self):
        doc = self.dynamic("live", live=[("match", 1), ("scrim", 2)])
        lt = self.check(self.comp(doc, "match"), "livetest")
        self.assertEqual((lt["status"], lt["passed"], lt["total"]), ("fail", 0, 1))
        doc = self.dynamic("live", live=[("match", 2)])
        self.assertEqual(self.check(self.comp(doc, "match"), "livetest")["status"], "pending")

    def test_dynamic_livetest_per_component_and_advisory(self):
        # fleet reports to the fleet component, forfeit / valve to match
        doc = self.dynamic("live", live=[("match", 0), ("scrim", 0), ("fleet", 1), ("forfeit", 0)])
        self.assertEqual(self.check(self.comp(doc, "match"), "livetest")["passed"], 3)
        fl = self.check(self.comp(doc, "fleet"), "livetest")
        self.assertEqual((fl["status"], fl["passed"], fl["total"]), ("fail", 0, 1))
        # an advisory live test that fails keeps its check pending (never fail), with the reason
        doc = self.dynamic("live", live=[("match", 0), ("fleet", 1), ("forfeit", 1), ("valve", 0)],
                           advisory=["fleet forfeit,valve"])
        fl = self.check(self.comp(doc, "fleet"), "livetest")
        self.assertEqual(fl["status"], "pending")
        self.assertIn("fleet: failed (exit 1); advisory", fl["failures"][0])
        lt = self.check(self.comp(doc, "match"), "livetest")
        self.assertEqual((lt["status"], lt["passed"]), ("pending", 2))
        self.assertIn("forfeit: failed (exit 1); advisory", lt["failures"][0])
        # advisory and passing: a normal pass
        doc = self.dynamic("live", live=[("match", 0), ("fleet", 0)], advisory=["fleet"])
        self.assertEqual(self.check(self.comp(doc, "fleet"), "livetest")["status"], "pass")
        # a required one still fails next to an advisory one
        doc = self.dynamic("live", live=[("match", 1), ("forfeit", 1)], advisory=["forfeit"])
        self.assertEqual(self.check(self.comp(doc, "match"), "livetest")["status"], "fail")
        # no fleet result (live test not run for it): its check stays pending
        doc = self.dynamic("live", live=[("match", 0)])
        self.assertEqual(self.check(self.comp(doc, "fleet"), "livetest")["status"], "pending")

    def test_dynamic_without_report_fails_core(self):
        doc = self.dynamic("selftest", selftest="")
        core = self.check(self.comp(doc, "core"), "selftest")
        self.assertEqual(core["status"], "fail")
        self.assertEqual(self.check(self.comp(doc, "match"), "selftest")["status"], "pending")
        self.assertEqual(doc["overall"], "fail")


class _Handler(http.server.BaseHTTPRequestHandler):
    received = []

    def do_POST(self):  # noqa: N802
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        _Handler.received.append((self.headers.get("Authorization"), json.loads(body)))
        self.send_response(202)
        self.end_headers()

    def log_message(self, *a):
        pass


class PostTest(unittest.TestCase):
    def setUp(self):
        fd, self.file = tempfile.mkstemp(suffix=".json")
        os.write(fd, b'{"schema": 1}')
        os.close(fd)
        self.addCleanup(os.unlink, self.file)

    def test_skipped_without_url(self):
        with mock.patch.dict(os.environ, {"COMPAT_INGEST_URL": ""}), \
                mock.patch("urllib.request.urlopen") as op:
            self.assertEqual(cr.main(["post", self.file]), 0)
            op.assert_not_called()

    def test_posts_with_bearer(self):
        _Handler.received.clear()
        srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        t = threading.Thread(target=srv.handle_request, daemon=True)
        t.start()
        env = {"COMPAT_INGEST_URL": "http://127.0.0.1:%d/ingest" % srv.server_port,
               "COMPAT_INGEST_TOKEN": "t0k", "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
        with mock.patch.dict(os.environ, env):
            self.assertEqual(cr.main(["post", self.file]), 0)
        t.join(5)
        srv.server_close()
        self.assertEqual(_Handler.received, [("Bearer t0k", {"schema": 1})])

    def test_failure_is_ignored(self):
        srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        port = srv.server_port
        srv.server_close()  # nothing listens there any more
        env = {"COMPAT_INGEST_URL": "http://127.0.0.1:%d/" % port, "NO_PROXY": "127.0.0.1",
               "no_proxy": "127.0.0.1"}
        with mock.patch.dict(os.environ, env):
            self.assertEqual(cr.main(["post", self.file, "--timeout", "2"]), 0)


def assert_steps_contract(tc, doc):
    """run.steps as the site validates it (website: lib/compat/document.ts)."""
    for s in doc["run"]["steps"]:
        tc.assertTrue(set(s) <= {"id", "name", "stage", "status", "started_at", "finished_at", "detail", "parent"}, s)
        tc.assertRegex(s["id"], r"^[A-Za-z0-9._:-]{1,64}$")
        tc.assertIn(s["status"], ("queued", "running", "pass", "fail", "skip"))
        tc.assertIn(s["stage"], ("setup", "static", "selftest", "live", "record"))
        tc.assertTrue(0 < len(s["name"]) <= 96)
        for k in ("started_at", "finished_at"):
            if k in s:
                tc.assertRegex(s[k], ISO)
    ids = [s["id"] for s in doc["run"]["steps"]]
    tc.assertEqual(len(ids), len(set(ids)))


class StepTest(unittest.TestCase):
    """compat-report.py step: the payload of a progress event with run.steps."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.state = os.path.join(self.tmp, "steps.json")
        self.base = os.path.join(self.tmp, "base.json")
        self.out = os.path.join(self.tmp, "out.json")
        self.env = mock.patch.dict(os.environ, {"COMPAT_INGEST_URL": "", "COMPAT_STEP_STATE": "", "COMPAT_STEP_BASE": "",
                                                "COMPAT_STEP_BUILD_ENV": "", "GITHUB_STEP_SUMMARY": ""})
        self.env.start()
        self.addCleanup(self.env.stop)

    def write_base(self, run_id="123", state="warn", overall="warn", stage="selftest"):
        doc = {"schema": 1, "cs2": {"buildid": "25537370", "patch": "1.41.8.5"},
               "readyup": {"version": "0.1.0", "commit": "7f61b71"},
               "run": {"id": run_id, "url": "https://example.invalid/run/" + run_id, "trigger": "build_change",
                       "stage": stage, "state": state, "started_at": "2026-09-25T22:00:00Z",
                       "finished_at": "2026-09-25T22:30:00Z"},
               "overall": overall,
               "components": [{"id": "core", "name": "Core", "status": "pass", "checks": []}],
               "checked_at": "2026-09-25T22:30:00Z"}
        cr.write_json(self.base, doc)

    def step(self, *argv):
        rc = cr.main(["step"] + RUN + ["--gamedata", str(FIX), "--state", self.state, "--base", self.base,
                                       "--out", self.out] + list(argv))
        with open(self.out, encoding="utf-8") as f:
            return rc, json.load(f)

    def test_plan_then_steps_while_running(self):
        self.write_base(run_id="static-9", stage="static")  # the static verdict of another run
        rc, doc = self.step("--plan", "dynamic", "--id", "build", "--status", "running")
        self.assertEqual(rc, 0)
        run = dict(doc["run"])
        steps = run.pop("steps")
        doc_no_steps = dict(doc, run=run)
        assert_contract(self, doc_no_steps)
        assert_steps_contract(self, doc)
        self.assertEqual([s["id"] for s in steps],
                         ["build", "update", "install", "selftest", "live-match", "live-scrim", "live-fleet",
                          "live-forfeit", "record"])
        self.assertEqual([s["name"] for s in steps][:4], ["Build bundle", "Update CS2", "Install bundle", "Boot + selftest"])
        self.assertEqual(steps[0]["status"], "running")
        self.assertRegex(steps[0]["started_at"], ISO)
        self.assertTrue(all(s["status"] == "queued" and "started_at" not in s for s in steps[1:]))
        # this run, not the static one; still checking, with the static components and build
        self.assertEqual((run["id"], run["state"], run["finished_at"], run["stage"]), ("123", "checking", None, "selftest"))
        self.assertEqual(doc["overall"], "checking")
        self.assertEqual(doc["cs2"]["buildid"], "25537370")
        self.assertEqual(doc["components"][0]["id"], "core")

        _, doc = self.step("--id", "build", "--status", "pass")
        build = doc["run"]["steps"][0]
        self.assertEqual(build["status"], "pass")
        self.assertIn("started_at", build)
        self.assertIn("finished_at", build)
        self.assertEqual(len(doc["run"]["steps"]), len(cr.DYNAMIC_PLAN))  # kept in --state
        self.assertEqual(doc["run"]["trigger"], "build_change")

    def test_stage_verdict_does_not_end_the_run_until_every_step_is_done(self):
        self.write_base()  # this run's selftest verdict: warn
        self.step("--plan", "dynamic")
        for sid in ("build", "update", "install", "selftest"):
            _, doc = self.step("--id", sid, "--status", "pass")
        self.assertEqual((doc["run"]["state"], doc["overall"]), ("checking", "checking"))
        _, doc = self.step("--id", "live-match", "--status", "fail", "--detail", "livetest exit 1\nsee log")
        self.assertEqual(doc["run"]["steps"][4]["detail"], "livetest exit 1 see log")
        for sid in ("live-scrim", "live-fleet", "live-forfeit"):
            self.step("--id", sid, "--status", "skip")
        _, doc = self.step("--id", "record", "--status", "pass")
        # every step done: the base's verdict, finished
        self.assertEqual((doc["run"]["state"], doc["overall"], doc["run"]["finished_at"]),
                         ("warn", "warn", "2026-09-25T22:30:00Z"))

    def test_nested_steps_go_after_their_parent(self):
        self.write_base()
        self.step("--plan", "dynamic")
        self.step("--id", "live-match.01", "--name", "warmup", "--stage", "live", "--parent", "live-match",
                  "--status", "running")
        _, doc = self.step("--updates", json.dumps([
            {"id": "live-match.01", "status": "pass"},
            {"id": "live-match.02", "name": "knife", "stage": "live", "parent": "live-match", "status": "running"}]))
        ids = [s["id"] for s in doc["run"]["steps"]]
        self.assertEqual(ids[4:8], ["live-match", "live-match.01", "live-match.02", "live-scrim"])
        self.assertEqual(doc["run"]["steps"][6]["parent"], "live-match")
        assert_steps_contract(self, doc)

    def test_close_fails_running_and_skips_queued(self):
        self.write_base(state="checking", overall="checking")
        self.step("--plan", "dynamic", "--id", "build", "--status", "pass")
        self.step("--id", "update", "--status", "running")
        _, doc = self.step("--close")
        st = {s["id"]: s for s in doc["run"]["steps"]}
        self.assertEqual((st["update"]["status"], st["update"]["detail"]), ("fail", "did not finish"))
        self.assertEqual(st["record"]["status"], "skip")
        # no verdict was reached (the base is still checking)
        self.assertEqual((doc["run"]["state"], doc["overall"]), ("no_verdict", "no_verdict"))
        self.assertIsNotNone(doc["run"]["finished_at"])

    def test_bad_calls_warn_and_long_text_is_cut(self):
        self.write_base()
        self.assertEqual(cr.main(["step", "--state", self.state, "--id", "bad id", "--status", "pass"]), 2)
        _, doc = self.step("--id", "x", "--name", "n" * 200, "--status", "fail", "--detail", "d" * 900)
        self.assertEqual(len(doc["run"]["steps"][0]["name"]), 96)
        self.assertEqual(len(doc["run"]["steps"][0]["detail"]), 500)

    def test_posts_and_throttles(self):
        self.write_base(state="checking", overall="checking")
        with mock.patch.dict(os.environ, {"COMPAT_INGEST_URL": "http://127.0.0.1:9/"}), \
                mock.patch.object(cr, "post_body", return_value=True) as post:
            self.step("--plan", "dynamic", "--id", "build", "--status", "running")
            self.assertEqual(post.call_count, 1)
            body = json.loads(post.call_args[0][0])
            self.assertEqual(body["run"]["steps"][0]["status"], "running")
            # within --min-interval: kept for later, not posted
            self.step("--id", "build", "--status", "pass", "--min-interval", "60")
            self.assertEqual(post.call_count, 1)
            self.step("--flush")
            self.assertEqual(post.call_count, 2)
            self.assertEqual(json.loads(post.call_args[0][0])["run"]["steps"][0]["status"], "pass")
            self.step("--flush")  # nothing left to send
            self.assertEqual(post.call_count, 2)
            self.step("--id", "update", "--status", "running", "--no-post")
            self.assertEqual(post.call_count, 2)

    def test_post_failure_never_fails_the_step(self):
        self.write_base()
        srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        port = srv.server_port
        srv.server_close()
        env = {"COMPAT_INGEST_URL": "http://127.0.0.1:%d/" % port, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
        with mock.patch.dict(os.environ, env):
            rc, _ = self.step("--id", "build", "--status", "pass", "--timeout", "2")
        self.assertEqual(rc, 0)

    def test_summary_table(self):
        self.write_base()
        summ = os.path.join(self.tmp, "summary.md")
        with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": summ}):
            self.step("--plan", "dynamic", "--id", "build", "--status", "fail", "--detail", "a|b", "--summary")
        with open(summ, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("| Build bundle | **FAIL** |", text)
        self.assertIn("a/b", text)
        self.assertIn("| Record | queued |", text)

    def test_annotate_selftest(self):
        path = os.path.join(self.tmp, "st.txt")
        with open(path, "w") as f:
            f.write("[core]\n  OK   hooks\n  FAIL schema  100% missing\n[plugins]\n  WARN match: event  late\n")
        buf = io.StringIO()
        with mock.patch("sys.stdout", buf):
            self.assertEqual(cr.main(["annotate-selftest", path]), 0)
        lines = buf.getvalue().splitlines()
        self.assertEqual(lines[0], "::error title=selftest core::schema  100%25 missing")
        self.assertEqual(lines[1], "::warning title=selftest plugins::match: event  late")


if __name__ == "__main__":
    unittest.main()
