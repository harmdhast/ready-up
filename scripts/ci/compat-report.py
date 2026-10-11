#!/usr/bin/env python3
"""CS2 compatibility report for .github/workflows/cs2-update-watch.yml (python3 stdlib only).

Turns the raw output of readyup_sigcheck / readyup_hookcheck (scripts/ci/verify-cs2.sh with
VERIFY_RAW_DIR set) into per-component results and writes compat.json + badge.json. The JSON
contract is documented in docs/CS2-COMPAT.md; the Auto Tournament platform builds against it,
so change it only together with `schema`.

  compat-report.py result --raw-dir DIR --gamedata DIR --build-env FILE --out-dir DIR [run options]
      static verdict -> DIR/compat.json + DIR/badge.json
  compat-report.py phase {queued,checking,no_verdict} --out FILE [--build-env FILE] [run options]
      progress event (same shape, no checks yet) -> FILE
  compat-report.py post FILE
      POST FILE to $COMPAT_INGEST_URL with "Authorization: Bearer $COMPAT_INGEST_TOKEN".
      Silently skipped when the URL is unset; never exits non-zero (failures are logged).
  compat-report.py dynamic --stage {selftest,live} --base FILE --selftest FILE [--livetest NAME=RC ...]
                   [--livetest-advisory NAMES] --out-dir DIR [run options]
      dynamic verdict (.github/workflows/cs2-dynamic.yml): the static compat.json plus the
      readyup_selftest.txt of a real server (plugin needs, plugin selftest lines) and, for
      stage live, the bot live test exit codes -> DIR/compat.json + DIR/badge.json. Each live
      test goes to its component's `livetest` check (LIVETEST_COMPONENT: fleet -> fleet, the
      rest -> match); an --livetest-advisory one that fails stays pending instead of failing.
  compat-report.py step [--plan dynamic] [--id ID --status STATUS [--name N] [--stage S] [--parent P]
                   [--detail D]] [--updates JSON] [--close] [--flush] [--summary] [--no-post]
                   [--min-interval SEC] [--state FILE] [--base FILE] [run options]
      live progress of a run (run.steps, docs/CS2-COMPAT.md "Run steps"): updates the steps in the
      --state file ($COMPAT_STEP_STATE) and POSTs the run like `post`, never failing the caller.
      The document is --base ($COMPAT_STEP_BASE, the last compat.json of this run) with state
      `checking` while a step is open. See cmd_step.
  compat-report.py annotate-selftest FILE
      print a GitHub ::error:: (FAIL) / ::warning:: (WARN) annotation per readyup_selftest.txt line
  compat-report.py needs-check [--plugins-dir DIR] [--gamedata DIR]
      every plugin's needs.json covers what its source calls (schema_offset literals,
      subscribe_game_event names, engine-facing ru_api members); exit 1 on drift
  compat-report.py needs-derive NAME
      print the needs.json the source of plugins/NAME implies (a starting point for a new plugin)

Run options (defaults from the GitHub Actions environment):
  --run-id, --run-url, --trigger, --started-at, --commit, --version-file, --buildid
"""
import argparse
import datetime
import glob
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

SCHEMA = 1

# Components with an engine-surface file. The base file is the core; every
# engine-surface.<id>.json fragment is its plugin's own component.
CORE_ID = "core"
# Plugins whose static verdict comes from their plugins/<id>/needs.json (the engine-surface entries
# its ru_api calls need), in display order. practice also owns a fragment
# (engine-surface.practice.json), whose entries count for it too.
RUNTIME_ONLY = ["match", "practice", "essentials", "midas", "whitelist", "deathmatch", "fleet"]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Plugins whose manifest is checked (needs-check) and used for verdicts. hello is the example.
NEEDS_PLUGINS = ["match", "practice", "essentials", "midas", "whitelist", "deathmatch", "fleet", "skins", "addons"]
NAMES = {"core": "Core", "skins": "Skins", "match": "Match", "practice": "Practice",
         "essentials": "Essentials", "midas": "Midas", "whitelist": "Whitelist", "deathmatch": "Deathmatch",
         "fleet": "Fleet", "addons": "Addons"}
# The component whose `livetest` check a live test (compat-report.py dynamic --livetest NAME=RC)
# reports to; every other name goes to LIVETEST_DEFAULT. A component listed here gets a pending
# `livetest` check from the static stage on, like match.
LIVETEST_DEFAULT = "match"
LIVETEST_COMPONENT = {"fleet": "fleet"}
# Order of static check kinds inside a component.
STATIC_KINDS = ["signature", "rtti", "vtable", "hook_site", "layout"]
TRIGGERS = ("build_change", "surface_change", "code_change", "nightly", "release", "manual")
BADGE = {
    "pass": ("compatible", "brightgreen"),
    "warn": ("static ok", "yellow"),
    "fail": ("incompatible", "red"),
    "checking": ("checking", "blue"),
}
MAX_FAILURE_LEN = 240


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def read_text(path):
    if not path or not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def read_env_file(path):
    out = {}
    for line in read_text(path).splitlines():
        line = line.strip()
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def component_name(cid):
    return NAMES.get(cid, cid.replace("-", " ").replace("_", " ").title())


# ---------------------------------------------------------------------------------------------
# Surface ownership: which component owns which entry
# ---------------------------------------------------------------------------------------------

def surface_files(gamedata_dir):
    """[(component id, path)] base first, then fragments sorted by name."""
    base = os.path.join(gamedata_dir, "engine-surface.json")
    files = [(CORE_ID, base)] if os.path.isfile(base) else []
    for path in sorted(glob.glob(os.path.join(gamedata_dir, "engine-surface.*.json"))):
        cid = os.path.basename(path)[len("engine-surface."):-len(".json")]
        if cid:
            files.append((cid, path))
    return files


def ownership(files):
    """{(kind, name): component id}, plus {function name: required?}."""
    owner, hooked = {}, set()
    for cid, path in files:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        for name, fn in (d.get("functions") or {}).items():
            owner[("signature", name)] = cid
            if isinstance(fn, dict) and fn.get("hook") == "funchook":
                owner[("hook_site", name)] = cid
                hooked.add(name)
        for name in (d.get("rtti") or {}):
            owner.setdefault(("rtti", name), cid)  # a fragment repeating the base's entry shares it
        for name in (d.get("vtable_indices") or {}):
            owner[("vtable", name)] = cid
        for name in (d.get("layouts") or {}):
            owner[("layout", name)] = cid
    return owner, hooked


# ---------------------------------------------------------------------------------------------
# Checker output parsing (formats: tools/sigcheck.cpp, tools/hookcheck.cpp)
# ---------------------------------------------------------------------------------------------

