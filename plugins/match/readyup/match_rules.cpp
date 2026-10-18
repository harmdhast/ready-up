#include <cctype>
#include "readyup/match_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace readyup {

MatchRules BuiltinDefaultRules() {
  MatchRules r;
  r.tech_pauses_per_team = 3;
  r.tech_pause_max_seconds = 300;
  r.disconnect_pause_max_seconds = 300;
  r.both_teams_unpause = 1;
  r.allow_force_ready = 1;
  r.team_ready = 0;
  r.min_players_to_ready = 0;
  r.min_spectators_to_ready = 0;
  r.forfeit_after_seconds = 240;
  r.gg_enabled = 0;
  r.gg_threshold_pct = 80;
  r.gg_min_score_diff = 8;
  r.stop_command_available = 0;
  r.stop_command_no_damage = 0;
  r.stop_vote_seconds = 30;
  r.pause_after_restore = 1;
  return r;
}

MatchRules ResolveRules(const MatchRules& match, const MatchRules& base) {
  const MatchRules d = BuiltinDefaultRules();
  auto pick = [](int a, int b, int c) { return a >= 0 ? a : b >= 0 ? b : c; };
  MatchRules r;
  r.tech_pauses_per_team = pick(match.tech_pauses_per_team, base.tech_pauses_per_team, d.tech_pauses_per_team);
  r.tech_pause_max_seconds =
      pick(match.tech_pause_max_seconds, base.tech_pause_max_seconds, d.tech_pause_max_seconds);
  r.disconnect_pause_max_seconds = pick(match.disconnect_pause_max_seconds, base.disconnect_pause_max_seconds,
                                        d.disconnect_pause_max_seconds);
  r.both_teams_unpause = pick(match.both_teams_unpause, base.both_teams_unpause, d.both_teams_unpause) ? 1 : 0;
  r.allow_force_ready = pick(match.allow_force_ready, base.allow_force_ready, d.allow_force_ready) ? 1 : 0;
  r.team_ready = pick(match.team_ready, base.team_ready, d.team_ready) ? 1 : 0;
  r.min_players_to_ready = pick(match.min_players_to_ready, base.min_players_to_ready, d.min_players_to_ready);
  r.min_spectators_to_ready =
      pick(match.min_spectators_to_ready, base.min_spectators_to_ready, d.min_spectators_to_ready);
  r.forfeit_after_seconds =
      pick(match.forfeit_after_seconds, base.forfeit_after_seconds, d.forfeit_after_seconds);
  r.gg_enabled = pick(match.gg_enabled, base.gg_enabled, d.gg_enabled) ? 1 : 0;
  r.gg_threshold_pct = std::clamp(pick(match.gg_threshold_pct, base.gg_threshold_pct, d.gg_threshold_pct), 1, 100);
  r.gg_min_score_diff = pick(match.gg_min_score_diff, base.gg_min_score_diff, d.gg_min_score_diff);
  r.stop_command_available =
      pick(match.stop_command_available, base.stop_command_available, d.stop_command_available) ? 1 : 0;
  r.stop_command_no_damage =
      pick(match.stop_command_no_damage, base.stop_command_no_damage, d.stop_command_no_damage) ? 1 : 0;
  r.stop_vote_seconds = std::clamp(pick(match.stop_vote_seconds, base.stop_vote_seconds, d.stop_vote_seconds), 5, 300);
  r.pause_after_restore = pick(match.pause_after_restore, base.pause_after_restore, d.pause_after_restore) ? 1 : 0;
  return r;
}

int GgThresholdPctFromText(const std::string& text) {
  if (text.empty()) return -1;
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(v) || v <= 0) return -1;
  const double pct = v <= 1.0 ? v * 100.0 : v;
  if (pct > 100.0) return -1;
  return std::max(1, static_cast<int>(std::lround(pct)));
}

