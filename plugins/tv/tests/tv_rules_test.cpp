// Offline tests for the tv plugin's logic (plugins/tv/tv_rules.h) and the shared broadcast profile
// (libs/readyup/broadcast_profile.h): parsing and validating profiles, the match.assign checks, the
// cvars a profile asks for, what gets sent and restored, which match profile is active, the saved
// settings, tv_status lines and the tvState JSON. ctest `tv_rules`.
#include "tv_rules.h"

#include "readyup/broadcast_profile.h"

#include <cstdio>
#include <string>

using namespace tv;
namespace bc = readyup::broadcast;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

static bool Has(const std::vector<std::string>& v, const std::string& x) {
  for (const auto& e : v) {
    if (e == x) return true;
  }
  return false;
}

static void ProfileTests() {
  Profile p;
  std::string err;
  CHECK(bc::ParseProfileText(R"({"delay_seconds":105,"allow_insecure_observers":true,"max_clients":8,)"
                             R"("relay_text_chat":false,"relay_voice":true})",
                             &p, &err));
  CHECK(p.delay_seconds == 105 && p.allow_insecure_observers == true && p.max_clients == 8 && p.relay_text_chat == false &&
        p.relay_voice == true);
  CHECK(bc::ProfileJson(p) ==
        R"({"delay_seconds":105,"allow_insecure_observers":true,"max_clients":8,"relay_text_chat":false,"relay_voice":true})");
  Profile back;
  CHECK(bc::ParseProfileText(bc::ProfileJson(p), &back, &err) && back == p);
  CHECK(bc::ParseProfileText("{}", &back, &err) && back.Empty() && bc::ProfileJson(back) == "{}");

  // Refused: unknown keys, aliases (JSON takes canonical names only), types, ranges, null.
  CHECK(!bc::ParseProfileText(R"({"tv_delay":105})", &back, &err) && Contains(err, "unknown key"));
  CHECK(!bc::ParseProfileText(R"({"delay":105})", &back, &err));
  CHECK(!bc::ParseProfileText(R"({"delay_seconds":"105"})", &back, &err));
  CHECK(!bc::ParseProfileText(R"({"delay_seconds":10.5})", &back, &err));
  CHECK(!bc::ParseProfileText(R"({"delay_seconds":961})", &back, &err) && Contains(err, "0..960"));
  CHECK(!bc::ParseProfileText(R"({"delay_seconds":-1})", &back, &err));
  CHECK(!bc::ParseProfileText(R"({"max_clients":256})", &back, &err));
  CHECK(!bc::ParseProfileText(R"({"relay_voice":1})", &back, &err) && Contains(err, "boolean"));
  CHECK(!bc::ParseProfileText(R"({"relay_voice":null})", &back, &err));
  CHECK(!bc::ParseProfileText(R"([1])", &back, &err));
  CHECK(!bc::ParseProfileText("{nope", &back, &err));
  // tv.set: null clears a key.
  std::vector<Key> cleared;
  CHECK(bc::ParseProfileText(R"({"relay_voice":null,"max_clients":4})", &back, &err, &cleared));
  CHECK(cleared.size() == 1 && cleared[0] == Key::Voice && back.max_clients == 4 && !back.relay_voice);

  // Console / chat values.
  Profile t;
  CHECK(bc::SetFromText(&t, Key::Delay, "90", &err) && t.delay_seconds == 90);
  CHECK(bc::SetFromText(&t, Key::Insecure, "on", &err) && t.allow_insecure_observers == true);
  CHECK(bc::SetFromText(&t, Key::Insecure, "No", &err) && t.allow_insecure_observers == false);
  CHECK(bc::SetFromText(&t, Key::Voice, "1", &err) && t.relay_voice == true);
  CHECK(!bc::SetFromText(&t, Key::Voice, "maybe", &err));
  CHECK(!bc::SetFromText(&t, Key::Delay, "9999", &err) && !bc::SetFromText(&t, Key::Delay, "-5", &err) &&
        !bc::SetFromText(&t, Key::MaxClients, "x", &err));
  CHECK(bc::SetFromText(&t, Key::Delay, "default", &err) && !t.delay_seconds);
  CHECK(bc::SetFromText(&t, Key::Voice, "-", &err) && !t.relay_voice);
  Key k;
  CHECK(bc::ParseKeyName("delay", &k) && k == Key::Delay && bc::ParseKeyName("INSECURE", &k) && k == Key::Insecure);
  CHECK(bc::ParseKeyName("maxclients", &k) && k == Key::MaxClients && !bc::ParseKeyName("tv_delay", &k));

  // Overlay: the top layer's set keys win, unset ones fall through.
  Profile base, over;
  base.delay_seconds = 90;
  base.relay_voice = true;
  over.delay_seconds = 105;
  over.allow_insecure_observers = true;
  const Profile o = bc::Overlay(base, over);
  CHECK(o.delay_seconds == 105 && o.relay_voice == true && o.allow_insecure_observers == true && !o.max_clients);
}

