// The step-3 protocol schemas (plugins/fleet/protocol/v1: match.defs.json, messages/match.*,
// cmd*, state.*, event.*, server.availability; proposed, docs/fleet-step3-platform-notes.md)
// against the example frames in plugins/fleet/protocol/examples/v1: every example envelope and
// payload must validate, and a few broken copies must not. Examples captured from the live test
// (scripts/livetest/fleet_livetest.py --save-examples) are real frames Ready Up sent. ctest
// `fleet_protocol`.
#include "fleet_json.h"
#include "fleet_store.h"
#include "schema_check.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
const std::string kBase = "https://auto-tournament.dev/fleet/v1/";

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

using fleet::json::Value;

bool ValidateFrame(const schema::Set& set, const Value& env, const std::string& what, bool quiet = false) {
  std::vector<std::string> errs;
  bool ok = set.Validate(env, kBase + "envelope.json", &errs);
  const Value* type = env.Get("type");
  const Value* payload = env.Get("payload");
  const std::string id = kBase + "messages/" + (type ? type->AsStr() : std::string("?")) + ".json";
  if (!set.Has(id)) {
    errs.push_back("no schema for type " + (type ? type->AsStr() : std::string("?")));
    ok = false;
  } else if (payload) {
    ok = set.Validate(*payload, id, &errs) && ok;
  }
  if (!ok && !quiet) {
    std::fprintf(stderr, "%s:\n", what.c_str());
    for (const auto& e : errs) std::fprintf(stderr, "  %s\n", e.c_str());
  }
  return ok;
}

Value Load(const std::string& path) {
  std::string text;
  Value v;
  if (!fleet::ReadFile(path, &text) || !fleet::json::Parse(text, &v)) std::fprintf(stderr, "cannot read %s\n", path.c_str());
  return v;
}

}  // namespace

