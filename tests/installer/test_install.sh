#!/usr/bin/env bash
# End-to-end test of install.sh against fake CS2 server trees, using the zips from
# scripts/package-release.sh (no network, no real server):
#
#   tests/installer/test_install.sh <dist-dir>
#
# Covers: fresh essentials install (with and without a Metamod line), rerun = no-op, user
# config kept (+ *.default when a template changes), fleet in essentials (idle template), removing
# and re-adding fleet (its data dir kept), adding skins from the full zip, removing
# it, the numbered-prompt and arrow-key pickers through a pty (`script`), uninstall, --purge,
# the license choice (--accept-license, --license-key, saved choice, an older
# license-acceptance.json, the prompt + "I AGREE" through a pty), migrating an older core + match
# install (practice and essentials come with the next update), the full bundle by name, release
# mode (ready-up-essentials-plugin-* is the essentials component) and the beta channel
# (--channel beta, --version for a pre-release, the message when only pre-releases exist).
set -euo pipefail

DIST="$(cd "${1:?usage: $0 <dist-dir>}" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INSTALL="$ROOT/install.sh"
ESS="$(ls "$DIST"/ready-up-essentials-[0-9]*.zip)"
FULL="$(ls "$DIST"/ready-up-full-*.zip)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
check() {  # <description> <command...>
  local d="$1"
  shift
  if "$@"; then pass "$d"; else fail "$d"; fi
}

make_server() {  # <dir> <with-metamod 0|1>
  mkdir -p "$1/game/csgo" "$1/game/bin/linuxsteamrt64"
  {
    printf '"GameInfo"\n{\n\tFileSystem\n\t{\n\t\tSearchPaths\n\t\t{\n'
    [[ "$2" == 1 ]] && printf '\t\t\tGame\tcsgo/addons/metamod\n'
    printf '\t\t\tGame_LowViolence\tcsgo_lv\n\t\t\tGame\tcsgo\n\t\t\tGame\tcsgo_imported\n\t\t\tGame\tcsgo_core\n'
    printf '\t\t\tGame\tcore\n\t\t}\n\t}\n}\n'
  } >"$1/game/csgo/gameinfo.gi"
  cp "$1/game/csgo/gameinfo.gi" "$1/game/csgo/gameinfo_branchspecific.gi"
}

line_of() { grep -nE "^[[:space:]]*Game[[:space:]]+$2[[:space:]]*$" "$1" | cut -d: -f1 | head -n1; }
count_ru() { grep -cE '^[[:space:]]*Game[[:space:]]+csgo/readyup[[:space:]]*$' "$1" || true; }
installed() { python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["components"])))' "$1/game/csgo/readyup/installed.json"; }
run() { bash "$INSTALL" "$@" </dev/null; }
# "<use> <how> <last char of accepted_at>" from cfg/ReadyUp/license.cfg (install.sh's answer).
lic() {
  python3 - "$1/game/csgo/cfg/ReadyUp/license.cfg" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
use = re.search(r'^readyup_license_accepted "([a-z]+)"$', text, re.M).group(1)
at = re.search(r'^readyup_license_accepted_at "([^"]+)"$', text, re.M).group(1)
how = re.search(r"accepted via: ([a-z]+)", text).group(1)
print(use, how, at[-1])
PY
}
key_of() { sed -n 's/^readyup_license_key "\(.*\)"$/\1/p' "$1/game/csgo/cfg/readyup_license.cfg"; }
TEST_KEY="ATL1.eyJ2IjoxfQ.c2lnbmF0dXJl"
# The file exists and has no active `key = value` line (only comments / blank lines).
not_in() { ! grep -q "$1" "$2"; }
idle_cfg() { [[ -f "$1" ]] && ! grep -Ev '^[[:space:]]*(//|#|$)' "$1" | grep -q .; }

echo "== license: unattended install without a choice is refused, nothing installed"
S="$T/server-nolic"
make_server "$S" 0
if run --dir "$S" --zip "$ESS" essentials >"$T/out" 2>&1; then
  fail "unattended install without --accept-license succeeded"
else
  check "clear error names --accept-license" grep -q "accept-license=noncommercial" "$T/out"
  check "nothing installed without a license choice" test ! -e "$S/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
fi
if run --dir "$S" --zip "$ESS" --accept-license=free essentials >"$T/out" 2>&1; then
  fail "--accept-license=free accepted"
