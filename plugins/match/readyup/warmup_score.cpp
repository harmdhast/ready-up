// Warmup score (see warmup_score.h).
#include "readyup/warmup_score.h"

#include "readyup/engine.h"
#include "readyup/fleet_bridge.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/modes.h"
#include "readyup/plugin_api.h"

#include <cstdint>
#include <cstring>

namespace readyup {
namespace {

// cs_team_manager entities (unassigned, spectator, T, CT) are looked up kSlice indices per tick.
constexpr int kMaxTeams = 4;
constexpr int kMaxIndex = 16384;
constexpr int kSlice = 1024;
// mp_restartgame 1 restarts a second later; the score stays until then.
constexpr double kRestartHoldSeconds = 3.0;

int g_offScore = -2;  // CTeam::m_iScore (-2 = not looked up)
int g_offTeamNum = -2;  // CBaseEntity::m_iTeamNum
uint32_t g_teams[kMaxTeams];
int g_teamCount = 0;
int g_cursor = 1;
double g_holdUntil = 0;
bool g_active = false;  // WarmupScoreResetActive on the last tick
bool g_presetActive = false;  // the resume warmup's score was written on the last tick

bool Resolve(const ru_api* a) {
  if (a->entity_system_status(a->self) != RU_ENTSYS_OK) return false;
  if (g_offScore == -2) {
    g_offScore = a->schema_offset(a->self, "CTeam", "m_iScore");
    if (g_offScore < 0) Print("warmup-score: schema field CTeam::m_iScore missing; warmup scores are not reset\n");
  }
  if (g_offTeamNum == -2) {
    g_offTeamNum = a->schema_offset(a->self, "CBaseEntity", "m_iTeamNum");
    if (g_offTeamNum < 0) Print("warmup-score: schema field CBaseEntity::m_iTeamNum missing; resume scores are not shown\n");
  }
  return g_offScore >= 0;
}

void ForgetTeams() {
  g_teamCount = 0;
  g_cursor = 1;
}

void ScanSlice(const ru_api* a) {
  const int end = g_cursor + kSlice < kMaxIndex ? g_cursor + kSlice : kMaxIndex;
  for (int i = g_cursor; i < end && g_teamCount < kMaxTeams; ++i) {
    void* ent = a->entity_by_index(a->self, i);
    if (!ent) continue;
    const char* cls = a->entity_classname(a->self, ent);
    if (!cls || std::strcmp(cls, "cs_team_manager") != 0) continue;
    const uint32_t h = a->entity_handle_of(a->self, ent);
    if (h != 0xFFFFFFFFu) g_teams[g_teamCount++] = h;
  }
  g_cursor = end;
  // No team found in a whole pass (map still loading): start over.
  if (g_cursor >= kMaxIndex && g_teamCount == 0) g_cursor = 1;
}

// The resumed round's score on the scoreboard while everyone is in warmup.
void PresetResumeScore(const ru_api* a, int team1, int team2, bool team1Ct) {
  if (!Resolve(a) || g_offTeamNum < 0) return;
  if (g_teamCount < kMaxTeams && g_cursor < kMaxIndex) ScanSlice(a);
  int ctScore = 0, tScore = 0;
  WarmupPresetScores(team1, team2, team1Ct, &ctScore, &tScore);
  for (int i = 0; i < g_teamCount; ++i) {
    void* team = a->entity_from_handle(a->self, g_teams[i]);
    if (!team) {
      ForgetTeams();
      return;
    }
    uint8_t num = 0;
    std::memcpy(&num, static_cast<unsigned char*>(team) + g_offTeamNum, sizeof(num));
    if (num != 2 && num != 3) continue;
    const int32_t want = num == 3 ? ctScore : tScore;
    int32_t score = 0;
    std::memcpy(&score, static_cast<unsigned char*>(team) + g_offScore, sizeof(score));
    if (score == want) continue;
    std::memcpy(static_cast<unsigned char*>(team) + g_offScore, &want, sizeof(want));
    a->entity_mark_changed(a->self, team);
  }
}

}  // namespace

void WarmupScoreInstall(const ru_api*) {
  g_offScore = -2;
  g_offTeamNum = -2;
  ForgetTeams();
  g_active = false;
  g_presetActive = false;
  g_holdUntil = 0;
}

void WarmupScoreTick(double now) {
  const char* mode = GetModeString();
  int team1 = 0, team2 = 0;
  bool team1Ct = true;
  const bool preset = FullRosterGateEnabled() && fleet_bridge::ResumeWarmupScore(&team1, &team2, &team1Ct) &&
                      WarmupMapReady() && !GoLiveTriggered();
  if (preset) {
    if (!g_presetActive) {
      ForgetTeams();
      g_presetActive = true;
    }
    g_active = false;
    if (const ru_api* a = host::Api()) PresetResumeScore(a, team1, team2, team1Ct);
    return;
  }
  g_presetActive = false;
  if (!WarmupScoreResetActive(mode, RecoveryGateEnabled(), GoLiveTriggered(), WarmupMapReady())) {
    g_active = false;
    return;
  }
  if (!g_active) {
    // Team handles cached before this warmup may be the last map's.
    ForgetTeams();
    g_active = true;
  }
  const ru_api* a = host::Api();
  if (!a || !Resolve(a)) return;
  if (g_teamCount < kMaxTeams && g_cursor < kMaxIndex) ScanSlice(a);
  int32_t top = 0;
  for (int i = 0; i < g_teamCount; ++i) {
    void* team = a->entity_from_handle(a->self, g_teams[i]);
    if (!team) {
      // Map change: new team entities.
      ForgetTeams();
      return;
    }
    int32_t score = 0;
    std::memcpy(&score, static_cast<unsigned char*>(team) + g_offScore, sizeof(score));
    if (score > top) top = score;
  }
  if (top <= 0 || now < g_holdUntil) return;
  // The scrim warmup rules end with mp_restartgame 1 themselves.
  const bool scrim = std::strcmp(mode, "scrim_warmup") == 0;
  if (!scrim && !EnqueueServerCommand("mp_restartgame 1")) return;
  g_holdUntil = now + kRestartHoldSeconds;
  ReapplyWarmupRules();
  Print("warmup-score: a team has %d round(s) in %s; warmup rules again%s\n", top, mode,
        scrim ? "" : ", mp_restartgame 1");
}

}  // namespace readyup