static void AssignTests() {
  using readyup::minijson::Parse;
  readyup::minijson::ParseError pe;
  auto cfg = [&](const char* j) { return *Parse(j, &pe); };
  std::string err;

  // The delay floor: valve ruleset 105, an explicit ruleset override, config.cvars.tv_delay.
  CHECK(bc::DelayFloor(cfg(R"({})")) == 0);
  CHECK(bc::DelayFloor(cfg(R"({"rules":{"ruleset":"valve"}})")) == 105);
  CHECK(bc::DelayFloor(cfg(R"({"rules":{"ruleset":"valve","overrides":{"tv_delay":30}}})")) == 30);
  CHECK(bc::DelayFloor(cfg(R"({"rules":{"ruleset":"default","overrides":{"tv_delay":90}}})")) == 90);
  CHECK(bc::DelayFloor(cfg(R"({"cvars":{"tv_delay":"120"}})")) == 120);
  CHECK(bc::DelayFloor(cfg(R"({"cvars":{"tv_delay":45},"rules":{"ruleset":"valve"}})")) == 105);

  CHECK(bc::ValidateAssignConfig(cfg(R"({})"), &err));
  CHECK(bc::ValidateAssignConfig(cfg(R"({"rules":{"broadcast":{"delay_seconds":105,"allow_insecure_observers":true}}})"), &err));
  CHECK(!bc::ValidateAssignConfig(cfg(R"({"rules":{"broadcast":{"delay_seconds":"x"}}})"), &err) &&
        Contains(err, "rules.broadcast"));
  CHECK(!bc::ValidateAssignConfig(cfg(R"({"rules":{"broadcast":[]}})"), &err));
  // Below the match's own delay: refused (a lower delay lets viewers ghost).
  CHECK(!bc::ValidateAssignConfig(cfg(R"({"rules":{"ruleset":"valve","broadcast":{"delay_seconds":30}}})"), &err) &&
        Contains(err, "below"));
  CHECK(bc::ValidateAssignConfig(cfg(R"({"rules":{"ruleset":"valve","broadcast":{"delay_seconds":120}}})"), &err));
  // Disagreeing with config.cvars: refused; agreeing: fine.
  CHECK(!bc::ValidateAssignConfig(cfg(R"({"cvars":{"tv_maxclients":"16"},"rules":{"broadcast":{"max_clients":8}}})"), &err) &&
        Contains(err, "tv_maxclients"));
  CHECK(bc::ValidateAssignConfig(cfg(R"({"cvars":{"tv_maxclients":8},"rules":{"broadcast":{"max_clients":8}}})"), &err));
  CHECK(!bc::ValidateAssignConfig(cfg(R"({"cvars":{"sv_deltaticks_enforce":"1"},"rules":{"broadcast":{"allow_insecure_observers":true}}})"), &err));
  CHECK(bc::ValidateAssignConfigText(R"({"rules":{"broadcast":{"relay_voice":false}}})", &err));
  CHECK(!bc::ValidateAssignConfigText("[]", &err));

  // What the platform sends with allow_insecure_observers: the pair in config.cvars too. Agrees,
  // so it is accepted; tv.so takes those cvars as the match's (not a baseline, no fight).
  const auto platform = cfg(R"({"cvars":{"tv_secure_bypass":"1","sv_deltaticks_enforce":0,"tv_delay":90,"mp_freezetime":15},)"
                            R"("rules":{"broadcast":{"allow_insecure_observers":true,"delay_seconds":90}}})");
  CHECK(bc::ValidateAssignConfig(platform, &err));
  const auto mc = bc::OwnedCvarsInAssign(platform);
  CHECK(mc.size() == 3 && mc[0] == std::make_pair(std::string("tv_delay"), std::string("90")) &&
        mc[1] == std::make_pair(std::string("tv_secure_bypass"), std::string("1")) &&
        mc[2] == std::make_pair(std::string("sv_deltaticks_enforce"), std::string("0")));
  const Profile fromCvars = bc::ProfileFromMatchCvars(mc);
  CHECK(fromCvars.allow_insecure_observers == true && !fromCvars.delay_seconds && !fromCvars.max_clients);
  Profile serverSays;
  serverSays.allow_insecure_observers = false;
  CHECK(bc::ConflictWithMatchCvars(serverSays, 90, mc) == "tv_secure_bypass");
  CHECK(bc::ConflictWithMatchCvars(bc::Overlay(serverSays, fromCvars), 90, mc).empty());  // the match's cvars win

  Profile p;
  CHECK(bc::ProfileFromAssign(cfg(R"({"rules":{"broadcast":{"max_clients":8}}})"), &p, &err) && p.max_clients == 8);
  CHECK(bc::ProfileFromAssign(cfg(R"({"rules":{}})"), &p, &err) && p.Empty());
  CHECK(bc::PasswordInAssign(cfg(R"({"cvars":{"tv_password":"s3cret"}})")));
  CHECK(!bc::PasswordInAssign(cfg(R"({"cvars":{"tv_password":""}})")) && !bc::PasswordInAssign(cfg(R"({})")));
}

