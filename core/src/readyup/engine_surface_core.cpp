#include "readyup/engine_surface_core.h"

#include "readyup/minijson.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace readyup::es {

const uint8_t* Image::Bytes(uintptr_t addr, size_t n) const {
  for (const auto& r : regions) {
    if (addr >= r.addr && addr - r.addr <= r.size && n <= r.size - (addr - r.addr)) return r.data + (addr - r.addr);
  }
  return nullptr;
}

bool Image::Contains(uintptr_t addr) const { return Bytes(addr, 1) != nullptr; }

std::optional<std::string> Image::CString(uintptr_t addr, size_t maxLen) const {
  for (const auto& r : regions) {
    if (addr < r.addr || addr - r.addr >= r.size) continue;
    const size_t avail = r.size - (addr - r.addr);
    const char* p = reinterpret_cast<const char*>(r.data + (addr - r.addr));
    const size_t lim = avail < maxLen ? avail : maxLen;
    const void* nul = std::memchr(p, 0, lim);
    if (!nul) return std::nullopt;
    return std::string(p, static_cast<const char*>(nul) - p);
  }
  return std::nullopt;
}

static int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

std::vector<int> ParsePattern(const std::string& pattern) {
  std::vector<int> out;
  size_t i = 0;
  while (i < pattern.size()) {
    const char c = pattern[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    if (c == '?') {
      out.push_back(-1);
      ++i;
      if (i < pattern.size() && pattern[i] == '?') ++i;
      continue;
    }
    if (i + 1 < pattern.size() && HexVal(c) >= 0 && HexVal(pattern[i + 1]) >= 0) {
      out.push_back((HexVal(c) << 4) | HexVal(pattern[i + 1]));
      i += 2;
      continue;
    }
    return {};  // reject malformed patterns instead of silently skipping tokens
  }
  return out;
}

std::vector<uintptr_t> ScanAll(const Image& img, const std::vector<int>& pat, int maxMatches) {
  std::vector<uintptr_t> out;
  if (pat.empty() || pat[0] < 0) return out;  // patterns must start with a concrete byte
  const uint8_t first = static_cast<uint8_t>(pat[0]);
  for (const auto& r : img.regions) {
    if (!r.exec || r.size < pat.size()) continue;
    const uint8_t* p = r.data;
    const uint8_t* end = r.data + (r.size - pat.size()) + 1;
    while (p < end) {
      p = static_cast<const uint8_t*>(std::memchr(p, first, static_cast<size_t>(end - p)));
      if (!p) break;
      bool ok = true;
      for (size_t j = 1; j < pat.size(); ++j) {
        if (pat[j] >= 0 && p[j] != static_cast<uint8_t>(pat[j])) {
          ok = false;
          break;
        }
      }
      if (ok) {
        out.push_back(r.addr + static_cast<uintptr_t>(p - r.data));
        if (static_cast<int>(out.size()) >= maxMatches) return out;
      }
      ++p;
    }
  }
  return out;
}

// Target of a RIP-relative `lea r64, [rip+disp32]` (REX.W 8D /r, mod=00 rm=101) at `at`, if it is one.
static std::optional<uintptr_t> LeaRipTarget(const Image& img, uintptr_t at) {
  const uint8_t* b = img.Bytes(at, 7);
  if (!b) return std::nullopt;
  if ((b[0] != 0x48 && b[0] != 0x4C) || b[1] != 0x8D || (b[2] & 0xC7) != 0x05) return std::nullopt;
  int32_t disp = 0;
  std::memcpy(&disp, b + 3, 4);
  return at + 7 + static_cast<intptr_t>(disp);
}

static bool IsString(const Image& img, uintptr_t addr, const std::string& want) {
  const uint8_t* b = img.Bytes(addr, want.size() + 1);
  return b && std::memcmp(b, want.data(), want.size()) == 0 && b[want.size()] == 0;
}

static bool LeaOfStringInRange(const Image& img, uintptr_t lo, uintptr_t hi, const std::string& s) {
  for (uintptr_t p = lo; p < hi; ++p) {
    auto t = LeaRipTarget(img, p);
    if (t && IsString(img, *t, s)) return true;
  }
  return false;
}

// Target of a RIP-relative `mov r64, [rip+disp32]` (REX.W 8B /r, mod=00 rm=101) at `at`, if it is one.
static std::optional<uintptr_t> MovLoadRipTarget(const Image& img, uintptr_t at) {
  const uint8_t* b = img.Bytes(at, 7);
  if (!b) return std::nullopt;
  if ((b[0] != 0x48 && b[0] != 0x4C) || b[1] != 0x8B || (b[2] & 0xC7) != 0x05) return std::nullopt;
  int32_t disp = 0;
  std::memcpy(&disp, b + 3, 4);
  return at + 7 + static_cast<intptr_t>(disp);
}

// Addresses of RIP-relative LEAs (REX.W 8D, mod=00 rm=101) in executable regions whose target is `target`.
static std::vector<uintptr_t> LeaSitesOf(const Image& img, uintptr_t target) {
  std::vector<uintptr_t> out;
  for (const auto& r : img.regions) {
    if (!r.exec || r.size < 7) continue;
    for (size_t i = 0; i + 7 <= r.size; ++i) {
      const uint8_t* b = r.data + i;
      if ((b[0] != 0x48 && b[0] != 0x4C) || b[1] != 0x8D || (b[2] & 0xC7) != 0x05) continue;
      int32_t disp = 0;
      std::memcpy(&disp, b + 3, 4);
      if (r.addr + i + 7 + static_cast<intptr_t>(disp) == target) out.push_back(r.addr + i);
    }
  }
  return out;
}

// Direct rel32 call/jmp sites (E8/E9) whose destination is in [lo, hi].
// (A byte scan: it may also see E8/E9 bytes inside other instructions, but a false site
// only matters if it also happens to be preceded by a LEA of the anchor string.)
static std::vector<uintptr_t> BranchSitesTo(const Image& img, uintptr_t lo, uintptr_t hi, bool callsOnly) {
  std::vector<uintptr_t> out;
  for (const auto& r : img.regions) {
    if (!r.exec || r.size < 5) continue;
    for (size_t i = 0; i + 5 <= r.size; ++i) {
      const uint8_t op = r.data[i];
      if (op != 0xE8 && (callsOnly || op != 0xE9)) continue;
      int32_t rel = 0;
      std::memcpy(&rel, r.data + i + 1, 4);
      const uintptr_t dst = r.addr + i + 5 + static_cast<intptr_t>(rel);
      if (dst >= lo && dst <= hi) out.push_back(r.addr + i);
    }
  }
  return out;
}

// Anchor strings in one-line reports: escape control characters ("\n" etc.).
static std::string Printable(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20 || c == 0x7F) out += "?";
    else out.push_back(static_cast<char>(c));
  }
  return out;
}

