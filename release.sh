#!/usr/bin/env bash
# Cut a Ready Up release: bump VERSION, commit, tag vX.Y.Z (or vX.Y.Z-beta.N / vX.Y.Z-rc.N) and push.
#
#   ./release.sh [--dry-run] [major|minor|patch|X.Y.Z|X.Y.Z-beta.N|X.Y.Z-rc.N]
#
#   (no version)     release the current VERSION
#   major|minor|patch  bump the current stable VERSION (refused while VERSION is a pre-release:
#                    name the version instead, e.g. 0.1.0 to finish 0.1.0-rc.2)
#   X.Y.Z            a stable release: the GitHub release is marked "latest"
#   X.Y.Z-beta.N, X.Y.Z-rc.N
#                    a pre-release (N >= 1): published with --prerelease, never "latest", so
#                    `install.sh` without flags and anything reading releases/latest skip it.
#                    Testers install it with `install.sh --channel beta` or `--version vX.Y.Z-beta.N`.
#   --dry-run        print what would happen (VERSION change, commit, tag, push, release kind)
#                    and change nothing. Needs no GitHub login.
#   A leading "v" (v0.1.0-beta.1) is accepted.
#
# The tag push triggers .github/workflows/build.yml, which builds libserver.so in the
# Steam Runtime sniper SDK, verifies it against the current CS2 build, packages the component
# zips and the essentials / full bundles (scripts/package-release.sh) and publishes the GitHub
# release (+ Discord announcement if the DISCORD_WEBHOOK_URL secret is set).
# Nothing is built or uploaded from this machine. See docs/RELEASING.md.
#
# Env: WATCH=0 to not follow the CI run after pushing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

REPO="Auto-Tournament/ready-up"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' BLUE='\033[0;34m' NC='\033[0m'
else
  RED='' GREEN='' YELLOW='' BLUE='' NC=''
fi

USAGE="Usage: ./release.sh [--dry-run] [major|minor|patch|X.Y.Z|X.Y.Z-beta.N|X.Y.Z-rc.N]"

die() {
  echo -e "${RED}ERROR:${NC} $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

read_version_file() {
  [[ -f "$ROOT_DIR/VERSION" ]] || die "VERSION file not found: $ROOT_DIR/VERSION"
  tr -d ' \t\r\n' <"$ROOT_DIR/VERSION"
}

# X.Y.Z, optionally -beta.N or -rc.N, optionally -custom.N (fork builds) (N >= 1). No leading zeros (SemVer 2.0.0).
NUM='(0|[1-9][0-9]*)'
SEMVER_RE="^${NUM}\.${NUM}\.${NUM}(-(beta|rc)\.[1-9][0-9]*)?(-custom\.[1-9][0-9]*)?$"
validate_semver() {
  [[ "$1" =~ $SEMVER_RE ]]
}
is_prerelease() {
  [[ "$1" == *-* ]]
}

bump_version() {
  local current="$1"
  local bump="$2"
  local major minor patch
  IFS=. read -r major minor patch <<<"$current"
  case "$bump" in
    major) major=$((major + 1)); minor=0; patch=0 ;;
    minor) minor=$((minor + 1)); patch=0 ;;
    patch) patch=$((patch + 1)) ;;
    *) return 1 ;;
  esac
  echo "${major}.${minor}.${patch}"
}

DRY_RUN=0
ARG="none"
while [[ $# -gt 0 ]]; do
  case "$1" in
    -n | --dry-run) DRY_RUN=1 ;;
    -h | --help)
      sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'
      exit 0
      ;;
    -*) die "Unknown option: '$1'. $USAGE" ;;
    *)
      [[ "$ARG" == "none" ]] || die "Only one version argument is allowed (got '$ARG' and '$1'). $USAGE"
      ARG="$1"
      ;;
  esac
  shift
done

require_cmd git

CURRENT_VERSION="$(read_version_file)"
validate_semver "$CURRENT_VERSION" || die "Invalid VERSION file: '$CURRENT_VERSION' (expected X.Y.Z, X.Y.Z-beta.N or X.Y.Z-rc.N)"

NEW_VERSION="$CURRENT_VERSION"
MODE="none"
ARG="${ARG#v}"

if [[ "$ARG" == "major" || "$ARG" == "minor" || "$ARG" == "patch" ]]; then
  if is_prerelease "$CURRENT_VERSION"; then
    die "VERSION is a pre-release ($CURRENT_VERSION); '$ARG' is ambiguous. Name the version, e.g. ./release.sh ${CURRENT_VERSION%%-*}"
  fi
  NEW_VERSION="$(bump_version "$CURRENT_VERSION" "$ARG")"
  MODE="$ARG"