static void DesiredTests() {
  Profile p;
  CHECK(bc::DesiredCvars(p, 0).empty());
  int d = 0;
  p.delay_seconds = 60;
  auto v = bc::DesiredCvars(p, 105, &d);
  CHECK(v.size() == 1 && v[0].first == "tv_delay" && v[0].second == "105" && d == 105);  // raised to the floor
  v = bc::DesiredCvars(p, 0, &d);
  CHECK(v[0].second == "60" && d == 60);
  // Insecure observers: the pair together.
  Profile q;
  q.allow_insecure_observers = true;
  v = bc::DesiredCvars(q, 0, &d);
  CHECK(d == -1 && v.size() == 2 && v[0] == std::make_pair(std::string("tv_secure_bypass"), std::string("1")) &&
        v[1] == std::make_pair(std::string("sv_deltaticks_enforce"), std::string("0")));
  q.allow_insecure_observers = false;
  v = bc::DesiredCvars(q, 0);
  CHECK(v.size() == 1 && v[0].first == "tv_secure_bypass" && v[0].second == "0");
  Profile r;
  r.max_clients = 8;
  r.relay_text_chat = true;
  r.relay_voice = false;
  v = bc::DesiredCvars(r, 0);
  CHECK(v.size() == 3 && v[0].first == "tv_maxclients" && v[1].first == "tv_relaytextchat" && v[1].second == "1" &&
        v[2].first == "tv_relayvoice" && v[2].second == "0");
  // A floor without a delay setting: tv_delay stays the match's (not touched).
  CHECK(bc::DesiredCvars(r, 105, &d).size() == 3 && d == -1);
}

