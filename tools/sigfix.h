#pragma once

// Signature repair for engine-surface functions that stopped resolving after a CS2 update
// (tools/sigfix_main.cpp). Finds the function again from its string anchors and writes a new
// signature from its prologue. It never guesses: a missing anchor string or more than one
// candidate leaves the entry failing, for a human.

#include "readyup/engine_surface_core.h"

#include <string>
#include <vector>

namespace readyup::sigfix {

struct Fix {
  bool fixed = false;
  std::string pattern;             // new signature (when fixed)
  bool same_shape = false;         // same instructions by length as the old signature (moved / registers changed)
  std::string anchors;             // anchor report on the new match
  std::string why;                 // why it was not fixed
  std::vector<std::string> hints;  // strings sharing the longest prefix with a missing anchor string
};

// Candidates: string_ref -> functions whose first `window` bytes LEA the string; caller_string ->
// call targets within `window` bytes after a LEA of the string. Intersected across anchors, then
// kept only if every anchor passes. Exactly one -> MakeSignature + the same check sigcheck runs.
Fix FixFunction(const es::Image& img, const es::FunctionSpec& spec);

// Signature of the code at `fn`, whole instructions with rel32/disp32/imm32 operands wildcarded,
// at least `minBytes` long and grown until it matches exactly once. Empty if not unique within 128 bytes.
std::string MakeSignature(const es::Image& img, uintptr_t fn, size_t minBytes);

}  // namespace readyup::sigfix