SIG_FUNC = re.compile(r"^(OK|FAIL)\s+(\S+)\s+(required|optional)(?:\s+\[\S+\])?\s+matches=(-?\d+)\s+rva=(\S+)\s*(.*)$")
SIG_OTHER = re.compile(r"^(OK|FAIL|SKIP)\s+(rtti|vtable|layout)\s+(\S+)\s*(.*)$")
HOOK_LINE = re.compile(r"^(OK|FAIL)\s+(\S+)\s+(.*)$")


def parse_sigcheck(text):
    """[(kind, name, ok, detail)] ; SKIP lines (runtime-only modules) are left out."""
    rows = []
    for line in text.splitlines():
        line = line.rstrip()
        m = SIG_OTHER.match(line)
        if m:
            if m.group(1) != "SKIP":
                rows.append((m.group(2), m.group(3), m.group(1) == "OK", m.group(4).strip()))
            continue
        m = SIG_FUNC.match(line)
        if m:
            detail = "%s matches=%s %s" % (m.group(3), m.group(4), m.group(6).strip())
            rows.append(("signature", m.group(2), m.group(1) == "OK", detail.strip()))
    return rows


def parse_hookcheck(text):
    rows = []
    for line in text.splitlines():
        m = HOOK_LINE.match(line.rstrip())
        if m:
            rows.append(("hook_site", m.group(2), m.group(1) == "OK", m.group(3).strip()))
    return rows


def short(s):
    s = " ".join(s.split())
    return s if len(s) <= MAX_FAILURE_LEN else s[:MAX_FAILURE_LEN - 3] + "..."


# ---------------------------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------------------------

def pending_check(kind="selftest"):
    return {"kind": kind, "status": "pending", "passed": 0, "total": 0, "failures": []}


def component_status(checks):
    """Worst of the finished checks; pending only if nothing has run yet."""
    done = [c["status"] for c in checks if c["status"] != "pending"]
    if not done:
        return "pending"
    for s in ("fail", "warn"):
        if s in done:
            return s
    return "pass"


def overall_status(components):
    statuses = [c["status"] for c in components]
    if "fail" in statuses:
        return "fail"
    if all(c["status"] == "pass" and all(k["status"] == "pass" for k in c["checks"]) for c in components):
        return "pass"
    # Static checks passed, dynamic (selftest/livetest) stages still pending.
    return "warn"


def build_components(sig_text, hook_text, sig_rc, hook_rc, files, needs=None):
    """needs: {plugin id: needs.json} (load_needs). A plugin's static checks are the rows of the
    engine-surface entries it needs (also counted for the entry's owner, usually core)."""
    needs = needs or {}
    owner, _ = ownership(files)
    rows = parse_sigcheck(sig_text) + parse_hookcheck(hook_text)
    static_ids = [cid for cid, _ in files] or [CORE_ID]
    plugin_ids = [c for c in RUNTIME_ONLY if c not in static_ids]
    per = {cid: {k: {"passed": 0, "total": 0, "failures": []} for k in STATIC_KINDS}
           for cid in static_ids + plugin_ids}
    wanted = {cid: set(n.get("surface") or []) for cid, n in needs.items() if cid in per}

    for kind, name, ok, detail in rows:
        cid = owner.get((kind, name), CORE_ID)
        if cid not in per:
            cid = CORE_ID
        targets = [cid] + [p for p, names in wanted.items() if name in names and p != cid]
        for t in targets:
            bucket = per[t][kind]
            bucket["total"] += 1
            if ok:
                bucket["passed"] += 1
            else:
                bucket["failures"].append(short("%s: %s" % (name, detail)))

    # A checker that failed without a parsable FAIL line (parse error, crash, no hook sites):
    # never let that read as a pass.
    for tool, rc, kind in (("readyup_sigcheck", sig_rc, "signature"), ("readyup_hookcheck", hook_rc, "hook_site")):
        tool_kinds = ["hook_site"] if kind == "hook_site" else ["signature", "rtti", "vtable", "layout"]
        any_fail = any(per[c][k]["failures"] for c in per for k in tool_kinds)
        if rc != 0 and not any_fail:
            bucket = per[CORE_ID if CORE_ID in per else static_ids[0]][kind]
            bucket["total"] += 1
            bucket["failures"].append("%s exited %d without a FAIL line; see the run log" % (tool, rc))

    components = []
    for cid in static_ids + plugin_ids:
        checks = []
        for kind in STATIC_KINDS:
            b = per[cid][kind]
            if b["total"] == 0:
                continue
            checks.append({"kind": kind, "status": "fail" if b["failures"] else "pass",
                           "passed": b["passed"], "total": b["total"], "failures": b["failures"]})
        checks.extend(dynamic_pending_checks(cid, needs.get(cid)))
        components.append({"id": cid, "name": component_name(cid), "status": component_status(checks),
                           "checks": checks})
    return components


def dynamic_pending_checks(cid, need):
    """What only a running server can confirm: schema fields and game events (from needs.json),
    the selftest, and for match the bot live test."""
    checks = []
    if need:
        n_schema = len(need.get("schema") or []) + len(need.get("schema_optional") or [])
        if n_schema:
            checks.append(dict(pending_check("schema"), total=n_schema))
        if need.get("events"):
            checks.append(dict(pending_check("event"), total=len(need["events"])))
    checks.append(pending_check("selftest"))
    if cid in livetest_components():
        checks.append(pending_check("livetest"))
    return checks


def livetest_components():
    return [LIVETEST_DEFAULT] + [c for c in LIVETEST_COMPONENT.values() if c != LIVETEST_DEFAULT]


def all_component_ids(files):
    ids = [cid for cid, _ in files] or [CORE_ID]
    return ids + [c for c in RUNTIME_ONLY if c not in ids]


# ---------------------------------------------------------------------------------------------
# Plugin needs (plugins/<id>/needs.json, docs/CS2-COMPAT.md "Plugin needs")
# ---------------------------------------------------------------------------------------------