static void ApplyTests() {
  Applier a;
  Profile p;
  p.allow_insecure_observers = true;
  p.delay_seconds = 105;
  auto desired = bc::DesiredCvars(p, 0);
  // Nothing set yet: read the baselines first.
  Plan plan = PlanApply(a, desired, false);
  CHECK(plan.commands.empty() && plan.needBaseline.size() == 3 && a.applied.empty());
  RecordBaseline(a, "tv_delay", "10");
  RecordBaseline(a, "tv_secure_bypass", "0");
  RecordBaseline(a, "sv_deltaticks_enforce", nullptr);  // no answer
  CHECK(a.baseline.at("tv_delay") == "10" && a.baselineUnknown.count("sv_deltaticks_enforce"));
  plan = PlanApply(a, desired, false);
  CHECK(plan.needBaseline.empty() && plan.commands.size() == 3);
  CHECK(Has(plan.commands, "tv_delay 105") && Has(plan.commands, "tv_secure_bypass 1") && Has(plan.commands, "sv_deltaticks_enforce 0"));
  // Again: nothing changed, nothing sent; forced (a map load): everything again.
  CHECK(PlanApply(a, desired, false).commands.empty());
  CHECK(PlanApply(a, desired, true).commands.size() == 3);
  // The profile ends: back to the baselines; tv_secure_bypass 0 even without one; a cvar without a
  // baseline is left (noted).
  Applier b = a;
  b.baseline.erase("tv_secure_bypass");
  plan = PlanApply(b, {}, false);
  CHECK(Has(plan.commands, "tv_delay 10") && Has(plan.commands, "tv_secure_bypass 0") &&
        Has(plan.commands, "sv_deltaticks_enforce 2") && plan.commands.size() == 3);  // CS2's defaults
  CHECK(plan.notes.empty() && b.applied.empty());
  Applier nb;
  nb.baselineUnknown = {"tv_maxclients"};
  (void)PlanApply(nb, {{"tv_maxclients", "8"}}, false);
  plan = PlanApply(nb, {}, false);
  CHECK(plan.commands.empty() && plan.notes.size() == 1 && Contains(plan.notes[0], "tv_maxclients"));
  plan = PlanApply(a, {}, false);
  CHECK(Has(plan.commands, "tv_secure_bypass 0") && Has(plan.commands, "tv_delay 10"));
  // Restoring a baseline equal to what is set sends nothing.
  Applier c;
  RecordBaseline(c, "tv_maxclients", "8");
  (void)PlanApply(c, {{"tv_maxclients", "8"}}, false);
  CHECK(PlanApply(c, {}, false).commands.empty());
  // Baseline values: numbers only; floats as integers; never a password.
  Applier d;
  RecordBaseline(d, "tv_delay", "105.000000");
  RecordBaseline(d, "tv_maxclients", "eight");
  RecordBaseline(d, "tv_password", "s3cret");
  CHECK(d.baseline.at("tv_delay") == "105" && d.baselineUnknown.count("tv_maxclients") && !d.baseline.count("tv_password") &&
        !d.baselineUnknown.count("tv_password"));
  // Stash round trip.
  Applier e;
  CHECK(ParseApplier(ApplierJson(a), &e) && e.baseline == a.baseline && e.applied == a.applied &&
        e.baselineUnknown == a.baselineUnknown);
  CHECK(!ParseApplier("nope", &e));
  CHECK(!Contains(ApplierJson(d), "s3cret"));
}

static void TrackerTests() {
  MatchTracker t;
  MatchProfile m;
  m.matchId = "m_1";
  m.profile.delay_seconds = 105;
  m.delayFloor = 105;
  t.Assign(m, 100);
  CHECK(t.pending && !t.active);
  CHECK(t.Observe("", 101) == TrackEvent::None && !t.active);       // not loaded yet
  CHECK(t.Observe("m_0", 102) == TrackEvent::None && !t.active);    // another match
  CHECK(t.Observe("m_1", 103) == TrackEvent::Activated && t.active && !t.pending);
  CHECK(t.Observe("m_1", 104) == TrackEvent::None);
  // A match plugin reload: the id is gone for a moment.
  CHECK(t.Observe("", 105) == TrackEvent::None && t.active);
  CHECK(t.Observe("m_1", 107) == TrackEvent::None && t.active);
  // Gone for good (series end): after the grace.
  CHECK(t.Observe("", 110) == TrackEvent::None && t.active);
  CHECK(t.Observe("", 114) == TrackEvent::None && t.active);
  CHECK(t.Observe("", 115.5) == TrackEvent::Deactivated && !t.active);

  // Unassign ends it at once; a new assign of the same match keeps the admin's override.
  t.Assign(m, 200);
  CHECK(t.Observe("m_1", 201) == TrackEvent::Activated);
  t.active->override_.relay_voice = false;
  MatchProfile again = m;
  again.profile.max_clients = 4;
  t.Assign(again, 202);
  CHECK(t.active && !t.pending && t.active->profile.max_clients == 4 && t.active->override_.relay_voice == false);
  CHECK(!t.Unassign("m_other") && t.active);
  CHECK(t.Unassign("m_1") && !t.active);

  // A pending profile whose match never loads (the match plugin refused it) is dropped.
  t.Assign(m, 300);
  CHECK(t.Observe("", 300 + MatchTracker::kPendingSeconds + 1) == TrackEvent::Dropped && !t.pending);
  t.Assign(m, 400, 900.0);
  CHECK(t.Observe("", 400 + MatchTracker::kPendingSeconds + 1) == TrackEvent::None && t.pending);

  // Switching straight to the next match.
  MatchTracker u;
  u.Assign(m, 0);
  (void)u.Observe("m_1", 1);
  MatchProfile n;
  n.matchId = "m_2";
  u.Assign(n, 2);
  CHECK(u.active->matchId == "m_1" && u.pending);
  CHECK(u.Observe("m_2", 3) == TrackEvent::Activated && u.active->matchId == "m_2");

  // match.update set_rules: rules.broadcast merge patch.
  {
    readyup::minijson::ParseError pe;
    auto J = [&](const char* j) { return *readyup::minijson::Parse(j, &pe); };
    Profile b;
    b.delay_seconds = 105;
    b.relay_voice = true;
    bool touched = false;
    std::string err;
    CHECK(ApplyBroadcastPatch(J(R"({"knife":{"side_pick_seconds":45}})"), &b, &touched, &err) && !touched && b.delay_seconds == 105);
    CHECK(ApplyBroadcastPatch(J(R"({"broadcast":{"allow_insecure_observers":true,"relay_voice":null}})"), &b, &touched, &err) &&
          touched && b.allow_insecure_observers == true && !b.relay_voice && b.delay_seconds == 105);
    CHECK(!ApplyBroadcastPatch(J(R"({"broadcast":{"delay_seconds":"x"}})"), &b, &touched, &err) && b.delay_seconds == 105);
    CHECK(ApplyBroadcastPatch(J(R"({"broadcast":null})"), &b, &touched, &err) && touched && b.Empty());
  }

  // active.json round trip.
  MatchProfile x;
  m.override_.max_clients = 3;
  m.passwordSet = true;
  m.matchCvars = {{"tv_secure_bypass", "1"}, {"sv_deltaticks_enforce", "0"}};
  CHECK(ParseMatchProfile(MatchProfileJson(m), &x) && x.matchId == "m_1" && x.profile == m.profile &&
        x.override_ == m.override_ && x.delayFloor == 105 && x.passwordSet && x.matchCvars == m.matchCvars);
  CHECK(!ParseMatchProfile("{}", &x) && !ParseMatchProfile("", &x));
}

