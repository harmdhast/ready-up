// Offline signature repair after a CS2 update.
//
//   readyup_sigfix [--game-version V] [--buildid N] <libserver.so> <engine-surface.json> [fragment.json ...]
//                  [--lib <module>=<lib.so> ...]
//
// For every function that fails sigcheck's check (signature + anchors), tools/sigfix.h looks for it
// again through its string anchors and writes a new signature into its gamedata file. Touched files
// get _meta.game_version / steam_buildid. Prints a markdown report (for the fix PR) on stdout.
// Exit 0 when nothing is left failing, 1 when some function could not be fixed, 2 on bad input.
// It only edits files: a human reviews the PR, since a wrong match calls the wrong function.

#include "sigfix.h"

#include <elf.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace readyup;

static std::string ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct LoadedModule {
  std::string bin;
  es::Image img;
};

static bool LoadModule(const char* path, LoadedModule* out) {
  out->bin = ReadFile(path);
  if (out->bin.size() < sizeof(Elf64_Ehdr)) return false;
  const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(out->bin.data());
  for (int i = 0; i < eh->e_phnum; ++i) {
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(out->bin.data() + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD) continue;
    es::Region r;
    r.addr = ph->p_vaddr;
    r.data = reinterpret_cast<const uint8_t*>(out->bin.data()) + ph->p_offset;
    r.size = ph->p_filesz;
    r.exec = (ph->p_flags & PF_X) != 0;
    out->img.regions.push_back(r);
  }
  return true;
}

// Replaces the "linux" signature of function `name` (the first one after its key) in the JSON text.
static bool ReplaceSignature(std::string* text, const std::string& name, const std::string& from, const std::string& to) {
  const size_t at = text->find("\"" + name + "\": {");
  const std::string old = "\"linux\": \"" + from + "\"";
  const size_t p = at == std::string::npos ? at : text->find(old, at);
  if (p == std::string::npos) return false;
  text->replace(p, old.size(), "\"linux\": \"" + to + "\"");
  return true;
}

// Sets the first `"key": <value>` (the _meta block comes first) to `json`.
static void SetMeta(std::string* text, const std::string& key, const std::string& json) {
  const std::string k = "\"" + key + "\": ";
  const size_t p = text->find(k);
  if (p == std::string::npos) return;
  const size_t v = p + k.size();
  text->replace(v, text->find_first_of(",\n}", v) - v, json);
}

static std::string Cell(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\n') out += "\\n";
    else if (c == '|') out += "\\|";
    else if (c == '`') out += '\'';
    else out += c;
  }
  return out;
}

int main(int argc, char** argv) {
  std::map<std::string, LoadedModule> modules;
  std::vector<std::string> files;
  std::string gameVersion, buildid, server;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "--lib" || a == "--game-version" || a == "--buildid") && i + 1 < argc) {
      const std::string v = argv[++i];
      if (a == "--game-version") gameVersion = v;
      else if (a == "--buildid") buildid = v;
      else {
        const auto eq = v.find('=');
        if (eq == std::string::npos || eq == 0 || !LoadModule(v.c_str() + eq + 1, &modules[v.substr(0, eq)])) {
          std::fprintf(stderr, "bad --lib %s (want <module>=<path to lib<module>.so>)\n", v.c_str());
          return 2;
        }
      }
    } else if (server.empty()) {
      server = a;
    } else {
      files.push_back(a);
    }
  }
  if (server.empty() || files.empty() || !LoadModule(server.c_str(), &modules["server"])) {
    std::fprintf(stderr,
                 "usage: %s [--game-version V] [--buildid N] <libserver.so> <engine-surface.json> [fragment.json ...] "
                 "[--lib <module>=<lib.so> ...]\n",
                 argv[0]);
    return 2;
  }

  std::string fixedRows, unfixed;
  int left = 0;
  for (const auto& path : files) {
    std::string text = ReadFile(path.c_str()), err;
    const auto surface = es::ParseEngineSurface(text, &err);
    if (!surface) {
      std::fprintf(stderr, "%s: parse error: %s\n", path.c_str(), err.c_str());
      return 2;
    }
    bool touched = false;
    for (const auto& f : surface->functions) {
      const auto m = modules.find(f.library);
      if (m == modules.end() || es::Resolve(m->second.img, f).ok) continue;
      const sigfix::Fix fix = sigfix::FixFunction(m->second.img, f);
      if (fix.fixed && ReplaceSignature(&text, f.name, f.pattern, fix.pattern)) {
        touched = true;
        fixedRows += "| `" + f.name + "` | `" + path + "` | `" + f.pattern + "` | `" + fix.pattern + "` | " +
                     (fix.same_shape ? "same instructions by length (reordered / registers changed)"
                                     : "**changed shape**: check the arguments") +
                     " | " + Cell(fix.anchors) + " |\n";
        continue;
      }
      ++left;
      unfixed += "- `" + f.name + "` (`" + path + "`, " + (f.required ? "required" : "optional") + "): " +
                 Cell(fix.fixed ? "could not edit its signature in the file" : fix.why) + "\n";
      for (const auto& h : fix.hints) unfixed += "  - nearest string: `" + Cell(h) + "`\n";
    }
    if (!touched) continue;
    if (!gameVersion.empty()) SetMeta(&text, "game_version", "\"" + gameVersion + "\"");
    if (!buildid.empty()) SetMeta(&text, "steam_buildid", buildid);
    std::ofstream(path, std::ios::binary) << text;
  }

  std::printf("### readyup_sigfix\n\n");
  if (!fixedRows.empty()) {
    std::printf(
        "New signatures. Each one is a changed prologue, and sigcheck can't see argument changes: compare the "
        "argument moves before merging, above all where the shape changed.\n\n"
        "| Function | File | Old signature | New signature | Prologue | Anchors |\n|---|---|---|---|---|---|\n%s\n",
        fixedRows.c_str());
  }
  if (!unfixed.empty()) std::printf("Not fixed (needs a human):\n\n%s\n", unfixed.c_str());
  if (fixedRows.empty() && unfixed.empty()) std::printf("Nothing failing.\n");
  return left == 0 ? 0 : 1;
}