else
  check "bad --accept-license value refused" grep -q "must be noncommercial or commercial" "$T/out"
fi
echo "== license: --accept-license=commercial is recorded"
run --dir "$S" --zip "$ESS" --accept-license=commercial essentials >"$T/out" 2>&1 || { cat "$T/out"; fail "commercial install exited non-zero"; }
check "license.cfg: commercial, flag, UTC time" test "$(lic "$S")" = "commercial flag Z"
check "commercial without a key says how to add it later" grep -q "license-key" "$T/out"
check "no key file without --license-key" test ! -e "$S/game/csgo/cfg/readyup_license.cfg"
echo "== license: --license-key goes to cfg/readyup_license.cfg (csm's file), other lines kept"
printf '// csm header\nreadyup_show_license 1\nreadyup_license_key "ATL1.old"\n' >"$S/game/csgo/cfg/readyup_license.cfg"
run --dir "$S" --zip "$ESS" --accept-license=commercial --license-key "$TEST_KEY" essentials >"$T/out" 2>&1 ||
  { cat "$T/out"; fail "install with --license-key exited non-zero"; }
check "key saved as readyup_license_key" test "$(key_of "$S")" = "$TEST_KEY"
check "old key line replaced, one key line" test "$(grep -c '^readyup_license_key' "$S/game/csgo/cfg/readyup_license.cfg")" = 1
check "other lines in readyup_license.cfg kept" grep -q '^readyup_show_license 1' "$S/game/csgo/cfg/readyup_license.cfg"
check "the key is not printed" not_in "c2lnbmF0dXJl" "$T/out"
if run --dir "$S" --zip "$ESS" --accept-license=commercial --license-key 'not a key' essentials >"$T/out" 2>&1; then
  fail "a malformed --license-key was accepted"
else
  check "malformed --license-key refused" grep -q "does not look like a Ready Up license key" "$T/out"
fi
echo "== license: an answer saved by an older installer (readyup/license-acceptance.json) still counts"
S5="$T/server-legacy-license"
make_server "$S5" 0
mkdir -p "$S5/game/csgo/readyup"
echo '{"use": "noncommercial", "accepted_at": "2026-09-01T00:00:00Z", "how": "prompt"}' >"$S5/game/csgo/readyup/license-acceptance.json"
run --dir "$S5" --zip "$ESS" -y >"$T/out" 2>&1 || { cat "$T/out"; fail "install with a legacy license answer exited non-zero"; }
check "legacy answer used" grep -q "license: noncommercial use (accepted earlier" "$T/out"
check "legacy answer moved to license.cfg" test "$(lic "$S5")" = "noncommercial earlier Z"