# Engine-facing ru_api members -> the engine-surface entries (gamedata/engine-surface*.json:
# function, hook, rtti, vtable or layout name) the core needs to serve them
# (core/src/readyup/plugin_engine_api.cpp, plugin_loader.cpp). Members not listed here touch no
# engine surface (config, stash, interfaces, logging, cvar_query, selftest_summary, ...).
_CONSOLE = ["ISource2GameClients::ClientCommand", "CCommand"]
_FRAME = ["ISource2Server::GameFrame"]
_HTML = ["LegacyGameEventListener", "CServerSideClient_GameEventLegacyProxy"]
_ENTSYS = ["UTIL_Remove"]  # g_pGameEntitySystem is found through UTIL_Remove
API_SURFACE = {
    "chat_all": ["UTIL_ClientPrintAll"],
    "chat_to_slot": ["ClientPrint"],
    "register_chat_command": ["Host_Say"],
    "register_chat_command_ex": ["Host_Say"],
    "set_chat_name_prefix": ["Host_Say"],
    "register_console_command": _CONSOLE,
    "register_console_command_ex": _CONSOLE,
    "register_ru_subcommand": _CONSOLE,
    "on_tick": _FRAME,
    "on_frame": _FRAME,
    "post_to_game_thread": _FRAME,
    "subscribe_game_event": ["CGameEventManager_Init", "CGameEventManager"],
    "center_html_to_slot": _HTML,
    "center_html_all": _HTML,
    "center_html_to_slot_prio": _HTML,
    "center_html_all_prio": _HTML,
    "schema_offset": ["CSchemaSystem", "CSchemaSystemTypeScope"],
    "entity_system_status": _ENTSYS,
    "entity_by_index": _ENTSYS,
    "entity_from_handle": _ENTSYS,
    "entity_handle_of": _ENTSYS,
    "entity_classname": _ENTSYS,
    "entity_remove": _ENTSYS,
    "entity_mark_changed": ["CBaseEntity_NetworkStateChanged", "CEntityInstance::NetworkStateChanged"],
    "econ_attr_set_by_name": ["CAttributeList_SetOrAddAttributeValueByName"],
    "entity_change_subclass": ["CBaseEntity_ChangeSubclass"],
    "entity_set_model": ["CBaseModelEntity_SetModel"],
    "entity_set_bodygroup_by_name": ["CBaseModelEntity_GetModel", "CModel_FindBodygroupByName",
                                     "CBaseModelEntity_SetBodygroup"],
    "entity_set_abs_origin": ["CBaseEntity_SetAbsOrigin"],
    "player_give_item": _ENTSYS + ["CCSPlayer_ItemServices_GiveNamedItem"],
    # 1.8: the entries are the plugin's own (its engine-surface.<plugin>.json), listed in "surface".
    "surface_function": [],
    "hook_function": [],
    "hook_vtable": [],
    # 1.12: the grenade Create functions live in the practice fragment (engine-surface.practice.json),
    # so the practice component's static check covers them. They are deliberately nobody's hard
    # need: a break turns grenade spawning off (grenade_spawn_available = 0), never a whole plugin.
    "grenade_spawn": [],
    "grenade_spawn_available": [],
    # 1.13: ChangeTeam / CCSPlayerPawn::Teleport / CreateEntityByName / DispatchSpawn live in the
    # practice fragment too (same rule: a break turns that feature off, engine_feature_available = 0).
    # The core finds the controller / pawn / entity through the entity system.
    "engine_feature_available": [],
    "player_change_team": _ENTSYS,
    "player_teleport": _ENTSYS,
    "entity_create": _ENTSYS,
    "entity_spawn": _ENTSYS,
    # 1.14: SetPawn / CCSPlayerController::Respawn are core entries, optional for the same reason
    # (a break turns respawn off, engine_feature_available("respawn") = 0).
    "player_respawn": _ENTSYS,
    "set_round_termination_suppressed": ["CCSGameRules_TerminateRound"],
}
NEEDS_KEYS = ("schema_version", "plugin", "api", "surface", "schema", "schema_optional", "events")
SRC_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")


def load_needs(plugins_dir):
    """{plugin id: needs.json dict} for every plugins/<id>/needs.json (missing dir: {})."""
    out = {}
    for path in sorted(glob.glob(os.path.join(plugins_dir or "", "*", "needs.json"))):
        with open(path, encoding="utf-8") as f:
            out[os.path.basename(os.path.dirname(path))] = json.load(f)
    return out


def plugin_sources(plugin_dir):
    """Concatenated source of a plugin (tests excluded), comments stripped."""
    texts = []
    for dirpath, dirnames, filenames in os.walk(plugin_dir):
        dirnames[:] = sorted(d for d in dirnames if d != "tests")
        for fn in sorted(filenames):
            if fn.endswith(SRC_EXT):
                texts.append(read_text(os.path.join(dirpath, fn)))
    src = "\n".join(texts)
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


_LIT = r'"([A-Za-z_][A-Za-z0-9_]*)"'
_LIST_OF_LITS = r'\{((?:\s*"[A-Za-z0-9_]+"\s*,?)+)\}'


def derive_needs(src):
    """What the source implies: {"api": set, "schema": {(cls, field)}, "fields": set, "events": set}."""
    api = {m for m in re.findall(r"->\s*([a-z_0-9]+)\s*\(", src) if m in API_SURFACE}
    pairs = set(re.findall(r'"([A-Z][A-Za-z0-9_]*)"\s*,\s*"(m_[A-Za-z0-9_]+)"', src))
    # Wrappers that try several classes: F({"A", "B"}, "m_x") and F("m_x", {"A", "B"}).
    for lits, field in re.findall(_LIST_OF_LITS + r"\s*,\s*\"(m_[A-Za-z0-9_]+)\"", src):
        pairs |= {(c, field) for c in re.findall(_LIT, lits)}
    for field, lits in re.findall(r"\"(m_[A-Za-z0-9_]+)\"\s*,\s*" + _LIST_OF_LITS, src):
        pairs |= {(c, field) for c in re.findall(_LIT, lits)}
    fields = set(re.findall(r"\"(m_[A-Za-z0-9_]+)\"", src))
    # Event names: a literal argument, or the loop variable over an inline list / named array.
    arrays = {n: re.findall(r'"([a-z0-9_]+)"', body)
              for n, body in re.findall(r"const\s+char\s*\*\s*(?:const\s+)?(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}", src)}
    loops = [(m.start(), m.group(1), m.group(2), m.group(3)) for m in re.finditer(
        r"for\s*\(\s*const\s+char\s*\*\s*(?:const\s+)?(\w+)\s*:\s*(?:\{([^}]*)\}|(\w+))\s*\)", src)]
    events = set()
    for m in re.finditer(r"subscribe_game_event\s*\(\s*[^,]+,\s*(?:\"([^\"]+)\"|(\w+))", src):
        if m.group(1):
            events.add(m.group(1))
            continue
        var = m.group(2)
        loop = [lp for lp in loops if lp[0] < m.start() and lp[1] == var]
        if loop:
            _, _, inline, arr = loop[-1]
            events |= set(re.findall(r'"([a-z0-9_]+)"', inline)) if inline else set(arrays.get(arr, []))
        else:
            events.add("<non-literal:%s>" % var)
    return {"api": api, "schema": pairs, "fields": fields, "events": events}


def schema_alternatives(entry):
    """"A.m_x|B.m_x" -> [("A", "m_x"), ("B", "m_x")]."""
    out = []
    for alt in entry.split("|"):
        cls, _, field = alt.strip().partition(".")
        out.append((cls, field))
    return out


