#include "sigfix.h"

#include "distorm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <set>
#include <string_view>

namespace readyup::sigfix {

using es::Image;

static bool IsExec(const Image& img, uintptr_t a) {
  for (const auto& r : img.regions)
    if (r.exec && a >= r.addr && a - r.addr < r.size) return true;
  return false;
}

// Addresses of `s` as a NUL-terminated string (also as the tail of a longer one, like IsString).
static std::vector<uintptr_t> FindString(const Image& img, const std::string& s) {
  std::vector<uintptr_t> out;
  const std::string needle = s + std::string(1, '\0');
  for (const auto& r : img.regions) {
    const std::string_view hay(reinterpret_cast<const char*>(r.data), r.size);
    for (size_t p = hay.find(needle); p != std::string_view::npos; p = hay.find(needle, p + 1)) out.push_back(r.addr + p);
  }
  return out;
}

// Whole strings (after a NUL) that start with `prefix`, at most 3.
static std::vector<std::string> StringsStartingWith(const Image& img, const std::string& prefix) {
  std::set<std::string> out;
  for (const auto& r : img.regions) {
    const std::string_view hay(reinterpret_cast<const char*>(r.data), r.size);
    for (size_t p = hay.find(prefix); p != std::string_view::npos && out.size() < 3; p = hay.find(prefix, p + 1)) {
      if (p > 0 && hay[p - 1] != '\0') continue;
      if (auto s = img.CString(r.addr + p)) out.insert(*s);
    }
  }
  return {out.begin(), out.end()};
}

// Strings sharing the longest prefix (8+ chars) with `s`. A prefix that occurs means every shorter
// one does too, so the longest is found by binary search.
static std::vector<std::string> NearestStrings(const Image& img, const std::string& s) {
  size_t lo = 8, hi = s.size();
  std::vector<std::string> best;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    auto found = StringsStartingWith(img, s.substr(0, mid));
    if (found.empty()) {
      hi = mid;
    } else {
      best = std::move(found);
      lo = mid + 1;
    }
  }
  return best;
}

