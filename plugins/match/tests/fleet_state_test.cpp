// Offline tests for the engine-free half of the fleet match link (readyup/fleet_state.h) and the
// stats rewind used by round restores (match_stats.h RewindTo): merge patches and live_rev,
// epoch fencing, config_rev compare-and-set, match.assign -> MAT config (through the real
// parser), match.update ops, sha256 / base64, validators, workshop map entries / loaded map names
// (map_names.h) and failover resume blocks. ctest `match_fleet_state`.
#include "readyup/fleet_state.h"
#include "readyup/map_names.h"
#include "readyup/match_config_parser.h"
#include "readyup/match_rules.h"
#include "readyup/match_stats.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace readyup;
using fleetstate::Json;
namespace fs = readyup::fleetstate;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    ++g_checks;                                                                         \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                                     \
    }                                                                                   \
  } while (0)

#define CHECK_STR(a, b)                                                                                   \
  do {                                                                                                    \
    ++g_checks;                                                                                           \
    const std::string va = (a), vb = (b);                                                                 \
    if (va != vb) {                                                                                       \
      std::fprintf(stderr, "%s:%d: CHECK_STR failed: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #a, \
                   va.c_str(), vb.c_str());                                                               \
      ++g_failures;                                                                                       \
    }                                                                                                     \
  } while (0)

static Json J(const std::string& text) {
  Json v;
  std::string err;
  if (!Json::Parse(text, &v, &err)) {
    std::fprintf(stderr, "bad test JSON: %s: %s\n", err.c_str(), text.c_str());
    ++g_failures;
  }
  return v;
}

// ---------------------------------------------------------------------------- merge patch / rev

static void TestLiveStream() {
  fs::LiveStream s;
  const Json base = J(R"({"match_id":"m1","epoch":2,"live_rev":0,"phase":"warmup","side":null,
    "teams":{"team1":{"players":{"7656":{"ready":false,"connected":true}}}},"pause":{"active":false}})");
  s.Reset(base, 0);
  CHECK(s.Rev() == 0);
  CHECK(s.State().Find("side") == nullptr);  // nulls stripped from the baseline

  // No change: no rev.
  Json patch;
  long long rev = -1;
  CHECK(!s.Advance(base, false, &patch, &rev));
  CHECK(s.Rev() == 0);

  // One field deep in the roster: the patch has only that path + live_rev.
  Json next = base;
  next["teams"]["team1"]["players"]["7656"]["ready"] = true;
  CHECK(s.Advance(next, false, &patch, &rev));
  CHECK(rev == 1);
  CHECK_STR(patch.Dump(), R"({"live_rev":1,"teams":{"team1":{"players":{"7656":{"ready":true}}}}})");

  // Forced (an event without a state change): rev + 1, patch = live_rev only.
  CHECK(s.Advance(next, true, &patch, &rev));
  CHECK(rev == 2);
  CHECK_STR(patch.Dump(), R"({"live_rev":2})");

  // A removed member becomes null; applying every patch in order reproduces the state.
  Json replay = fs::StripNulls(base);
  replay["live_rev"] = 0;
  s.Reset(replay, 0);
  Json a = next;
  a["phase"] = "live";
  Json b = a;
  b["pause"] = J(R"({"active":true,"type":"admin","by":"platform:u1"})");
  Json c = b;
  c["pause"] = J(R"({"active":false})");
  c["round"] = J(R"({"number":3})");
  for (const Json* st : {&a, &b, &c}) {
    CHECK(s.Advance(*st, false, &patch, &rev));
    replay = status::MergePatchApply(replay, patch);
  }
  CHECK(rev == 3);
  Json want = fs::StripNulls(c);
  want["live_rev"] = 3;
  CHECK(replay == want);
  CHECK(replay == s.State());
  // pause.type / pause.by were removed with a null in the last patch.
  CHECK(replay.Find("pause")->Find("type") == nullptr);
}

// ---------------------------------------------------------------------------- epoch fence / CAS

static void TestFence() {
  fs::Fence f;
  fs::Assignment none;
  CHECK(f.CheckAssign(none, "m1", 1, false) == fs::Verdict::Ok);
  CHECK(f.CheckAssign(none, "m1", 0, false) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckAssign(none, "m1", 1, /*localMatchActive=*/true) == fs::Verdict::Busy);

  fs::Assignment cur;
  cur.active = true;
  cur.match_id = "m1";
  cur.epoch = 3;
  f.Retire("m1", 3);
  CHECK(f.CheckAssign(cur, "m1", 3, false) == fs::Verdict::Duplicate);   // replayed assign
  CHECK(f.CheckAssign(cur, "m1", 2, false) == fs::Verdict::StaleEpoch);  // late, lower epoch
  CHECK(f.CheckAssign(cur, "m1", 4, false) == fs::Verdict::Ok);          // re-issued, newer epoch
  CHECK(f.CheckAssign(cur, "m2", 1, false) == fs::Verdict::Busy);        // one match at a time

  CHECK(f.CheckScoped(cur, "m1", 3) == fs::Verdict::Ok);
  CHECK(f.CheckScoped(cur, "m1", 2) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckScoped(cur, "m1", 4) == fs::Verdict::NotAssigned);
  CHECK(f.CheckScoped(cur, "m2", 1) == fs::Verdict::NotAssigned);
  CHECK_STR(fs::VerdictCode(fs::Verdict::StaleEpoch), "stale_epoch");

  // After the unassign (retired at epoch 3): a late assign / cmd of that epoch is fenced off.
  CHECK(f.CheckAssign(none, "m1", 3, false) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckAssign(none, "m1", 2, false) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckScoped(none, "m1", 3) == fs::Verdict::StaleEpoch);
  CHECK(f.CheckAssign(none, "m1", 4, false) == fs::Verdict::Ok);

  // Survives a plugin reload.
  fs::Fence g;
  g.FromJson(f.ToJson());
  CHECK(g.Retired("m1") == 3);
  // Bounded.
  for (int i = 0; i < 100; ++i) g.Retire("x" + std::to_string(i), 1);
  CHECK(g.Retired("m1") == 0);
  CHECK(g.Retired("x99") == 1);

  CHECK(fs::ConfigCas(4, 4));
  CHECK(!fs::ConfigCas(3, 4));
}

// ---------------------------------------------------------------------------- assign / update

static const char* kAssign = R"({
  "match_id": "ko-r1-m3", "epoch": 1, "config_rev": 5,
  "config": {
    "num_maps": 3,
    "maps": [{"number":1,"name":"de_mirage","sides":"knife"},
             {"number":2,"name":"de_inferno","sides":"team1_ct"},
             {"number":3,"name":"de_nuke","workshop_id":"3070284539","sides":"team2_ct"}],
    "team1": {"id":"t-a","name":"Alpha","tag":"ALP","flag":"NO","captain":"76561198000000001",
              "players":[{"steamid64":"76561198000000001","name":"a1"},
                         {"steamid64":"76561198000000002","name":"a2","role":"sub"},
                         {"steamid64":"76561198000000003","name":"coachA","role":"coach"}]},
    "team2": {"id":"t-b","name":"Bravo","players":[{"steamid64":"76561198000000011","name":"b1"}]},
    "spectators": ["76561198000000099"],
    "admins": ["76561198000000077"],
    "password": "s3cret-pw",
    "rules": {"max_rounds": 24, "overtime": {"enabled": true, "rounds_per_half": 3, "max_overtimes": 2},
              "tiebreak": {"damage": true, "sudden_death_on_tie": false},
              "ready": {"min_per_team": 4, "allow_force_ready": false, "autoready": false},
              "pause": {"tactical_per_team": 2, "tactical_seconds": 45, "technical_per_team": 5,
                        "technical_seconds": 120, "unpause": "caller_team"},
              "forfeit": {"team_absent_seconds": 0},
              "knife": {"side_pick_seconds": 45}, "clinch_series": false, "whitelist": true},
    "cvars": {"mp_freezetime": 12, "sv_cheats": 1, "ru_hack": "1", "tv_delay": "90", "sv_password": "x",
              "mp_bad;cmd": "1", "mp_quote": "a\"b"}
  }
})";

