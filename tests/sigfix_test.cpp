// readyup_sigfix (tools/sigfix.h): replays CS2 1.41.8.4 -> 1.41.9.2 on a hand-built image.
// CSmokeGrenadeProjectile_Create's prologue changed register allocation (PR #260); its anchors
// still find one function, so the fix must be exactly the signature written by hand then.
// CBaseEntity_SetAbsOrigin's anchor string was reworded: no fix, only a hint.
#include "sigfix.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace readyup;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}

static const char* kOld =
    "55 4C 89 C1 48 89 E5 41 57 49 89 FF 41 56 45 89 CE 41 55 4D 89 C5 41 54 53 48 81 EC ? ? ? ? 48 89 95 ? ? ? ? 48 89 F2";
static const char* kNew =
    "55 4C 89 C1 48 89 E5 41 57 41 56 49 89 FE 41 55 45 89 CD 41 54 4D 89 C4 53 48 81 EC ? ? ? ? 48 89 95 ? ? ? ? 48 89 F2";

constexpr uintptr_t kCode = 0x1000, kData = 0x10000, kFn = 0x1100, kCaller = 0x1800;
static std::vector<uint8_t> code(0x2000, 0xCC), data;

static uintptr_t AddString(const std::string& s) {
  const uintptr_t at = kData + data.size();
  data.insert(data.end(), s.begin(), s.end());
  data.push_back(0);
  return at;
}

// Writes `bytes` at `at`, returns the address after them.
static uintptr_t Put(uintptr_t at, const std::vector<uint8_t>& bytes) {
  std::memcpy(&code[at - kCode], bytes.data(), bytes.size());
  return at + bytes.size();
}

// `prefix` + rel32 to `target`, relative to the end of the instruction.
static uintptr_t PutRel(uintptr_t at, std::vector<uint8_t> prefix, uintptr_t target) {
  const int32_t rel = static_cast<int32_t>(target - (at + prefix.size() + 4));
  prefix.resize(prefix.size() + 4);
  std::memcpy(&prefix[prefix.size() - 4], &rel, 4);
  return Put(at, prefix);
}

int main() {
  data.push_back(0);
  const uintptr_t smoke = AddString("smokegrenade_projectile");
  const uintptr_t spawn = AddString("SpawnGrenadeProjectile");
  AddString("Method Entity.Move invoked with bad value for argument 0 (newValues). Got %s.");

  // The 1.41.9.2 prologue (wildcards: sub rsp, 0x110; mov [rbp-0x110], rdx), then the string_ref LEA.
  std::vector<uint8_t> prologue;
  for (int b : es::ParsePattern(kNew)) prologue.push_back(b < 0 ? 0 : static_cast<uint8_t>(b));
  std::memcpy(&prologue[28], "\x10\x01\x00\x00", 4);
  std::memcpy(&prologue[35], "\xF0\xFE\xFF\xFF", 4);
  uintptr_t p = Put(kFn, prologue);
  p = PutRel(p, {0x48, 0x8D, 0x3D}, smoke);  // lea rdi, "smokegrenade_projectile"
  Put(p, {0xC3});
  // SpawnGrenadeProjectile(): lea rsi, "SpawnGrenadeProjectile"; call CSmokeGrenadeProjectile::Create
  p = PutRel(kCaller, {0x48, 0x8D, 0x35}, spawn);
  p = PutRel(p, {0xE8}, kFn);
  Put(p, {0xC3});

  es::Image img;
  img.regions.push_back({kCode, code.data(), code.size(), true});
  img.regions.push_back({kData, data.data(), data.size(), false});

  es::FunctionSpec smokeSpec;
  smokeSpec.name = "CSmokeGrenadeProjectile_Create";
  smokeSpec.pattern = kOld;
  smokeSpec.anchors = {{"string_ref", "smokegrenade_projectile", 96},
                       {"caller_string", "SpawnGrenadeProjectile", 768}};
  Check(!es::Resolve(img, smokeSpec).ok, "the 1.41.8.4 signature fails");
  const sigfix::Fix fix = sigfix::FixFunction(img, smokeSpec);
  Check(fix.fixed, "smoke grenade: fixed");
  Check(fix.pattern == kNew, "smoke grenade: same signature as PR #260");
  if (fix.pattern != kNew) std::printf("  got  %s\n  want %s\n  why  %s\n", fix.pattern.c_str(), kNew, fix.why.c_str());
  Check(fix.same_shape, "smoke grenade: same shape (register change only)");

  es::FunctionSpec moveSpec;
  moveSpec.name = "CBaseEntity_SetAbsOrigin";
  moveSpec.pattern = "55 48 89 E5";
  moveSpec.anchors = {{"caller_string", "Method Entity.Move invoked with bad newValues value.\n", 128}};
  const sigfix::Fix move = sigfix::FixFunction(img, moveSpec);
  Check(!move.fixed && move.why.find("not in the binary") != std::string::npos, "reworded string: not fixed");
  Check(move.hints.size() == 1 && move.hints[0].rfind("Method Entity.Move invoked with bad value", 0) == 0,
        "reworded string: the new wording as hint");

  std::printf("%s\n", g_failed ? "FAILED" : "OK");
  return g_failed ? 1 : 0;
}