// Function entry points: direct call/jmp targets in code plus code pointers in data (vtables).
static std::vector<uintptr_t> Entries(const Image& img) {
  std::vector<uintptr_t> out;
  for (const auto& r : img.regions) {
    if (r.exec) {
      for (size_t i = 0; i + 5 <= r.size; ++i) {
        if (r.data[i] != 0xE8 && r.data[i] != 0xE9) continue;
        int32_t rel = 0;
        std::memcpy(&rel, r.data + i + 1, 4);
        const uintptr_t dst = r.addr + i + 5 + static_cast<intptr_t>(rel);
        if (IsExec(img, dst)) out.push_back(dst);
      }
    } else {
      for (uintptr_t a = (r.addr + 7) & ~static_cast<uintptr_t>(7); a + 8 <= r.addr + r.size; a += 8) {
        uintptr_t v = 0;
        std::memcpy(&v, r.data + (a - r.addr), 8);
        if (IsExec(img, v)) out.push_back(v);
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static std::string Format(const std::vector<int>& pat) {
  std::string out;
  for (int b : pat) {
    char tok[4];
    std::snprintf(tok, sizeof(tok), b < 0 ? "?" : "%02X", b);
    if (!out.empty()) out += ' ';
    out += tok;
  }
  return out;
}

static unsigned Decode(const uint8_t* code, size_t n, uintptr_t addr, _DInst* out, unsigned max) {
  _CodeInfo ci{};
  ci.codeOffset = addr;
  ci.code = code;
  ci.codeLen = static_cast<int>(n);
  ci.dt = Decode64Bits;
  ci.features = DF_STOP_ON_RET;
  unsigned cnt = 0;
  distorm_decompose(&ci, out, max, &cnt);
  return cnt;
}

std::string MakeSignature(const Image& img, uintptr_t fn, size_t minBytes) {
  constexpr size_t kMax = 128;
  const uint8_t* code = img.Bytes(fn, kMax);
  if (!code) return "";
  _DInst ins[kMax];
  const unsigned n = Decode(code, kMax, fn, ins, kMax);
  std::vector<int> pat;
  for (unsigned i = 0; i < n; ++i) {
    const _DInst& d = ins[i];
    if (d.flags == FLAG_NOT_DECODABLE) break;
    // Encoding ends [... disp][imm]; distorm gives the operand sizes in bits.
    size_t imm = 0;
    bool wildImm = false;
    for (int j = 0; j < d.opsNo; ++j) {
      const auto t = d.ops[j].type;
      if (t != O_IMM && t != O_IMM1 && t != O_IMM2 && t != O_PC) continue;
      imm += d.ops[j].size / 8;
      if ((t == O_IMM || t == O_PC) && d.ops[j].size >= 32) wildImm = true;
    }
    const size_t disp = d.dispSize / 8;
    const size_t off = d.addr - fn;
    for (size_t b = 0; b < d.size; ++b) {
      const bool inImm = b >= d.size - imm;
      const bool inDisp = !inImm && b + imm + disp >= d.size;
      pat.push_back((inImm && wildImm) || (inDisp && disp >= 4) ? -1 : code[off + b]);
    }
    if (pat.size() >= minBytes && es::ScanAll(img, pat, 2).size() == 1) return Format(pat);
  }
  return "";
}

// Instruction lengths of a signature (wildcards read as 00), sorted: reordered moves keep the shape.
static std::vector<int> Shape(const std::string& pattern) {
  std::vector<uint8_t> bytes;
  for (int b : es::ParsePattern(pattern)) bytes.push_back(b < 0 ? 0 : static_cast<uint8_t>(b));
  _DInst ins[128];
  const unsigned n = Decode(bytes.data(), bytes.size(), 0, ins, 128);
  std::vector<int> out;
  for (unsigned i = 0; i < n; ++i) out.push_back(ins[i].size);
  std::sort(out.begin(), out.end());
  return out;
}

Fix FixFunction(const Image& img, const es::FunctionSpec& spec) {
  Fix fix;
  std::optional<std::set<uintptr_t>> cand;
  std::vector<uintptr_t> entries;
  for (const auto& a : spec.anchors) {
    if (a.type != "string_ref" && a.type != "caller_string") continue;
    const auto strs = FindString(img, a.str);
    if (strs.empty()) {
      fix.why = "anchor string \"" + a.str + "\" is not in the binary";
      fix.hints = NearestStrings(img, a.str);
      return fix;
    }
    std::set<uintptr_t> mine;
    for (uintptr_t s : strs) {
      for (uintptr_t site : es::LeaSitesOf(img, s)) {
        if (a.type == "string_ref") {
          if (entries.empty()) entries = Entries(img);
          // entries F with F <= site < F + window
          auto it = std::upper_bound(entries.begin(), entries.end(), site - a.window);
          for (; it != entries.end() && *it <= site; ++it) mine.insert(*it);
        } else {
          for (uintptr_t c = site + 1; c <= site + a.window; ++c) {
            const uint8_t* b = img.Bytes(c, 5);
            if (!b || b[0] != 0xE8) continue;
            int32_t rel = 0;
            std::memcpy(&rel, b + 1, 4);
            const uintptr_t t = c + 5 + static_cast<intptr_t>(rel);
            if (IsExec(img, t)) mine.insert(t);
          }
        }
      }
    }
    if (!cand) {
      cand = std::move(mine);
    } else {
      std::set<uintptr_t> both;
      std::set_intersection(cand->begin(), cand->end(), mine.begin(), mine.end(), std::inserter(both, both.end()));
      cand = std::move(both);
    }
  }
  if (!cand) {
    fix.why = "no string_ref / caller_string anchor to search from";
    return fix;
  }
  std::vector<uintptr_t> pass;
  for (uintptr_t c : *cand) {
    bool all = true;
    for (const auto& a : spec.anchors) all = all && es::CheckAnchor(img, c, a, nullptr);
    if (all) pass.push_back(c);
  }
  if (pass.size() != 1) {
    fix.why = std::to_string(pass.size()) + " functions pass every anchor (want 1)";
    return fix;
  }
  const std::string pattern = MakeSignature(img, pass[0], es::ParsePattern(spec.pattern).size());
  if (pattern.empty()) {
    fix.why = "no unique signature within 128 bytes of the function start";
    return fix;
  }
  es::FunctionSpec next = spec;
  next.pattern = pattern;
  const es::Resolution r = es::Resolve(img, next);
  fix.anchors = r.detail;
  if (!r.ok || r.addr != pass[0]) {
    fix.why = "new signature does not resolve: " + r.detail;
    return fix;
  }
  fix.pattern = pattern;
  fix.same_shape = Shape(spec.pattern) == Shape(pattern);
  fix.fixed = true;
  return fix;
}

}  // namespace readyup::sigfix