static void TestAssign() {
  const Json p = J(kAssign);
  std::string err;
  CHECK(fs::ValidateAssign(p, &err));
  if (!err.empty()) std::fprintf(stderr, "validate: %s\n", err.c_str());

  std::vector<std::string> dropped;
  const Json mat = fs::AssignToMatConfig("ko-r1-m3", *p.Find("config"), &dropped);
  CHECK(dropped.size() == 5);  // sv_cheats, ru_hack, sv_password, mp_bad;cmd, mp_quote
  auto ctx = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  CHECK(ctx.has_value());
  if (!ctx) {
    std::fprintf(stderr, "parse: %s\n%s\n", err.c_str(), mat.Dump().c_str());
    return;
  }
  CHECK(ctx->matchid == fs::NumericMatchId("ko-r1-m3"));
  CHECK_STR(ctx->slug, "ko-r1-m3");
  CHECK(ctx->num_maps == 3);
  CHECK(ctx->maplist.size() == 3 && ctx->maplist[2] == "workshop/3070284539/de_nuke");  // host_workshop_map
  CHECK(ctx->map_sides.size() == 3 && ctx->map_sides[1] == "team1_ct");
  CHECK_STR(ctx->team1_name, "Alpha");
  CHECK(ctx->team1_captain_steamid64 == 76561198000000001ull);
  CHECK(ctx->roster_team.size() == 3);  // a1, a2 (sub), b1; the coach is a spectator
  CHECK(ctx->roster_team.count(76561198000000002ull) == 1);
  CHECK(ctx->roster_team.count(76561198000000003ull) == 0);
  CHECK(ctx->spectators.count(76561198000000003ull) == 1);
  CHECK(ctx->spectators.count(76561198000000099ull) == 1);
  CHECK(ctx->admins.count(76561198000000077ull) == 1);
  CHECK(ctx->maxRounds == 24);
  CHECK(ctx->overtime_enabled && ctx->overtimeSegments == 3 && ctx->maxOvertimes == 2);
  CHECK(ctx->damageTiebreakEnabled && !ctx->suddenDeathOnDamageTie);
  CHECK(ctx->knifeDecisionSeconds == 45);
  CHECK(!ctx->clinch_series);
  CHECK(ctx->cvars.count("mp_freezetime") && ctx->cvars["mp_freezetime"] == "12");
  CHECK(ctx->cvars["tv_delay"] == "90");
  CHECK(ctx->cvars["mp_maxrounds"] == "24");
  CHECK(ctx->cvars["mp_overtime_maxrounds"] == "6");
  CHECK(ctx->cvars.count("sv_cheats") == 0 && ctx->cvars.count("ru_hack") == 0 && ctx->cvars.count("sv_password") == 0);
  // Pause / ready / forfeit rules and team flags (match_rules.h); tactical timeouts are engine cvars.
  CHECK_STR(ctx->team1_flag, "NO");
  CHECK(ctx->team2_flag.empty());
  CHECK(ctx->rules.tech_pauses_per_team == 5);
  CHECK(ctx->rules.tech_pause_max_seconds == 120);
  CHECK(ctx->rules.both_teams_unpause == 0);
  CHECK(ctx->rules.allow_force_ready == 0);
  CHECK(ctx->rules.min_players_to_ready == 4);
  CHECK(ctx->rules.forfeit_after_seconds == 0);
  CHECK(ctx->cvars["mp_team_timeout_max"] == "2");
  CHECK(ctx->cvars["mp_team_timeout_time"] == "45");
  // Server settings the match sets (server_settings.h): a team with a substitute gives the team size
  // (1 starter each here), rules.whitelist / rules.ready.autoready; playout left out stays unset.
  CHECK(ctx->players_per_team == 1);
  // The substitute is named as one (ready count, live state role).
  CHECK(ctx->substitutes.size() == 1 && ctx->substitutes.count(76561198000000002ull) == 1);
  CHECK(ctx->whitelist == 1 && ctx->autoready == 0 && ctx->playout == -1);
  {
    Json po = *p.Find("config");
    po["rules"] = J(R"({"max_rounds": 24, "playout": true, "whitelist": false})");
    auto c3 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", po, nullptr).Dump(), &err);
    CHECK(c3 && c3->playout == 1 && c3->whitelist == 0 && c3->autoready == -1);
  }
  {
    // Rules left out stay unset (-1) so the server's readyup.cfg / defaults apply.
    Json bare = *p.Find("config");
    bare["rules"] = J(R"({"max_rounds": 24})");
    auto c2 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", bare, nullptr).Dump(), &err);
    CHECK(c2 && c2->rules.tech_pauses_per_team == -1 && c2->rules.forfeit_after_seconds == -1 &&
          c2->rules.both_teams_unpause == -1 && c2->cvars.count("mp_team_timeout_max") == 0);
    CHECK(c2 && c2->playout == -1 && c2->whitelist == -1 && c2->autoready == -1);
    // No substitute: no team size (the ready gate caps a full team at 5).
    Json noSub = bare;
    noSub["team1"] = J(R"({"name":"A","players":[{"steamid64":"76561198000000001","name":"a1"}]})");
    auto c4 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", noSub, nullptr).Dump(), &err);
    CHECK(c4 && c4->players_per_team == 0);
  }

  {
    // Wingman and simulation (wingman.h, simulation.h): rules.wingman, rules.simulation.timescale.
    Json ws = *p.Find("config");
    ws["rules"] = J(R"({"max_rounds": 16, "wingman": true, "simulation": {"timescale": 2}})");
    auto c5 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", ws, nullptr).Dump(), &err);
    CHECK(c5 && c5->wingman && c5->simulation && c5->simulation_timescale == 2.0 && c5->maxRounds == 16);
    CHECK(c5 && c5->roster_names[76561198000000011ull] == "b1");
    Json plain = *p.Find("config");
    plain["rules"] = J(R"({"max_rounds": 24})");
    auto c6 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", plain, nullptr).Dump(), &err);
    CHECK(c6 && !c6->wingman && !c6->simulation);
    // rules.demo (demo_recorder.h): absent = the server's settings (-1).
    CHECK(c6 && c6->demo_record == -1 && c6->demo_upload == -1);
    CHECK(!fs::AssignToMatConfig("x", plain, nullptr).Find("config")->Find("demo_record"));
  }
  {
    // rules.demo.record / .upload -> demo_record / demo_upload, per match.
    Json dm = *p.Find("config");
    dm["rules"] = J(R"({"max_rounds": 24, "demo": {"record": true, "upload": false}})");
    const Json m = fs::AssignToMatConfig("x", dm, nullptr);
    const Json& mc = *m.Find("config");
    CHECK(mc.Find("demo_record") && mc.Find("demo_record")->AsBool());
    CHECK(mc.Find("demo_upload") && !mc.Find("demo_upload")->AsBool());
    auto c7 = ParseWebhookMatchContextFromJson(m.Dump(), &err);
    CHECK(c7 && c7->demo_record == 1 && c7->demo_upload == 0);
    dm["rules"] = J(R"({"max_rounds": 24, "demo": {"record": false}})");
    auto c8 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", dm, nullptr).Dump(), &err);
    CHECK(c8 && c8->demo_record == 0 && c8->demo_upload == -1);
    dm["rules"] = J(R"({"max_rounds": 24, "demo": {"record": "yes", "upload": 1}})");  // not booleans: ignored
    auto c9 = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("x", dm, nullptr).Dump(), &err);
    CHECK(c9 && c9->demo_record == -1 && c9->demo_upload == -1);
  }

  CHECK(fs::NumericMatchId("12345") == 12345);
  CHECK(fs::NumericMatchId("abc") != 0 && fs::NumericMatchId("abc") < (1ull << 52));
  CHECK(fs::NumericMatchId("abc") == fs::NumericMatchId("abc"));

  // Hand-over membership (D16).
  const Json& cfg = *p.Find("config");
  CHECK(fs::InAssignedMatch(cfg, 76561198000000001ull));
  CHECK(fs::InAssignedMatch(cfg, 76561198000000003ull));  // coach
  CHECK(fs::InAssignedMatch(cfg, 76561198000000099ull));  // spectator
  CHECK(fs::InAssignedMatch(cfg, 76561198000000077ull));  // match admin
  CHECK(!fs::InAssignedMatch(cfg, 76561198000000555ull));
  CHECK_STR(fs::TeamOf(cfg, 76561198000000011ull), "team2");
  CHECK_STR(fs::TeamOf(cfg, 76561198000000003ull), "spectator");

  // Invalid payloads.
  auto bad = [&](const char* path, const Json& value) {
    Json q = p;
    Json* cur = &q;
    std::string ps = path;
    size_t pos = 0;
    while ((pos = ps.find('.')) != std::string::npos) {
      cur = &(*cur)[ps.substr(0, pos)];
      ps = ps.substr(pos + 1);
    }
    (*cur)[ps] = value;
    std::string e;
    return !fs::ValidateAssign(q, &e) && !e.empty();
  };
  CHECK(bad("epoch", Json(0)));
  CHECK(bad("match_id", Json("bad id")));
  CHECK(bad("config.password", Json("has space")));
  CHECK(bad("config.password", Json("semi;colon")));
  CHECK(bad("config.num_maps", Json(5)));  // only 3 maps listed
  CHECK(bad("config.maps", J(R"([{"name":"de_dust2; quit","sides":"knife"}])")));
  CHECK(bad("config.team1", J(R"({"name":"X","players":[{"steamid64":"123abc"}]})")));
  CHECK(bad("config.team2", J(R"({"name":"","players":[]})")));
}