static void SettingsTests() {
  bool ok = false;
  Settings s = ParseSettings("", &ok);
  CHECK(ok && s.server.Empty() && s.casters.empty() && !s.castersSet);
  s.server.allow_insecure_observers = true;
  s.server.delay_seconds = 105;
  s.casters = {76561198000000001ull, 76561198000000002ull};
  s.castersSet = true;
  Settings b = ParseSettings(SettingsJson(s), &ok);
  CHECK(ok && b.server == s.server && b.casters == s.casters && b.castersSet);
  Settings noCasters;
  CHECK(!ParseSettings(SettingsJson(noCasters), &ok).castersSet && ok);
  ParseSettings("{broken", &ok);
  CHECK(!ok);
  ParseSettings(R"({"server":{"delay_seconds":"x"}})", &ok);
  CHECK(!ok);

  int bad = 0;
  const auto ids = ParseSteamIdList("76561198000000001, 76561198000000002;nope 123", &bad);
  CHECK(ids.size() == 2 && bad == 2);
  CHECK(ParseSteamIdList("", &bad).empty() && bad == 0);

  std::vector<std::string> errors;
  const Profile p = ProfileFromConfig({{"delay_seconds", "105"}, {"allow_insecure_observers", "1"}, {"max_clients", "x"},
                                       {"relay_voice", "default"}, {"other", "1"}},
                                      &errors);
  CHECK(p.delay_seconds == 105 && p.allow_insecure_observers == true && !p.max_clients && !p.relay_voice);
  CHECK(errors.size() == 1 && Contains(errors[0], "max_clients"));
}

