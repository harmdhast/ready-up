// Round restore commands and the steps after every restore (see round_restore.h).
#include "readyup/round_restore.h"

#include "readyup/backup_files.h"
#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/fleet_bridge.h"
#include "readyup/game_timers.h"
#include "readyup/logging.h"
#include "readyup/match_recovery.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace readyup::round_restore {
namespace {

std::atomic<int> g_consolePauseAfterRestore{-1};
unsigned g_unpauseGen = 0;  // game thread: a newer restore cancels the older one's unpause

std::vector<std::string> Tokens(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == '"') continue;
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// Rounds played on the current map: the stats model's score while live, else the log's.
int RoundsPlayed() {
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    if (stats::Current().Live()) return stats::Current().Team1Score() + stats::Current().Team2Score();
  }
  const auto ms = MatchStateGet();
  return ms.team1_score + ms.team2_score;
}

// Restores `round` of the current map; "" or the reason it did not.
std::string Restore(int round, const std::string& by, const Reply& announce) {
  const auto ctx = WebhookGetMatchContext();
  restore::RestoreCheck c;
  c.match_loaded = static_cast<bool>(ctx);
  c.live = GetMode() == ReadyUpMode::MatchLive;
  c.rounds_played = RoundsPlayed();
  c.round = round;
  if (std::string why = restore::RestoreRefusal(c); !why.empty()) return why;
  std::string err;
  bool autoUnpause = false;
  if (!fleet_bridge::RestoreRoundFromLocalBackup(round, by, "admin", &err, &autoUnpause)) return restore::ExplainRestoreError(err) + ".";
  Print("restore: round %d restored by %s%s\n", round, by.c_str(), autoUnpause ? " (live in 3 s)" : "");
  if (autoUnpause) {
    announce("round " + std::to_string(round) + " restored by " + by + ". Live in 3 seconds.");
  } else {
    // Like `.stop`: the players resume it (.unpause, both teams by default); admins can .fup.
    PauseStateOnPaused("technical", "restore:" + by);
    announce("round " + std::to_string(round) + " restored by " + by +
             ". The match is paused; type .unpause when ready.");
  }
  return {};
}

void PrintBackups(uint64_t matchid, const Reply& reply) {
  const auto list = ListBackups(matchid);
  if (list.empty()) {
    reply("no round backups for match " + std::to_string(static_cast<unsigned long long>(matchid)) + " on this server.");
    return;
  }
  stats::MapStats snap;
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    snap = stats::Current().Snapshot();
  }
  const int currentMap = std::max(1, MatchStateGet().map_number);
  for (const auto& b : list) {
    int s1 = -1, s2 = -1;
    if (b.map_number != currentMap || !restore::ScoreAtRoundStart(snap, b.round, &s1, &s2)) s1 = s2 = -1;
    reply(restore::BackupListLine(b, s1, s2));
  }
}

}  // namespace

void RestoreCommand(const std::vector<std::string>& args, const std::string& by, const Reply& reply,
                    const Reply& announce) {
  std::string err;
  const int round = restore::ParseRestoreRound(args.empty() ? std::string() : args[0], &err);
  if (round < 0) return reply(err);
  const std::string why = Restore(round, by, announce);
  if (!why.empty()) {
    Print("restore: round %d refused (%s): %s\n", round, by.c_str(), why.c_str());
    reply("restore refused: " + why);
  }
}

void BackupsCommand(const Reply& reply) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return reply("no match loaded.");
  PrintBackups(ctx->matchid, reply);
}