// Rulesets through match.assign: rules.ruleset / rules.overrides validated (cmd.result
// invalid_config), knife sides refused under valve, the MAT config carries them, coaches listed.
static void TestRulesetAssign() {
  const Json p = J(kAssign);
  std::string err;
  auto with = [&](const std::string& rulesPatch, const char* maps) {
    Json q = p;
    Json& c = q["config"];
    c["rules"] = status::MergePatchApply(*c.Find("rules"), J(rulesPatch));
    if (maps) c["maps"] = J(maps);
    return q;
  };
  const char* veto = R"([{"name":"de_mirage","sides":"team1_ct"},{"name":"de_inferno","sides":"team2_ct"},
                         {"name":"de_nuke","sides":"team1_ct"}])";
  // kAssign map 1 is a knife map: refused under valve, with the reason.
  CHECK(!fs::ValidateAssign(with(R"({"ruleset":"valve"})", nullptr), &err));
  CHECK(err.find("map 1") != std::string::npos && err.find("allow_knife") != std::string::npos);
  CHECK(fs::ValidateAssign(with(R"({"ruleset":"valve","overrides":{"allow_knife":true}})", nullptr), &err));
  // Bad ruleset / unknown or bad override keys: refused, the key named.
  CHECK(!fs::ValidateAssign(with(R"({"ruleset":"esl"})", veto), &err) && err.find("rules.ruleset") != std::string::npos);
  CHECK(!fs::ValidateAssign(with(R"({"ruleset":"valve","overrides":{"freeztime":3}})", veto), &err));
  CHECK(err.find("rules.overrides: unknown override \"freeztime\"") != std::string::npos);
  CHECK(!fs::ValidateAssign(with(R"({"overrides":{"overtime":{"limit":-1}}})", veto), &err));
  CHECK(err.find("overtime.limit") != std::string::npos);

  const Json q = with(R"({"ruleset":"valve","overrides":{"freezetime":3,"overtime":{"startmoney":12500}}})", veto);
  CHECK(fs::ValidateAssign(q, &err));
  const Json mat = fs::AssignToMatConfig("ko-r1-m3", *q.Find("config"), nullptr);
  CHECK_STR(mat.Find("config")->Find("ruleset")->AsString(), "valve");
  CHECK(mat.Find("config")->Find("coaches")->Items().size() == 1);
  auto ctx = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  CHECK(ctx.has_value());
  if (ctx) {
    CHECK_STR(ctx->ruleset, "valve");
    CHECK_STR(ctx->overrides_json, R"({"freezetime":3,"overtime":{"startmoney":12500}})");
    CHECK(ctx->coaches.count(76561198000000003ull) == 1);
    CHECK(ctx->spectators.count(76561198000000003ull) == 0);  // online valve match: no coach
    CHECK(ctx->maxOvertimes == -1 && !ctx->damageTiebreakEnabled);
  }
  // match.update set_rules is validated the same way.
  Json cfg = *q.Find("config");
  bool pw = false;
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"set_rules","rules":{"overrides":{"bogus":1}}}])"), &err, &pw));
  CHECK(err.find("bogus") != std::string::npos);
  CHECK(fs::ApplyUpdateOps(&cfg, J(R"([{"op":"set_rules","rules":{"overrides":{"lan":true}}}])"), &err, &pw));
}