bool CheckAnchor(const Image& img, uintptr_t fn, const Anchor& a, std::string* detail) {
  if (a.type == "string_ref") {
    const bool ok = LeaOfStringInRange(img, fn, fn + a.window, a.str);
    if (detail) *detail = std::string(ok ? "ok" : "FAIL") + " string_ref \"" + Printable(a.str) + "\"";
    return ok;
  }
  if (a.type == "caller_string") {
    std::vector<std::pair<uintptr_t, uintptr_t>> entries{{fn, fn}};
    if (a.via_thunk) {
      // Thunks look like `add rdi, imm8; jmp fn`: accept calls landing up to 8 bytes
      // before each tail-jump (E9) to fn.
      for (uintptr_t j : BranchSitesTo(img, fn, fn, /*callsOnly=*/false)) {
        const uint8_t* b = img.Bytes(j, 1);
        if (b && *b == 0xE9) entries.emplace_back(j - 8, j);
      }
    }
    int sites = 0;
    for (const auto& e : entries) {
      for (uintptr_t c : BranchSitesTo(img, e.first, e.second, /*callsOnly=*/true)) {
        ++sites;
        if (LeaOfStringInRange(img, c - a.window, c, a.str)) {
          if (detail) *detail = "ok caller_string \"" + Printable(a.str) + "\"";
          return true;
        }
      }
    }
    if (detail) *detail = "FAIL caller_string \"" + Printable(a.str) + "\" (" + std::to_string(sites) + " call sites checked)";
    return false;
  }
  if (a.type == "callee_string") {
    int calls = 0;
    for (uintptr_t p = fn; p + 5 <= fn + a.window; ++p) {
      const uint8_t* b = img.Bytes(p, 5);
      if (!b || b[0] != 0xE8) continue;
      int32_t rel = 0;
      std::memcpy(&rel, b + 1, 4);
      const uintptr_t callee = p + 5 + static_cast<intptr_t>(rel);
      if (!img.Contains(callee)) continue;
      ++calls;
      if (LeaOfStringInRange(img, callee, callee + a.callee_window, a.str)) {
        if (detail) *detail = "ok callee_string \"" + Printable(a.str) + "\"";
        return true;
      }
    }
    if (detail) *detail = "FAIL callee_string \"" + Printable(a.str) + "\" (" + std::to_string(calls) + " callees checked)";
    return false;
  }
  if (a.type == "global_string") {
    std::optional<uintptr_t> global;
    for (uintptr_t p = fn; p < fn + 32 && !global; ++p) global = MovLoadRipTarget(img, p);
    if (!global) {
      if (detail) *detail = "FAIL global_string \"" + Printable(a.str) + "\" (no RIP-relative global load in first 32 bytes)";
      return false;
    }
    const auto sites = LeaSitesOf(img, *global);
    for (uintptr_t c : sites) {
      if (LeaOfStringInRange(img, c - a.window, c + a.window, a.str)) {
        if (detail) *detail = "ok global_string \"" + Printable(a.str) + "\"";
        return true;
      }
    }
    if (detail) {
      *detail = "FAIL global_string \"" + Printable(a.str) + "\" (" + std::to_string(sites.size()) + " LEA sites of global checked)";
    }
    return false;
  }
  if (a.type == "mov_disp") {
    // 8B /r with ModRM mod=10: [base + disp32]; rm=100 adds a SIB byte before the displacement.
    for (uintptr_t p = fn; p + 7 <= fn + a.window; ++p) {
      const uint8_t* b = img.Bytes(p, 7);
      if (!b || b[0] != 0x8B || (b[1] >> 6) != 2) continue;
      const size_t dispAt = ((b[1] & 7) == 4) ? 3 : 2;
      int32_t disp = 0;
      std::memcpy(&disp, b + dispAt, 4);
      if (disp == a.disp) {
        if (detail) *detail = "ok mov_disp " + std::to_string(a.disp);
        return true;
      }
    }
    if (detail) *detail = "FAIL mov_disp " + std::to_string(a.disp) + " (no `mov r,[r+disp32]` in window)";
    return false;
  }
  if (detail) *detail = "FAIL unknown anchor type \"" + a.type + "\"";
  return false;
}

