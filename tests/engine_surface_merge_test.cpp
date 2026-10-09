// Gamedata fragments (core/src/readyup/engine_surface_core.h MergeEngineSurface): a fragment only
// adds entries; an rtti class both declare with the same typeinfo name and module is shared.
#include "readyup/engine_surface_core.h"

#include <cstdio>
#include <string>

namespace es = readyup::es;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}

static es::EngineSurface Parse(const char* json) {
  std::string err;
  auto s = es::ParseEngineSurface(json, &err);
  if (!s) std::printf("parse error: %s\n", err.c_str());
  return s ? *s : es::EngineSurface{};
}

static const char* kBase = R"({
  "functions": { "F": { "library": "server", "linux": "55", "anchors": [] } },
  "rtti": { "CCSPlayerController": { "typeinfo_name": "19CCSPlayerController", "note": "core" } }
})";

int main() {
  std::string err;

  es::EngineSurface base = Parse(kBase);
  const es::EngineSurface same = Parse(R"({
    "functions": {},
    "rtti": { "CCSPlayerController": { "typeinfo_name": "19CCSPlayerController", "note": "practice" },
              "CCSPlayerPawn": { "typeinfo_name": "13CCSPlayerPawn" } }
  })");
  Check(es::MergeEngineSurface(&base, same, &err), "an identical rtti entry merges");
  Check(base.rtti.size() == 2 && base.FindRtti("CCSPlayerPawn"), "it is kept once, the fragment's other classes are added");

  es::EngineSurface base2 = Parse(kBase);
  const es::EngineSurface other = Parse(R"({
    "functions": {},
    "rtti": { "CCSPlayerController": { "typeinfo_name": "20CCSPlayerController2" } }
  })");
  err.clear();
  Check(!es::MergeEngineSurface(&base2, other, &err) && err.find("duplicate rtti") != std::string::npos,
        "a conflicting rtti entry is refused");

  es::EngineSurface base3 = Parse(kBase);
  const es::EngineSurface otherModule = Parse(R"({
    "functions": {},
    "rtti": { "CCSPlayerController": { "typeinfo_name": "19CCSPlayerController", "module": "schemasystem" } }
  })");
  Check(!es::MergeEngineSurface(&base3, otherModule, &err), "the same class in another module is refused");

  es::EngineSurface base4 = Parse(kBase);
  const es::EngineSurface dupFn = Parse(R"({ "functions": { "F": { "library": "server", "linux": "55", "anchors": [] } } })");
  Check(!es::MergeEngineSurface(&base4, dupFn, &err), "a duplicate function is refused");

  std::printf("engine_surface_merge_test: %s\n", g_failed ? "FAIL" : "PASS");
  return g_failed ? 1 : 0;
}