for mm in 0 1; do
  S="$T/server-mm$mm"
  make_server "$S" "$mm"
  CS="$S/game/csgo"
  echo "== fresh essentials install (metamod=$mm)"
  run --dir "$S" --zip "$ESS" --accept-license=noncommercial essentials >"$T/out" 2>&1 || { cat "$T/out"; fail "install exited non-zero"; continue; }
  check "license.cfg: noncommercial, flag, UTC time" test "$(lic "$S")" = "noncommercial flag Z"
  check "core libserver.so installed" test -x "$CS/readyup/bin/linuxsteamrt64/libserver.so"
  check "match.so installed" test -x "$CS/readyup/plugins/match.so"
  check "engine-surface.json installed" test -f "$CS/readyup/bin/linuxsteamrt64/engine-surface.json"
  check "no skins.so in essentials" test ! -e "$CS/readyup/plugins/skins.so"
  check "no skins gamedata in essentials" test ! -e "$CS/readyup/bin/linuxsteamrt64/engine-surface.skins.json"
  check "no deathmatch.so in essentials (Full only)" test ! -e "$CS/readyup/plugins/deathmatch.so"
  check "readyup.cfg created from the example" test -f "$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  check "cfg/ReadyUp templates seeded" test -f "$CS/cfg/ReadyUp/live.cfg"
  check "fleet.so installed" test -x "$CS/readyup/plugins/fleet.so"
  check "fleet.cfg seeded, all comments (fleet stays idle)" idle_cfg "$CS/cfg/ReadyUp/fleet.cfg"
  check "practice.so installed (essentials)" test -x "$CS/readyup/plugins/practice.so"
  check "practice gamedata next to the core (grenade spawning)" test -f "$CS/readyup/bin/linuxsteamrt64/engine-surface.practice.json"
  check "essentials.so installed (essentials bundle)" test -x "$CS/readyup/plugins/essentials.so"
  check "mapvote.so installed (essentials bundle)" test -x "$CS/readyup/plugins/mapvote.so"
  check "prac.cfg seeded by the practice component" test -f "$CS/cfg/ReadyUp/prac.cfg"
  check "practice.cfg seeded, all comments (always off)" idle_cfg "$CS/cfg/ReadyUp/practice.cfg"
  check "installed.json lists core essentials fleet match practice" test "$(installed "$S")" = "core essentials fleet mapvote match practice"
  for gi in gameinfo.gi gameinfo_branchspecific.gi; do
    check "$gi: exactly one readyup line" test "$(count_ru "$CS/$gi")" = 1
    check "$gi: readyup before Game csgo" test "$(line_of "$CS/$gi" csgo/readyup)" -lt "$(line_of "$CS/$gi" csgo)"
    if [[ $mm == 1 ]]; then
      check "$gi: readyup right after metamod" test "$(line_of "$CS/$gi" csgo/readyup)" = "$(($(line_of "$CS/$gi" csgo/addons/metamod) + 1))"
    fi
    check "$gi: backup written" compgen -G "$CS/$gi.readyup-backup-*" >/dev/null
  done

  echo "== rerun is a no-op; user config kept"
  echo "// my edit" >>"$CS/cfg/ReadyUp/live.cfg"
  echo "debug=1" >>"$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  cp "$CS/gameinfo.gi" "$T/gi.before"
  nbak="$(compgen -G "$CS/gameinfo.gi.readyup-backup-*" | wc -l)"
  run --dir "$S" --zip "$ESS" essentials >"$T/out" 2>&1 || { cat "$T/out"; fail "rerun exited non-zero"; }
  check "gameinfo.gi unchanged on rerun" cmp -s "$T/gi.before" "$CS/gameinfo.gi"
  check "no new backup on rerun" test "$(compgen -G "$CS/gameinfo.gi.readyup-backup-*" | wc -l)" = "$nbak"
  check "edited live.cfg kept" grep -q "my edit" "$CS/cfg/ReadyUp/live.cfg"
  check "edited readyup.cfg kept" grep -q "^debug=1" "$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  check "no .default when the template did not change" test ! -e "$CS/cfg/ReadyUp/live.cfg.default"
  check "rerun reports up to date" grep -q "already up to date" "$T/out"
  check "rerun uses the saved license choice" grep -q "license: noncommercial use (accepted earlier" "$T/out"

  echo "== changed template -> .default next to the user's copy"
  echo "// older template" >>"$CS/readyup/cfg-templates/ReadyUp/live.cfg"
  run --dir "$S" --zip "$ESS" -y >"$T/out" 2>&1 || { cat "$T/out"; fail "update exited non-zero"; }
  check "live.cfg.default written" test -f "$CS/cfg/ReadyUp/live.cfg.default"
  check "user live.cfg still kept" grep -q "my edit" "$CS/cfg/ReadyUp/live.cfg"
done

S="$T/server-mm0"
CS="$S/game/csgo"
echo "== add skins from the full zip, then remove it"
run --dir "$S" --zip "$FULL" skins >"$T/out" 2>&1 || { cat "$T/out"; fail "skins install exited non-zero"; }
check "skins.so installed" test -f "$CS/readyup/plugins/skins.so"
check "skins gamedata next to the core" test -f "$CS/readyup/bin/linuxsteamrt64/engine-surface.skins.json"
check "installed.json has skins" grep -q '"skins"' "$CS/readyup/installed.json"
run --dir "$S" --remove skins >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove skins exited non-zero"; }
check "skins.so removed" test ! -e "$CS/readyup/plugins/skins.so"
check "skins gamedata removed" test ! -e "$CS/readyup/bin/linuxsteamrt64/engine-surface.skins.json"
check "core still installed" test -f "$CS/readyup/bin/linuxsteamrt64/libserver.so"
check "installed.json back to core essentials fleet match practice" test "$(installed "$S")" = "core essentials fleet mapvote match practice"