static std::string Hex(uintptr_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%lx", static_cast<unsigned long>(v));
  return buf;
}

static bool ReadPtr(const Image& img, uintptr_t addr, uintptr_t* out) {
  const uint8_t* b = img.Bytes(addr, sizeof(*out));
  if (!b) return false;
  std::memcpy(out, b, sizeof(*out));
  return true;
}

static bool IsCode(const Image& img, uintptr_t addr) {
  for (const auto& r : img.regions) {
    if (r.exec && addr >= r.addr && addr - r.addr < r.size) return true;
  }
  return false;
}

// Aligned pointer-sized slots in non-executable regions whose value is in `targets`.
static std::vector<uintptr_t> DataSlotsPointingTo(const Image& img, const std::vector<uintptr_t>& targets) {
  std::vector<uintptr_t> out;
  if (targets.empty()) return out;
  for (const auto& r : img.regions) {
    if (r.exec || r.size < 8) continue;
    const uintptr_t first = (r.addr + 7) & ~static_cast<uintptr_t>(7);
    for (uintptr_t a = first; a + 8 <= r.addr + r.size; a += 8) {
      uintptr_t v = 0;
      std::memcpy(&v, r.data + (a - r.addr), 8);
      for (uintptr_t t : targets) {
        if (v == t) {
          out.push_back(a);
          break;
        }
      }
    }
  }
  return out;
}