std::vector<std::string> GoLiveRulesLines(int mapNumber, int numMaps, const std::string& map, int maxRounds,
                                          bool overtime, int overtimeHalfRounds, int maxOvertimes,
                                          bool damageTiebreak) {
  std::string name = map.rfind("de_", 0) == 0 || map.rfind("cs_", 0) == 0 ? map.substr(3) : map;
  if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
  std::vector<std::string> out;
  std::string first = numMaps > 1 ? "Ready Up: map " + std::to_string(mapNumber) + " of " + std::to_string(numMaps) +
                                        " (first to " + std::to_string(numMaps / 2 + 1) + " maps)"
                                  : std::string("Ready Up: one map");
  out.push_back(first + (name.empty() ? "." : ": " + name + "."));
  if (maxRounds <= 0) return out;
  const int half = maxRounds / 2;
  out.push_back("Ready Up: " + std::to_string(maxRounds) + " rounds, first to " + std::to_string(half + 1) + " wins.");
  out.push_back("Ready Up: sides switch after " + std::to_string(half) + " rounds.");
  const std::string tie = "Ready Up: at " + std::to_string(half) + "-" + std::to_string(half) + ", ";
  if (overtime && overtimeHalfRounds > 0) {
    std::string ot = tie + "overtime: " + std::to_string(overtimeHalfRounds * 2) + " rounds, first to " +
                     std::to_string(overtimeHalfRounds + 1);
    if (maxOvertimes > 0) {
      ot += ", at most " + std::to_string(maxOvertimes) + (damageTiebreak ? ", then total damage decides." : ", then a draw.");
    } else {
      ot += ", until someone wins.";
    }
    out.push_back(ot);
  } else {
    out.push_back(tie + (damageTiebreak ? "total damage decides." : "the map is a draw."));
  }
  return out;
}

std::string PlayerLeftChatLine(const std::string& name, const std::string& team, bool autoPause, int maxSeconds) {
  std::string msg = "Ready Up: " + name + " (" + team + ") left the server.";
  if (!autoPause) return msg + " The match goes on; they can rejoin with the same connect link.";
  msg += " The match pauses at the next freeze time until they are back";
  if (maxSeconds > 0) msg += " (at most " + std::to_string(std::max(1, maxSeconds / 60)) + " min)";
  return msg + ".";
}

bool TechPauseAllowed(int used, int limit) { return limit <= 0 || used < limit; }

int TechPausesLeft(int used, int limit) {
  if (limit <= 0) return -1;
  return std::max(0, limit - used);
}

int TechPauseSecondsLeft(int elapsedSeconds, int maxSeconds) {
  if (maxSeconds <= 0) return -1;
  return std::max(0, maxSeconds - std::max(0, elapsedSeconds));
}

bool UnpauseSatisfied(bool team1Confirmed, bool team2Confirmed, bool bothRequired, int pauserTeam) {
  if (bothRequired) return team1Confirmed && team2Confirmed;
  if (pauserTeam == 1) return team1Confirmed;
  if (pauserTeam == 2) return team2Confirmed;
  return team1Confirmed || team2Confirmed;
}

bool Auto5v5PauseExpected(bool ruleOn, bool liveMap, bool alreadyPaused, int ctPlayers, int tPlayers) {
  if (!ruleOn || !liveMap || alreadyPaused) return false;
  return Auto5v5ShortSide(ctPlayers, tPlayers) != 0;
}

Auto5v5Check Auto5v5Decide(bool ruleOn, bool liveMap, bool alreadyPaused, int enginePaused, int ctPlayers,
                           int tPlayers) {
  if (!ruleOn || !liveMap || alreadyPaused) return Auto5v5Check::Done;
  // Paused with both sides full: someone else's pause (an admin's mp_pause_match), not auto 5v5.
  if (enginePaused == 1) return Auto5v5ShortSide(ctPlayers, tPlayers) != 0 ? Auto5v5Check::Mark : Auto5v5Check::Done;
  if (enginePaused == 0) return Auto5v5Check::Wait;  // not (yet) paused: look again until freeze ends
  return Auto5v5ShortSide(ctPlayers, tPlayers) != 0 ? Auto5v5Check::Mark : Auto5v5Check::Done;
}

