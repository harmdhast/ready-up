#!/usr/bin/env bash
# Package Ready Up release zips from one build. Every zip's root is the contents of
# game/csgo, so it can be extracted straight into a server's game/csgo (or handed to the
# installer: install.sh --zip <zip>).
#
#   scripts/package-release.sh <build-dir> <version> <out-dir>
#
# <build-dir> holds libserver.so, plugins/match.so, plugins/fleet.so, plugins/skins.so,
# plugins/hello.so, plugins/midas.so, plugins/whitelist.so, plugins/practice.so, plugins/essentials.so,
# plugins/deathmatch.so, plugins/addons.so and (optionally) readyup_sigcheck / readyup_hookcheck. Every plugin but
# hello ships its plugins/<name>/needs.json as plugins/<name>.needs.json (the core does not load a
# plugin whose needs this CS2 build does not meet; docs/CS2-COMPAT.md "Plugin needs").
#
# Component zips (the installer mixes these):
#   ready-up-core-<v>-linuxsteamrt64.zip    the core (libserver.so, engine-surface.json,
#                                            readyup.cfg.example, tools, docs, LICENSE,
#                                            THIRD_PARTY_NOTICES.txt)
#   ready-up-match-<v>-linuxsteamrt64.zip   plugins/match.so (ready-up, scrims, knife, pauses,
#                                            practice, match configs, webhooks, demos) + the
#                                            cfg/ReadyUp/*.cfg templates it execs
#   ready-up-fleet-<v>-linuxsteamrt64.zip   plugins/fleet.so (link to the Auto Tournament platform)
#                                            + a commented cfg/ReadyUp/fleet.cfg template. Idle
#                                            until a url is configured, so bundles carry it.
#   ready-up-skins-<v>-linuxsteamrt64.zip   plugins/skins.so + engine-surface.skins.json
#   ready-up-hello-<v>-linuxsteamrt64.zip   plugins/hello.so (example plugin)
#   ready-up-midas-<v>-linuxsteamrt64.zip   plugins/midas.so + cfg template (fun: gold weapons, off by default)
#   ready-up-addons-<v>-linuxsteamrt64.zip  plugins/addons.so + engine-surface.addons.json + an all-comments
#                                            addons.cfg template (Steam Workshop addons; idle until
#                                            workshop_addons is set)
#   ready-up-whitelist-<v>-linuxsteamrt64.zip plugins/whitelist.so (only listed players; off by default)
#   ready-up-practice-<v>-linuxsteamrt64.zip  plugins/practice.so + engine-surface.practice.json + prac.cfg /
#                                            practice.cfg templates + tools/scenario/ru_scenario.py + seed scenarios
#   ready-up-essentials-plugin-<v>-...zip   plugins/essentials.so (admins, map commands); "essentials" alone is the bundle
#   ready-up-deathmatch-<v>-linuxsteamrt64.zip plugins/deathmatch.so + deathmatch.cfg template (off until .ru dm ffa|tdm)
#   ready-up-tv-<v>-linuxsteamrt64.zip      plugins/tv.so + an all-comments tv.cfg template (GOTV broadcast
#                                            settings; touches nothing until a key is set)
# Bundles (for manual download):
#   ready-up-essentials-<v>-...zip          core + essentials + match + fleet + practice. The default. NO skins.
#   ready-up-full-<v>-...zip                core + essentials + match + fleet + practice + skins + midas + whitelist + deathmatch + addons + readyup_sigcheck/hookcheck
# Plus SHA256SUMS over every zip.
#
# Each component ships readyup/manifests/<component>.json ({component, version, files}),
# which is how install.sh knows what to update or remove. Nothing in any zip overwrites
# admin-owned files: readyup.cfg, cfg/ReadyUp/*.cfg and the plugins' JSON data (admins,
# state, loadouts) are never shipped as such (readyup.cfg.example and readyup/cfg-templates/ are).
set -euo pipefail

BUILD="${1:?usage: $0 <build-dir> <version> <out-dir>}"
VERSION="${2:?usage: $0 <build-dir> <version> <out-dir>}"
OUT="${3:?usage: $0 <build-dir> <version> <out-dir>}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(cd "$BUILD" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
EPOCH="${SOURCE_DATE_EPOCH:-$(date +%s)}"
SUFFIX="$VERSION-linuxsteamrt64.zip"

for f in libserver.so plugins/match.so plugins/fleet.so plugins/skins.so plugins/hello.so plugins/midas.so plugins/whitelist.so plugins/practice.so plugins/essentials.so plugins/deathmatch.so plugins/mapvote.so plugins/community.so plugins/movement.so plugins/addons.so plugins/tv.so; do
  [[ -f "$BUILD/$f" ]] || { echo "package-release: missing $BUILD/$f" >&2; exit 1; }
done