elif [[ "$ARG" != "none" ]]; then
  validate_semver "$ARG" || die "Invalid version: '$ARG'. $USAGE (N >= 1, no leading zeros)"
  NEW_VERSION="$ARG"
  MODE="explicit"
fi

validate_semver "$NEW_VERSION" || die "Invalid new version: '$NEW_VERSION'"

if is_prerelease "$NEW_VERSION"; then
  KIND="pre-release"
  GH_FLAGS="--prerelease (not marked latest)"
else
  KIND="stable"
  GH_FLAGS="--latest"
fi

if [[ $DRY_RUN -eq 0 ]]; then
  require_cmd gh
  if ! gh auth status >/dev/null 2>&1; then
    die "GitHub CLI is not authenticated. Run: gh auth login"
  fi
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
  die "Working tree is not clean. Commit or stash changes before releasing."
fi

TAG="v${NEW_VERSION}"
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null 2>&1; then
  die "Tag ${TAG} already exists."
fi
if git ls-remote --tags origin "refs/tags/${TAG}" 2>/dev/null | grep -q "refs/tags/${TAG}$"; then
  die "Remote tag ${TAG} already exists on origin."
fi

echo -e "${GREEN}Ready Up release${NC}$([[ $DRY_RUN -eq 1 ]] && echo " (dry run: nothing is changed)")"
echo "========================================="
echo -e "${BLUE}Repository:${NC}      ${REPO}"
echo -e "${BLUE}Current version:${NC} ${CURRENT_VERSION}"
echo -e "${BLUE}New version:${NC}     ${NEW_VERSION} (${MODE})"
echo -e "${BLUE}Kind:${NC}            ${KIND} (GitHub release ${GH_FLAGS})"
echo -e "${BLUE}Tag:${NC}             ${TAG}"
echo -e "${BLUE}Git branch:${NC}      $(git branch --show-current 2>/dev/null || echo unknown)"
echo ""

if [[ $DRY_RUN -eq 1 ]]; then
  echo "Would run:"
  if [[ "$NEW_VERSION" != "$CURRENT_VERSION" ]]; then
    echo "  write VERSION: ${CURRENT_VERSION} -> ${NEW_VERSION}"
    echo "  git add VERSION && git commit -m \"Release ${TAG}\""
  else
    echo "  (VERSION already ${NEW_VERSION}; no commit)"
  fi
  echo "  git tag ${TAG}"
  echo "  git push -u origin HEAD"
  echo "  git push origin ${TAG}"
  echo "Then CI (.github/workflows/build.yml) builds, verifies and publishes"
  echo "  https://github.com/${REPO}/releases/tag/${TAG} as a ${KIND} release (${GH_FLAGS})."
  if [[ "$KIND" == "pre-release" ]]; then
    echo "Testers install it with: install.sh --channel beta   (or --version ${TAG})"
  fi
  exit 0
fi

echo "CI builds, verifies and publishes the release after the tag is pushed."
echo ""

read -rp "$(echo -e "${YELLOW}Continue with ${KIND} release ${TAG}? [y/N]: ${NC}")" CONFIRM
case "$CONFIRM" in
  y|Y|yes|YES) ;;
  *) echo "Aborted."; exit 0 ;;
esac

if [[ "$NEW_VERSION" != "$CURRENT_VERSION" ]]; then
  echo -e "${BLUE}Updating VERSION...${NC}"
  printf '%s\n' "$NEW_VERSION" >"$ROOT_DIR/VERSION"
  git add VERSION
  git commit -m "Release ${TAG}"
fi
git tag "$TAG"

echo -e "${BLUE}Pushing branch + tag...${NC}"
git push -u origin HEAD
git push origin "$TAG"

RELEASE_URL="https://github.com/${REPO}/releases/tag/${TAG}"
echo -e "${GREEN}Tag pushed.${NC} The Build workflow publishes ${RELEASE_URL} (${KIND})"

if [[ "${WATCH:-1}" == "1" ]]; then
  echo -e "${BLUE}Waiting for the Build run for ${TAG}...${NC}"
  run_id=""
  for _ in $(seq 1 30); do
    run_id="$(gh run list -R "$REPO" --workflow build.yml --branch "$TAG" --limit 1 --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null || true)"
    [[ -n "$run_id" ]] && break
    sleep 5
  done
  if [[ -n "$run_id" ]]; then
    if gh run watch -R "$REPO" "$run_id" --exit-status; then
      echo -e "${GREEN}Released:${NC} ${RELEASE_URL}"
    else
      die "Build/release run failed: https://github.com/${REPO}/actions/runs/${run_id}"
    fi
  else
    echo -e "${YELLOW}Could not find the run yet; check https://github.com/${REPO}/actions${NC}"
  fi
fi
