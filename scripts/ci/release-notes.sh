#!/usr/bin/env bash
# Release notes for a tag (used by .github/workflows/build.yml).
#
#   scripts/ci/release-notes.sh v1.2.3 [cs2-build.env] [dist-dir] > notes.md
#
# The download table is built from the zips in <dist-dir> (scripts/package-release.sh output):
# each zip's readyup/manifests/*.json names its component(s) and description, so a new
# component shows up here without editing this script. A tag with a suffix (v1.2.3-beta.1,
# v1.2.3-rc.1) gets pre-release wording and an install command that pins the tag.
set -euo pipefail

TAG="${1:?usage: $0 <tag> [cs2-build.env] [dist-dir]}"
ENV_FILE="${2:-}"
DIST="${3:-}"
VERSION="${TAG#v}"
PRERELEASE=0
[[ "$VERSION" == *-* ]] && PRERELEASE=1

# Pre-releases sort before their release (v1.2.3-beta.1 < v1.2.3-rc.1 < v1.2.3). A stable
# release lists the changes since the previous stable one; a pre-release since the previous tag.
tags="$(git -c versionsort.suffix=-beta. -c versionsort.suffix=-rc. tag --sort=-v:refname --merged "$TAG" 2>/dev/null |
  grep -E '^v[0-9]' | grep -vx "$TAG" || true)"
if [[ $PRERELEASE -eq 0 ]]; then
  tags="$(grep -v -- '-' <<<"$tags" || true)"
fi
prev="$(head -n1 <<<"$tags")"
range="${prev:+$prev..}$TAG"
changes="$(git log "$range" --no-merges --pretty='- %s' 2>/dev/null |
  grep -viE '^- Release v[0-9]+\.[0-9]+\.[0-9]+(-[0-9a-z.]+)?$' | head -n 100 || true)"
[[ -n "$changes" ]] || changes="- Release $TAG"

BUILDID="" PATCH_VERSION=""
# shellcheck disable=SC1090
[[ -n "$ENV_FILE" && -f "$ENV_FILE" ]] && source "$ENV_FILE"

REPO="${GITHUB_REPOSITORY:-Auto-Tournament/ready-up}"
INSTALL_URL="https://raw.githubusercontent.com/$REPO/master/install.sh"
INSTALL_ENV=""
# A fork's release: the installer fetches from the fork only with READYUP_REPO.
if [[ "$REPO" != "Auto-Tournament/ready-up" ]]; then
  INSTALL_URL="https://raw.githubusercontent.com/$REPO/$TAG/install.sh"
  INSTALL_ENV="READYUP_REPO=$REPO "
fi

if [[ $PRERELEASE -eq 1 ]]; then
  cat <<EOF
> **Pre-release** for testing. It is not marked latest: the installer's default and the
> platform's update check keep the last stable release. Install it on purpose with
> \`--version $TAG\` (this build) or \`--channel beta\` (the newest build, pre-releases included).

EOF
fi

cat <<EOF
## Changes${prev:+ since $prev}

$changes

## Install

Ready Up runs on Linux dedicated servers (\`linuxsteamrt64\`). From the server root (the folder with \`game/\`):

EOF
if [[ $PRERELEASE -eq 1 ]]; then
  cat <<EOF
\`\`\`bash
curl -fsSL $INSTALL_URL | ${INSTALL_ENV}bash -s -- --version $TAG
\`\`\`
EOF
else
  cat <<EOF
\`\`\`bash
curl -fsSL $INSTALL_URL | ${INSTALL_ENV}bash
\`\`\`
EOF
fi
cat <<EOF

It shows the components (installed -> $VERSION), installs or updates what you tick, patches \`gameinfo.gi\` (after Metamod if present) and keeps your config. Add \`essentials --accept-license=noncommercial\` to install without questions; run it again to update.

EOF

if [[ -n "$DIST" && -d "$DIST" ]]; then
  python3 - "$DIST" "$VERSION" <<'PY'
import json, os, sys, zipfile

dist, version = sys.argv[1], sys.argv[2]
suffix = "-%s-linuxsteamrt64.zip" % version
zips = sorted(f for f in os.listdir(dist) if f.startswith("ready-up-") and f.endswith(suffix))
if not zips:
    sys.exit("release-notes: no ready-up-*%s in %s" % (suffix, dist))

def manifests(path):
    out = {}
    with zipfile.ZipFile(path) as z:
        for n in z.namelist():
            if n.startswith("readyup/manifests/") and n.endswith(".json"):
                m = json.loads(z.read(n))
                out[m.get("component") or os.path.basename(n)[:-5]] = m.get("description", "")
    return out

# The order package-release.sh builds them in: core first, the rest as the installer lists them.
ORDER = ["core", "essentials", "match", "fleet", "practice", "skins", "hello", "midas",
         "whitelist", "deathmatch", "addons", "tools"]
def rank(c):
    return (ORDER.index(c) if c in ORDER else len(ORDER), c)

bundles, singles = [], []
for f in zips:
    comps = manifests(os.path.join(dist, f))
    if not comps:
        sys.exit("release-notes: %s has no readyup/manifests/*.json" % f)
    (bundles if len(comps) > 1 else singles).append((f, comps))

rows = []
for f, comps in sorted(bundles, key=lambda b: len(b[1])):
    names = sorted(comps, key=rank)
    text = "Bundle: " + " + ".join(c for c in names if c != "tools")
    if "tools" in comps:
        text += " + offline gamedata checkers"
    if "skins" not in comps:
        text += ". No skins"
    if f.startswith("ready-up-essentials-") and not f.startswith("ready-up-essentials-plugin-"):
        text += ". The default"
    rows.append((f, text + "."))
for f, comps in sorted(singles, key=lambda s: rank(next(iter(s[1])))):
    c, desc = next(iter(comps.items()))
    rows.append((f, "`%s` alone: %s" % (c, desc.replace("|", "/"))))
if os.path.exists(os.path.join(dist, "SHA256SUMS")):
    rows.append(("SHA256SUMS", "checksums of every zip (the installer verifies them)"))

print("| Download | Contents |")
print("|---|---|")
for f, text in rows:
    print("| `%s` | %s |" % (f, text))
PY
else
  echo "Every component comes as its own \`ready-up-<component>-$VERSION-linuxsteamrt64.zip\`, plus the \`essentials\` and \`full\` bundles and \`SHA256SUMS\` (see the assets below)."
fi

cat <<EOF

Skins (weapon paints, knives, gloves, agents) can get a server's GSLT banned; only install them on purpose.

Manual install: extract a zip into \`game/csgo\`, then \`python3 readyup/tools/patch_gameinfo.py gameinfo.gi\` (and \`gameinfo_branchspecific.gi\`), and restart. CS2 updates rewrite \`gameinfo.gi\`: run the installer (or the patcher) again after each one.

## Build

Built in the Steam Runtime 3 "sniper" SDK. OpenSSL, libcurl, libstdc++ and libgcc are linked in, so the core and plugins only need glibc 2.31+ on the host. \`ru version\` and the platform show this build as \`$VERSION (<commit>)\`.
EOF
if [[ -n "$BUILDID" ]]; then
  echo
  echo "Engine surface verified against CS2 build \`$BUILDID\`${PATCH_VERSION:+ (PatchVersion $PATCH_VERSION)}."
fi
