#include "readyup/match_stats.h"

#include "readyup/minijson.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace readyup::stats {

namespace {

constexpr double kTradeWindowSeconds = 5.0;

bool IsUtilityWeapon(const std::string& w) {
  return w == "hegrenade" || w == "inferno" || w == "molotov" || w == "incgrenade" || w == "weapon_hegrenade" ||
         w == "weapon_molotov" || w == "weapon_incgrenade";
}

bool IsKnifeWeapon(const std::string& w) {
  return w.find("knife") != std::string::npos || w.find("bayonet") != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------- accumulator

void StatsAccumulator::BeginMap(bool team1IsCt) {
  Clear();
  team1IsCt_ = team1IsCt;
  live_ = true;
}

void StatsAccumulator::Clear() {
  live_ = false;
  players_.clear();
  round_ = Round{};
  t1_ = TeamLine{};
  t2_ = TeamLine{};
  rounds_.clear();
}

int StatsAccumulator::SlotForSide(int side) const {
  if (side == 3) return team1IsCt_ ? 1 : 2;
  if (side == 2) return team1IsCt_ ? 2 : 1;
  return 0;
}

int StatsAccumulator::SideOf(uint64_t id) const {
  auto it = round_.side.find(id);
  if (it != round_.side.end()) return it->second;
  auto p = players_.find(id);
  return p == players_.end() ? 0 : p->second.lastSide;
}

PlayerRound& StatsAccumulator::R(uint64_t id) {
  auto& r = round_.pr[id];
  r.id = id;
  return r;
}

void StatsAccumulator::ObservePlayer(uint64_t id, const std::string& name, int side, int teamSlot, bool bot) {
  if (id == 0) return;
  auto& p = P(id);
  if (!name.empty()) p.name = name;
  if (bot) p.bot = true;
  if (side == 2 || side == 3) p.lastSide = side;
  if (teamSlot == 1 || teamSlot == 2) p.team = teamSlot;
  else if (p.team == 0) p.team = SlotForSide(p.lastSide);
  if (live_ && (side == 2 || side == 3)) {
    round_.side[id] = side;
    auto& r = R(id);
    r.side = side;
    r.team = p.team;
  }
}

void StatsAccumulator::OnRoundStart() { round_ = Round{}; }

void StatsAccumulator::OnPlayerDeath(uint64_t victim, uint64_t attacker, uint64_t assister, bool assistedFlash,
                                     bool headshot, const std::string& weapon, double time) {
  if (!live_ || victim == 0) return;
  const int victimSide = SideOf(victim);
  if (victimSide == 2 || victimSide == 3) round_.side.emplace(victim, victimSide);
  P(victim).s.deaths += 1;
  round_.dead.insert(victim);
  R(victim).died = true;

  const bool suicide = attacker == 0 || attacker == victim;
  const int attackerSide = suicide ? 0 : SideOf(attacker);
  const bool teamKill = !suicide && attackerSide != 0 && attackerSide == victimSide;

  if (suicide) {
    P(victim).s.suicides += 1;
  } else if (teamKill) {
    P(attacker).s.team_kills += 1;
  } else {
    auto& a = P(attacker).s;
    auto& ar = R(attacker);
    a.kills += 1;
    ar.kills += 1;
    ar.kast = true;
    if (headshot) {
      a.headshot_kills += 1;
      ar.headshot_kills += 1;
    }
    if (IsKnifeWeapon(weapon)) a.knife_kills += 1;

    if (!round_.entryDone) {
      round_.entryDone = true;
      ar.entry_kill = true;
      R(victim).entry_death = true;
      if (attackerSide == 2) a.entry_kills_t += 1;
      else if (attackerSide == 3) a.entry_kills_ct += 1;
      if (victimSide == 2) P(victim).s.entry_deaths_t += 1;
      else if (victimSide == 3) P(victim).s.entry_deaths_ct += 1;
    }

    // Trade: the victim had just killed one of the attacker's teammates.
    bool traded = false;
    for (const auto& kv : round_.deathBy) {
      const uint64_t mate = kv.first;
      if (kv.second.first != victim || mate == attacker) continue;
      if (time - kv.second.second > kTradeWindowSeconds) continue;
      if (SideOf(mate) != attackerSide) continue;
      auto& mr = R(mate);
      if (!mr.traded) {
        mr.traded = true;
        mr.kast = true;
        P(mate).s.traded_deaths += 1;
      }
      traded = true;
    }
    if (traded) a.trade_kills += 1;
  }
  if (!suicide) round_.deathBy[victim] = {attacker, time};

  if (assister != 0 && assister != attacker && assister != victim) {
    const int assisterSide = SideOf(assister);
    if (assisterSide == 0 || assisterSide != victimSide) {
      auto& as = P(assister).s;
      auto& asr = R(assister);
      if (assistedFlash) {
        as.flash_assists += 1;
        asr.flash_assists += 1;
      } else {
        as.assists += 1;
        asr.assists += 1;
      }
      asr.kast = true;
    }
  }

  // Clutch: the victim's side is down to its last player.
  if ((victimSide == 2 || victimSide == 3) && round_.clutch.find(victimSide) == round_.clutch.end()) {
    uint64_t last = 0;
    int alive = 0;
    int enemies = 0;
    for (const auto& kv : round_.side) {
      if (round_.dead.count(kv.first)) continue;
      if (kv.second == victimSide) {
        ++alive;
        last = kv.first;
      } else if (kv.second == 2 || kv.second == 3) {
        ++enemies;
      }
    }
    if (alive == 1 && enemies >= 1) {
      round_.clutch[victimSide] = {last, std::min(enemies, 5)};
      R(last).clutch_vs = std::min(enemies, 5);
    }
  }
}

void StatsAccumulator::OnPlayerHurt(uint64_t victim, uint64_t attacker, int dmgHealth, int healthAfter,
                                    const std::string& weapon) {
  if (!live_ || victim == 0) return;
  auto hit = round_.health.find(victim);
  const int before = hit == round_.health.end() ? 100 : hit->second;
  const int actual = std::max(0, std::min(dmgHealth, before));
  round_.health[victim] = std::max(0, healthAfter);
  if (attacker == 0 || attacker == victim) return;
  const int as = SideOf(attacker);
  if (as != 0 && as == SideOf(victim)) return;  // team damage does not count
  auto& a = P(attacker).s;
  auto& ar = R(attacker);
  a.damage += actual;
  ar.damage += actual;
  if (IsUtilityWeapon(weapon)) {
    a.utility_damage += actual;
    ar.utility_damage += actual;
  }
}

void StatsAccumulator::OnPlayerBlind(uint64_t victim, uint64_t attacker, double duration) {
  if (!live_ || victim == 0 || attacker == 0 || attacker == victim || duration <= 0.0) return;
  const int as = SideOf(attacker);
  auto& a = P(attacker).s;
  if (as != 0 && as == SideOf(victim)) a.friendlies_flashed += 1;
  else a.enemies_flashed += 1;
}

void StatsAccumulator::OnBombPlanted(uint64_t id) {
  if (live_ && id) P(id).s.bomb_plants += 1;
}

void StatsAccumulator::OnBombDefused(uint64_t id) {
  if (live_ && id) P(id).s.bomb_defuses += 1;
}

void StatsAccumulator::OnRoundMvp(uint64_t id) {
  if (!live_ || !id) return;
  P(id).s.mvp += 1;
  R(id).mvp = true;
}

void StatsAccumulator::SetScore(uint64_t id, int score) {
  if (id && score >= 0) P(id).s.score = score;
}

void StatsAccumulator::OnRoundEnd(int winnerSide, int reason) {
  if (!live_) return;
  const int slot = SlotForSide(winnerSide);
  TeamLine* w = slot == 1 ? &t1_ : slot == 2 ? &t2_ : nullptr;
  if (w) {
    w->score += 1;
    (winnerSide == 3 ? w->score_ct : w->score_t) += 1;
  }

  RoundSummary sum;
  sum.round_number = static_cast<int>(rounds_.size()) + 1;
  sum.winner_side = (winnerSide == 2 || winnerSide == 3) ? winnerSide : 0;
  sum.winner_team = slot;
  sum.reason = reason;
  sum.team1_score = t1_.score;
  sum.team2_score = t2_.score;
  sum.team1_was_ct = team1IsCt_;

  for (const auto& kv : round_.side) {
    auto& p = P(kv.first);
    auto& r = R(kv.first);
    r.side = kv.second;
    if (r.team == 0) r.team = p.team;
    r.survived = round_.dead.count(kv.first) == 0;
    if (r.survived) r.kast = true;
    p.s.rounds_played += 1;
    if (r.kast) p.s.kast_rounds += 1;
  }
  for (auto& kv : round_.pr) {
    auto& r = kv.second;
    auto& s = P(kv.first).s;
    if (r.kills > 0) s.multi_kills[static_cast<size_t>(std::min(r.kills, 5) - 1)] += 1;
    if (r.clutch_vs > 0 && round_.clutch.count(winnerSide) && round_.clutch[winnerSide].first == kv.first) {
      r.clutch_won = true;
      s.clutches_won[static_cast<size_t>(r.clutch_vs - 1)] += 1;
    }
    if (r.team == 0) r.team = P(kv.first).team;
    sum.players.push_back(r);
  }
  std::sort(sum.players.begin(), sum.players.end(),
            [](const PlayerRound& a, const PlayerRound& b) { return a.team != b.team ? a.team < b.team : a.id < b.id; });
  rounds_.push_back(std::move(sum));
  round_ = Round{};
}

MapStats StatsAccumulator::Snapshot() const {
  MapStats m;
  m.live = live_;
  m.team1_is_ct = team1IsCt_;
  m.team1 = t1_;
  m.team2 = t2_;
  for (const auto& kv : players_) {
    PlayerLine l;
    l.id = kv.first;
    l.name = kv.second.name;
    l.team = kv.second.team;
    l.last_side = kv.second.lastSide;
    l.bot = kv.second.bot;
    l.stats = kv.second.s;
    m.players.push_back(std::move(l));
  }
  std::sort(m.players.begin(), m.players.end(),
            [](const PlayerLine& a, const PlayerLine& b) { return a.team != b.team ? a.team < b.team : a.id < b.id; });
  m.rounds = rounds_;
  return m;
}

void StatsAccumulator::Restore(const MapStats& m) {
  Clear();
  live_ = m.live;
  team1IsCt_ = m.team1_is_ct;
  t1_ = m.team1;
  t2_ = m.team2;
  for (const auto& l : m.players) {
    auto& p = players_[l.id];
    p.name = l.name;
    p.team = l.team;
    p.lastSide = l.last_side;
    p.bot = l.bot;
    p.s = l.stats;
  }
  rounds_ = m.rounds;
}

// ---------------------------------------------------------------------------- JSON

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(c));
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

namespace {

class Obj {
 public:
  Obj& I(const char* k, long long v) { return R(k, std::to_string(v)); }
  Obj& B(const char* k, bool v) { return R(k, v ? "true" : "false"); }
  Obj& S(const char* k, const std::string& v) { return R(k, "\"" + JsonEscape(v) + "\""); }
  Obj& R(const char* k, const std::string& raw) {
    body_ += first_ ? "\"" : ",\"";
    first_ = false;
    body_ += k;
    body_ += "\":";
    body_ += raw;
    return *this;
  }
  // The round's economy fields, when they were read (PlayerRound money_start >= 0).
  Obj& Econ(const PlayerRound& p) {
    if (p.money_start < 0 && p.money_after_buy < 0) return *this;
    std::string lo = "[";
    for (size_t i = 0; i < p.loadout.size(); ++i) lo += (i ? ",\"" : "\"") + JsonEscape(p.loadout[i]) + "\"";
    lo += "]";
    return I("money_start", p.money_start)
        .I("money_after_buy", p.money_after_buy)
        .I("armor", p.armor)
        .B("helmet", p.helmet)
        .B("defuser", p.defuser)
        .R("loadout", lo);
  }
  std::string Done() const { return "{" + body_ + "}"; }