static void TestUpdateOps() {
  Json cfg = *J(kAssign).Find("config");
  std::string err;
  bool pw = false;
  CHECK(fs::ApplyUpdateOps(&cfg, J(R"([
      {"op":"add_player","team":"team2","steamid64":"76561198000000012","name":"b2"},
      {"op":"add_player","team":"team1","steamid64":"76561198000000011","name":"b1-moved"},
      {"op":"remove_player","steamid64":"76561198000000099"},
      {"op":"rename_team","team":"team2","name":"Bravo Esports"},
      {"op":"set_password","password":"n3w"},
      {"op":"set_rules","rules":{"max_rounds":12,"knife":{"side_pick_seconds":30}}}])"),
                            &err, &pw));
  CHECK(pw);
  CHECK_STR(fs::TeamOf(cfg, 76561198000000011ull), "team1");  // moved, not duplicated
  CHECK_STR(fs::TeamOf(cfg, 76561198000000012ull), "team2");
  CHECK_STR(fs::TeamOf(cfg, 76561198000000099ull), "");
  CHECK_STR(cfg.Find("team2")->Find("name")->AsString(), "Bravo Esports");
  CHECK_STR(cfg.Find("password")->AsString(), "n3w");
  CHECK(cfg.Find("rules")->Find("max_rounds")->AsInt() == 12);
  CHECK(cfg.Find("rules")->Find("overtime")->Find("rounds_per_half")->AsInt() == 3);  // merged, not replaced
  CHECK(cfg.Find("rules")->Find("knife")->Find("side_pick_seconds")->AsInt() == 30);

  // A bad op leaves the config untouched (all or nothing).
  const std::string before = cfg.Dump();
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"rename_team","team":"team1","name":"ok"},
                                       {"op":"remove_player","steamid64":"76561198000000999"}])"),
                            &err, &pw));
  CHECK_STR(cfg.Dump(), before);
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"set_password","password":"a b"}])"), &err, &pw));
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([{"op":"explode"}])"), &err, &pw));
  CHECK(!fs::ApplyUpdateOps(&cfg, J(R"([])"), &err, &pw));
}

// ---------------------------------------------------------------------------- codecs / validators

