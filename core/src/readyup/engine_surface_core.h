#pragma once

// Engine-surface resolution core: signature scanning + identity-anchor verification.
//
// This file has no dependency on the running server (no logging, no dlopen) so the
// same code runs in-process (readyup) and offline against a libserver.so on disk
// (tools/sigcheck.cpp).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace readyup::es {

// A view of one loaded segment. `addr` is the address code in the image uses
// (the runtime address in-process, or the ELF vaddr offline); `data` points at the bytes.
struct Region {
  uintptr_t addr = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool exec = false;
};

struct Image {
  std::vector<Region> regions;

  // Returns a pointer to `n` readable bytes at `addr`, or nullptr if not fully inside one region.
  const uint8_t* Bytes(uintptr_t addr, size_t n) const;
  // True if `addr` lies inside any region.
  bool Contains(uintptr_t addr) const;
  // Reads a NUL-terminated string at addr (max `maxLen`), or nullopt.
  std::optional<std::string> CString(uintptr_t addr, size_t maxLen = 256) const;
};

// Pattern: "55 48 89 E5 ? ?? 41" ("?"/"??" = wildcard). Returns empty on parse error.
std::vector<int> ParsePattern(const std::string& pattern);

// All matches in executable regions (stops after maxMatches).
std::vector<uintptr_t> ScanAll(const Image& img, const std::vector<int>& pat, int maxMatches);

struct Anchor {
  // "string_ref":    the function references `str` via a RIP-relative LEA within `window` bytes of its start.
  // "caller_string": some direct call/jmp to the function (or, with via_thunk, to a short thunk
  //                  that jumps to it) is preceded within `window` bytes by a RIP-relative LEA of `str`.
  // "global_string": the function loads a global via a RIP-relative `mov r64, [rip+disp32]` in its
  //                  first 32 bytes, and somewhere in the image a RIP-relative LEA of that same
  //                  global lies within `window` bytes (either side) of a RIP-relative LEA of `str`.
  //                  For leaf accessors with no strings of their own (ties them to the TU/system
  //                  that owns the global).
  // "callee_string": a direct call within the first `window` bytes of the function targets a
  //                  function that references `str` via a RIP-relative LEA within its first
  //                  `callee_window` bytes (default 256). For small functions whose only
  //                  identifying string lives in a private helper they call.
  // "mov_disp":      the function loads a register from `[reg + disp32]` (opcode 8B, ModRM mod=10)
  //                  with displacement `disp` within its first `window` bytes. Pins a struct
  //                  layout (e.g. CCommand argc/argv) to the engine code that reads it.
  std::string type;
  std::string str;
  size_t window = 0;
  bool via_thunk = false;
  size_t callee_window = 256;
  int64_t disp = 0;
};

struct FunctionSpec {
  std::string name;
  std::string pattern;
  bool required = false;
  std::vector<Anchor> anchors;
  std::string description;
  std::string hook;  // "funchook" when Ready Up detours it (readyup_hookcheck checks the prologue)
  // Module the function lives in: "server" = the real libserver.so, else lib<library>.so
  // ("engine2" = libengine2.so). Only plugin fragments list other modules; the core's own
  // engine-surface.json stays libserver-only.
  std::string library = "server";
};

// One virtual slot we patch or call through. Verified by locating the implementing class's
// primary vtable through its RTTI typeinfo name (no live object needed, so it also works
// offline), then checking the slot's target: it must equal `function` (a verified
// engine-surface function) when set, and pass every anchor. At runtime the live object's vptr
// must additionally equal that vtable before anything is patched or called.
struct VtableSpec {
  std::string name;               // "ISource2Server::GameFrame"
  int index = -1;                 // slot index (Itanium ABI, linux)
  std::string cls;                // "rtti" key of the implementing class ("CSource2Server")
  std::string function;           // optional: slot must point at this engine-surface function
  std::vector<Anchor> anchors;    // identity anchors checked on the slot target
  std::string module = "server";  // "server": verifiable offline; other modules: runtime RTTI only
  std::string note;
};

// A struct layout that is trusted only when the engine code that reads it verified: the
// `verified_by` vtable/function entry carries mov_disp anchors for these offsets.
struct LayoutSpec {
  std::string name;                                 // "CCommand"
  std::string verified_by;                          // vtable_indices or functions key
  std::vector<std::pair<std::string, int>> fields;  // field -> byte offset
  std::optional<int> Field(const std::string& f) const;
};

struct RttiSpec {
  std::string cls;
  std::string typeinfo_name;  // mangled, e.g. "14CSource2Server"
  std::string module = "server";
};

struct EngineSurface {
  std::string game_version;
  std::vector<FunctionSpec> functions;
  std::vector<VtableSpec> vtables;
  std::vector<RttiSpec> rtti;
  std::vector<LayoutSpec> layouts;

  const FunctionSpec* Find(const std::string& name) const;
  const VtableSpec* FindVtable(const std::string& name) const;
  const LayoutSpec* FindLayout(const std::string& name) const;
  const RttiSpec* FindRtti(const std::string& cls) const;
  std::optional<int> VtableIndex(const std::string& name) const;
  std::optional<std::string> RttiName(const std::string& cls) const;
};

// Parses gamedata/engine-surface.json. On failure returns nullopt and sets *err.
std::optional<EngineSurface> ParseEngineSurface(const std::string& json, std::string* err);

// Appends a fragment (e.g. gamedata/engine-surface.skins.json) to `base`. A fragment may only
// add entries: a function, vtable, rtti class or layout that `base` already has is an error
// (nothing is merged then), except an rtti class with the same typeinfo name and module, which
// both share. Fragment vtables/layouts may refer to base rtti and functions.
bool MergeEngineSurface(EngineSurface* base, const EngineSurface& fragment, std::string* err);

struct Resolution {
  uintptr_t addr = 0;     // valid only if ok
  int matches = 0;        // signature match count (capped at 3)
  bool anchors_ok = false;
  bool ok = false;        // matches == 1 && anchors_ok
  std::string detail;     // human-readable reason / anchor report
};

bool CheckAnchor(const Image& img, uintptr_t fn, const Anchor& a, std::string* detail);
Resolution Resolve(const Image& img, const FunctionSpec& spec);

// Primary vtable (address point = address of slot 0) of the class whose RTTI typeinfo name is
// `typeinfoName`: the typeinfo is found through its name pointer, the vtable through a pointer to
// that typeinfo which is preceded by offset_to_top == 0 and followed by a code pointer. Must be
// unique; otherwise nullopt and *detail says why.
std::optional<uintptr_t> FindVtableByRtti(const Image& img, const std::string& typeinfoName, std::string* detail);

struct VtableCheck {
  bool ok = false;
  uintptr_t vtable = 0;  // address point
  uintptr_t target = 0;  // slot value
  std::string detail;
};

// Checks slot `spec.index` of `vtable`: the target is code inside the image, equals `expectedFn`
// when the spec names a function (pass 0 when that function is unresolved -> fails), and passes
// every anchor.
VtableCheck CheckVtableSlot(const Image& img, const VtableSpec& spec, uintptr_t vtable, uintptr_t expectedFn);

// FindVtableByRtti(typeinfo name of spec.cls) + CheckVtableSlot.
VtableCheck VerifyVtable(const Image& img, const EngineSurface& es, const VtableSpec& spec, uintptr_t expectedFn);

}  // namespace readyup::es