echo "== add midas from the full zip (off by default), then remove it"
run --dir "$S" --zip "$FULL" midas >"$T/out" 2>&1 || { cat "$T/out"; fail "midas install exited non-zero"; }
check "midas.so installed" test -x "$CS/readyup/plugins/midas.so"
check "midas.cfg seeded, all comments (midas stays off)" idle_cfg "$CS/cfg/ReadyUp/midas.cfg"
run --dir "$S" --remove midas >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove midas exited non-zero"; }
check "midas.so removed" test ! -e "$CS/readyup/plugins/midas.so"

echo "== add whitelist from the full zip, then remove it"
run --dir "$S" --zip "$FULL" whitelist >"$T/out" 2>&1 || { cat "$T/out"; fail "whitelist install exited non-zero"; }
check "whitelist.so installed" test -x "$CS/readyup/plugins/whitelist.so"
run --dir "$S" --remove whitelist >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove whitelist exited non-zero"; }
check "whitelist.so removed" test ! -e "$CS/readyup/plugins/whitelist.so"

echo "== add deathmatch from the full zip (off until .ru dm ffa|tdm), then remove it"
run --dir "$S" --zip "$FULL" deathmatch >"$T/out" 2>&1 || { cat "$T/out"; fail "deathmatch install exited non-zero"; }
check "deathmatch.so installed" test -x "$CS/readyup/plugins/deathmatch.so"
check "deathmatch.cfg seeded, all comments (defaults)" idle_cfg "$CS/cfg/ReadyUp/deathmatch.cfg"
run --dir "$S" --remove deathmatch >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove deathmatch exited non-zero"; }
check "deathmatch.so removed" test ! -e "$CS/readyup/plugins/deathmatch.so"

echo "== add addons from the full zip (idle until workshop_addons is set), then remove it"
run --dir "$S" --zip "$FULL" addons >"$T/out" 2>&1 || { cat "$T/out"; fail "addons install exited non-zero"; }
check "addons.so installed" test -x "$CS/readyup/plugins/addons.so"
check "addons gamedata next to the core" test -f "$CS/readyup/bin/linuxsteamrt64/engine-surface.addons.json"
check "addons.cfg seeded, all comments (no workshop_addons)" idle_cfg "$CS/cfg/ReadyUp/addons.cfg"
run --dir "$S" --remove addons >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove addons exited non-zero"; }
check "addons.so removed" test ! -e "$CS/readyup/plugins/addons.so"
check "addons gamedata removed" test ! -e "$CS/readyup/bin/linuxsteamrt64/engine-surface.addons.json"

echo "== remove fleet (its data dir and user fleet.cfg stay), add it back from the fleet zip"
mkdir -p "$CS/readyup/plugins/fleet" && echo '{}' >"$CS/readyup/plugins/fleet/credentials.json"
echo "url = https://t.example.com" >>"$CS/cfg/ReadyUp/fleet.cfg"
run --dir "$S" --remove fleet >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove fleet exited non-zero"; }
check "fleet.so removed" test ! -e "$CS/readyup/plugins/fleet.so"
check "fleet credentials kept" test -f "$CS/readyup/plugins/fleet/credentials.json"
check "user fleet.cfg kept" grep -q "t.example.com" "$CS/cfg/ReadyUp/fleet.cfg"
check "installed.json is core essentials match practice" test "$(installed "$S")" = "core essentials mapvote match practice"
run --dir "$S" --zip "$(ls "$DIST"/ready-up-fleet-*.zip)" fleet >"$T/out" 2>&1 || { cat "$T/out"; fail "fleet install exited non-zero"; }
check "fleet.so back" test -x "$CS/readyup/plugins/fleet.so"
check "user fleet.cfg not overwritten" grep -q "t.example.com" "$CS/cfg/ReadyUp/fleet.cfg"
check "installed.json is core essentials fleet match practice again" test "$(installed "$S")" = "core essentials fleet mapvote match practice"

echo "== older install (core + match, before practice / essentials were split out) -> update adds both"
S3="$T/server-old"
make_server "$S3" 0
CS3="$S3/game/csgo"
run --dir "$S3" --zip "$(ls "$DIST"/ready-up-core-*.zip)" --zip "$(ls "$DIST"/ready-up-match-*.zip)" \
  --accept-license=noncommercial core match >"$T/out" 2>&1 || { cat "$T/out"; fail "core + match install exited non-zero"; }