static void TestCodecs() {
  CHECK_STR(fs::Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_STR(fs::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_STR(fs::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string million(1000000, 'a');
  CHECK_STR(fs::Sha256Hex(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  CHECK_STR(fs::Base64Encode(""), "");
  CHECK_STR(fs::Base64Encode("f"), "Zg==");
  CHECK_STR(fs::Base64Encode("fo"), "Zm8=");
  CHECK_STR(fs::Base64Encode("foo"), "Zm9v");
  CHECK_STR(fs::Base64Encode("foobar"), "Zm9vYmFy");
  std::string bin;
  for (int i = 0; i < 256; ++i) bin.push_back(static_cast<char>(i));
  std::string back;
  CHECK(fs::Base64Decode(fs::Base64Encode(bin), &back) && back == bin);
  CHECK(fs::Base64Decode("Zm8=", &back) && back == "fo");
  CHECK(!fs::Base64Decode("Zm8=x", &back));
  CHECK(!fs::Base64Decode("Z!8=", &back));
}

static void TestValidators() {
  std::string err;
  CHECK(fs::ValidateExec("mp_roundtime 2", &err));
  CHECK(fs::ValidateExec("status; mp_restartgame 1", &err));
  CHECK(!fs::ValidateExec("", &err));
  CHECK(!fs::ValidateExec(std::string(513, 'a'), &err));
  CHECK(!fs::ValidateExec("say a\nquit", &err));
  CHECK(!fs::ValidateExec("ru fleet enroll x", &err));
  CHECK(!fs::ValidateExec("status;  FLEET reconnect", &err));
  CHECK(!fs::ValidateExec("\"ru\" \"fleet\" status", &err));
  CHECK(fs::ValidateExec("ru status", &err));

  CHECK(fs::ValidPassword(""));
  CHECK(fs::ValidPassword("Ab-12_x!"));
  CHECK(!fs::ValidPassword("a b"));
  CHECK(!fs::ValidPassword("a\"b"));
  CHECK(!fs::ValidPassword(std::string(65, 'a')));
  CHECK(fs::SafeMapName("de_dust2"));
  CHECK(fs::SafeMapName("workshop/3070284539/de_nuke"));
  CHECK(!fs::SafeMapName("de_dust2 ; quit"));
  CHECK(!fs::SafeMapName("../../etc"));
  CHECK(fs::SafeWorkshopId("3070284539"));
  CHECK(!fs::SafeWorkshopId("30x"));

  CHECK(fs::BackupRoundFromName("readyup_backup_12_map1_round07.txt") == 7);
  CHECK(fs::BackupRoundFromName("backup_round00.txt") == 0);
  CHECK(fs::BackupRoundFromName("readyup_backup_12_map1_.txt") == -1);
  CHECK(fs::SafeBackupFileName("readyup_backup_12_map1_round07.txt"));
  CHECK(!fs::SafeBackupFileName("../x_round07.txt"));
  CHECK(!fs::SafeBackupFileName("x_round07.cfg"));
  CHECK(!fs::SafeBackupFileName(".hidden.txt"));

  // Backup scan: only this map's files written since the match went live (live at t = 100 s).
  const std::string pre = "readyup_backup_12_map1_";
  CHECK(fs::LiveSessionBackup("readyup_backup_12_map1__round03.txt", pre, 130000, 100000));
  CHECK(fs::LiveSessionBackup("readyup_backup_12_map1__round00.txt", pre, 99500, 100000));  // the go-live round's file
  CHECK(!fs::LiveSessionBackup("readyup_backup_12_map1__round974.txt", pre, 60000, 100000));  // a warmup, a restart
  CHECK(!fs::LiveSessionBackup("readyup_backup_12_map1__round03.txt", pre, 130000, -1));     // not live
  CHECK(!fs::LiveSessionBackup("readyup_backup_12_map2__round03.txt", pre, 130000, 100000));
  CHECK(!fs::LiveSessionBackup("readyup_resume_12_map1_round03.txt", pre, 130000, 100000));
  CHECK(!fs::LiveSessionBackup("readyup_backup_12_map1__round03.txt.tmp", pre, 130000, 100000));
  CHECK(!fs::LiveSessionBackup("readyup_backup_12_map1_.txt", pre, 130000, 100000));

  // File times are Unix ms: libstdc++'s file clock counts from 2174, so its raw count is negative
  // today and read as "not live" (-1 = not live).
  const long long sysNow = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  const long long fileNow = fs::FileTimeUnixMs(std::filesystem::file_time_type::clock::now());
  CHECK(fileNow > 0);
  CHECK(fileNow - sysNow < 1000 && sysNow - fileNow < 1000);
  CHECK(fs::LiveSessionBackup("readyup_backup_12_map1__round03.txt", pre, fileNow + 30000, fileNow));
}

// ---------------------------------------------------------------------------- stats rewind

constexpr uint64_t A1 = 76561198000000001ull, A2 = 76561198000000002ull;
constexpr uint64_t B1 = 76561198000000011ull, B2 = 76561198000000012ull;

static void ObserveAll(stats::StatsAccumulator& st, bool team1Ct) {
  const int s1 = team1Ct ? 3 : 2, s2 = team1Ct ? 2 : 3;
  st.ObservePlayer(A1, "A1", s1, 1);
  st.ObservePlayer(A2, "A2", s1, 1);
  st.ObservePlayer(B1, "B1", s2, 2);
  st.ObservePlayer(B2, "B2", s2, 2);
}

// Round 1: team1 wins (A1 entry kill, A1 2k). Round 2: team2 wins (B1 3k incl. a clutch-free
// entry, B2 MVP). Round 3 (after a side swap): team2 wins by time.
static void PlayRound(stats::StatsAccumulator& st, int n, double t0) {
  st.OnRoundStart();
  if (n == 1) {
    ObserveAll(st, true);
    st.OnPlayerHurt(B1, A1, 100, 0, "ak47");
    st.OnPlayerDeath(B1, A1, 0, false, true, "ak47", t0 + 1);
    st.OnPlayerHurt(B2, A1, 100, 0, "ak47");
    st.OnPlayerDeath(B2, A1, A2, false, false, "ak47", t0 + 20);
    st.OnRoundMvp(A1);
    st.OnRoundEnd(3, 8);
  } else if (n == 2) {
    ObserveAll(st, true);
    st.OnPlayerHurt(A1, B1, 100, 0, "hegrenade");
    st.OnPlayerDeath(A1, B1, 0, false, false, "hegrenade", t0 + 1);
    st.OnPlayerHurt(A2, B1, 100, 0, "m4a1");
    st.OnPlayerDeath(A2, B1, B2, true, true, "m4a1", t0 + 30);
    st.OnRoundMvp(B2);
    st.OnRoundEnd(2, 9);
  } else {
    st.SetTeam1IsCt(false);
    ObserveAll(st, false);
    st.OnPlayerHurt(B1, A2, 40, 60, "glock");
    st.OnRoundEnd(3, 12);
  }
}

static void TestRewind() {
  stats::StatsAccumulator full, oneRound;
  full.BeginMap(true);
  oneRound.BeginMap(true);
  for (int n = 1; n <= 3; ++n) PlayRound(full, n, n * 100.0);
  PlayRound(oneRound, 1, 100.0);

  stats::MapStats m = full.Snapshot();
  CHECK(m.rounds.size() == 3);
  CHECK(stats::RewindTo(&m, 2) == 2);
  const stats::MapStats want = oneRound.Snapshot();
  CHECK(m.rounds.size() == 1);
  CHECK(m.team1_is_ct == true);  // back to the side of round 2
  CHECK(m.team1.score == want.team1.score && m.team1.score_ct == want.team1.score_ct);
  CHECK(m.team2.score == want.team2.score && m.team2.score_t == want.team2.score_t &&
        m.team2.score_ct == want.team2.score_ct);
  for (const auto& p : want.players) {
    const stats::PlayerLine* got = nullptr;
    for (const auto& q : m.players) {
      if (q.id == p.id) got = &q;
    }
    CHECK(got != nullptr);
    if (got) CHECK_STR(stats::ToJson(got->stats), stats::ToJson(p.stats));
  }
  // Rewinding to round 1 clears everything the rounds carried; nothing past the end is dropped.
  stats::MapStats all = full.Snapshot();
  CHECK(stats::RewindTo(&all, 1) == 3);
  CHECK(all.rounds.empty() && all.team1.score == 0 && all.team2.score == 0);
  for (const auto& p : all.players) {
    CHECK(p.stats.kills == 0 && p.stats.deaths == 0 && p.stats.rounds_played == 0 && p.stats.damage == 0);
  }
  stats::MapStats same = full.Snapshot();
  CHECK(stats::RewindTo(&same, 4) == 0);
  CHECK(stats::RewindTo(nullptr, 1) == 0);

  // A restored accumulator continues from the rewound snapshot.
  stats::StatsAccumulator resumed;
  resumed.Restore(m);
  CHECK(resumed.Team1Score() == want.team1.score);
}

// ---------------------------------------------------------------------------- workshop maps

static void TestMapNames() {
  namespace mn = readyup::mapnames;
  mn::ResetBindings();
  mn::MapRef r;
  CHECK(mn::ParseEntry("de_dust2", &r) && r.workshop_id.empty() && r.name == "de_dust2");
  CHECK(mn::ParseEntry("3084291314", &r) && r.workshop_id == "3084291314" && r.name.empty());
  CHECK(mn::ParseEntry("ws:3084291314", &r) && r.workshop_id == "3084291314" && r.name.empty());
  CHECK(mn::ParseEntry("workshop/3084291314", &r) && r.workshop_id == "3084291314" && r.name.empty());
  CHECK(mn::ParseEntry("workshop/3084291314/aim_map", &r) && r.workshop_id == "3084291314" && r.name == "aim_map");
  CHECK(!mn::ValidEntry(""));
  CHECK(!mn::ValidEntry("ws:"));
  CHECK(!mn::ValidEntry("ws:12a"));
  CHECK(!mn::ValidEntry("workshop/x/aim_map"));
  CHECK(!mn::ValidEntry("workshop/123/../x"));
  CHECK(!mn::ValidEntry("de_dust2;quit"));
  CHECK(!mn::ValidEntry("de dust2"));
  CHECK(!mn::ValidEntry(std::string(21, '1')));  // > 20 digits

  // Entries from match.assign maps[] {name, workshop_id}.
  CHECK_STR(mn::MakeEntry("de_nuke", ""), "de_nuke");
  CHECK_STR(mn::MakeEntry("aim_map", "3084291314"), "workshop/3084291314/aim_map");
  CHECK_STR(mn::MakeEntry("ws:3084291314", ""), "ws:3084291314");
  CHECK_STR(mn::MakeEntry("ws:1", "3084291314"), "workshop/3084291314");  // the id field wins
  CHECK_STR(mn::MakeEntry("", "3084291314"), "workshop/3084291314");
  CHECK_STR(mn::MakeEntry("bad name", ""), "");
  CHECK_STR(mn::MakeEntry("x", "12y"), "");

  CHECK_STR(mn::LoadCommand("de_dust2"), "changelevel de_dust2");
  CHECK_STR(mn::LoadCommand("3084291314"), "host_workshop_map 3084291314");
  CHECK_STR(mn::LoadCommand("ws:3084291314"), "host_workshop_map 3084291314");
  CHECK_STR(mn::LoadCommand("workshop/3084291314/aim_map"), "host_workshop_map 3084291314");
  CHECK_STR(mn::LoadCommand("de_dust2; quit"), "");

  // Loaded map names: the bsp name (CS2 1.41 logs `Loading map "aim_map"` after
  // host_workshop_map) or "workshop/<id>/<name>".
  CHECK_STR(mn::LoadedBaseName("aim_map"), "aim_map");
  CHECK_STR(mn::LoadedBaseName("workshop/3084291314/aim_map"), "aim_map");
  CHECK_STR(mn::LoadedBaseName("maps/aim_map.vpk"), "aim_map");
  CHECK_STR(mn::LoadedBaseName("workshop/3084291314"), "");
  CHECK_STR(mn::WorkshopIdOfLoaded("workshop/3084291314/aim_map"), "3084291314");
  CHECK_STR(mn::WorkshopIdOfLoaded("aim_map"), "");

  CHECK(mn::EntryMatchesLoaded("de_dust2", "de_dust2"));
  CHECK(mn::EntryMatchesLoaded("de_dust2", "DE_DUST2"));
  CHECK(!mn::EntryMatchesLoaded("de_dust2", "de_nuke"));
  CHECK(!mn::EntryMatchesLoaded("de_dust2", ""));
  CHECK(mn::EntryMatchesLoaded("workshop/3084291314/aim_map", "aim_map"));
  CHECK(mn::EntryMatchesLoaded("workshop/3084291314/aim_map", "workshop/3084291314/aim_map"));
  CHECK(!mn::EntryMatchesLoaded("workshop/3084291314/aim_map", "workshop/999/aim_map"));
  CHECK(mn::EntryMatchesLoaded("ws:3084291314", "workshop/3084291314/aim_map"));
  // An id-only entry matches a bare bsp name once its host_workshop_map loaded that map.
  CHECK(!mn::EntryMatchesLoaded("3084291314", "aim_map"));
  CHECK_STR(mn::DisplayName("3084291314"), "workshop/3084291314");
  mn::NoteWorkshopLoad("3084291314");
  mn::NoteMapLoaded("aim_map");
  CHECK_STR(mn::BoundName("3084291314"), "aim_map");
  CHECK(mn::EntryMatchesLoaded("3084291314", "aim_map"));
  CHECK(mn::EntryMatchesLoaded("ws:3084291314", "aim_map"));
  CHECK(!mn::EntryMatchesLoaded("3084291314", "de_dust2"));
  CHECK_STR(mn::DisplayName("3084291314"), "aim_map");
  CHECK_STR(mn::DisplayName("workshop/3084291314/aim_map"), "aim_map");
  CHECK_STR(mn::DisplayName("de_dust2"), "de_dust2");
  // The next map change without a pending load binds nothing; a "workshop/<id>/x" name binds itself.
  mn::NoteMapLoaded("de_dust2");
  CHECK_STR(mn::BoundName("3084291314"), "aim_map");
  mn::NoteMapLoaded("workshop/3070244462/aim_botz");
  CHECK_STR(mn::BoundName("3070244462"), "aim_botz");
  // A pending load overtaken by another workshop map does not bind the wrong name.
  mn::NoteWorkshopLoad("111");
  mn::NoteMapLoaded("workshop/222/other");
  CHECK_STR(mn::BoundName("111"), "");
  CHECK_STR(mn::BoundName("222"), "other");
  mn::ResetBindings();
  CHECK_STR(mn::BoundName("3084291314"), "");

  // match.assign: workshop maps in the maplist the MAT parser takes, "ws:<id>" names accepted.
  const Json p = J(R"({"match_id":"ws-1","epoch":1,"config":{"num_maps":3,
    "maps":[{"name":"ws:3084291314","sides":"knife"},{"name":"aim_map","workshop_id":"3084291314","sides":"team1_ct"},
            {"name":"workshop/3070244462","sides":"knife"}],
    "team1":{"name":"A","players":[]},"team2":{"name":"B","players":[]},"password":""}})");
  std::string err;
  CHECK(fs::ValidateAssign(p, &err));
  if (!err.empty()) std::fprintf(stderr, "validate: %s\n", err.c_str());
  const Json mat = fs::AssignToMatConfig("ws-1", *p.Find("config"), nullptr);
  auto ctx = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  CHECK(ctx.has_value());
  if (ctx) {
    CHECK(ctx->maplist.size() == 3);
    CHECK_STR(ctx->maplist[0], "ws:3084291314");
    CHECK_STR(ctx->maplist[1], "workshop/3084291314/aim_map");
    CHECK_STR(ctx->maplist[2], "workshop/3070244462");
  }
  Json bad = p;
  bad["config"]["maps"] = J(R"([{"name":"ws:abc","sides":"knife"}])");
  bad["config"]["num_maps"] = 1;
  CHECK(!fs::ValidateAssign(bad, &err));
}

// ---------------------------------------------------------------------------- failover resume

static Json ResumeConfig() {
  return J(R"({"num_maps":3,
    "maps":[{"name":"de_mirage","sides":"knife"},{"name":"de_inferno","sides":"knife"},{"name":"de_nuke","sides":"team2_ct"}],
    "team1":{"name":"A","players":[]},"team2":{"name":"B","players":[]},"password":"",
    "rules":{"pause":{"pause_after_restore":false}}})");
}

static Json InlineBackup(const std::string& raw, int map, int round) {
  Json b = Json::Object();
  b["map_number"] = map;
  b["round"] = round;
  b["file"] = "readyup_backup_77_map" + std::to_string(map) + "_round0" + std::to_string(round - 1) + ".txt";
  b["size"] = static_cast<long long>(raw.size());
  b["sha256"] = fs::Sha256Hex(raw);
  Json sc = Json::Object();
  sc["team1"] = 4;
  sc["team2"] = 2;
  b["score"] = sc;
  b["encoding"] = "base64";
  b["data"] = fs::Base64Encode(raw);
  return b;
}

static void TestResume() {
  const Json cfg = ResumeConfig();
  const std::string raw = "\"SaveFile\"\n{\n\t\"round\"\t\"7\"\n}\n";
  std::string code, err;
  fs::ResumePlan p;

  // Inline backup + the platform's state: series score, map 1's result, the knife-decided sides.
  Json r = J(R"({"from_epoch":2,"map_number":2,"round":7,
    "state":{"match_id":"m","epoch":2,"config_rev":1,"live_rev":40,"phase":"live",
      "series":{"num_maps":3,"current_map":2,"score":{"team1":1,"team2":0},
        "maps":{"1":{"name":"de_mirage","sides":"team1_ct","status":"done","score":{"team1":13,"team2":7},"winner":"team1"},
                "2":{"name":"de_inferno","sides":"team2_ct","status":"live","score":{"team1":4,"team2":2}},
                "3":{"name":"de_nuke","sides":"team2_ct","status":"pending"}}},
      "teams":{"team1":{"id":"a","name":"A","side":"t","score":4,"players":{}},
               "team2":{"id":"b","name":"B","side":"ct","score":2,"players":{}}}}})");
  r["backup"] = InlineBackup(raw, 2, 7);
  CHECK(fs::ParseResume(r, cfg, 3, &p, &code, &err));
  if (!err.empty()) std::fprintf(stderr, "resume: %s\n", err.c_str());
  CHECK(p.present && p.from_epoch == 2 && p.map_number == 2 && p.round == 7);
  CHECK(p.inline_backup && p.raw == raw && p.sha256 == fs::Sha256Hex(raw));
  CHECK(p.series_team1 == 1 && p.series_team2 == 0);
  CHECK(p.maps_done.size() == 1 && p.maps_done[0].map_number == 1 && p.maps_done[0].team1 == 13 &&
        p.maps_done[0].winner == "team1");
  CHECK_STR(p.sides, "team2_ct");
  CHECK(p.score_team1 == 4 && p.score_team2 == 2);  // backup.score
  CHECK_STR(p.team1_side, "t");
  CHECK(!p.pause_after_restore);

  // Round-trip for plugin reloads (the raw file is not kept).
  const fs::ResumePlan q = fs::ResumeFromJson(fs::ResumeToJson(p));
  CHECK(q.present && q.map_number == 2 && q.round == 7 && q.sha256 == p.sha256 && q.raw.empty());
  CHECK(q.maps_done.size() == 1 && q.maps_done[0].team1 == 13 && q.sides == "team2_ct" && !q.pause_after_restore);
  CHECK(q.score_team1 == 4 && q.team1_side == "t" && q.from_epoch == 2);

  // Explicit fields win over the state.
  Json r2 = r;
  r2["series_score"] = J(R"({"team1":0,"team2":1})");
  r2["maps"] = J(R"({"1":{"score":{"team1":8,"team2":13},"winner":"team2"}})");
  r2["sides"] = "team1_ct";
  r2["score"] = J(R"({"team1":3,"team2":3})");
  CHECK(fs::ParseResume(r2, cfg, 3, &p, &code, &err));
  CHECK(p.series_team2 == 1 && p.maps_done.size() == 1 && p.maps_done[0].winner == "team2");
  CHECK_STR(p.sides, "team1_ct");
  CHECK(p.score_team1 == 3 && p.score_team2 == 3);

  // Errors: codes the platform shows.
  auto rejects = [&](Json bad, long long epoch, const char* wantCode) {
    std::string c, e;
    const bool ok = fs::ParseResume(bad, cfg, epoch, nullptr, &c, &e);
    if (ok || c != wantCode) std::fprintf(stderr, "  resume case: ok=%d code=%s (%s)\n", ok, c.c_str(), e.c_str());
    return !ok && c == wantCode;
  };
  Json b = r;
  b["backup"]["sha256"] = std::string(64, '0');
  CHECK(rejects(b, 3, "checksum"));
  b = r;
  b["backup"]["size"] = 1;
  CHECK(rejects(b, 3, "checksum"));
  b = r;
  b["backup"]["parts"] = 2;
  CHECK(rejects(b, 3, "unsupported"));
  b = r;
  b["backup"]["data"] = "!!";
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["backup"]["file"] = "../x_round06.txt";
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["round"] = 6;  // not the backup's round
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["map_number"] = 4;  // > num_maps
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["from_epoch"] = 3;  // not older than the new epoch
  CHECK(rejects(b, 3, "invalid_config"));
  b = r;
  b["backup_ref"] = Json::Object();  // both
  CHECK(rejects(b, 3, "invalid_config"));
  // A knife map without decided sides cannot resume mid-map.
  b = J(R"({"map_number":1,"round":3,"backup_ref":{}})");
  CHECK(rejects(b, 3, "invalid_config"));
  b["sides"] = "team1_ct";
  CHECK(fs::ParseResume(b, cfg, 3, &p, &code, &err));
  CHECK(!p.inline_backup && p.file.empty() && p.round == 3 && p.series_team1 == 0);
  // backup_ref needs a round; its sha256 must look like one.
  CHECK(rejects(J(R"({"map_number":3,"backup_ref":{"file":"x_round01.txt"}})"), 3, "invalid_config"));
  CHECK(rejects(J(R"({"map_number":3,"round":2,"backup_ref":{"sha256":"xyz"}})"), 3, "invalid_config"));
  CHECK(fs::ParseResume(J(R"({"map_number":3,"round":2,"backup_ref":{"file":"x_round01.txt"}})"), cfg, 3, &p, &code, &err));
  CHECK_STR(p.file, "x_round01.txt");
  CHECK(p.sides.empty());  // map 3 has fixed sides in the config
  // No backup, no round: map 3 restarts from warmup (the knife map 1 could too).
  CHECK(fs::ParseResume(J(R"({"map_number":1,"series_score":{"team1":0,"team2":0}})"), cfg, 3, &p, &code, &err));
  CHECK(p.round == 0 && p.file.empty() && p.pause_after_restore == false);
  CHECK(rejects(J(R"({"map_number":1,"series_score":{"team1":-1,"team2":0}})"), 3, "invalid_config"));
  CHECK(rejects(J(R"([1])"), 3, "invalid_config"));
}

// server.config settings -> server settings, console settings and the demo upload target.
static void TestServerConfigPlan() {
  const Json full = J(R"({
    "chat_prefix": "<Green>[AT]<Default>", "admin_chat_prefix": "<Red>[Staff]<Default>",
    "hostname_format": "{TEAM1} vs {TEAM2}",
    "demo": {"path": "demos/", "name_format": "{MATCH_ID}_{MAP}"},
    "series_end_kick_delay": {"no_demo": 5, "demo_no_upload": 10, "demo_upload": 60},
    "offline_pause_minutes": 4, "scrim_when_idle": false, "scrim_knife": true,
    "warmup": {"message_html": "<b>\"hi\"</b>", "respawn": true, "money": 16000},
    "status_http": {"token": "0123456789abcdef0123"}
  })");
  const fs::ServerConfigPlan plan = fs::PlanServerConfig(full);
  auto find = [](const std::vector<std::pair<std::string, std::string>>& v, const std::string& k) {
    for (const auto& kv : v) {
      if (kv.first == k) return kv.second;
    }
    return std::string("<absent>");
  };
  CHECK(plan.settings.size() == 8);
  CHECK_STR(find(plan.settings, "hostname_format"), "{TEAM1} vs {TEAM2}");
  CHECK_STR(find(plan.settings, "knife_enabled_default"), "1");
  CHECK_STR(find(plan.settings, "scrim_when_idle"), "0");
  CHECK_STR(find(plan.settings, "chat_prefix"), "<Green>[AT]<Default>");
  CHECK_STR(find(plan.settings, "admin_chat_prefix"), "<Red>[Staff]<Default>");
  CHECK_STR(find(plan.settings, "series_end_kick_delay_no_demo"), "5");
  CHECK_STR(find(plan.settings, "series_end_kick_delay_demo_no_upload"), "10");
  CHECK_STR(find(plan.settings, "series_end_kick_delay_demo_upload"), "60");
  CHECK(plan.console.size() == 5);
  CHECK_STR(find(plan.console, "ru_demo_path"), "demos/");
  CHECK_STR(find(plan.console, "ru_demo_name_format"), "{MATCH_ID}_{MAP}");
  CHECK_STR(find(plan.console, "ru_warmup_message_html"), "<b>\"hi\"</b>");
  CHECK_STR(find(plan.console, "ru_warmup_respawn"), "1");
  CHECK_STR(find(plan.console, "ru_warmup_startmoney"), "16000");
  CHECK(plan.skipped.empty());
  // fleet.so's fields are not the match plugin's.
  CHECK_STR(find(plan.settings, "offline_pause_minutes"), "<absent>");
  CHECK_STR(find(plan.console, "status_http_token"), "<absent>");

  // Absent fields are left alone.
  const fs::ServerConfigPlan some = fs::PlanServerConfig(J(R"({"scrim_knife": false, "demo": {"path": "x/"}})"));
  CHECK(some.settings.size() == 1 && some.settings[0].first == "knife_enabled_default" && some.settings[0].second == "0");
  CHECK(some.console.size() == 1 && some.console[0].first == "ru_demo_path");
  CHECK(fs::PlanServerConfig(J("{}")).settings.empty());
  CHECK(fs::PlanServerConfig(J("[]")).settings.empty());

  // Wrong types are skipped and named.
  const fs::ServerConfigPlan bad =
      fs::PlanServerConfig(J(R"({"chat_prefix": ["x"], "warmup": {"money": 1.5}, "scrim_when_idle": null})"));
  CHECK(bad.settings.empty() && bad.console.empty());
  CHECK(bad.skipped.size() == 2);
}