# stage_component <name> <description> <src>:<dest-under-readyup>[:mode] ...
stage_component() {
  local name="$1" desc="$2"
  shift 2
  local dir="$WORK/c/$name" spec src dst mode
  mkdir -p "$dir/readyup/manifests"
  local files=()
  for spec in "$@"; do
    IFS=: read -r src dst mode <<<"$spec"
    install -D -m "${mode:-644}" "$src" "$dir/readyup/$dst"
    files+=("readyup/$dst")
  done
  python3 - "$dir/readyup/manifests/$name.json" "$name" "$VERSION" "$desc" "${files[@]}" <<'PY'
import json, sys
path, name, version, desc, *files = sys.argv[1:]
json.dump({"component": name, "version": version, "description": desc, "files": sorted(files)},
          open(path, "w"), indent=2)
open(path, "a").write("\n")
PY
}

core_files=(
  "$BUILD/libserver.so:bin/linuxsteamrt64/libserver.so:755"
  "$ROOT_DIR/gamedata/engine-surface.json:bin/linuxsteamrt64/engine-surface.json"
  "$ROOT_DIR/cfg/readyup.cfg.example:bin/linuxsteamrt64/readyup.cfg.example"
  "$ROOT_DIR/scripts/patch_gameinfo.py:tools/patch_gameinfo.py:755"
  "$ROOT_DIR/scripts/migrate-postgres-to-json.py:tools/migrate-postgres-to-json.py:755"
  "$ROOT_DIR/install.sh:tools/install.sh:755"
  "$ROOT_DIR/README.md:README.md"
  "$ROOT_DIR/docs/INSTALL.md:INSTALL.md"
)
[[ -f "$ROOT_DIR/LICENSE" ]] && core_files+=("$ROOT_DIR/LICENSE:LICENSE")
[[ -f "$ROOT_DIR/THIRD_PARTY_NOTICES.txt" ]] && core_files+=("$ROOT_DIR/THIRD_PARTY_NOTICES.txt:THIRD_PARTY_NOTICES.txt")
printf '%s\n' "$VERSION" >"$WORK/VERSION"
{
  echo "version=$VERSION"
  echo "commit=$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "built=$(date -u -d "@$EPOCH" +%Y-%m-%dT%H:%M:%SZ)"
  echo "libserver_sha256=$(sha256sum "$BUILD/libserver.so" | cut -d' ' -f1)"
  [[ -n "${CS2_BUILDID:-}" ]] && echo "verified_cs2_buildid=$CS2_BUILDID"
  [[ -n "${CS2_PATCH_VERSION:-}" ]] && echo "verified_cs2_patch_version=$CS2_PATCH_VERSION"
} >"$WORK/BUILD_INFO"
core_files+=("$WORK/VERSION:VERSION" "$WORK/BUILD_INFO:BUILD_INFO")
stage_component core "Ready Up core: libserver.so, engine surface, plugin host, status endpoint" "${core_files[@]}"

# The match flow (plugins/match) and the ReadyUp/*.cfg files it execs (warmup, knife, live, ...).
match_files=("$BUILD/plugins/match.so:plugins/match.so:755"
  "$ROOT_DIR/plugins/match/needs.json:plugins/match.needs.json")