std::optional<uintptr_t> FindVtableByRtti(const Image& img, const std::string& typeinfoName, std::string* detail) {
  auto fail = [&](const std::string& why) -> std::optional<uintptr_t> {
    if (detail) *detail = why;
    return std::nullopt;
  };
  if (typeinfoName.empty()) return fail("no typeinfo name");

  // 1) Every NUL-terminated occurrence of the mangled name in non-executable data.
  std::vector<uintptr_t> names;
  for (const auto& r : img.regions) {
    if (r.exec || r.size <= typeinfoName.size()) continue;
    const uint8_t* p = r.data;
    const uint8_t* end = r.data + r.size - typeinfoName.size();
    while (p < end) {
      p = static_cast<const uint8_t*>(std::memchr(p, static_cast<unsigned char>(typeinfoName[0]), static_cast<size_t>(end - p)));
      if (!p) break;
      if (std::memcmp(p, typeinfoName.data(), typeinfoName.size()) == 0 && p[typeinfoName.size()] == 0) {
        names.push_back(r.addr + static_cast<uintptr_t>(p - r.data));
      }
      ++p;
    }
  }
  if (names.empty()) return fail("typeinfo name \"" + typeinfoName + "\" not in image");

  // 2) typeinfo objects: {vptr, const char* name, ...}; the name pointer is at +8.
  std::vector<uintptr_t> typeinfos;
  for (uintptr_t slot : DataSlotsPointingTo(img, names)) typeinfos.push_back(slot - 8);
  if (typeinfos.empty()) return fail("no typeinfo object for \"" + typeinfoName + "\"");

  // 3) vtables: {offset_to_top, typeinfo*, slot0, ...}. Primary vtable: offset_to_top == 0 and
  //    slot 0 is code (rules out typeinfo base-class entries that also point at the typeinfo).
  std::vector<uintptr_t> primaries;
  for (uintptr_t slot : DataSlotsPointingTo(img, typeinfos)) {
    uintptr_t top = 1, slot0 = 0;
    if (!ReadPtr(img, slot - 8, &top) || top != 0) continue;
    if (!ReadPtr(img, slot + 8, &slot0) || !IsCode(img, slot0)) continue;
    primaries.push_back(slot + 8);
  }
  if (primaries.empty()) return fail("no primary vtable for \"" + typeinfoName + "\"");
  if (primaries.size() != 1) {
    return fail(std::to_string(primaries.size()) + " primary vtables for \"" + typeinfoName + "\" (want 1)");
  }
  if (detail) *detail = "vtable " + Hex(primaries[0]);
  return primaries[0];
}

VtableCheck CheckVtableSlot(const Image& img, const VtableSpec& spec, uintptr_t vtable, uintptr_t expectedFn) {
  VtableCheck c;
  c.vtable = vtable;
  if (spec.index < 0) {
    c.detail = "no slot index";
    return c;
  }
  if (!ReadPtr(img, vtable + static_cast<uintptr_t>(spec.index) * 8, &c.target)) {
    c.detail = "slot " + std::to_string(spec.index) + " outside the image";
    return c;
  }
  if (!IsCode(img, c.target)) {
    c.detail = "slot " + std::to_string(spec.index) + " = " + Hex(c.target) + " is not code";
    return c;
  }
  c.detail = "slot[" + std::to_string(spec.index) + "]=" + Hex(c.target);
  bool ok = true;
  if (!spec.function.empty()) {
    if (expectedFn == 0) {
      c.detail += "; FAIL " + spec.function + " unresolved";
      ok = false;
    } else if (expectedFn != c.target) {
      c.detail += "; FAIL != " + spec.function + " " + Hex(expectedFn);
      ok = false;
    } else {
      c.detail += "; ok == " + spec.function;
    }
  }
  if (spec.function.empty() && spec.anchors.empty()) {
    c.detail += "; FAIL no identity anchor";
    ok = false;
  }
  for (const auto& a : spec.anchors) {
    std::string d;
    if (!CheckAnchor(img, c.target, a, &d)) ok = false;
    c.detail += "; " + d;
  }
  c.ok = ok;
  return c;
}

VtableCheck VerifyVtable(const Image& img, const EngineSurface& es, const VtableSpec& spec, uintptr_t expectedFn) {
  VtableCheck c;
  const RttiSpec* rt = es.FindRtti(spec.cls);
  if (!rt) {
    c.detail = "class \"" + spec.cls + "\" has no rtti entry";
    return c;
  }
  std::string why;
  auto vt = FindVtableByRtti(img, rt->typeinfo_name, &why);
  if (!vt) {
    c.detail = why;
    return c;
  }
  c = CheckVtableSlot(img, spec, *vt, expectedFn);
  c.detail = spec.cls + " vtable " + Hex(*vt) + " " + c.detail;
  return c;
}