// M1 play-test: a sub added to a 5v5 by match.update counted toward the ready total (10/11).
static void TestSubstitutes() {
  std::string err;
  Json cfg = J(R"({
    "num_maps": 1, "maps": [{"number":1,"name":"de_mirage","sides":"knife"}],
    "team1": {"name":"A","players":[{"steamid64":"76561198000000001","name":"a1"},{"steamid64":"76561198000000002","name":"a2"},
      {"steamid64":"76561198000000003","name":"a3"},{"steamid64":"76561198000000004","name":"a4"},{"steamid64":"76561198000000005","name":"a5"}]},
    "team2": {"name":"B","players":[{"steamid64":"76561198000000011","name":"b1"},{"steamid64":"76561198000000012","name":"b2"},
      {"steamid64":"76561198000000013","name":"b3"},{"steamid64":"76561198000000014","name":"b4"},{"steamid64":"76561198000000015","name":"b5"}]},
    "password": "pw", "rules": {"max_rounds": 24}
  })");
  auto before = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("m", cfg, nullptr).Dump(), &err);
  CHECK(before && before->substitutes.empty() && before->players_per_team == 0);
  bool pw = false;
  CHECK(fs::ApplyUpdateOps(&cfg, J(R"([{"op":"add_player","team":"team1","steamid64":"76561198000000006","name":"sub1","role":"sub"}])"),
                           &err, &pw));
  auto after = ParseWebhookMatchContextFromJson(fs::AssignToMatConfig("m", cfg, nullptr).Dump(), &err);
  CHECK(after.has_value());
  if (!after) return;
  CHECK(after->roster_team.size() == 11);  // on the roster (whitelisted, may stand in)
  CHECK(after->substitutes.size() == 1 && after->substitutes.count(76561198000000006ull) == 1);
  CHECK(after->players_per_team == 5);
  int roster[2] = {0, 0};
  for (const auto& kv : after->roster_team) ++roster[kv.second == WebhookTeam::Team2 ? 1 : 0];
  const ReadyTally t1 = TeamReadyTally(roster[0], 5, after->players_per_team);
  const ReadyTally t2 = TeamReadyTally(roster[1], 5, after->players_per_team);
  CHECK(t1.ready + t2.ready == 10 && t1.total + t2.total == 10);  // 10/10, not 10/11

  // A MAT config naming substitutes without players_per_team: a full team is the starters.
  auto mat = ParseWebhookMatchContextFromJson(R"({"matchid": 7, "num_maps": 1, "maplist": ["de_mirage"],
    "team1": {"name":"A","players":{"76561198000000001":"a1","76561198000000002":"a2"},"substitutes":{"76561198000000003":"s1"}},
    "team2": {"name":"B","players":{"76561198000000011":"b1","76561198000000012":"b2"}}})", &err);
  CHECK(mat.has_value());
  if (!mat) return;
  CHECK(mat->substitutes.count(76561198000000003ull) == 1 && mat->roster_team.count(76561198000000003ull) == 1);
  CHECK(mat->players_per_team == 2);
}

int main() {
  TestLiveStream();
  TestSubstitutes();
  TestFence();
  TestAssign();
  TestUpdateOps();
  TestRulesetAssign();
  TestCodecs();
  TestValidators();
  TestRewind();
  TestMapNames();
  TestResume();
  TestServerConfigPlan();
  if (g_failures) {
    std::fprintf(stderr, "fleet_state_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("fleet_state_test: all %d checks passed\n", g_checks);
  return 0;
}