for f in "$ROOT_DIR"/cfg/ReadyUp/*.cfg; do
  case "$(basename "$f")" in fleet.cfg | midas.cfg | prac.cfg | practice.cfg | deathmatch.cfg | mapvote.cfg | community.cfg | movement.cfg | addons.cfg | tv.cfg) continue ;; esac  # the fleet / midas / practice / deathmatch / addons components' own
  match_files+=("$f:cfg-templates/ReadyUp/$(basename "$f")")
done
stage_component match "Ready-up, scrims, knife round, pauses, practice, match configs, webhooks, demos" "${match_files[@]}"

# The platform link. fleet.so stays idle without a url, and the template is all comments. The
# protocol schemas (plugins/fleet/protocol) are for tests only; the plugin does not read them.
stage_component fleet "Link to the Auto Tournament platform (idle until configured)" \
  "$BUILD/plugins/fleet.so:plugins/fleet.so:755" \
  "$ROOT_DIR/plugins/fleet/needs.json:plugins/fleet.needs.json" \
  "$ROOT_DIR/cfg/ReadyUp/fleet.cfg:cfg-templates/ReadyUp/fleet.cfg"

cat >"$WORK/SKINS-WARNING.txt" <<'EOF'
Ready Up skins (weapon paints, knives, gloves, agents)

Servers that run skin changers risk a GSLT ban from Valve. Only install this on servers
where you accept that risk. Remove readyup/plugins/skins.so and
readyup/bin/linuxsteamrt64/engine-surface.skins.json (or run install.sh and untick Skins)
to go back to a skins-free server.
EOF
stage_component skins "Weapon paints, knives, gloves, agents (servers running skin changers risk GSLT bans)" \
  "$BUILD/plugins/skins.so:plugins/skins.so:755" \
  "$ROOT_DIR/plugins/skins/needs.json:plugins/skins.needs.json" \
  "$ROOT_DIR/gamedata/engine-surface.skins.json:bin/linuxsteamrt64/engine-surface.skins.json" \
  "$WORK/SKINS-WARNING.txt:SKINS-WARNING.txt"

stage_component hello "Example plugin (.hello, hello_status); for plugin developers" \
  "$BUILD/plugins/hello.so:plugins/hello.so:755"

# Fun plugin; the template keeps it off (enabled=0).
stage_component midas "Fun: weapons picked up by chosen players turn gold (off by default, never under the valve ruleset)" \
  "$BUILD/plugins/midas.so:plugins/midas.so:755" \
  "$ROOT_DIR/plugins/midas/needs.json:plugins/midas.needs.json" \
  "$ROOT_DIR/cfg/ReadyUp/midas.cfg:cfg-templates/ReadyUp/midas.cfg"

# Steam Workshop addons (downloaded, mounted with every map change, their models precached). Idle
# without workshop_addons: the template is all comments, so it downloads nothing and never reloads
# the map. Its gamedata fragment ships next to the core's engine-surface.json, like the skins one.
stage_component addons "Steam Workshop addons for the server (idle until workshop_addons is set in addons.cfg)" \
  "$BUILD/plugins/addons.so:plugins/addons.so:755" \
  "$ROOT_DIR/plugins/addons/needs.json:plugins/addons.needs.json" \
  "$ROOT_DIR/gamedata/engine-surface.addons.json:bin/linuxsteamrt64/engine-surface.addons.json" \
  "$ROOT_DIR/cfg/ReadyUp/addons.cfg:cfg-templates/ReadyUp/addons.cfg"

# Server basics apart from the match flow: admins (admins.json) and map change / reload / restart.
stage_component essentials "Server basics: admins (admins.json), map change / reload / restart, default maps per mode" \
  "$BUILD/plugins/essentials.so:plugins/essentials.so:755" \
  "$ROOT_DIR/plugins/essentials/needs.json:plugins/essentials.needs.json"

# Practice mode + tools (.prac, .savepos, .rethrow, .bot, ...): its own plugin, so a server can
# run it without the match flow. Ships prac.cfg (the cvars it execs) and its settings template, its
# gamedata fragment (engine-surface.practice.json: the grenade projectile factories behind ru_api
# grenade_spawn, next to the core's engine-surface.json like the skins / addons ones), the
# scenario converter (tools/scenario/ru_scenario.py, `ru scenario convert`) and the seed scenarios
# (plugins/practice/scenarios/*.json: derived data with attribution, docs/SCENARIOS.md). Scenarios
# admins add to that folder are not in the manifest, so updates never touch them.
practice_files=("$BUILD/plugins/practice.so:plugins/practice.so:755"
  "$ROOT_DIR/plugins/practice/needs.json:plugins/practice.needs.json"
  "$ROOT_DIR/gamedata/engine-surface.practice.json:bin/linuxsteamrt64/engine-surface.practice.json"
  "$ROOT_DIR/cfg/ReadyUp/prac.cfg:cfg-templates/ReadyUp/prac.cfg"
  "$ROOT_DIR/cfg/ReadyUp/practice.cfg:cfg-templates/ReadyUp/practice.cfg"
  "$ROOT_DIR/tools/scenario/ru_scenario.py:tools/scenario/ru_scenario.py:755")
for f in "$ROOT_DIR"/plugins/practice/scenarios/*.json; do
  [[ -f "$f" ]] && practice_files+=("$f:plugins/practice/scenarios/$(basename "$f")")
done
stage_component practice "Practice mode and tools (.prac, .savepos/.loadpos, .spawn, .rethrow, .bot, .scen replays)" \
  "${practice_files[@]}"

stage_component whitelist "Only listed players may stay on the server (off until ru whitelist on)" \
  "$BUILD/plugins/whitelist.so:plugins/whitelist.so:755" \
  "$ROOT_DIR/plugins/whitelist/needs.json:plugins/whitelist.needs.json"

# Deathmatch (FFA / TDM on CS2's deathmatch game mode): off until an admin switches it on.
stage_component deathmatch "Deathmatch: free for all / team deathmatch with kill + time limits and a leaderboard (off until .ru dm ffa|tdm)" \
  "$BUILD/plugins/deathmatch.so:plugins/deathmatch.so:755" \
  "$ROOT_DIR/plugins/deathmatch/needs.json:plugins/deathmatch.needs.json" \
  "$ROOT_DIR/cfg/ReadyUp/deathmatch.cfg:cfg-templates/ReadyUp/deathmatch.cfg"

# Map voting (.rtv, .nominate, end-of-map vote) for community servers: off until mapvote.cfg has maps,
# and always off while a match is loaded.
stage_component mapvote "Map voting: .rtv, .nominate, an end-of-map vote and .timeleft (off until mapvote.cfg lists maps; never during a match)" \
  "$BUILD/plugins/mapvote.so:plugins/mapvote.so:755" \
  "$ROOT_DIR/plugins/mapvote/needs.json:plugins/mapvote.needs.json" \
  "$ROOT_DIR/cfg/ReadyUp/mapvote.cfg:cfg-templates/ReadyUp/mapvote.cfg"

# Community-server commands (.rank, .top, .speclist, admin .kick); stats only while no match is loaded.
stage_component community "Community commands: .rank, .top, .speclist and admin .kick (stats counted while no match is loaded)" \
  "$BUILD/plugins/community.so:plugins/community.so:755" \
  "$ROOT_DIR/plugins/community/needs.json:plugins/community.needs.json" \
  "$ROOT_DIR/cfg/ReadyUp/community.cfg:cfg-templates/ReadyUp/community.cfg"
# Surf / bhop / kz timers: zones from the map's triggers, personal bests sent to the platform.
stage_component movement "Movement: surf / bhop / kz run timer, zones from the map, records (.rs, .pb, .wr, .cp/.tp)" \
  "$BUILD/plugins/movement.so:plugins/movement.so:755" \
  "$ROOT_DIR/plugins/movement/needs.json:plugins/movement.needs.json" \
  "$ROOT_DIR/cfg/ReadyUp/movement.cfg:cfg-templates/ReadyUp/movement.cfg"

# GOTV broadcast settings (docs/design/tv-plugin.md): the template is all comments, so it touches
# nothing until a key is set (or a platform match carries rules.broadcast).
stage_component tv "GOTV broadcast settings: delay, insecure observers for HUD tools, casters (idle until set)" \
  "$BUILD/plugins/tv.so:plugins/tv.so:755" \
  "$ROOT_DIR/plugins/tv/needs.json:plugins/tv.needs.json" \
  "$ROOT_DIR/cfg/ReadyUp/tv.cfg:cfg-templates/ReadyUp/tv.cfg"

extras=()
for t in readyup_sigcheck readyup_hookcheck; do
  [[ -f "$BUILD/$t" ]] && extras+=("$BUILD/$t:tools/$t:755")
done
if [[ ${#extras[@]} -gt 0 ]]; then
  stage_component tools "Offline gamedata checkers (readyup_sigcheck, readyup_hookcheck)" "${extras[@]}"
fi

# Guard: the old dev installer (stops csm servers, wipes DB rows) must never ship.
if grep -rq 'csm stop\|TRUNCATE readyup_admins' "$WORK/c"; then
  echo "package-release: refusing to package a dev-only script" >&2
  exit 1
fi

# make_zip <zip-name> <component>...
make_zip() {
  local zip="$1" stage="$WORK/z/$1" c
  shift
  mkdir -p "$stage"
  for c in "$@"; do cp -a "$WORK/c/$c/." "$stage/"; done
  rm -f "$OUT/$zip"
  (cd "$stage" && find readyup -exec touch -h -d "@$EPOCH" {} + && find readyup -type f | LC_ALL=C sort | zip -qX "$OUT/$zip" -@)
  echo "Packaged: $zip ($*)"
}

make_zip "ready-up-core-$SUFFIX" core
make_zip "ready-up-match-$SUFFIX" match
make_zip "ready-up-skins-$SUFFIX" skins
make_zip "ready-up-hello-$SUFFIX" hello
make_zip "ready-up-midas-$SUFFIX" midas
make_zip "ready-up-addons-$SUFFIX" addons
make_zip "ready-up-whitelist-$SUFFIX" whitelist
make_zip "ready-up-deathmatch-$SUFFIX" deathmatch
make_zip "ready-up-mapvote-$SUFFIX" mapvote
make_zip "ready-up-community-$SUFFIX" community
make_zip "ready-up-movement-$SUFFIX" movement
make_zip "ready-up-tv-$SUFFIX" tv
make_zip "ready-up-practice-$SUFFIX" practice
make_zip "ready-up-essentials-plugin-$SUFFIX" essentials
make_zip "ready-up-fleet-$SUFFIX" fleet
make_zip "ready-up-essentials-$SUFFIX" core essentials match fleet practice mapvote
full=(core essentials match fleet practice skins midas whitelist deathmatch mapvote community movement addons tv)
[[ -d "$WORK/c/tools" ]] && full+=(tools)
make_zip "ready-up-full-$SUFFIX" "${full[@]}"

(cd "$OUT" && sha256sum ready-up-*-"$SUFFIX" >SHA256SUMS)
echo "SHA256SUMS:"
cat "$OUT/SHA256SUMS"