def surface_names(gamedata_dir):
    names = set()
    for _, path in surface_files(gamedata_dir):
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        for key in ("functions", "rtti", "vtable_indices", "layouts"):
            names |= set(d.get(key) or {})
    return names


def check_needs(pid, need, derived, known_surface):
    """Problems (strings) with one plugin's manifest; empty = in sync with its source."""
    errs = []
    extra = set(need) - set(NEEDS_KEYS)
    if extra:
        errs.append("unknown keys: %s" % ", ".join(sorted(extra)))
    if need.get("plugin") != pid:
        errs.append('"plugin" is %r, expected %r' % (need.get("plugin"), pid))
    api = set(need.get("api") or [])
    for m in sorted(derived["api"] - api):
        errs.append('source calls ru_api %s: add it to "api"' % m)
    for m in sorted(api - set(API_SURFACE)):
        errs.append('"api" lists %s, which touches no engine surface (not in API_SURFACE)' % m)
    surface = set(need.get("surface") or [])
    for m in sorted(api & set(API_SURFACE)):
        for s in API_SURFACE[m]:
            if s not in surface:
                errs.append('ru_api %s needs engine-surface entry %s: add it to "surface"' % (m, s))
    for s in sorted(surface - known_surface):
        errs.append('"surface" lists %s, which is in no gamedata/engine-surface*.json' % s)
    declared = set()
    for e in (need.get("schema") or []) + (need.get("schema_optional") or []):
        alts = schema_alternatives(e)
        if any(not c or not f.startswith("m_") for c, f in alts):
            errs.append('bad schema entry %r (want "Class.m_field" or "A.m_x|B.m_x")' % e)
        declared |= set(alts)
    for c, f in sorted(derived["schema"] - declared):
        errs.append('source reads schema %s.%s: add it to "schema" (or "schema_optional")' % (c, f))
    declared_fields = {f for _, f in declared}
    for f in sorted(derived["fields"] - declared_fields):
        errs.append('source uses schema field literal "%s" with no class in "schema"' % f)
    events = set(need.get("events") or [])
    for e in sorted(derived["events"] - events):
        errs.append('source subscribes to game event %s: add it to "events"' % e)
    return errs


def cmd_needs_check(args):
    known = surface_names(args.gamedata)
    needs = load_needs(args.plugins_dir)
    bad = 0
    for pid in NEEDS_PLUGINS:
        d = os.path.join(args.plugins_dir, pid)
        if not os.path.isdir(d):
            continue
        if pid not in needs:
            print("FAIL %s: no plugins/%s/needs.json" % (pid, pid))
            bad += 1
            continue
        errs = check_needs(pid, needs[pid], derive_needs(plugin_sources(d)), known)
        for e in errs:
            print("FAIL %s: %s" % (pid, e))
        if not errs:
            n = needs[pid]
            print("OK   %s: %d api, %d surface, %d schema, %d events" % (
                pid, len(n.get("api") or []), len(n.get("surface") or []),
                len(n.get("schema") or []) + len(n.get("schema_optional") or []), len(n.get("events") or [])))
        bad += bool(errs)
    return 1 if bad else 0


def cmd_needs_derive(args):
    d = derive_needs(plugin_sources(os.path.join(args.plugins_dir, args.plugin)))
    surface = sorted({s for m in d["api"] for s in API_SURFACE[m]})
    by_field = {}
    for c, f in sorted(d["schema"]):
        by_field.setdefault(f, []).append("%s.%s" % (c, f))
    doc = {"schema_version": 1, "plugin": args.plugin, "api": sorted(d["api"]), "surface": surface,
           "schema": sorted("|".join(v) for v in by_field.values()), "schema_optional": [],
           "events": sorted(d["events"])}
    print(json.dumps(doc, indent=1))
    return 0


# ---------------------------------------------------------------------------------------------
# Dynamic stages (selftest + live), .github/workflows/cs2-dynamic.yml
# ---------------------------------------------------------------------------------------------

# readyup_selftest.txt check line: "  <STATUS> <name padded to 46> <detail>" (core/src/readyup/selftest.cpp).
ST_LINE = re.compile(r"^\s+(OK|FAIL|PEND|SKIP|INFO|WARN)\s+(.*?)\s*$")
ST_NEED = re.compile(r"^need (\S+) (surface|schema|schema_optional|event) (\S+)(?:\s+(.*))?$")
ST_SECTION = re.compile(r"^\[(.+)\]\s*$")


def split_name_detail(rest):
    parts = re.split(r"\s{2,}", rest.strip(), maxsplit=1)
    return parts[0], (parts[1] if len(parts) > 1 else "")


def parse_selftest(text):
    """{"core": [(status, name, detail)], "plugins": {pid: [(status, check, detail)]},
        "needs": {pid: [(status, kind, entry, detail)]}, "loaded": set | None, "exit_code": int | None}"""
    out = {"core": [], "plugins": {}, "needs": {}, "loaded": None, "exit_code": None, "load_failures": []}
    section = ""
    for raw in text.splitlines():
        line = raw.rstrip()
        m = ST_SECTION.match(line)
        if m:
            section = m.group(1)
            continue
        if line.startswith("exit_code:"):
            try:
                out["exit_code"] = int(line.split(":", 1)[1])
            except ValueError:
                pass
            continue
        m = ST_LINE.match(line)
        if not m:
            continue
        status, rest = m.group(1), m.group(2)
        n = ST_NEED.match(rest)
        if n:
            out["needs"].setdefault(n.group(1), []).append((status, n.group(2), n.group(3), (n.group(4) or "").strip()))
            continue
        if section == "plugins":
            if rest.startswith("plugin host"):
                lm = re.search(r"loaded: (.*)$", rest)
                out["loaded"] = set(x.strip().split(" ")[0] for x in lm.group(1).split(",")) if lm else set()
                continue
            if rest.startswith("plugin load"):
                out["load_failures"].append(rest[len("plugin load"):].strip())
                continue
            pm = re.match(r"^([a-z0-9_-]+): (.*)$", rest)
            if pm:
                name, detail = split_name_detail(pm.group(2))
                out["plugins"].setdefault(pm.group(1), []).append((status, name, detail))
                continue
        out["core"].append((status,) + split_name_detail(rest))
    return out


def tally(kind, rows, fail_statuses=("FAIL",), warn_statuses=("WARN",)):
    """rows: [(status, label)] -> check dict; PEND rows keep the check pending if nothing failed."""
    passed = sum(1 for s, _ in rows if s == "OK")
    failures = [short(label) for s, label in rows if s in fail_statuses]
    warns = [short(label) for s, label in rows if s in warn_statuses]
    counted = [r for r in rows if r[0] in ("OK",) + tuple(fail_statuses) + tuple(warn_statuses)]
    if failures:
        status = "fail"
    elif not counted or any(s == "PEND" for s, _ in rows):
        status = "pending" if not warns else "warn"
    elif warns:
        status = "warn"
    else:
        status = "pass"
    return {"kind": kind, "status": status, "passed": passed, "total": len(counted), "failures": failures + warns}