static void EngineViewTests() {
  LaunchInfo li = ParseLaunchArgs({"cs2", "-dedicated", "-port", "27015", "+tv_port", "27020", "+tv_password", "abc", "-insecure"});
  CHECK(li.tvPort == 27020 && li.passwordSet == true && li.insecure);
  li = ParseLaunchArgs({"cs2", "+tv_password", "+map", "de_dust2"});
  CHECK(li.passwordSet == false && !li.insecure && li.tvPort == 0);
  CHECK(!ParseLaunchArgs({"cs2"}).passwordSet);

  TvStatus st;
  CHECK(ParseTvStatusLine("SourceTV Master \"GOTV\", delay 105", &st) && st.delay == 105 && st.active == true);
  CHECK(ParseTvStatusLine("Local IP 10.0.0.5:27020, KB/sec In 0.1, Out 12.0,", &st) && st.port == 27020);
  CHECK(ParseTvStatusLine("Local Slots 8, Spectators 3, Proxies 0", &st) && st.clients == 3 && st.slots == 8);
  CHECK(ParseTvStatusLine("Total Slots 128, Spectators 40, Proxies 1", &st) && st.clients == 3);  // relays' total ignored
  TvStatus g;
  CHECK(ParseTvStatusLine("GOTV[0]: Local Slots 16, Spectators 0, Proxies 0", &g) && g.clients == 0 && g.slots == 16);
  CHECK(ParseTvStatusLine("GOTV[0]: Master \"x\", delay 90", &g) && g.delay == 90);
  CHECK(!ParseTvStatusLine("GOTV[1]: Local Slots 4, Spectators 2, Proxies 0", &g) && g.clients == 0);  // instance 1
  TvStatus off;
  // CS2 1.41.9.1 (live server, 2026-10-17).
  TvStatus live;
  CHECK(ParseTvStatusLine("Master \"SourceTV\", delay 105, frames 1026 / ticks 1026", &live) && live.delay == 105);
  CHECK(ParseTvStatusLine("Local Slots 8, Spectators 1, Proxies 0", &live) && live.clients == 1 && live.slots == 8);
  CHECK(ParseTvStatusLine("Total Slots 8, Spectators 1, Proxies 0", &live) && live.clients == 1);
  CHECK(!ParseTvStatusLine("   0    01:16    0    0     active 1572864 192.168.199.229:53211 'NTLAN'", &live) &&
        live.clients == 1 && !live.port);
  CHECK(ParseTvStatusLine("SourceTV not active.", &off) && off.active == false);
  CHECK(ParseTvStatusLine("GOTV[0]: not active", &off) && off.active == false);
  TvStatus none;
  CHECK(!ParseTvStatusLine("L 10/17/2026 - 20:00:00: \"player<2><[U:1:1]><CT>\" say \"hello\"", &none) && !none.clients);
  CHECK(!ParseTvStatusLine("tv_delay = 105", &none));
}

static void StateTests() {
  View v;
  v.port = 27020;
  v.observed.cvars = {{"tv_enable", "1"}, {"tv_delay", "105"}, {"tv_maxclients", "8"}, {"tv_secure_bypass", "1"},
                      {"sv_deltaticks_enforce", "0"}, {"tv_relaytextchat", "1"}, {"tv_relayvoice", "0"}};
  v.observed.clients = 3;
  v.effective.delay_seconds = 105;
  v.effective.allow_insecure_observers = true;
  v.source = "match";
  v.matchId = "m_1";
  v.delayFloor = 105;
  v.effectiveDelay = 105;
  v.passwordSet = true;
  v.casters = 2;
  const std::string j = StateJson(v);
  CHECK(j == R"({"available":true,"port":27020,"enabled":true,"delay_seconds":105,"max_clients":8,"clients":3,)"
             R"("secure_bypass":true,"deltaticks_enforce":false,"relay_text_chat":true,"relay_voice":false,)"
             R"("password_set":true,"profile":{"source":"match","match_id":"m_1","settings":{"delay_seconds":105,)"
             R"("allow_insecure_observers":true}},"delay_floor":105,"casters":2,"server_insecure":false})");
  CHECK(StateJson(v) == j);  // stable
  View empty;
  const std::string e = StateJson(empty);
  CHECK(Contains(e, R"("port":null)") && Contains(e, R"("clients":null)") && Contains(e, R"("password_set":null)") &&
        Contains(e, R"("profile":{"source":"server","settings":{}})"));
  // tv_port from the cvar when the command line had none.
  empty.observed.cvars["tv_port"] = "27021";
  CHECK(Contains(StateJson(empty), R"("port":27021)"));

  const auto lines = StatusLines(v);
  std::string all;
  for (const auto& l : lines) all += l + "\n";
  CHECK(Contains(all, "port 27020") && Contains(all, "delay 105 s") && Contains(all, "clients 3/8"));
  CHECK(Contains(all, "insecure observers (HLAE / HUD tools) on GOTV: allowed") && Contains(all, "VAC-secured"));
  CHECK(Contains(all, "password set") && Contains(all, "profile: match m_1") && Contains(all, "casters: 2"));
  v.whitelistLoaded = false;
  v.serverInsecure = true;
  all.clear();
  for (const auto& l : StatusLines(v)) all += l + "\n";
  CHECK(Contains(all, "whitelist.so is not loaded") && Contains(all, "runs -insecure"));
}

int main() {
  ProfileTests();
  AssignTests();
  DesiredTests();
  ApplyTests();
  TrackerTests();
  SettingsTests();
  EngineViewTests();
  StateTests();
  std::printf("tv_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