check "old-style install is core match" test "$(installed "$S3")" = "core match"
run --dir "$S3" --zip "$ESS" -y >"$T/out" 2>&1 || { cat "$T/out"; fail "update of the old install exited non-zero"; }
check "update added practice.so" test -x "$CS3/readyup/plugins/practice.so"
check "update added essentials.so" test -x "$CS3/readyup/plugins/essentials.so"
check "update did not add fleet (not asked for)" test ! -e "$CS3/readyup/plugins/fleet.so"
check "installed.json is core essentials match practice" test "$(installed "$S3")" = "core essentials match practice"

echo "== full bundle by name"
S4="$T/server-full"
make_server "$S4" 0
run --dir "$S4" --zip "$FULL" --accept-license=noncommercial full >"$T/out" 2>&1 || { cat "$T/out"; fail "full install exited non-zero"; }
# (+ tools: the offline gamedata checkers, when the build had them)
check "installed.json is every component" \
  test "$(installed "$S4" | sed "s/ tools / /; s/ tools$//")" = "addons community core deathmatch essentials fleet mapvote match midas movement practice skins tv whitelist"
for so in match fleet practice essentials skins midas whitelist deathmatch mapvote community movement addons tv; do
  check "full: $so.so installed" test -x "$S4/game/csgo/readyup/plugins/$so.so"
done

if command -v script >/dev/null 2>&1; then
  echo "== numbered picker (TERM=dumb) through a pty"
  printf '1,2,3,4\ny\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S' --zip '$FULL'" /dev/null >"$T/out" 2>&1 || true
  check "numbered picker installed skins" test -f "$CS/readyup/plugins/skins.so"
  check "numbered picker showed the prompt" grep -q "Components to install" "$T/out"

  echo "== arrow-key picker through a pty: untick skins (down x3, space, enter), confirm"
  printf '\e[B\e[B\e[B \ny\n' | script -qec "TERM=xterm bash '$INSTALL' --dir '$S' --zip '$FULL'" /dev/null >"$T/out" 2>&1 || true
  check "arrow picker removed skins" test ! -e "$CS/readyup/plugins/skins.so"
  check "arrow picker kept core" test -f "$CS/readyup/bin/linuxsteamrt64/libserver.so"

  echo "== license prompt through a pty: the use, then \"I AGREE\" (anything else installs nothing)"
  S2="$T/server-prompt"
  make_server "$S2" 0
  printf '1\nyes\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S2' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "\"yes\" is not I AGREE: nothing installed" test ! -e "$S2/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "declined prompt saves nothing" test ! -e "$S2/game/csgo/cfg/ReadyUp/license.cfg"
  check "the summary links the license" grep -q "polyformproject.org/licenses/noncommercial" "$T/out"
  check "the prompt asks for I AGREE" grep -q "Type I AGREE" "$T/out"
  printf '\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S2' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "no answer installs nothing" test ! -e "$S2/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  printf '1\ni agree\n\ny\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S2' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "noncommercial + i agree (any case) installs" test -f "$S2/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "license.cfg: noncommercial, prompt" test "$(lic "$S2")" = "noncommercial prompt Z"

  echo "== license prompt through a pty: commercial, key pasted"
  S6="$T/server-prompt-commercial"
  make_server "$S6" 0
  printf '2\nnot-a-key\n%s\nI AGREE\n\ny\n' "$TEST_KEY" | script -qec "TERM=dumb bash '$INSTALL' --dir '$S6' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "commercial answer names the contact" grep -q "sivert@autotournament.gg" "$T/out"
  check "a malformed pasted key is asked again" grep -q "does not look like a Ready Up license key" "$T/out"
  check "commercial + I AGREE installs" test -f "$S6/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "license.cfg: commercial, prompt" test "$(lic "$S6")" = "commercial prompt Z"
  check "pasted key saved" test "$(key_of "$S6")" = "$TEST_KEY"
  S7="$T/server-prompt-commercial-later"
  make_server "$S7" 0
  printf '2\n\nI AGREE\n\ny\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S7' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "commercial, key later: installs" test -f "$S7/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "commercial, key later: no key file" test ! -e "$S7/game/csgo/cfg/readyup_license.cfg"
  check "commercial, key later: says how to add it" grep -q "license-key" "$T/out"

  echo "== answering n changes nothing"
  printf '\ny\n' >/dev/null
  printf '\nn\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S' --zip '$FULL'" /dev/null >"$T/out" 2>&1 || true
  check "declined run said nothing changed" grep -q "Nothing changed" "$T/out"
