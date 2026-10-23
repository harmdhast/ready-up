// Offline tests for the license check-in's pure helpers (plugins/match/readyup/license_checkin.h).
#include <cstdio>
#include <string>

#include "readyup/license_checkin.h"

static int g_failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

// The plugin's log functions (engine.cpp in the real build): quiet here.
namespace readyup {
void Debug(const char*, ...) {}
void Print(const char*, ...) {}
}  // namespace readyup

using namespace readyup::license_checkin;

int main() {
  // {"id":"L-abc123","kind":"month"} as base64url: the license id comes out; anything else is "".
  CHECK(KeyId("ATL1.eyJpZCI6IkwtYWJjMTIzIiwia2luZCI6Im1vbnRoIn0.sig") == "L-abc123");
  CHECK(KeyId("ATL1.bm90IGpzb24.sig").empty());
  CHECK(KeyId("no dots").empty());
  CHECK(KeyId("ATL1.eyJpZCI6IjxzY3JpcHQ-In0.sig").empty());  // {"id":"<script>"}: not an id

  const std::string args = std::string("cs2\0-dedicated\0-port\0" "27020\0+map\0de_dust2", 40);
  CHECK(PortFromCmdline(args) == 27020);
  CHECK(PortFromCmdline(std::string("cs2\0-dedicated", 14)) == 27015);
  CHECK(PortFromCmdline(std::string("cs2\0-port\0abc", 13)) == 27015);

  if (g_failures) {
    std::fprintf(stderr, "license_checkin_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("license_checkin_test: ok");
  return 0;
}