EnginePauseAction DecideEnginePause(int enginePaused, bool readyUpPause, bool playing, bool goLiveWindow,
                                    bool autoRuleOn, int ct, int t, int fullTeam, double unmarkedForSeconds) {
  if (enginePaused != 1 || readyUpPause || !playing) return EnginePauseAction::None;
  const int full = fullTeam > 0 ? fullTeam : kFullTeamPlayers;
  const bool shortSide = ct < full || t < full;
  const EnginePauseAction mark = shortSide && autoRuleOn ? EnginePauseAction::MarkAuto5v5 : EnginePauseAction::MarkEngine;
  if (goLiveWindow) return shortSide ? mark : EnginePauseAction::Clear;
  return unmarkedForSeconds >= kEnginePauseMarkHoldSeconds ? mark : EnginePauseAction::None;
}

int Auto5v5ShortSide(int ctPlayers, int tPlayers) {
  if (ctPlayers >= kFullTeamPlayers && tPlayers >= kFullTeamPlayers) return 0;
  return ctPlayers <= tPlayers ? 3 : 2;
}

std::string Auto5v5UnpauseReason(int ctPlayers, int tPlayers, double fullForSeconds, double pausedSeconds,
                                 int maxSeconds) {
  if (Auto5v5ShortSide(ctPlayers, tPlayers) == 0 && fullForSeconds >= kAuto5v5FullHoldSeconds) return "full";
  if (maxSeconds > 0 && pausedSeconds >= maxSeconds) return "timeout";
  return "";
}

int FullTeamSize(int rosterSize, int playersPerTeam) {
  if (rosterSize <= 0) return 0;
  return std::min(rosterSize, playersPerTeam > 0 ? playersPerTeam : kFullTeamPlayers);
}

int ForceReadyRequired(int rosterSize, int minPlayers, int playersPerTeam) {
  if (rosterSize <= 0) return 0;
  if (minPlayers <= 0) return FullTeamSize(rosterSize, playersPerTeam);
  return std::min(minPlayers, rosterSize);
}

int ReadyMinPlayers(int minPlayersRule, bool fullRoster) { return fullRoster ? 0 : minPlayersRule; }

bool ForceReadyAllowed(int connected, int rosterSize, int minPlayers, int playersPerTeam) {
  if (rosterSize <= 0) return false;
  return connected >= ForceReadyRequired(rosterSize, minPlayers, playersPerTeam);
}

TeamReadyAction DecideTeamReady(bool teamReadyRule, bool ready, int connected, int rosterSize, int minPlayers,
                                int playersPerTeam) {
  if (!teamReadyRule) return TeamReadyAction::Single;
  if (!ready) return TeamReadyAction::Team;
  return ForceReadyAllowed(connected, rosterSize, minPlayers, playersPerTeam) ? TeamReadyAction::Team
                                                                               : TeamReadyAction::NotEnough;
}

int TeamReadyNeeded(int rosterSize, int connected, int minPlayers, int playersPerTeam) {
  if (rosterSize <= 0) return 0;
  const int full = FullTeamSize(rosterSize, playersPerTeam);
  return std::max(ForceReadyRequired(rosterSize, minPlayers, playersPerTeam), std::min(std::max(0, connected), full));
}

bool TeamReadyToGoLive(int rosterSize, int connected, int ready, int minPlayers, int playersPerTeam) {
  if (rosterSize <= 0) return true;
  return ready >= TeamReadyNeeded(rosterSize, connected, minPlayers, playersPerTeam);
}

ReadyTally TeamReadyTally(int rosterSize, int ready, int playersPerTeam) {
  ReadyTally t;
  t.total = FullTeamSize(rosterSize, playersPerTeam);
  t.ready = std::min(std::max(0, ready), t.total);
  return t;
}