else
  echo "SKIP pty picker tests (no \`script\`)"
fi

if command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1; then
  echo "== release mode against a fake GitHub API (local http server)"
  W="$T/www"
  mkdir -p "$W/repos/test/ready-up/releases"
  cp "$DIST"/ready-up-*.zip "$DIST/SHA256SUMS" "$W/"
  PORT=$((20000 + RANDOM % 20000))
  python3 - "$W" "$PORT" "$DIST" <<'PY'
import json, os, re, sys
www, port, dist = sys.argv[1], sys.argv[2], sys.argv[3]
names = sorted(f for f in os.listdir(dist) if f.endswith(".zip") or f == "SHA256SUMS")
ver = next(re.match(r"ready-up-core-(.*)-linuxsteamrt64\.zip", n).group(1) for n in names if n.startswith("ready-up-core-"))
rel = {"tag_name": "v" + ver, "body": "Test release notes line 1\nline 2",
       "assets": [{"name": n, "browser_download_url": "http://127.0.0.1:%s/%s" % (port, n)} for n in names]}
json.dump(rel, open(os.path.join(www, "repos/test/ready-up/releases/latest"), "w"))
PY
  (cd "$W" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
  HTTP_PID=$!
  for _ in $(seq 50); do
    (exec 9<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
    sleep 0.1
  done
  S="$T/server-release"
  make_server "$S" 0
  READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up run --dir "$S" --accept-license=noncommercial essentials >"$T/out" 2>&1 ||
    { cat "$T/out"; fail "release-mode install exited non-zero"; }
  check "release mode installed core" test -f "$S/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "release mode: essentials.so from ready-up-essentials-plugin-*" test -x "$S/game/csgo/readyup/plugins/essentials.so"
  check "release mode installed core essentials fleet match practice" \
    test "$(installed "$S")" = "core essentials fleet mapvote match practice"
  check "release mode printed the release notes" grep -q "Test release notes line 1" "$T/out"
  # Pretend an older core is installed: the update shows old -> new.
  python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["components"]["core"]="0.0.1"; json.dump(d,open(p,"w"))' \
    "$S/game/csgo/readyup/installed.json"
  READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up run --dir "$S" -y >"$T/out" 2>&1 ||
    { cat "$T/out"; fail "release-mode update exited non-zero"; }
  check "update shows old → new" grep -q "core 0.0.1 → " "$T/out"

  echo "== beta channel: only pre-releases published (test/ready-up-beta)"
  # Same zips, served as a pre-release tagged v<version>-beta.1 (the tag is what the installer
  # asks for; the files keep their names). releases/latest does not exist, like on GitHub when
  # there is no stable release; the list has an older stable-looking draft that must be ignored.
  mkdir -p "$W/repos/test/ready-up-beta/releases/tags"
  python3 - "$W" "$PORT" "$DIST" <<'PY'
import json, os, re, sys
www, port, dist = sys.argv[1], sys.argv[2], sys.argv[3]
names = sorted(f for f in os.listdir(dist) if f.endswith(".zip") or f == "SHA256SUMS")
ver = next(re.match(r"ready-up-core-(.*)-linuxsteamrt64\.zip", n).group(1) for n in names if n.startswith("ready-up-core-"))
assets = [{"name": n, "browser_download_url": "http://127.0.0.1:%s/%s" % (port, n)} for n in names]
beta = {"tag_name": "v%s-beta.1" % ver, "prerelease": True, "draft": False, "published_at": "2026-09-29T10:00:00Z",
        "body": "Beta notes line 1", "assets": assets}
old = {"tag_name": "v0.0.1-beta.1", "prerelease": True, "draft": False, "published_at": "2026-01-01T00:00:00Z",
       "body": "old", "assets": []}
draft = {"tag_name": "v9.9.9", "prerelease": False, "draft": True, "published_at": None, "body": "draft", "assets": []}
base = os.path.join(www, "repos/test/ready-up-beta/releases")
# GET .../releases?per_page=30: http.server redirects the directory to .../releases/ and serves index.html.
json.dump([old, beta, draft], open(os.path.join(base, "index.html"), "w"))
json.dump(beta, open(os.path.join(base, "tags", beta["tag_name"]), "w"))
open(os.path.join(www, "beta-tag"), "w").write(beta["tag_name"])
PY
  BETA_TAG="$(cat "$W/beta-tag")"
  beta_run() { READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up-beta run "$@"; }
  S="$T/server-beta"
  make_server "$S" 0
  if beta_run --dir "$S" --accept-license=noncommercial essentials >"$T/out" 2>&1; then
    fail "default channel installed a pre-release"
  else
    check "no stable release: points to --channel beta" grep -q -- "--channel beta" "$T/out"
    check "no stable release: names the newest pre-release for --version" grep -q -- "--version $BETA_TAG" "$T/out"
    check "no stable release: nothing installed" test ! -e "$S/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  fi
  beta_run --dir "$S" --accept-license=noncommercial --channel beta essentials >"$T/out" 2>&1 ||
    { cat "$T/out"; fail "--channel beta install exited non-zero"; }
  check "--channel beta installed core" test -f "$S/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "--channel beta took the newest pre-release" grep -q "Release notes ($BETA_TAG)" "$T/out"
  check "--channel beta warns it is a pre-release" grep -q "is a pre-release" "$T/out"
  S="$T/server-beta-version"
  make_server "$S" 0
  beta_run --dir "$S" --accept-license=noncommercial --version "${BETA_TAG#v}" essentials >"$T/out" 2>&1 ||
    { cat "$T/out"; fail "--version <pre-release> install exited non-zero"; }
  check "--version without v installs the pre-release" grep -q "Release notes ($BETA_TAG)" "$T/out"
  if beta_run --dir "$S" --version "$BETA_TAG" --channel beta -y >"$T/out" 2>&1; then
    fail "--version with --channel accepted"
  else
    check "--version and --channel together refused" grep -q "not both" "$T/out"
  fi
  if beta_run --dir "$S" --channel nightly -y >"$T/out" 2>&1; then
    fail "--channel nightly accepted"
  else
    check "unknown channel refused" grep -q "must be stable or beta" "$T/out"
  fi
  # A repo with a stable release: --channel beta still picks the newest (here the stable one is newest).
  READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up run --dir "$S" --channel stable -y >"$T/out" 2>&1 ||
    { cat "$T/out"; fail "--channel stable update exited non-zero"; }
  check "--channel stable uses releases/latest" grep -q "Test release notes line 1" "$T/out"
  echo "tampered" >>"$W/$(basename "$ESS" | sed 's/essentials/core/')"
  if READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up run --dir "$S" core >"$T/out" 2>&1; then
    fail "a zip that does not match SHA256SUMS was installed"
  else
    check "checksum mismatch refused" grep -q "checksum mismatch" "$T/out"
  fi
  kill "$HTTP_PID" 2>/dev/null || true
fi

echo "== not a server root"
if (cd "$T" && bash "$INSTALL" --zip "$ESS" -y </dev/null >"$T/out" 2>&1); then
  fail "ran outside a server root"
else
  check "clear error outside a server root" grep -q "not a CS2 server root" "$T/out"
fi

echo "== uninstall keeps config; --purge deletes it"
for mm in 0 1; do
  S="$T/server-mm$mm"
  CS="$S/game/csgo"
  run --dir "$S" --uninstall >"$T/out" 2>&1 || { cat "$T/out"; fail "uninstall exited non-zero"; }
  for gi in gameinfo.gi gameinfo_branchspecific.gi; do
    check "$gi: readyup line removed (metamod=$mm)" test "$(count_ru "$CS/$gi")" = 0
    check "$gi: Game csgo still there" test -n "$(line_of "$CS/$gi" csgo)"
  done
  [[ $mm == 1 ]] && check "metamod line untouched" test -n "$(line_of "$CS/gameinfo.gi" csgo/addons/metamod)"
  check "libserver.so removed" test ! -e "$CS/readyup/bin/linuxsteamrt64/libserver.so"
  check "readyup.cfg kept" test -f "$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  check "cfg/ReadyUp kept" test -f "$CS/cfg/ReadyUp/live.cfg"
done
S="$T/server-mm0"
run --dir "$S" --uninstall --purge >"$T/out" 2>&1 || { cat "$T/out"; fail "purge exited non-zero"; }
check "--purge removed readyup/" test ! -e "$S/game/csgo/readyup"
check "--purge removed cfg/ReadyUp" test ! -e "$S/game/csgo/cfg/ReadyUp"

echo
if [[ $fails -ne 0 ]]; then
  echo "installer tests: $fails FAILED"
  exit 1
fi
echo "installer tests: ALL OK"