int main() {
  schema::Set set;
  std::string err;
  if (!set.LoadDir(FLEET_PROTOCOL_DIR, &err)) {
    std::fprintf(stderr, "schemas: %s\n", err.c_str());
    return 1;
  }
  for (const char* t : {"match.assign", "match.update", "match.unassign", "cmd", "cmd.result", "state.snapshot",
                        "state.patch", "state.request", "server.availability", "event.round_end", "event.map_result",
                        "event.backup", "event.phase", "event.pause", "event.demo", "event.series_end", "admins.set",
                        "server.drain", "server.undrain", "server.cs2_update_required", "server.selftest", "skins.loadout", "skins.invalidate", "skins.stattrak", "event.admin_called"}) {
    CHECK(set.Has(kBase + "messages/" + t + ".json"));
  }

  // Every example validates.
  std::vector<std::string> files;
  if (DIR* d = opendir(FLEET_EXAMPLES_DIR)) {
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) files.push_back(n);
    }
    closedir(d);
  }
  std::sort(files.begin(), files.end());
  CHECK(files.size() >= 10);
  for (const auto& f : files) {
    const Value env = Load(std::string(FLEET_EXAMPLES_DIR) + "/" + f);
    CHECK(ValidateFrame(set, env, f));
  }

  // Broken copies do not.
  auto broken = [&](const char* file, void (*mutate)(Value*)) {
    Value env = Load(std::string(FLEET_EXAMPLES_DIR) + "/" + file);
    mutate(env.Get("payload"));
    return !ValidateFrame(set, env, file, /*quiet=*/true);
  };
  CHECK(broken("match.assign.json", [](Value* p) { p->Get("config")->Set("password", Value::Str("has space")); }));
  CHECK(broken("match.assign.json", [](Value* p) { p->Set("epoch", Value::Int(0)); }));
  CHECK(broken("match.assign.json", [](Value* p) {
    p->Get("config")->Get("maps")->a[0].Set("sides", Value::Str("ct"));
  }));
  CHECK(broken("match.update.json", [](Value* p) {
    p->Get("ops")->a[0].Set("op", Value::Str("teleport"));
  }));
  CHECK(broken("cmd.pause.json", [](Value* p) { p->Set("name", Value::Str("rm_rf")); }));
  CHECK(broken("backup.part.json", [](Value* p) { p->Get("backup")->Set("file", Value::Str("../x.txt")); }));
  CHECK(broken("backup.part.json", [](Value* p) { p->Set("match_id", Value::Int(1)); }));
  CHECK(broken("cmd.result.json", [](Value* p) { p->Set("status", Value::Str("maybe")); }));
  CHECK(broken("state.patch.json", [](Value* p) { p->Set("rev", Value::Int(0)); }));
  CHECK(broken("event.pause.json", [](Value* p) { p->Get("data")->Set("type", Value::Str("coffee")); }));

  // Demo streaming (FLEET.md §12.2): the contract the platform implements.
  CHECK(broken("demo.begin.json", [](Value* p) { p->Set("file", Value::Str("../../etc/passwd.dem")); }));
  CHECK(broken("demo.begin.json", [](Value* p) { p->Set("chunk_size", Value::Int(1 << 20)); }));
  CHECK(broken("demo.begin.json", [](Value* p) { p->Set("demo_id", Value::Str("not-a-ulid")); }));
  CHECK(broken("demo.begin.json", [](Value* p) { p->Set("extra", Value::Int(1)); }));
  CHECK(broken("demo.chunk.json", [](Value* p) { p->Set("offset", Value::Int(-1)); }));
  CHECK(broken("demo.chunk.json", [](Value* p) { p->Set("size", Value::Int(0)); }));
  CHECK(broken("demo.end.json", [](Value* p) { p->Set("sha256", Value::Str("ABC")); }));
  CHECK(broken("demo.ack.json", [](Value* p) { p->Set("offset", Value::Str("12")); }));
  CHECK(broken("demo.ack.gap.json", [](Value* p) { p->Get("error")->Set("code", Value::Str("oops")); }));

  // hello.plugins_disabled (core needs.json enforcement): name + reason required.
  CHECK(broken("hello.plugins_disabled.json", [](Value* p) {
    p->Get("plugins_disabled")->a[0] = Value::Object();
  }));
  CHECK(broken("hello.plugins_disabled.json", [](Value* p) { p->Set("plugins_disabled", Value::Str("skins")); }));

  // hello.plugins_state (installed .so files, plugins.json disabled): both lists, plugin names.
  CHECK(broken("hello.plugins_state.json", [](Value* p) { p->Get("plugins_state")->Set("installed", Value::Str("match")); }));
  CHECK(broken("hello.plugins_state.json", [](Value* p) {
    p->Get("plugins_state")->Get("disabled")->a[0] = Value::Str("../skins");
  }));
  CHECK(broken("hello.plugins_state.json", [](Value* p) { p->Get("plugins_state")->Set("extra", Value::Int(1)); }));

  // hello.host.public_addr (fleet.cfg public_addr / +ip / net_public_adr): optional string.
  CHECK(broken("hello.public_addr.json", [](Value* p) { p->Get("host")->Set("public_addr", Value::Int(27055)); }));
  CHECK(broken("hello.public_addr.json", [](Value* p) {
    p->Get("host")->Set("public_addr", Value::Str(std::string(256, 'a')));
  }));

  // hello.selftest / server.selftest (core selftest_summary, ru_api 1.11): all four fields, typed.
  CHECK(broken("hello.selftest.json", [](Value* p) { p->Get("selftest")->Set("pass", Value::Str("yes")); }));
  CHECK(broken("hello.selftest.json", [](Value* p) { p->Set("selftest", Value::Object()); }));
  CHECK(broken("server.selftest.json", [](Value* p) { p->Set("passed", Value::Int(-1)); }));
  CHECK(broken("server.selftest.json", [](Value* p) { p->Get("failures")->Push(Value::Int(3)); }));
  CHECK(broken("server.selftest.json", [](Value* p) { *p = Value::Object(); }));
  CHECK(broken("event.admin_called.json", [](Value* p) {
    p->Get("data")->Get("player")->Set("team", Value::Str("coach"));
  }));
  CHECK(broken("event.admin_called.json", [](Value* p) {
    p->Get("data")->Get("player")->Set("steamid64", Value::Int(76561198000000001LL));
  }));
  CHECK(broken("event.admin_called.json", [](Value* p) { p->Get("data")->Set("called_at", Value::Str("yesterday")); }));
  CHECK(broken("event.admin_called.json", [](Value* p) { p->Get("data")->Set("message", Value::Str(std::string(801, 'x'))); }));

  // `.admin` from a spectator / a player whose team or side is unknown, with no message.
  {
    Value env = Load(std::string(FLEET_EXAMPLES_DIR) + "/event.admin_called.json");
    Value* data = env.Get("payload")->Get("data");
    data->Get("player")->Set("team", Value::Str("spectator"));
    data->Get("player")->Set("side", Value::Null());
    data->Set("message", Value::Str(""));
    CHECK(ValidateFrame(set, env, "event.admin_called.json spectator"));
    data->Get("player")->Set("team", Value::Null());
    CHECK(ValidateFrame(set, env, "event.admin_called.json team null"));
  }

  // Every pause type the match plugin reports (pause_state.h) validates.
  for (const char* type : {"tactical", "technical", "admin", "offline", "halftime", "auto_5v5"}) {
    Value env = Load(std::string(FLEET_EXAMPLES_DIR) + "/event.pause.json");
    env.Get("payload")->Get("data")->Set("type", Value::Str(type));
    CHECK(ValidateFrame(set, env, std::string("event.pause.json type=") + type));
  }

  if (g_failures) {
    std::fprintf(stderr, "fleet_protocol_test: %d of %d checks FAILED (%zu examples)\n", g_failures, g_checks, files.size());
    return 1;
  }
  std::printf("fleet_protocol_test: all %d checks passed (%zu schemas, %zu examples)\n", g_checks, set.size(),
              files.size());
  return 0;
}
