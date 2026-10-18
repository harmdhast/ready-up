#!/usr/bin/env bash
# The live stage of the CS2 dynamic check (.github/workflows/cs2-dynamic.yml): the bot live
# tests match, scrim, fleet and forfeit, one after the other on the same CI server.
#
# On a csm instance (CS2_CI_INSTANCE) the workflow runs this whole script inside
# `csm instance exec N`: the instance's view ($CS2_CI_DIR) exists only in that namespace, so
# the tmux server that runs the CI server (TMUX_TMPDIR, private to the run) has to start in it
# too, and every test reuses that one server. With a CS2 install of its own it runs as is.
#
# Needs: OUT (the run's temp dir), CS2_CI_DIR, CS2_CI_PORT, GITHUB_RUN_ID, LIVETEST_ADVISORY,
# GITHUB_STEP_SUMMARY, the COMPAT_STEP_* settings. Writes one `mode=rc` line per test to
# $OUT/live-results.txt. Exit 0 unless it could not run at all.
set -uo pipefail

: "${OUT:?}" "${CS2_CI_DIR:?}" "${CS2_CI_PORT:?}" "${GITHUB_RUN_ID:?}"
case "$OUT" in /*) ;; *) echo "OUT must be absolute" >&2; exit 1 ;; esac
LIVETEST_ADVISORY="${LIVETEST_ADVISORY:-}"
GITHUB_STEP_SUMMARY="${GITHUB_STEP_SUMMARY:-/dev/null}"
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1

# A throwaway target for scripts/livetest: run.sh starts the CI server in tmux, the console is
# piped into console.log (--boot). +log on as in the selftest boot: without it there are no
# round / bot log lines (the knife winner, round ends, bots' sides). -allow_round_backup_files:
# since the October 2026 update CS2 writes round backup files only with it (csm adds it since
# v1.27.4); the fleet test's event.backup / restore_round need them.
t="$OUT/livetest-target"
mkdir -p "$t"
cat > "$t/run.sh" <<EOF
#!/usr/bin/env bash
exec "$CS2_CI_DIR/game/cs2.sh" -dedicated -console -usercon -allow_round_backup_files -port "$CS2_CI_PORT" +sv_lan 1 +sv_hibernate_when_empty 0 +log on +game_type 0 +game_mode 1 +map de_dust2
EOF
chmod +x "$t/run.sh"
session="ru-ci-${GITHUB_RUN_ID}"
# A tmux server of this run's own (never the host's, which may run live servers): started here,
# so inside the instance's namespace, and killed with everything in it when the stage ends
# (in an instance the CI server holds the view).
export TMUX_TMPDIR="$OUT/tmux"
mkdir -p "$TMUX_TMPDIR"
trap 'tmux kill-server 2>/dev/null' EXIT

: > "$OUT/live-results.txt"
# match + scrim are required. fleet (scripts/livetest/fleet_livetest.py: a mock platform on
# loopback drives the fleet link) and forfeit (pauses + team-left forfeit) are newer: while
# listed in LIVETEST_ADVISORY a failure keeps their livetest check pending instead of failing
# the component (docs/CS2-COMPAT.md). Not here: --ruleset valve, which needs GOTV (tv_enable 1
# + a tv_port) on the CI server.
for mode in match scrim fleet forfeit; do
  python3 scripts/ci/compat-report.py step --id "live-$mode" --status running || true
  extra=()
  # COMPAT_STEP_EVENTS: the live test reports its own steps under live-<mode>; forfeit does not
  # (a run has at most 100 steps; its result is on live-forfeit).
  events=1
  case $mode in
    match) extra+=(--simulation) ;;  # match config `simulation: true`: Ready Up adds the bots
    scrim) extra+=(--scrim) ;;
    forfeit) extra+=(--forfeit); events=0 ;;
  esac
  if [ "$mode" = fleet ]; then
    # fleet.cfg + fleet.so's data dir are in the install ($CS2_CI_DIR), the console in $t; the
    # test moves both aside afterwards and reloads fleet.so standalone.
    COMPAT_STEP_EVENTS=1 COMPAT_STEP_PARENT="live-$mode" \
      python3 -u scripts/livetest/fleet_livetest.py --ssh '' --target "$t" --game-dir "$CS2_CI_DIR" \
        --session "$session" --boot --wait-session 1 --mock local --out "$OUT/livetest-$mode"
  else
    COMPAT_STEP_EVENTS=$events COMPAT_STEP_PARENT="live-$mode" \
      scripts/livetest/run.sh --ssh '' --target "$t" --session "$session" --boot --wait-session 1 --out "$OUT/livetest-$mode" "${extra[@]}"
  fi
  rc=$?
  advisory=""
  case " $LIVETEST_ADVISORY " in *" $mode "*) advisory=" (advisory: not required yet)" ;; esac
  echo "livetest $mode: exit $rc$advisory" | tee -a "$GITHUB_STEP_SUMMARY"
  echo "$mode=$rc" >> "$OUT/live-results.txt"
  case $rc in
    0) python3 scripts/ci/compat-report.py step --id "live-$mode" --status pass || true ;;
    2) python3 scripts/ci/compat-report.py step --id "live-$mode" --status skip --detail "no verdict (exit 2: server busy, unavailable or restarted)" || true ;;
    *) python3 scripts/ci/compat-report.py step --id "live-$mode" --status fail --detail "live test failed (exit $rc)$advisory" || true ;;
  esac
done
exit 0