bool HandleConsoleLine(const std::string& line) {
  const auto t = Tokens(line);
  if (t.empty()) return false;
  const Reply print = [](const std::string& s) { PrintLine(s.c_str()); };
  if (t[0] == "ru_pause_after_restore") {
    if (t.size() >= 2) {
      const std::string& v = t[1];
      if (v == "1" || v == "true" || v == "on") SetConsolePauseAfterRestore(1);
      else if (v == "0" || v == "false" || v == "off") SetConsolePauseAfterRestore(0);
      else {
        PrintLine("Usage: ru_pause_after_restore 0|1");
        return true;
      }
    }
    Print("pause after restore: %d%s\n", PauseAfterRestore() ? 1 : 0,
          WebhookGetMatchContext() && WebhookGetMatchContext()->rules.pause_after_restore >= 0 ? " (set by the match)" : "");
    return true;
  }
  if (t[0] == "ru_listbackups") {
    uint64_t id = 0;
    if (t.size() >= 2) id = std::strtoull(t[1].c_str(), nullptr, 10);
    else if (const auto ctx = WebhookGetMatchContext()) id = ctx->matchid;
    if (id == 0) {
      PrintLine("Usage: ru_listbackups <matchid> (no match loaded)");
      return true;
    }
    PrintBackups(id, print);
    return true;
  }
  if (t[0] == "ru_loadbackup") {
    const auto ctx = WebhookGetMatchContext();
    if (t.size() < 2) {
      PrintLine("Usage: ru_loadbackup <readyup_backup_..._roundNN.txt> (ru_listbackups lists them)");
      return true;
    }
    std::string file = t[1];
    if (const size_t slash = file.find_last_of("/\\"); slash != std::string::npos) file = file.substr(slash + 1);
    restore::BackupInfo b;
    if (!ctx || !restore::ParseBackupFileName(file, ctx->matchid, &b)) {
      Print("restore: %s is not a round backup of the loaded match\n", file.c_str());
      return true;
    }
    if (b.map_number != std::max(1, MatchStateGet().map_number)) {
      Print("restore: %s is for map %d; the server is on map %d\n", file.c_str(), b.map_number,
            std::max(1, MatchStateGet().map_number));
      return true;
    }
    const std::string why = Restore(b.round, "Console", [](const std::string& s) {
      PrintLine(s.c_str());
      SendToChat((AdminPrefix() + " " + s).c_str());
    });
    if (!why.empty()) Print("restore: %s refused: %s\n", file.c_str(), why.c_str());
    return true;
  }
  return false;
}

int ConsolePauseAfterRestore() { return g_consolePauseAfterRestore.load(); }
void SetConsolePauseAfterRestore(int v) { g_consolePauseAfterRestore.store(v < 0 ? -1 : (v ? 1 : 0)); }

bool PauseAfterRestore() {
  // valve ruleset (docs/ESPORTS-MODE.md): a restore is always followed by a pause.
  if (!PlayerExtrasAllowed(CurrentEffectiveRules())) return true;
  const auto ctx = WebhookGetMatchContext();
  return restore::PauseAfterRestoreFor(ctx ? ctx->rules.pause_after_restore : -1, ConsolePauseAfterRestore(),
                                       Cfg().rules.pause_after_restore) != 0;
}

bool AfterRestore(int mapNumber, int round, const std::string& file, const std::string& reason) {
  WebhookEmitBackupLoaded(mapNumber, std::max(0, round - 1), file);
  match_recovery::NoteProgress();
  ++g_unpauseGen;
  if (reason == "resume" || reason == "recovery" || PauseAfterRestore()) return false;
  const unsigned gen = g_unpauseGen;
  ScheduleOnGameThread(3.0, [gen, mapNumber] {
    if (gen != g_unpauseGen || !PauseStateGet().paused) return;
    if (!EnqueueServerCommand("mp_unpause_match")) return;
    const int dur = PauseStatePauseDurationSeconds();
    PauseStateOnUnpaused();
    WebhookEmitMatchUnpaused(mapNumber, dur);
    Print("restore: pause_after_restore 0 -> unpaused\n");
  });
  return true;
}

std::vector<restore::BackupInfo> ListBackups(uint64_t matchid) {
  std::vector<restore::BackupInfo> found;
  for (const auto& dir : backup_files::BackupDirs()) {
    std::error_code ec;
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      restore::BackupInfo b;
      if (!restore::ParseBackupFileName(it.path().filename().string(), matchid, &b)) continue;
      std::error_code ec2;
      if (!it.is_regular_file(ec2) || ec2) continue;
      b.mtime = static_cast<long long>(
          std::chrono::duration_cast<std::chrono::seconds>(it.last_write_time(ec2).time_since_epoch()).count());
      b.size = static_cast<long long>(it.file_size(ec2));
      found.push_back(std::move(b));
    }
  }
  return restore::SortBackups(std::move(found));
}

void EnableRoundBackups(uint64_t matchid, int mapNumber) {
  if (matchid == 0) return;
  (void)EnqueueServerCommand("mp_backup_round_auto 1");
  (void)EnqueueServerCommand(("mp_backup_round_file " + restore::BackupPrefix(matchid, mapNumber)).c_str());
}

}  // namespace readyup::round_restore
