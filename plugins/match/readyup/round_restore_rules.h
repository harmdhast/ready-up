#pragma once

// Round restore and crash recovery: the pure half (no engine calls; ctest `match_restore`). The
// engine side is round_restore.h (commands, the pause after a restore) and match_recovery.h
// (server restart).
//
// Round numbers: `round` is the round a restore replays, 1-based (`.restore 5` plays round 5
// again, from its start). CS2 names the backup it writes at the start of round N
// "<prefix>round<N-1>.txt" (rounds played so far); that N-1 is what the `backup_loaded` webhook
// reports as round_number (the Auto Tournament CS2 contract).

#include "readyup/match_events.h"
#include "readyup/match_stats.h"

#include <cstdint>
#include <string>
#include <vector>

namespace readyup::restore {

// ---- backup files ------------------------------------------------------------------------------

// "readyup_backup_<matchid>_map<N>_" (what match load / go-live set as mp_backup_round_file).
std::string BackupPrefix(uint64_t matchid, int mapNumber);

struct BackupInfo {
  std::string file;       // basename
  int map_number = 0;     // 1-based
  int round = 0;          // the round it restores (1-based)
  long long mtime = 0;    // seconds (any epoch), newest wins for the same map / round
  long long size = 0;
};

// A CS2 round backup of match `matchid`: readyup_backup_<matchid>_map<N>__round<NN>.txt (what CS2
// writes for the BackupPrefix above: it adds "_round<NN>.txt") or ..._map<N>_round<NN>.txt. Fills
// map_number and round (NN + 1). False for anything else (another match, a temp file, a path).
bool ParseBackupFileName(const std::string& file, uint64_t matchid, BackupInfo* out);

// Map, then round order; one entry per (map, round) (the newest mtime).
std::vector<BackupInfo> SortBackups(std::vector<BackupInfo> in);

// The newest round on `mapNumber` (highest round), or nullptr.
const BackupInfo* LatestForMap(const std::vector<BackupInfo>& sorted, int mapNumber);

// One line of `ru match backups`: "map 1 round 5 (3-1) readyup_backup_..._round04.txt".
// score1 / score2 < 0: unknown (left out).
std::string BackupListLine(const BackupInfo& b, int score1, int score2);

// The map score at the start of `round` from the stats model's round summaries (0-0 for round 1).
// False when the model does not have round - 1.
bool ScoreAtRoundStart(const stats::MapStats& m, int round, int* team1, int* team2);

// ---- `.restore <round>` ----------------------------------------------------------------------

// A round number (1..999); -1 with *err otherwise.
int ParseRestoreRound(const std::string& arg, std::string* err);

struct RestoreCheck {
  bool match_loaded = false;
  bool live = false;         // mode MatchLive (not warmup, knife or postgame)
  int rounds_played = 0;     // on the current map
  int round = 0;             // asked for
};
// "" = the restore may run; otherwise the reason (one chat line).
std::string RestoreRefusal(const RestoreCheck& c);

// CS2 writes round backups only when started with this flag; without it no restore finds a file.
inline constexpr const char* kRoundBackupFlag = "-allow_round_backup_files";
// The hint shown when a restore finds no backup file.
const char* BackupFlagHint();
// `err` from a failed restore ("no backup readyup_backup_..."), with BackupFlagHint() appended when
// the file was simply missing. Other errors come back unchanged.
std::string ExplainRestoreError(const std::string& err);
// Whether raw /proc/<pid>/cmdline bytes (NUL-separated arguments) contain `arg` exactly.
bool CmdlineHasArg(const std::string& raw, const std::string& arg);

// ---- pause after a restore -------------------------------------------------------------------

// Effective `pause_after_restore` (1 / 0): the match config (MAT key / fleet
// rules.pause.pause_after_restore), else the console setting ru_pause_after_restore, else
// readyup.cfg, else on. -1 = not set at that level.
int PauseAfterRestoreFor(int matchValue, int consoleValue, int cfgValue);

// ---- crash / restart recovery ----------------------------------------------------------------

// What state.json keeps about a running match besides its config (persisted_match_state.h),
// rewritten at every round end, go-live, map end and restore.
struct Progress {
  int map_number = 1;
  // "warmup" (before the map went live, knife included), "live", "map_over" (the map is decided,
  // postgame).
  std::string phase = "warmup";
  int series_team1 = 0;  // maps won
  int series_team2 = 0;
  std::string stats_json;      // live: stats::ToJson of the map so far ("" = none)
  bool have_events = false;    // live: the event counters / totals below are set
  MatchEventsState events;
};

std::string ProgressToJson(const Progress& p);
bool ProgressFromJson(const std::string& json, Progress* out);

struct RecoveryPlan {
  enum class Kind { None, Warmup, Live };
  Kind kind = Kind::None;
  int map_number = 1;
  std::string map_entry;  // maplist[map_number - 1]
  std::string why;        // for the log
};

// What a restarted server does with the persisted match:
//   - no progress record (state.json of an older build): live flag -> Live on map 1, else Warmup
//     on map 1 (what recovery did before),
//   - "map_over": Warmup on the next map, or None when the series is over (the series end was
//     already reported),
//   - "live": Live on that map (restore its newest round backup, wait for everyone to ready),
//   - "warmup": Warmup on that map.
// None too when the map number is outside the map list.
RecoveryPlan PlanRecovery(const Progress* p, bool legacyLiveFlag, const std::vector<std::string>& maplist, int numMaps,
                          bool clinchSeries);

}  // namespace readyup::restore