TeamHudState TeamHudStatus(int rosterSize, int connected, int readyConnected, int playersPerTeam) {
  TeamHudState st;
  st.total = FullTeamSize(rosterSize, playersPerTeam);
  st.here = std::min(std::max(0, connected), st.total);
  st.allHere = st.total > 0 && st.here >= st.total;
  st.ready = st.total > 0 && std::min(std::max(0, readyConnected), st.total) >= st.total;
  return st;
}

bool BotFillTeamReady(int connected, int readyConnected) {
  return std::max(0, readyConnected) >= std::max(0, connected);
}

bool ResumeMayGoLive(int rosterTotal, int presentTotal) {
  return presentTotal > 0 && presentTotal >= rosterTotal;
}

ResumeHeadcount ResumeCount(const int roster[2], const int present[2], int playersPerTeam) {
  ResumeHeadcount c;
  for (int t = 0; t < 2; ++t) {
    const int full = FullTeamSize(roster[t], playersPerTeam);
    c.needed += full;
    c.back += std::min(std::max(0, present[t]), full);
  }
  return c;
}

bool BotFillEmptyMayGoLive(double secondsEmpty) { return secondsEmpty >= kBotFillEmptyGraceSeconds; }

ReadyTally BotFillReadyTally(int connected, int readyConnected) {
  ReadyTally t;
  t.total = std::max(0, connected);
  t.ready = std::min(std::max(0, readyConnected), t.total);
  return t;
}

bool SpectatorsReadyToGoLive(int required, int readySpectators) { return required <= 0 || readySpectators >= required; }

bool SpecCommandAllowed(bool onLoadedMatchRoster) { return !onLoadedMatchRoster; }

bool PlayoutRoundsLeft(int maxRounds, bool overtimeEnabled, int overtimeSegments, int maxOvertimes, int team1Score,
                       int team2Score) {
  maxRounds = std::max(1, maxRounds);
  const int sum = std::max(0, team1Score) + std::max(0, team2Score);
  if (sum < maxRounds) return true;
  if (sum == maxRounds || !overtimeEnabled) return false;
  const int block = 2 * std::max(1, overtimeSegments);
  const int past = sum - maxRounds;
  if (maxOvertimes >= 0 && past >= maxOvertimes * block) return false;  // sudden death past the cap
  return past % block != 0;
}

std::string SanitizeTeamName(const std::string& in) {
  std::string s;
  s.reserve(in.size());
  for (char c : in) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || c == '"' || c == ';' || c == '\\' || u == 0x7F) continue;
    s += c;
  }
  const auto b = s.find_first_not_of(' ');
  if (b == std::string::npos) return {};
  s = s.substr(b, s.find_last_not_of(' ') - b + 1);
  if (s.size() > 32) {
    size_t cut = 32;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
  }
  return s;
}

int ForfeitTimer::SecondsLeft(double now) const {
  if (absent_ == 0) return 0;
  return std::max(0, static_cast<int>(std::ceil(deadline_ - now - 1e-9)));
}

void ForfeitTimer::Reset() {
  absent_ = 0;
  deadline_ = 0;
  fired_ = false;
}

ForfeitTimer::Result ForfeitTimer::Update(double now, bool active, int limitSeconds, bool team1Present,
                                          bool team2Present) {
  Result r;
  int absent = 0;
  if (active && limitSeconds > 0 && team1Present != team2Present) absent = team1Present ? 2 : 1;
  if (absent == 0) {
    if (absent_ != 0 && !fired_) r.action = Action::Cancelled;
    if (absent_ != 0) r.absentTeam = absent_;
    Reset();
    return r;
  }
  if (fired_) {
    r.absentTeam = absent_;
    return r;  // stays fired until reset (the map ended)
  }
  if (absent_ != absent) {
    absent_ = absent;
    deadline_ = now + limitSeconds;
    r.action = Action::Started;
  }
  r.absentTeam = absent_;
  r.secondsLeft = SecondsLeft(now);
  if (now >= deadline_) {
    fired_ = true;
    r.action = Action::Expired;
    r.secondsLeft = 0;
  }
  return r;
}

}  // namespace readyup
