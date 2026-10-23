// Match half of the old core log_receiver.cpp LifecycleImpl (see match_log.h). The core keeps
// the log listener / file tail and the normalized plugin events; this is the match policy.
#include "readyup/match_log.h"
#include "readyup/match_recovery.h"

#include "readyup/backup_files.h"
#include "readyup/engine.h"
#include "readyup/knife_tracker.h"
#include "readyup/logging.h"
#include "readyup/map_names.h"
#include "readyup/match_events.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/persisted_match_state.h"
#include "readyup/player_registry.h"
#include "readyup/steamid.h"
#include "readyup/webhook.h"
#include "readyup/welcome.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace readyup {
namespace {

std::recursive_mutex g_mu;

int g_mapNumber = 1;
int g_roundNumber = 0;
int g_team1Score = 0;  // best-effort: CT -> team1, T -> team2 (log notices are CT/T based)
int g_team2Score = 0;
std::string g_currentMap;
// The last `Loading map` changed the map name: its `Started map` is the same load.
bool g_loadingChangedMap = false;

struct KD {
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
  int kills = 0;
  int deaths = 0;
};
std::unordered_map<uint64_t, KD> g_kd;

bool ExtractQuoted(const std::string& s, size_t& i, std::string& out) {
  const size_t q1 = s.find('"', i);
  if (q1 == std::string::npos) return false;
  const size_t q2 = s.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  out = s.substr(q1 + 1, q2 - (q1 + 1));
  i = q2 + 1;
  return true;
}

bool ExtractBetween(const std::string& s, const char* a, const char* b, std::string& out) {
  const size_t p1 = s.find(a);
  if (p1 == std::string::npos) return false;
  const size_t start = p1 + std::strlen(a);
  const size_t p2 = s.find(b, start);
  if (p2 == std::string::npos) return false;
  out = s.substr(start, p2 - start);
  return true;
}

bool ParseScorePair(const std::string& line, const char* leftKey, const char* rightKey, int& leftOut, int& rightOut) {
  std::string a, b;
  if (!ExtractBetween(line, leftKey, "\"", a)) return false;
  if (!ExtractBetween(line, rightKey, "\"", b)) return false;
  leftOut = std::atoi(a.c_str());
  rightOut = std::atoi(b.c_str());
  return true;
}

WebhookTeam TeamForSteamid(uint64_t steamid64) {
  auto ctx = WebhookGetMatchContext();
  if (!ctx) return WebhookTeam::Unknown;
  auto it = ctx->roster_team.find(steamid64);
  return it == ctx->roster_team.end() ? WebhookTeam::Unknown : it->second;
}

void MapChanged(const std::string& loadedName) {
  // Workshop maps: the engine reports the bsp name ("aim_map") or "workshop/<id>/aim_map"; map
  // tracking, MatchState and demo names use the bsp name (map_names.h). A maplist entry that is
  // a workshop id matches through the id or the name its host_workshop_map loaded.
  mapnames::NoteMapLoaded(loadedName);
  const std::string nextMapName = mapnames::LoadedBaseName(loadedName);
  const bool hadPrev = !g_currentMap.empty();
  if (g_mapNumber < 1) g_mapNumber = 1;
  // Map number only means something for a loaded match; without one (idle, scrim warmup) it
  // stays 1 so a match loaded later does not inherit the server's map change count. With one,
  // prefer the map's position in the maplist (match load changelevels to map 1, series advance
  // to map N).
  if (auto ctx = WebhookGetMatchContext()) {
    int idx = -1;
    for (size_t k = static_cast<size_t>(g_mapNumber); k < ctx->maplist.size(); ++k) {
      if (mapnames::EntryMatchesLoaded(ctx->maplist[k], loadedName)) {
        idx = static_cast<int>(k);
        break;
      }
    }
    if (idx < 0) {
      for (size_t k = 0; k < ctx->maplist.size(); ++k) {
        if (mapnames::EntryMatchesLoaded(ctx->maplist[k], loadedName)) {
          idx = static_cast<int>(k);
          break;
        }
      }
    }
    if (idx >= 0) g_mapNumber = idx + 1;
    else if (hadPrev) g_mapNumber += 1;
  } else {
    g_mapNumber = 1;
  }
  g_currentMap = nextMapName;
  g_roundNumber = 0;
  g_team1Score = 0;
  g_team2Score = 0;
  g_kd.clear();
  MatchStateSetMap(g_mapNumber, g_currentMap);
  MatchStateSetRound(g_roundNumber);
  MatchStateSetScore(g_team1Score, g_team2Score);
}

void ParseHeader(const std::string& header, std::string& nameOut, int& slotOut, uint64_t& steamid64Out) {
  // name<userid><steamid><team>
  nameOut.clear();
  slotOut = -1;
  steamid64Out = 0;
  const size_t a = header.find('<');
  if (a == std::string::npos) {
    nameOut = header;
    return;
  }
  nameOut = header.substr(0, a);
  const size_t b = header.find('>', a + 1);
  if (b != std::string::npos) slotOut = std::atoi(header.substr(a + 1, b - (a + 1)).c_str());
  const size_t c = header.find('<', b == std::string::npos ? 0 : b + 1);
  const size_t d = header.find('>', c == std::string::npos ? 0 : c + 1);
  if (c == std::string::npos || d == std::string::npos) return;
  steamid64Out = ParseSteamId64Loose(header.substr(c + 1, d - (c + 1)));
}

void LifecycleLocked(const std::string& line) {
  // Chat lines can contain arbitrary text (a player typing `World triggered "Round_Start"`);
  // never treat them as world/map events.
  const bool isChat = line.find("\" say \"") != std::string::npos || line.find("\" say_team \"") != std::string::npos;

  // Knife round: deaths / `attacked` health for the time-out tiebreak.
  if (!isChat && KnifeTrackerActive()) KnifeTrackerObserveLine(line);

  // `Game Over: competitive  de_mirage score 3:0 after 3 min` (CT:T): the engine ended the map.
  if (!isChat) {
    const size_t go = line.find("Game Over: ");
    if (go != std::string::npos) {
      const size_t sc = line.find(" score ", go);
      int ct = -1, t = -1;
      if (sc != std::string::npos && std::sscanf(line.c_str() + sc + 7, "%d:%d", &ct, &t) == 2 && ct >= 0 && t >= 0) {
        ModesOnEngineGameOver(ct, t);
      }
    }
  }

  if (!isChat && (line.find("Loading map \"") != std::string::npos || line.find("Started map \"") != std::string::npos)) {
    std::string map;
    // "Loading map" and "Started map" of one load are one change, whichever form they use. A
    // "Started map" of the map already on is a reload of it (the gamemode cfg ran again).
    const bool started = line.find("Started map \"") != std::string::npos;
    if (!started) ModesOnMapLoading();
    if (ExtractBetween(line, "map \"", "\"", map) && !map.empty()) {
      if (mapnames::LoadedBaseName(map) != g_currentMap) {
        MapChanged(map);
        g_loadingChangedMap = !started;
      } else if (started) {
        if (!g_loadingChangedMap) ModesOnMapLoaded();
        g_loadingChangedMap = false;
      }
    }
  }

  // CS2's own warmup started (typically when the first human joins a map). Ready Up emulates
  // warmup; the real one's WARMUP text hides the center HTML.
  if (!isChat && line.find("World triggered \"Warmup_Start\"") != std::string::npos) {
    OnNativeWarmupStarted("log Warmup_Start");
    return;
  }

  // mp_restartgame / warmup end: `World triggered "Match_Start"` right before the first
  // Round_Start of the fresh game. Reset the log-derived round counter + score.
  if (!isChat && line.find("World triggered \"Match_Start\"") != std::string::npos) {
    if (GameEventsListenerInstalled()) {
      MatchEventsOnMatchStart();  // the engine-event counters start over too
      return;
    }
    g_roundNumber = 0;
    g_team1Score = 0;
    g_team2Score = 0;
    g_kd.clear();
    MatchStateSetRound(g_roundNumber);
    MatchStateSetScore(g_team1Score, g_team2Score);
    return;
  }

  if (!isChat && line.find("World triggered \"Round_Start\"") != std::string::npos) {
    // Knife round (log-driven; an engine round_start may also arrive, deduped by knife phase).
    if (GetMode() == ReadyUpMode::MatchKnife) {
      KnifeOnRoundStart(g_mapNumber, "log");
      return;
    }
    if (GameEventsListenerInstalled()) return;  // engine events drive the round lifecycle
    g_roundNumber += 1;
    OnMatchRoundStarted();
    MatchStateSetRound(g_roundNumber);
    WebhookEmitRoundStarted(g_mapNumber, g_roundNumber, g_team1Score, g_team2Score);
    return;
  }

  // End-of-round notices carry CT/T scores.
  if (!isChat && line.find("triggered \"SFUI_Notice_") != std::string::npos) {
    // Knife round end, e.g.
    //   Team "TERRORIST" triggered "SFUI_Notice_Terrorists_Win" (CT "0") (T "1")
    // Not a match round: no score update, no webhooks, no map-end check.
    if (GetMode() == ReadyUpMode::MatchKnife) {
      std::string team, notice;
      (void)ExtractBetween(line, "Team \"", "\"", team);
      (void)ExtractBetween(line, "triggered \"", "\"", notice);
      const int cs = (team == "CT") ? 3 : (team == "TERRORIST") ? 2 : 0;
      const bool elimination = (notice == "SFUI_Notice_CTs_Win" || notice == "SFUI_Notice_Terrorists_Win");
      KnifeOnRoundEnd(g_mapNumber, cs, elimination, "log", notice);
      return;
    }
    int ct = 0, tt = 0;
    if (ParseScorePair(line, "(CT \"", "(T \"", ct, tt)) {
      g_team1Score = ct;
      g_team2Score = tt;
    }
    MatchStateSetScore(g_team1Score, g_team2Score);
    if (GameEventsListenerInstalled()) return;  // scores from logs, round_end from events

    const char* winner = nullptr;
    if (line.find("CTs_Win") != std::string::npos) winner = "team1";
    else if (line.find("Terrorists_Win") != std::string::npos) winner = "team2";
    if (!winner) winner = (g_team1Score >= g_team2Score) ? "team1" : "team2";
    if (g_roundNumber == 0) g_roundNumber = 1;
    std::vector<WebhookPlayerStatLine> players;
    players.reserve(g_kd.size());
    for (const auto& kv : g_kd) {
      if (kv.first == 0 || kv.second.team == WebhookTeam::Unknown) continue;
      WebhookPlayerStatLine out;
      out.steamid64 = kv.first;
      out.name = kv.second.name;
      out.team = kv.second.team;
      out.kills = kv.second.kills;
      out.deaths = kv.second.deaths;
      players.push_back(std::move(out));
    }
    WebhookEmitRoundEndWithPlayerStats(g_mapNumber, g_roundNumber, /*round_time=*/0, /*reason=*/0, winner,
                                       g_team1Score, g_team2Score, players);
    if (auto ctxOpt = WebhookGetMatchContext()) {
      persisted_match_state::PersistSnapshot(g_mapNumber, g_roundNumber, g_team1Score, g_team2Score);
      const std::string prefix = "readyup_backup_" + std::to_string(static_cast<unsigned long long>(ctxOpt->matchid)) +
                                 "_map" + std::to_string(g_mapNumber <= 0 ? 1 : g_mapNumber) + "_";
      persisted_match_state::PersistBackupPrefix(prefix);
      (void)EnqueueServerCommand(("mp_backup_round_file " + prefix).c_str());
      backup_files::DiscoverAndPersistNewestBackupFileAsync(prefix);
    }
    OnMatchRoundEnded(g_mapNumber, g_team1Score, g_team2Score, g_currentMap);
    match_recovery::NoteProgress();
    return;
  }

  // Player header lines: `"Name<2><[U:1:x]><CT>" ...`.
  size_t i = 0;
  std::string header;
  if (!ExtractQuoted(line, i, header)) return;
  {
    std::string name;
    int slot = -1;
    uint64_t steamid64 = 0;
    ParseHeader(header, name, slot, steamid64);
    // `ru admins add <name fragment>` resolves players seen in any line.
    ObservePlayer(steamid64, name);
    if (steamid64 != 0 && !isChat) {
      if (line.find(" connected, address") != std::string::npos) {
        WebhookEmitPlayerConnect(WebhookPlayer{steamid64, name, TeamForSteamid(steamid64)});
      } else if (line.find("\" disconnected") != std::string::npos) {
        WebhookEmitPlayerDisconnect(WebhookPlayer{steamid64, name, TeamForSteamid(steamid64)});
      }
    }
  }

  // K/D from kill lines for the log-derived round_end payload (engine events: stats there).
  if (!isChat && line.find(" killed \"") != std::string::npos) {
    if (GameEventsListenerInstalled()) return;
    std::string victimHeader;
    if (ExtractQuoted(line, i, victimHeader)) {
      std::string aName, vName;
      int aSlot = -1, vSlot = -1;
      uint64_t aSid = 0, vSid = 0;
      ParseHeader(header, aName, aSlot, aSid);
      ParseHeader(victimHeader, vName, vSlot, vSid);
      if (aSid != 0) {
        auto& s = g_kd[aSid];
        if (!aName.empty()) s.name = aName;
        s.team = TeamForSteamid(aSid);
        s.kills += 1;
      }
      if (vSid != 0) {
        auto& s = g_kd[vSid];
        if (!vName.empty()) s.name = vName;
        s.team = TeamForSteamid(vSid);
        s.deaths += 1;
      }
    }
  }
}

}  // namespace

void MatchObserveLogLine(const std::string& line) {
  if (line.empty()) return;
  // Welcome screen: `"Name<slot><steam>" switched from team <X> to <CT|TERRORIST>`.
  WelcomeObserveLogLine(line);
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  LifecycleLocked(line);
}

MatchLogState MatchLogSnapshot() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  return MatchLogState{g_mapNumber, g_roundNumber, g_team1Score, g_team2Score, g_currentMap};
}

void MatchLogRestore(const MatchLogState& s) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  g_mapNumber = s.mapNumber;
  g_roundNumber = s.roundNumber;
  g_team1Score = s.team1Score;
  g_team2Score = s.team2Score;
  g_currentMap = s.currentMap;
}

void MatchLogSeedMap(const std::string& map) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (map.empty() || mapnames::LoadedBaseName(map) == g_currentMap) return;
  MapChanged(map);
}

}  // namespace readyup