Resolution Resolve(const Image& img, const FunctionSpec& spec) {
  Resolution res;
  const auto pat = ParsePattern(spec.pattern);
  if (pat.empty()) {
    res.detail = "bad pattern";
    return res;
  }
  const auto hits = ScanAll(img, pat, 3);
  res.matches = static_cast<int>(hits.size());
  if (hits.size() != 1) {
    res.detail = "signature matched " + std::to_string(hits.size()) + (hits.size() >= 3 ? "+" : "") + " times (want 1)";
    return res;
  }
  res.addr = hits[0];
  if (spec.anchors.empty()) {
    res.detail = "no anchor (unverified)";
    return res;
  }
  res.anchors_ok = true;
  for (const auto& a : spec.anchors) {
    std::string d;
    if (!CheckAnchor(img, res.addr, a, &d)) res.anchors_ok = false;
    if (!res.detail.empty()) res.detail += "; ";
    res.detail += d;
  }
  res.ok = res.anchors_ok;
  return res;
}

bool MergeEngineSurface(EngineSurface* base, const EngineSurface& frag, std::string* err) {
  if (!base) return false;
  auto dup = [&](const char* what, const std::string& name) {
    if (err) *err = std::string("duplicate ") + what + " \"" + name + "\"";
    return false;
  };
  for (const auto& f : frag.functions)
    if (base->Find(f.name)) return dup("function", f.name);
  for (const auto& v : frag.vtables)
    if (base->FindVtable(v.name)) return dup("vtable", v.name);
  std::vector<RttiSpec> rtti;
  for (const auto& r : frag.rtti) {
    const RttiSpec* have = base->FindRtti(r.cls);
    if (!have) rtti.push_back(r);
    else if (have->typeinfo_name != r.typeinfo_name || have->module != r.module) return dup("rtti", r.cls);
  }
  for (const auto& l : frag.layouts)
    if (base->FindLayout(l.name)) return dup("layout", l.name);
  base->functions.insert(base->functions.end(), frag.functions.begin(), frag.functions.end());
  base->vtables.insert(base->vtables.end(), frag.vtables.begin(), frag.vtables.end());
  base->rtti.insert(base->rtti.end(), rtti.begin(), rtti.end());
  base->layouts.insert(base->layouts.end(), frag.layouts.begin(), frag.layouts.end());
  return true;
}

const FunctionSpec* EngineSurface::Find(const std::string& name) const {
  for (const auto& f : functions)
    if (f.name == name) return &f;
  return nullptr;
}

const VtableSpec* EngineSurface::FindVtable(const std::string& name) const {
  for (const auto& v : vtables)
    if (v.name == name) return &v;
  return nullptr;
}

const LayoutSpec* EngineSurface::FindLayout(const std::string& name) const {
  for (const auto& l : layouts)
    if (l.name == name) return &l;
  return nullptr;
}

const RttiSpec* EngineSurface::FindRtti(const std::string& cls) const {
  for (const auto& r : rtti)
    if (r.cls == cls) return &r;
  return nullptr;
}

std::optional<int> EngineSurface::VtableIndex(const std::string& name) const {
  const VtableSpec* v = FindVtable(name);
  if (!v || v->index < 0) return std::nullopt;
  return v->index;
}

std::optional<std::string> EngineSurface::RttiName(const std::string& cls) const {
  const RttiSpec* r = FindRtti(cls);
  if (!r) return std::nullopt;
  return r->typeinfo_name;
}

std::optional<int> LayoutSpec::Field(const std::string& f) const {
  for (const auto& kv : fields)
    if (kv.first == f) return kv.second;
  return std::nullopt;
}

static Anchor ParseAnchor(const minijson::Value& av) {
  Anchor a;
  if (auto t = minijson::AsString(av.get("type"))) a.type = *t;
  if (auto s = minijson::AsString(av.get("string"))) a.str = *s;
  if (auto w = minijson::AsInt(av.get("window"))) a.window = static_cast<size_t>(*w);
  if (const auto* vt = av.get("via_thunk"); vt && vt->type == minijson::Value::Type::Bool) a.via_thunk = vt->b;
  if (auto cw = minijson::AsInt(av.get("callee_window"))) a.callee_window = static_cast<size_t>(*cw);
  if (auto d = minijson::AsInt(av.get("disp"))) a.disp = *d;
  return a;
}