def dynamic_checks(cid, need, st, has_report):
    """schema / event / selftest checks for one component from a parsed selftest."""
    checks = []
    needs_rows = st["needs"].get(cid, []) if has_report else []
    if need:
        n_schema = len(need.get("schema") or []) + len(need.get("schema_optional") or [])
        if n_schema:
            rows = [(s if k == "schema" else ("MISS" if s == "WARN" else s), "%s: %s" % (e, d or "not found"))
                    for s, k, e, d in needs_rows if k in ("schema", "schema_optional")]
            # a required field that is missing fails the plugin; an optional one only warns
            rows = [("FAIL" if s == "WARN" else ("WARN" if s == "MISS" else s), label) for s, label in rows]
            checks.append(tally("schema", rows) if rows else dict(pending_check("schema"), total=n_schema))
        if need.get("events"):
            rows = [(s, "%s: %s" % (e, d or "unknown")) for s, k, e, d in needs_rows if k == "event"]
            checks.append(tally("event", rows) if rows else dict(pending_check("event"), total=len(need["events"])))
    if not has_report:
        checks.append(pending_check("selftest"))
        return checks
    if cid == CORE_ID:
        # "[runtime lookup]" schema lines are plugin lookups (alternatives a plugin tries); the
        # plugin's own needs lines judge those.
        rows = [(s, "%s: %s" % (n, d)) for s, n, d in st["core"]
                if not n.startswith("selftest:") and not d.endswith("[runtime lookup]")]
        sel = tally("selftest", rows)
        if st["exit_code"] is None:
            sel["failures"].append("no exit_code line (report cut short)")
            sel["status"] = "fail"
        checks.append(sel)
        return checks
    rows = []
    for s, n, d in st["plugins"].get(cid, []):
        if n == "disabled":
            rows.append(("FAIL", "disabled: %s" % d))
        else:
            rows.append((s, "%s: %s" % (n, d)))
    for f in st["load_failures"]:
        if f.startswith(cid + ":") or f.startswith(cid + ".so"):
            rows.append(("FAIL", "load failed: %s" % f))
    loaded = st["loaded"]
    if loaded is not None and cid not in loaded and not any(s == "FAIL" for s, _ in rows):
        rows.append(("FAIL", "not loaded (not in the plugin host's list)"))
    # Loaded, and its own lines are only INFO (or none): being loaded is the result. INFO lines alone
    # would otherwise leave the check pending forever.
    if loaded is not None and cid in loaded and not any(s in ("OK", "FAIL", "WARN", "PEND") for s, _ in rows):
        rows.append(("OK", "loaded"))
    checks.append(tally("selftest", rows) if rows else pending_check("selftest"))
    return checks


def livetest_check(results, advisory=()):
    """results: [(name, rc)] ; rc 0 pass, 1 fail, 2 no verdict (server busy/down). A name in
    `advisory` (a new live test that has not proven itself yet) never fails the check: its
    failure keeps the check pending, with the reason in `failures`."""
    rows = []
    for name, rc in results:
        if rc == 0:
            rows.append(("OK", name))
        elif rc == 2:
            rows.append(("PEND", "%s: no verdict (exit 2)" % name))
        elif name in advisory:
            rows.append(("PEND", "%s: failed (exit %d); advisory (not required yet), see the run" % (name, rc)))
        else:
            rows.append(("FAIL", "%s: bot live test failed (exit %d); see the run" % (name, rc)))
    if not rows:
        return pending_check("livetest")
    chk = tally("livetest", rows)
    if chk["status"] == "pending":
        chk["failures"] = [short(label) for s, label in rows if s == "PEND"]
    return chk


def cmd_dynamic(args):
    base = {}
    if args.base and os.path.isfile(args.base):
        with open(args.base, encoding="utf-8") as f:
            base = json.load(f)
    needs = load_needs(args.plugins_dir)
    text = read_text(args.selftest)
    has_report = bool(text.strip())
    st = parse_selftest(text)
    env = read_env_file(args.build_env)
    buildid = str(env.get("BUILDID") or args.buildid or "")
    if base and buildid and base.get("cs2", {}).get("buildid") not in ("", buildid):
        print("::warning::static verdict is for CS2 build %s, this server runs %s; static checks kept as-is"
              % (base["cs2"]["buildid"], buildid))
    ids = [c["id"] for c in base.get("components") or []] or all_component_ids(surface_files(args.gamedata))
    by_id = {c["id"]: c for c in base.get("components") or []}
    live = [(n, int(rc)) for n, _, rc in (x.partition("=") for x in args.livetest or [])]
    advisory = {n for x in args.livetest_advisory or [] for n in re.split(r"[\s,]+", x) if n}
    live_for = {}
    for n, rc in live:
        live_for.setdefault(LIVETEST_COMPONENT.get(n, LIVETEST_DEFAULT), []).append((n, rc))
    components = []
    for cid in ids:
        old = by_id.get(cid, {"checks": []})
        static = [k for k in old["checks"] if k["kind"] in STATIC_KINDS]
        checks = static + dynamic_checks(cid, needs.get(cid), st, has_report)
        if cid in livetest_components():
            checks.append(livetest_check(live_for.get(cid, []), advisory) if args.stage == "live"
                          else pending_check("livetest"))
        components.append({"id": cid, "name": component_name(cid), "status": component_status(checks), "checks": checks})
    if not has_report:
        # the server never wrote a report: that is Ready Up failing to boot on this build
        core = next((c for c in components if c["id"] == CORE_ID), None)
        if core:
            core["checks"] = [k for k in core["checks"] if k["kind"] != "selftest"] + [
                {"kind": "selftest", "status": "fail", "passed": 0, "total": 1,
                 "failures": ["no readyup_selftest.txt: the server did not finish booting with Ready Up"]}]
            core["status"] = component_status(core["checks"])
    overall = overall_status(components)
    if not args.patch and base:
        args.patch = base.get("cs2", {}).get("patch", "")
    if not args.buildid and base:
        args.buildid = base.get("cs2", {}).get("buildid", "")
    doc = document(args, overall, overall, components, now_iso(), args.build_env, stage=args.stage)
    write_json(os.path.join(args.out_dir, "compat.json"), doc)
    write_json(os.path.join(args.out_dir, "badge.json"), badge(doc))
    print("compat[%s]: overall=%s (%s)" % (args.stage, overall,
                                          ", ".join("%s=%s" % (c["id"], c["status"]) for c in components)))
    return 0