 private:
  std::string body_;
  bool first_ = true;
};

std::string IntArray(const std::array<int, 5>& a) {
  std::string s = "[";
  for (size_t i = 0; i < a.size(); ++i) s += (i ? "," : "") + std::to_string(a[i]);
  return s + "]";
}

std::string TeamJson(const TeamLine& t) {
  return Obj().I("score", t.score).I("score_ct", t.score_ct).I("score_t", t.score_t).Done();
}

}  // namespace

std::string ToJson(const PlayerStats& s) {
  return Obj()
      .I("kills", s.kills)
      .I("deaths", s.deaths)
      .I("assists", s.assists)
      .I("flash_assists", s.flash_assists)
      .I("team_kills", s.team_kills)
      .I("suicides", s.suicides)
      .I("headshot_kills", s.headshot_kills)
      .I("knife_kills", s.knife_kills)
      .I("damage", s.damage)
      .I("utility_damage", s.utility_damage)
      .I("enemies_flashed", s.enemies_flashed)
      .I("friendlies_flashed", s.friendlies_flashed)
      .I("bomb_plants", s.bomb_plants)
      .I("bomb_defuses", s.bomb_defuses)
      .I("entry_kills_t", s.entry_kills_t)
      .I("entry_kills_ct", s.entry_kills_ct)
      .I("entry_deaths_t", s.entry_deaths_t)
      .I("entry_deaths_ct", s.entry_deaths_ct)
      .I("trade_kills", s.trade_kills)
      .I("traded_deaths", s.traded_deaths)
      .I("kast_rounds", s.kast_rounds)
      .I("rounds_played", s.rounds_played)
      .I("mvp", s.mvp)
      .I("score", s.score)
      .R("multi_kills", IntArray(s.multi_kills))
      .R("clutches_won", IntArray(s.clutches_won))
      .Done();
}

void StatsAccumulator::SetMoneyStart(uint64_t id, int money) {
  if (!live_ || id == 0) return;
  R(id).money_start = money;
}

void StatsAccumulator::SetLoadout(uint64_t id, int moneyAfterBuy, int armor, bool helmet, bool defuser,
                                  const std::vector<std::string>& loadout) {
  if (!live_ || id == 0) return;
  auto& r = R(id);
  r.money_after_buy = moneyAfterBuy;
  r.armor = armor;
  r.helmet = helmet;
  r.defuser = defuser;
  r.loadout = loadout;
}

std::string ToJson(const RoundSummary& r) {
  std::string players = "[";
  for (size_t i = 0; i < r.players.size(); ++i) {
    const auto& p = r.players[i];
    players += (i ? "," : "") + Obj()
                                    .S("id", std::to_string(p.id))
                                    .I("team", p.team)
                                    .I("side", p.side)
                                    .I("kills", p.kills)
                                    .I("assists", p.assists)
                                    .I("flash_assists", p.flash_assists)
                                    .I("damage", p.damage)
                                    .I("utility_damage", p.utility_damage)
                                    .I("headshot_kills", p.headshot_kills)
                                    .B("died", p.died)
                                    .B("survived", p.survived)
                                    .B("traded", p.traded)
                                    .B("kast", p.kast)
                                    .B("entry_kill", p.entry_kill)
                                    .B("entry_death", p.entry_death)
                                    .B("mvp", p.mvp)
                                    .I("clutch_vs", p.clutch_vs)
                                    .B("clutch_won", p.clutch_won)
                                    .Econ(p)
                                    .Done();
  }
  players += "]";
  return Obj()
      .I("round_number", r.round_number)
      .I("winner_side", r.winner_side)
      .I("winner_team", r.winner_team)
      .I("reason", r.reason)
      .I("team1_score", r.team1_score)
      .I("team2_score", r.team2_score)
      .B("team1_was_ct", r.team1_was_ct)
      .R("players", players)
      .Done();
}

std::string ToJson(const MapStats& m) {
  std::string players = "[";
  for (size_t i = 0; i < m.players.size(); ++i) {
    const auto& p = m.players[i];
    players += (i ? "," : "") + Obj()
                                    .S("id", std::to_string(p.id))
                                    .S("name", p.name)
                                    .I("team", p.team)
                                    .I("last_side", p.last_side)
                                    .B("bot", p.bot)
                                    .R("stats", ToJson(p.stats))
                                    .Done();
  }
  players += "]";
  std::string rounds = "[";
  for (size_t i = 0; i < m.rounds.size(); ++i) rounds += (i ? "," : "") + ToJson(m.rounds[i]);
  rounds += "]";
  return Obj()
      .B("live", m.live)
      .B("team1_is_ct", m.team1_is_ct)
      .R("team1", TeamJson(m.team1))
      .R("team2", TeamJson(m.team2))
      .R("players", players)
      .R("rounds", rounds)
      .Done();
}

namespace {

using minijson::Value;

int JI(const Value* o, const char* k) {
  const Value* v = o ? o->get(k) : nullptr;
  return v && v->type == Value::Type::Number ? static_cast<int>(v->num) : 0;
}
bool JB(const Value* o, const char* k) {
  const Value* v = o ? o->get(k) : nullptr;
  return v && v->type == Value::Type::Bool && v->b;
}
std::string JS(const Value* o, const char* k) {
  const Value* v = o ? o->get(k) : nullptr;
  return v && v->type == Value::Type::String ? v->str : std::string();
}
uint64_t JId(const Value* o) { return std::strtoull(JS(o, "id").c_str(), nullptr, 10); }
void JA(const Value* o, const char* k, std::array<int, 5>* out) {
  const Value* v = o ? o->get(k) : nullptr;
  if (!v || v->type != Value::Type::Array) return;
  for (size_t i = 0; i < out->size() && i < v->arr.size(); ++i) (*out)[i] = static_cast<int>(v->arr[i].num);
}
TeamLine JTeam(const Value* o) {
  TeamLine t;
  t.score = JI(o, "score");
  t.score_ct = JI(o, "score_ct");
  t.score_t = JI(o, "score_t");
  return t;
}
PlayerStats JStats(const Value* o) {
  PlayerStats s;
  s.kills = JI(o, "kills");
  s.deaths = JI(o, "deaths");
  s.assists = JI(o, "assists");
  s.flash_assists = JI(o, "flash_assists");
  s.team_kills = JI(o, "team_kills");
  s.suicides = JI(o, "suicides");
  s.headshot_kills = JI(o, "headshot_kills");
  s.knife_kills = JI(o, "knife_kills");
  s.damage = JI(o, "damage");
  s.utility_damage = JI(o, "utility_damage");
  s.enemies_flashed = JI(o, "enemies_flashed");
  s.friendlies_flashed = JI(o, "friendlies_flashed");
  s.bomb_plants = JI(o, "bomb_plants");
  s.bomb_defuses = JI(o, "bomb_defuses");
  s.entry_kills_t = JI(o, "entry_kills_t");
  s.entry_kills_ct = JI(o, "entry_kills_ct");
  s.entry_deaths_t = JI(o, "entry_deaths_t");
  s.entry_deaths_ct = JI(o, "entry_deaths_ct");
  s.trade_kills = JI(o, "trade_kills");
  s.traded_deaths = JI(o, "traded_deaths");
  s.kast_rounds = JI(o, "kast_rounds");
  s.rounds_played = JI(o, "rounds_played");
  s.mvp = JI(o, "mvp");
  s.score = JI(o, "score");
  JA(o, "multi_kills", &s.multi_kills);
  JA(o, "clutches_won", &s.clutches_won);
  return s;
}

}  // namespace

bool FromJson(const std::string& json, MapStats* out) {
  minijson::ParseError err;
  auto root = minijson::Parse(json, &err);
  if (!root || root->type != Value::Type::Object || !out) return false;
  MapStats m;
  m.live = JB(&*root, "live");
  m.team1_is_ct = JB(&*root, "team1_is_ct");
  m.team1 = JTeam(root->get("team1"));
  m.team2 = JTeam(root->get("team2"));
  if (const Value* ps = root->get("players"); ps && ps->type == Value::Type::Array) {
    for (const auto& p : ps->arr) {
      PlayerLine l;
      l.id = JId(&p);
      l.name = JS(&p, "name");
      l.team = JI(&p, "team");
      l.last_side = JI(&p, "last_side");
      l.bot = JB(&p, "bot");
      l.stats = JStats(p.get("stats"));
      if (l.id) m.players.push_back(std::move(l));
    }
  }
  if (const Value* rs = root->get("rounds"); rs && rs->type == Value::Type::Array) {
    for (const auto& r : rs->arr) {
      RoundSummary s;
      s.round_number = JI(&r, "round_number");
      s.winner_side = JI(&r, "winner_side");
      s.winner_team = JI(&r, "winner_team");
      s.reason = JI(&r, "reason");
      s.team1_score = JI(&r, "team1_score");
      s.team2_score = JI(&r, "team2_score");
      s.team1_was_ct = JB(&r, "team1_was_ct");
      if (const Value* pl = r.get("players"); pl && pl->type == Value::Type::Array) {
        for (const auto& p : pl->arr) {
          PlayerRound x;
          x.id = JId(&p);
          x.team = JI(&p, "team");
          x.side = JI(&p, "side");
          x.kills = JI(&p, "kills");
          x.assists = JI(&p, "assists");
          x.flash_assists = JI(&p, "flash_assists");
          x.damage = JI(&p, "damage");
          x.utility_damage = JI(&p, "utility_damage");
          x.headshot_kills = JI(&p, "headshot_kills");
          x.died = JB(&p, "died");
          x.survived = JB(&p, "survived");
          x.traded = JB(&p, "traded");
          x.kast = JB(&p, "kast");
          x.entry_kill = JB(&p, "entry_kill");
          x.entry_death = JB(&p, "entry_death");
          x.mvp = JB(&p, "mvp");
          x.clutch_vs = JI(&p, "clutch_vs");
          x.clutch_won = JB(&p, "clutch_won");
          if (p.get("money_start")) {
            x.money_start = JI(&p, "money_start");
            x.money_after_buy = JI(&p, "money_after_buy");
            x.armor = JI(&p, "armor");
            x.helmet = JB(&p, "helmet");
            x.defuser = JB(&p, "defuser");
            if (const Value* lo = p.get("loadout"); lo && lo->type == Value::Type::Array) {
              for (const auto& it : lo->arr)
                if (it.type == Value::Type::String) x.loadout.push_back(it.str);
            }
          }
          s.players.push_back(x);
        }
      }
      m.rounds.push_back(std::move(s));
    }
  }
  *out = std::move(m);
  return true;
}

StatsAccumulator& Current() {
  static StatsAccumulator s;
  return s;
}

std::recursive_mutex& Mutex() {
  static std::recursive_mutex m;
  return m;
}


int RewindTo(MapStats* m, int fromRound) {
  if (!m || fromRound < 1) return 0;
  auto dec = [](int& v, int by = 1) { v = v - by < 0 ? 0 : v - by; };
  int dropped = 0;
  bool sideSet = false;
  std::vector<RoundSummary> kept;
  for (const auto& r : m->rounds) {
    if (r.round_number < fromRound) {
      kept.push_back(r);
      continue;
    }
    ++dropped;
    if (!sideSet) {
      m->team1_is_ct = r.team1_was_ct;
      sideSet = true;
    }
    if (r.winner_team == 1 || r.winner_team == 2) {
      TeamLine& t = r.winner_team == 1 ? m->team1 : m->team2;
      const bool winnerCt = r.winner_team == 1 ? r.team1_was_ct : !r.team1_was_ct;
      dec(t.score);
      dec(winnerCt ? t.score_ct : t.score_t);
    }
    for (const auto& pr : r.players) {
      auto it = std::find_if(m->players.begin(), m->players.end(), [&](const PlayerLine& p) { return p.id == pr.id; });
      if (it == m->players.end()) continue;
      PlayerStats& s = it->stats;
      dec(s.kills, pr.kills);
      dec(s.assists, pr.assists);
      dec(s.flash_assists, pr.flash_assists);
      dec(s.damage, pr.damage);
      dec(s.utility_damage, pr.utility_damage);
      dec(s.headshot_kills, pr.headshot_kills);
      if (pr.died) dec(s.deaths);
      if (pr.kast) dec(s.kast_rounds);
      dec(s.rounds_played);
      if (pr.mvp) dec(s.mvp);
      if (pr.entry_kill) dec(pr.side == 2 ? s.entry_kills_t : s.entry_kills_ct);
      if (pr.entry_death) dec(pr.side == 2 ? s.entry_deaths_t : s.entry_deaths_ct);
      if (pr.traded) dec(s.traded_deaths);
      if (pr.kills > 0) dec(s.multi_kills[static_cast<size_t>(std::min(pr.kills, 5) - 1)]);
      if (pr.clutch_won && pr.clutch_vs >= 1 && pr.clutch_vs <= 5) dec(s.clutches_won[static_cast<size_t>(pr.clutch_vs - 1)]);
    }
  }
  m->rounds = std::move(kept);
  return dropped;
}

}  // namespace readyup::stats