static std::vector<Anchor> ParseAnchors(const minijson::Value& o) {
  std::vector<Anchor> out;
  if (const auto* anchors = o.get("anchors"); anchors && anchors->type == minijson::Value::Type::Array) {
    for (const auto& av : anchors->arr) out.push_back(ParseAnchor(av));
  }
  return out;
}

template <typename T>
static void SortByName(std::vector<T>& v, std::string T::*key) {
  std::sort(v.begin(), v.end(), [key](const T& a, const T& b) { return a.*key < b.*key; });
}

std::optional<EngineSurface> ParseEngineSurface(const std::string& json, std::string* err) {
  minijson::ParseError pe;
  auto root = minijson::Parse(json, &pe);
  if (!root || !minijson::IsObject(&*root)) {
    if (err) *err = "json parse error at " + std::to_string(pe.offset) + ": " + pe.msg;
    return std::nullopt;
  }
  EngineSurface es;
  if (const auto* meta = root->get("_meta")) {
    if (auto v = minijson::AsString(meta->get("game_version"))) es.game_version = *v;
  }
  const auto* fns = root->get("functions");
  if (!minijson::IsObject(fns)) {
    if (err) *err = "missing \"functions\" object";
    return std::nullopt;
  }
  for (const auto& kv : fns->obj) {
    FunctionSpec f;
    f.name = kv.first;
    const auto& o = kv.second;
    auto lin = minijson::AsString(o.get("linux"));
    if (!lin) {
      if (err) *err = "function " + f.name + " missing \"linux\" signature";
      return std::nullopt;
    }
    f.pattern = *lin;
    if (const auto* rq = o.get("required"); rq && rq->type == minijson::Value::Type::Bool) f.required = rq->b;
    if (auto d = minijson::AsString(o.get("description"))) f.description = *d;
    if (auto h = minijson::AsString(o.get("hook"))) f.hook = *h;
    if (auto l = minijson::AsString(o.get("library")); l && !l->empty()) f.library = *l;
    f.anchors = ParseAnchors(o);
    es.functions.push_back(std::move(f));
  }
  if (const auto* vt = root->get("vtable_indices"); minijson::IsObject(vt)) {
    for (const auto& kv : vt->obj) {
      VtableSpec v;
      v.name = kv.first;
      const auto& o = kv.second;
      auto i = minijson::AsInt(o.get("linux"));
      if (!i) {
        if (err) *err = "vtable " + v.name + " missing \"linux\" index";
        return std::nullopt;
      }
      v.index = static_cast<int>(*i);
      if (auto c = minijson::AsString(o.get("class"))) v.cls = *c;
      if (auto f = minijson::AsString(o.get("function"))) v.function = *f;
      if (auto m = minijson::AsString(o.get("module"))) v.module = *m;
      if (auto n = minijson::AsString(o.get("note"))) v.note = *n;
      v.anchors = ParseAnchors(o);
      es.vtables.push_back(std::move(v));
    }
  }
  if (const auto* rt = root->get("rtti"); minijson::IsObject(rt)) {
    for (const auto& kv : rt->obj) {
      RttiSpec r;
      r.cls = kv.first;
      if (auto n = minijson::AsString(kv.second.get("typeinfo_name"))) r.typeinfo_name = *n;
      if (auto m = minijson::AsString(kv.second.get("module"))) r.module = *m;
      if (!r.typeinfo_name.empty()) es.rtti.push_back(std::move(r));
    }
  }
  if (const auto* ly = root->get("layouts"); minijson::IsObject(ly)) {
    for (const auto& kv : ly->obj) {
      LayoutSpec l;
      l.name = kv.first;
      if (auto v = minijson::AsString(kv.second.get("verified_by"))) l.verified_by = *v;
      if (const auto* fields = kv.second.get("fields"); minijson::IsObject(fields)) {
        for (const auto& fk : fields->obj) {
          if (auto off = minijson::AsInt(&fk.second)) l.fields.emplace_back(fk.first, static_cast<int>(*off));
        }
      }
      std::sort(l.fields.begin(), l.fields.end());
      es.layouts.push_back(std::move(l));
    }
  }
  // The JSON object is unordered; keep reports deterministic.
  SortByName(es.functions, &FunctionSpec::name);
  SortByName(es.vtables, &VtableSpec::name);
  SortByName(es.rtti, &RttiSpec::cls);
  SortByName(es.layouts, &LayoutSpec::name);
  return es;
}

}  // namespace readyup::es
