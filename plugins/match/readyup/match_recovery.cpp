// Server restart recovery (see match_recovery.h).
#include "readyup/match_recovery.h"

#include "readyup/cvar_snapshot.h"
#include "readyup/engine.h"
#include "readyup/fleet_bridge.h"
#include "readyup/game_timers.h"
#include "readyup/logging.h"
#include "readyup/map_names.h"
#include "readyup/match_config_parser.h"
#include "readyup/match_console.h"
#include "readyup/match_events.h"
#include "readyup/match_log.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/persisted_match_state.h"
#include "readyup/round_restore.h"
#include "readyup/round_restore_rules.h"
#include "readyup/webhook.h"

#include <tuple>
#include <algorithm>
#include <atomic>
#include <optional>
#include <string>

namespace readyup::match_recovery {
namespace {

// Game thread.
bool g_pending = false;          // waiting for the match map (Step)
bool g_changeRequested = false;  // the changelevel to it went out
restore::RecoveryPlan g_plan;
std::optional<restore::Progress> g_progress;

std::atomic<bool> g_saveQueued{false};

// The map number is the plan's, whatever map tracking derived (it was seeded before the match
// context came back).
void SetMapNumber(int mapNumber) {
  MatchLogState ls = MatchLogSnapshot();
  ls.mapNumber = mapNumber;
  MatchLogRestore(ls);
  MatchStateSetMap(mapNumber, MatchStateGet().current_map);
}

// On the match map, the map was live: continue it from the newest round backup.
void RestoreLiveMap() {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return;
  // The platform assigned (or resumes) the match since Step looked: it restores it, not us.
  if (fleet_bridge::Assigned() || fleet_bridge::ResumeWaiting()) {
    Print("recovery: the platform resumes the match; local recovery stands down\n");
    return;
  }
  const int mapNumber = g_plan.map_number;
  // A restart reset these (match load / go-live set them).
  round_restore::EnableRoundBackups(ctx->matchid, mapNumber);
  (void)EnqueueServerCommand("mp_backup_restore_load_autopause 1");

  if (g_progress) {
    stats::MapStats m;
    if (!g_progress->stats_json.empty() && stats::FromJson(g_progress->stats_json, &m)) {
      m.live = true;
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      stats::Current().Restore(m);
    }
    if (g_progress->have_events) MatchEventsRestore(g_progress->events);
  }

  const auto list = round_restore::ListBackups(ctx->matchid);
  const restore::BackupInfo* latest = restore::LatestForMap(list, mapNumber);
  int round = 1;
  std::string err;
  if (latest && fleet_bridge::RestoreRoundFromLocalBackup(latest->round, "server", "recovery", &err, nullptr)) {
    round = latest->round;
    Print("recovery: map %d restored at round %d (%s); waiting for every player to ready\n", mapNumber, round,
          latest->file.c_str());
  } else {
    // No backup: the map restarts from its first round once everyone is back.
    (void)EnqueueServerCommand("mp_pause_match");
    if (!PauseStateGet().paused) PauseStateOnPaused("admin", "server");
    Print("recovery: no round backup for map %d%s%s; the map continues without one\n", mapNumber,
          err.empty() ? "" : ": ", err.c_str());
  }
  // The platform rebuilds its view of the match (Auto Tournament: rounds played).
  WebhookEmitRecoverRequested(mapNumber, round - 1);
}

void Step() {
  if (!g_pending) return;
  // The platform owns the match once it assigned it (also a failover resume of the same match):
  // its match.assign says which map and round. Local recovery stands down instead of changing
  // the map back (a live test resumed map 2 on de_nuke, then recovery sent it to map 1).
  if (fleet_bridge::Assigned()) {
    g_pending = false;
    Print("recovery: the platform assigned a match; local recovery stands down\n");
    return;
  }
  const std::string current = MatchStateGet().current_map;
  if (current.empty()) return;  // no map yet: OnMapStart comes back here
  if (mapnames::EntryMatchesLoaded(g_plan.map_entry, current)) {
    g_pending = false;
    SetMapNumber(g_plan.map_number);
    if (g_plan.kind == restore::RecoveryPlan::Kind::Live) RestoreLiveMap();
    return;
  }
  if (!g_changeRequested) {
    g_changeRequested = true;
    Print("recovery: the server is on %s; changing to map %d (%s)\n", current.c_str(), g_plan.map_number,
          g_plan.map_entry.c_str());
    (void)LoadMapEntry(g_plan.map_entry);
  }
}

void Recover() {
  auto jsonOpt = persisted_match_state::GetActiveMatchJson();
  if (!jsonOpt) return;
  if (fleet_bridge::Assigned()) {
    Print("recovery: the platform assigned a match; local recovery stands down\n");
    return;
  }

  std::string parseErr;
  auto ctxOpt = ParseWebhookMatchContextFromJson(*jsonOpt, &parseErr);
  if (!ctxOpt) {
    Print("recovery: the saved match config does not parse (%s); not recovered\n",
          parseErr.empty() ? "unknown" : parseErr.c_str());
    return;
  }

  restore::Progress p;
  const auto progressJson = persisted_match_state::GetProgressJson();
  if (progressJson && restore::ProgressFromJson(*progressJson, &p)) g_progress = p;
  else g_progress.reset();
  g_plan = restore::PlanRecovery(g_progress ? &*g_progress : nullptr, persisted_match_state::GetLiveFlag(),
                                 ctxOpt->maplist, ctxOpt->num_maps, ctxOpt->clinch_series);
  const std::string name = ctxOpt->slug.empty() ? std::to_string(ctxOpt->matchid) : ctxOpt->slug;
  if (g_plan.kind == restore::RecoveryPlan::Kind::None) {
    Print("recovery: match %s not recovered: %s\n", name.c_str(), g_plan.why.c_str());
    persisted_match_state::ClearActiveMatch();
    cvar_snapshot::Discard();
    persisted_match_state::PersistCvarSnapshot({});
    return;
  }

  WebhookStartSenderThread();
  // The pre-match cvar values read before the restart (cvar_snapshot.h); setting the context
  // below reads only the names missing there (this fresh server still has its pre-match values).
  cvar_snapshot::RestorePersisted();
  WebhookSetMatchContext(*ctxOpt);
  // Boot mode is idle; a restored match needs match_warmup gating.
  SetModeMatchWarmupForRecovery();
  ModesSetSeriesWins(g_progress ? g_progress->series_team1 : 0, g_progress ? g_progress->series_team2 : 0);
  ClearReadyStates();
  WebhookSetHeartbeatStatus("warmup");
  ApplyMatchCvarsNow();
  SetMapNumber(g_plan.map_number);
  const bool live = g_plan.kind == restore::RecoveryPlan::Kind::Live;
  if (live) SetRecoveryGate(true);  // paused until every roster player is back and ready

  Print("recovery: match %s: %s -> map %d (%s) %s, series %d-%d\n", name.c_str(), g_plan.why.c_str(),
        g_plan.map_number, g_plan.map_entry.c_str(), live ? "live" : "warmup",
        g_progress ? g_progress->series_team1 : 0, g_progress ? g_progress->series_team2 : 0);
  g_pending = true;
  g_changeRequested = false;
  Step();
}

// Game thread.
void SaveProgress() {
  g_saveQueued.store(false);
  if (g_pending || RecoveryGateEnabled()) return;  // a recovered live map keeps its record until it resumes
  if (!persisted_match_state::GetActiveMatchJson()) return;  // scrims, practice: nothing to recover
  restore::Progress p;
  p.map_number = std::max(1, MatchStateGet().map_number);
  std::tie(p.series_team1, p.series_team2) = ModesGetSeriesWins();
  switch (GetMode()) {
    case ReadyUpMode::MatchLive: {
      p.phase = "live";
      {
        std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
        if (stats::Current().Live()) p.stats_json = stats::ToJson(stats::Current().Snapshot());
      }
      p.events = MatchEventsSnapshot();
      p.have_events = true;
      break;
    }
    case ReadyUpMode::Postgame: p.phase = "map_over"; break;
    case ReadyUpMode::MatchWarmup:
    case ReadyUpMode::MatchKnife: p.phase = "warmup"; break;
    default: return;
  }
  persisted_match_state::PersistProgress(restore::ProgressToJson(p));
}

}  // namespace

void TryRecoverAsync() {
  // Give other init a moment to run first (settings restore, the map seed).
  ScheduleOnGameThread(0.5, [] { Recover(); });
}

void OnMapStart() {
  if (!g_pending) return;
  // The map-load log lines (map tracking) arrive around the event; look a moment later.
  ScheduleOnGameThread(2.0, [] { Step(); });
}

void Cancel() {
  g_pending = false;
  g_changeRequested = false;
}

void NoteProgress() {
  if (g_saveQueued.exchange(true)) return;
  ScheduleOnGameThread(0, [] { SaveProgress(); });
}

}  // namespace readyup::match_recovery