# ---------------------------------------------------------------------------------------------
# Document assembly
# ---------------------------------------------------------------------------------------------

def readyup_version(version_file, commit):
    version = read_text(version_file).strip()
    ref_type = os.environ.get("GITHUB_REF_TYPE", "")
    if version and commit and ref_type != "tag":
        version = "%s-dev.%s" % (version, commit[:7])
    return version


def document(args, state, overall, components, finished_at, build_env, stage="static"):
    env = read_env_file(build_env)
    checked = now_iso()
    return {
        "schema": SCHEMA,
        "cs2": {"buildid": str(env.get("BUILDID") or args.buildid or ""),
                "patch": env.get("PATCH_VERSION") or args.patch or ""},
        "readyup": {"version": readyup_version(args.version_file, args.commit), "commit": args.commit or ""},
        "run": {"id": str(args.run_id or ""), "url": args.run_url or "", "trigger": args.trigger,
                "stage": stage, "state": state, "started_at": args.started_at or checked,
                "finished_at": finished_at},
        "overall": overall,
        "components": components,
        "checked_at": checked,
    }


def badge(doc):
    message, color = BADGE.get(doc["overall"], BADGE["checking"])
    patch = doc["cs2"].get("patch") or doc["cs2"].get("buildid") or "?"
    return {"schemaVersion": 1, "label": "CS2 %s" % patch, "message": message, "color": color}


def write_json(path, obj):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False)
        f.write("\n")


def cmd_result(args):
    raw = args.raw_dir
    sig_text = read_text(os.path.join(raw, "sigcheck.txt"))
    hook_text = read_text(os.path.join(raw, "hookcheck.txt"))
    rcs = read_env_file(os.path.join(raw, "rc.env"))
    sig_rc = int(rcs.get("SIGCHECK_RC", "2") or 2)
    hook_rc = int(rcs.get("HOOKCHECK_RC", "2") or 2)
    components = build_components(sig_text, hook_text, sig_rc, hook_rc, surface_files(args.gamedata),
                                  load_needs(args.plugins_dir))
    overall = overall_status(components)
    doc = document(args, overall, overall, components, now_iso(), args.build_env)
    write_json(os.path.join(args.out_dir, "compat.json"), doc)
    write_json(os.path.join(args.out_dir, "badge.json"), badge(doc))
    print("compat: overall=%s (%s)" % (overall, ", ".join("%s=%s" % (c["id"], c["status"]) for c in components)))
    return 0


def cmd_phase(args):
    ids = all_component_ids(surface_files(args.gamedata))
    if args.state == "no_verdict":
        comp_status, overall, finished = "pending", "no_verdict", now_iso()
    else:
        comp_status, overall, finished = "checking", "checking", None
    components = [{"id": cid, "name": component_name(cid), "status": comp_status, "checks": []} for cid in ids]
    doc = document(args, args.state, overall, components, finished, args.build_env, stage=args.stage)
    write_json(args.out, doc)
    print("compat: phase %s -> %s" % (args.state, args.out))
    return 0


def cmd_post(args):
    if not os.environ.get("COMPAT_INGEST_URL", "").strip():
        return 0
    try:
        with open(args.file, "rb") as f:
            body = f.read()
        json.loads(body)
    except (OSError, ValueError) as e:
        print("::warning::compat ingest: cannot read %s: %s" % (args.file, e))
        return 0
    post_body(body, args.timeout)
    return 0


def post_body(body, timeout):
    """POST one document to $COMPAT_INGEST_URL. Never raises: a failed POST is a warning only."""
    url = os.environ.get("COMPAT_INGEST_URL", "").strip()
    token = os.environ.get("COMPAT_INGEST_TOKEN", "").strip()
    if not url:
        return False
    headers = {"Content-Type": "application/json", "User-Agent": "readyup-cs2-watch"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            print("compat ingest: HTTP %d" % resp.status)
            return True
    except urllib.error.HTTPError as e:
        print("::warning::compat ingest: HTTP %d (ignored)" % e.code)
    except Exception as e:  # noqa: BLE001 - never fail the check because the POST failed
        print("::warning::compat ingest: %s (ignored)" % type(e).__name__)
    return False


# ---------------------------------------------------------------------------------------------
# Run steps: the live progress of a run (docs/CS2-COMPAT.md "Run steps")
# ---------------------------------------------------------------------------------------------

# The steps of .github/workflows/cs2-dynamic.yml, in order: (id, name, stage).
DYNAMIC_PLAN = [
    ("build", "Build bundle", "setup"),
    ("update", "Update CS2", "setup"),
    ("install", "Install bundle", "setup"),
    ("selftest", "Boot + selftest", "selftest"),
    ("live-match", "Live: match", "live"),
    ("live-scrim", "Live: scrim", "live"),
    ("live-fleet", "Live: fleet", "live"),
    ("live-forfeit", "Live: forfeit + pauses", "live"),
    ("record", "Record", "record"),
]
PLANS = {"dynamic": DYNAMIC_PLAN}
STEP_STATUSES = ("queued", "running", "pass", "fail", "skip")
STEP_STAGES = ("setup", "static", "selftest", "live", "record")
STEP_OPEN = ("queued", "running")
STEP_ID = re.compile(r"^[A-Za-z0-9._:-]{1,64}$")
# The site's limits (website: lib/compat/document.ts COMPAT_LIMITS).
MAX_STEPS, MAX_STEP_NAME, MAX_STEP_DETAIL = 100, 96, 500


def step_text(value, limit):
    """One line, no control characters, at most `limit` characters (the site refuses anything else)."""
    text = re.sub(r"[\x00-\x1f\x7f]+", " ", str(value or "")).strip()
    return text if len(text) <= limit else text[:limit - 3].rstrip() + "..."


def load_step_state(path):
    if path and os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as f:
                state = json.load(f)
            if isinstance(state, dict) and isinstance(state.get("steps"), list):
                return state
        except (OSError, ValueError):
            print("::warning::compat step: %s is unreadable, starting over" % path)
    return {"steps": [], "run": {}, "last_post": 0.0, "dirty": False}


def save_step_state(path, state):
    if not path:
        return
    tmp = path + ".tmp"
    write_json(tmp, state)
    os.replace(tmp, path)


def apply_step(steps, upd, stamp=True, now=None):
    """Add or update one step in `steps` (a list, in order). `upd`: id, status and optionally name,
    stage, parent, detail. Running stamps started_at, a finished status stamps finished_at."""
    now = now or now_iso()
    sid = upd["id"]
    if not STEP_ID.match(sid):
        raise ValueError("step id %r: letters, digits, . _ : - only (max 64)" % sid)
    status = upd["status"]
    if status not in STEP_STATUSES:
        raise ValueError("step status %r: one of %s" % (status, ", ".join(STEP_STATUSES)))
    old = next((s for s in steps if s["id"] == sid), None)
    step = dict(old) if old else {"id": sid, "name": sid, "stage": "setup", "status": "queued"}
    if upd.get("name"):
        step["name"] = step_text(upd["name"], MAX_STEP_NAME)
    if upd.get("stage"):
        if upd["stage"] not in STEP_STAGES:
            raise ValueError("step stage %r: one of %s" % (upd["stage"], ", ".join(STEP_STAGES)))
        step["stage"] = upd["stage"]
    if upd.get("parent"):
        step["parent"] = upd["parent"]
    step["status"] = status
    if status == "queued":
        for k in ("started_at", "finished_at", "detail"):
            step.pop(k, None)
    elif status == "running":
        step.pop("finished_at", None)
        step.pop("detail", None)
        if stamp:
            step.setdefault("started_at", now)
    elif stamp:
        step["finished_at"] = now
    if upd.get("detail"):
        step["detail"] = step_text(upd["detail"], MAX_STEP_DETAIL)
    if old:
        steps[steps.index(old)] = step
    elif step.get("parent") and any(s["id"] == step["parent"] or s.get("parent") == step["parent"] for s in steps):
        # a nested step goes after its parent and its siblings, not at the end
        last = max(i for i, s in enumerate(steps) if s["id"] == step["parent"] or s.get("parent") == step["parent"])
        steps.insert(last + 1, step)
    else:
        steps.append(step)
    return step


def close_steps(steps, stamp=True):
    """The run is over: a step still running failed, one still queued never ran. True if any changed."""
    changed = False
    for s in list(steps):
        if s["status"] == "running":
            apply_step(steps, {"id": s["id"], "status": "fail", "detail": s.get("detail") or "did not finish"}, stamp)
            changed = True
        elif s["status"] == "queued":
            apply_step(steps, {"id": s["id"], "status": "skip"}, stamp)
            changed = True
    return changed


def step_document(args, steps, run_meta, done):
    """The run as a compat.json with run.steps: --base's cs2 and components, state `checking`
    while not done; once done, --base's own verdict (or --final-state)."""
    base = {}
    if args.base and os.path.isfile(args.base):
        try:
            with open(args.base, encoding="utf-8") as f:
                base = json.load(f)
        except (OSError, ValueError):
            base = {}
    same_run = str(base.get("run", {}).get("id", "")) == str(args.run_id or "")
    if not args.buildid:
        args.buildid = base.get("cs2", {}).get("buildid", "")
    if not args.patch:
        args.patch = base.get("cs2", {}).get("patch", "")
    args.trigger = args.trigger or run_meta.get("trigger") or (base.get("run", {}).get("trigger") if same_run else "") or "manual"
    args.started_at = args.started_at or run_meta.get("started_at") or (base.get("run", {}).get("started_at") if same_run else "")
    components = base.get("components")
    if not isinstance(components, list):
        ids = all_component_ids(surface_files(args.gamedata))
        components = [{"id": cid, "name": component_name(cid), "status": "checking", "checks": []} for cid in ids]
    stage = base.get("run", {}).get("stage") if same_run else "selftest"
    if stage not in ("static", "selftest", "live"):
        stage = "selftest"
    if done:
        state = args.final_state or (base.get("run", {}).get("state") if same_run else "") or "no_verdict"
        if state in ("queued", "checking"):
            state = "no_verdict"
        overall = args.final_state or (base.get("overall") if same_run and state not in ("no_verdict",) else "") or state
        if overall not in ("pass", "warn", "fail", "checking", "no_verdict"):
            overall = "no_verdict"
        finished = (base.get("run", {}).get("finished_at") if same_run else None) or now_iso()
    else:
        state, overall, finished = "checking", "checking", None
    doc = document(args, state, overall, components, finished, args.build_env, stage=stage)
    doc["run"]["steps"] = steps[:MAX_STEPS]
    return doc


def step_summary(steps):
    """Markdown table of the steps, for $GITHUB_STEP_SUMMARY."""
    icon = {"queued": "queued", "running": "running", "pass": "pass", "fail": "**FAIL**", "skip": "skip"}
    rows = ["| step | result | time | detail |", "|---|---|---|---|"]
    for s in steps:
        took = ""
        try:
            a = datetime.datetime.strptime(s["started_at"], "%Y-%m-%dT%H:%M:%SZ")
            b = datetime.datetime.strptime(s["finished_at"], "%Y-%m-%dT%H:%M:%SZ")
            took = "%ds" % (b - a).total_seconds()
        except (KeyError, ValueError):
            pass
        name = ("&nbsp;&nbsp;&nbsp;" if s.get("parent") else "") + s["name"].replace("|", "/")
        rows.append("| %s | %s | %s | %s |" % (name, icon[s["status"]], took, (s.get("detail") or "").replace("|", "/")))
    return "\n".join(rows) + "\n"


def cmd_step(args):
    """Update the run's steps and POST the run. Never exits non-zero on a POST problem; exit 2 only
    for a malformed call (a bug in the caller), which callers treat as a warning too."""
    state = load_step_state(args.state)
    steps = state["steps"]
    run_meta = state.setdefault("run", {})
    if args.trigger:
        run_meta["trigger"] = args.trigger
    if args.started_at:
        run_meta["started_at"] = args.started_at
    stamp = not args.no_time
    changed = False
    try:
        for plan in args.plan or []:
            for sid, name, stage in PLANS[plan]:
                if not any(s["id"] == sid for s in steps):
                    apply_step(steps, {"id": sid, "name": name, "stage": stage, "status": "queued"}, stamp)
                    changed = True
        updates = json.loads(args.updates) if args.updates else []
        if args.id:
            updates.append({"id": args.id, "status": args.status, "name": args.name, "stage": args.stage,
                            "parent": args.parent, "detail": args.detail})
        for upd in updates:
            if not upd.get("status"):
                raise ValueError("step %s: --status is required" % upd.get("id"))
            apply_step(steps, upd, stamp)
            changed = True
    except (ValueError, KeyError, TypeError) as e:
        print("::warning::compat step: %s" % e)
        return 2
    if args.close and close_steps(steps, stamp):
        changed = True
    open_steps = [s for s in steps if s["status"] in STEP_OPEN]
    done = bool(args.close) or (bool(args.state) and bool(steps) and not open_steps)
    if args.summary:
        summ = os.environ.get("GITHUB_STEP_SUMMARY")
        if summ and steps:
            with open(summ, "a", encoding="utf-8") as fh:
                fh.write("\n### Compatibility run steps\n\n" + step_summary(steps) + "\n")
    want_post = changed or (args.flush and state.get("dirty"))
    now = time.time()
    doc = step_document(args, steps, run_meta, done)
    if args.out:
        write_json(args.out, doc)
    if want_post and not args.no_post:
        if args.min_interval and now - float(state.get("last_post") or 0) < args.min_interval and not done:
            state["dirty"] = True
        elif not doc["cs2"]["buildid"]:
            print("::warning::compat step: no CS2 build id known yet (--base / --build-env / --buildid); not posted")
        else:
            post_body(json.dumps(doc).encode("utf-8"), args.timeout)
            state["last_post"], state["dirty"] = now, False
    elif want_post:
        state["dirty"] = True
    save_step_state(args.state, state)
    current = ", ".join("%s=%s" % (s["id"], s["status"]) for s in steps if not s.get("parent"))
    print("compat step: %s%s" % (current or "(no steps)", " (done)" if done else ""))
    return 0


def gh_escape(text):
    return str(text).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def cmd_annotate_selftest(args):
    """One annotation per FAIL / WARN line of readyup_selftest.txt, with its section."""
    section = ""
    n = 0
    for raw in read_text(args.file).splitlines():
        m = ST_SECTION.match(raw.rstrip())
        if m:
            section = m.group(1)
            continue
        m = ST_LINE.match(raw.rstrip())
        if m and m.group(1) in ("FAIL", "WARN"):
            level = "error" if m.group(1) == "FAIL" else "warning"
            title = "selftest %s" % section if section else "selftest"
            print("::%s title=%s::%s" % (level, gh_escape(title).replace(",", " ").replace(":", " "), gh_escape(m.group(2))))
            n += 1
    print("selftest: %d annotation(s)" % n)
    return 0


def add_run_options(p):
    e = os.environ.get
    server, repo, run_id = e("GITHUB_SERVER_URL", "https://github.com"), e("GITHUB_REPOSITORY", ""), e("GITHUB_RUN_ID", "")
    p.add_argument("--run-id", default=run_id)
    p.add_argument("--run-url", default="%s/%s/actions/runs/%s" % (server, repo, run_id) if repo and run_id else "")
    p.add_argument("--trigger", default="manual", choices=TRIGGERS)
    p.add_argument("--started-at", default="")
    p.add_argument("--commit", default=e("GITHUB_SHA", ""))
    p.add_argument("--version-file", default="VERSION")
    p.add_argument("--buildid", default="", help="used when --build-env has no BUILDID")
    p.add_argument("--patch", default="", help="used when --build-env has no PATCH_VERSION")
    p.add_argument("--build-env", default="", help="cs2-build.env from fetch-cs2-binaries.sh")
    p.add_argument("--gamedata", default="gamedata")
    p.add_argument("--plugins-dir", default=os.path.join(ROOT, "plugins"), help="plugins/<id>/needs.json")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("result")
    add_run_options(r)
    r.add_argument("--raw-dir", required=True, help="VERIFY_RAW_DIR of verify-cs2.sh")
    r.add_argument("--out-dir", required=True)
    ph = sub.add_parser("phase")
    add_run_options(ph)
    ph.add_argument("state", choices=("queued", "checking", "no_verdict"))
    ph.add_argument("--stage", default="static", choices=("static", "selftest", "live"))
    ph.add_argument("--out", required=True)
    po = sub.add_parser("post")
    po.add_argument("file")
    po.add_argument("--timeout", type=float, default=15)
    dy = sub.add_parser("dynamic")
    add_run_options(dy)
    dy.add_argument("--stage", required=True, choices=("selftest", "live"))
    dy.add_argument("--base", default="", help="compat.json of the static stage (cs2-build)")
    dy.add_argument("--selftest", default="", help="readyup_selftest.txt of the server")
    dy.add_argument("--livetest", action="append", default=[], metavar="NAME=RC",
                    help="scripts/livetest/run.sh exit code (0 pass, 1 fail, 2 no verdict); repeatable")
    dy.add_argument("--livetest-advisory", action="append", default=[], metavar="NAMES",
                    help="live tests (space / comma separated; repeatable) whose failure keeps their "
                         "check pending instead of failing it: new checks until they have proven themselves")
    dy.add_argument("--out-dir", required=True)
    stp = sub.add_parser("step")
    add_run_options(stp)
    # run-wide fields come from the first call that names them (kept in --state)
    stp.set_defaults(trigger="", started_at="")
    e = os.environ.get
    stp.add_argument("--plan", action="append", choices=sorted(PLANS), help="add these planned steps as queued")
    stp.add_argument("--id")
    stp.add_argument("--name")
    stp.add_argument("--stage", choices=STEP_STAGES)
    stp.add_argument("--status", choices=STEP_STATUSES)
    stp.add_argument("--parent", help="nest under this step (the live test's own steps)")
    stp.add_argument("--detail")
    stp.add_argument("--updates", default="", help='JSON list of {"id","status",...} applied in order')
    stp.add_argument("--close", action="store_true",
                     help="the run is over: running steps fail, queued ones are skipped; post the verdict")
    stp.add_argument("--final-state", default="", choices=("", "pass", "warn", "fail", "no_verdict"),
                     help="verdict when done (default: --base's own)")
    stp.add_argument("--no-time", action="store_true", help="do not stamp started_at / finished_at")
    stp.add_argument("--no-post", action="store_true", help="only update --state")
    stp.add_argument("--flush", action="store_true", help="post if an earlier call skipped its post")
    stp.add_argument("--min-interval", type=float, default=0.0,
                     help="skip the POST when the last one was less than this many seconds ago (--flush sends it)")
    stp.add_argument("--summary", action="store_true", help="append a table of the steps to $GITHUB_STEP_SUMMARY")
    stp.add_argument("--state", default=e("COMPAT_STEP_STATE", ""), help="steps so far (JSON), kept between calls")
    stp.add_argument("--base", default=e("COMPAT_STEP_BASE", ""), help="the last compat.json of this run")
    stp.add_argument("--out", default="", help="also write the document here")
    stp.add_argument("--timeout", type=float, default=15)
    stp.set_defaults(build_env=e("COMPAT_STEP_BUILD_ENV", ""))
    an = sub.add_parser("annotate-selftest")
    an.add_argument("file")
    nc = sub.add_parser("needs-check")
    nc.add_argument("--plugins-dir", default=os.path.join(ROOT, "plugins"))
    nc.add_argument("--gamedata", default=os.path.join(ROOT, "gamedata"))
    nd = sub.add_parser("needs-derive")
    nd.add_argument("plugin")
    nd.add_argument("--plugins-dir", default=os.path.join(ROOT, "plugins"))
    args = ap.parse_args(argv)
    return {"result": cmd_result, "phase": cmd_phase, "post": cmd_post, "needs-check": cmd_needs_check, "dynamic": cmd_dynamic,
            "needs-derive": cmd_needs_derive, "step": cmd_step, "annotate-selftest": cmd_annotate_selftest}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
