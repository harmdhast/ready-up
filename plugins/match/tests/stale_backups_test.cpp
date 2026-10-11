// Offline tests for readyup/stale_backups.h: a fresh match assignment removes the round backups
// an earlier match with the same numeric id left on disk. ctest `match_stale_backups`.
#include "readyup/stale_backups.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fsys = std::filesystem;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static void Touch(const fsys::path& p) { std::ofstream(p) << "x"; }

int main() {
  const fsys::path a = fsys::temp_directory_path() / "ru_stale_backups_a";
  const fsys::path b = fsys::temp_directory_path() / "ru_stale_backups_b";
  for (const auto& d : {a, b}) {
    fsys::remove_all(d);
    fsys::create_directories(d);
  }
  Touch(a / "readyup_backup_123_map1__round00.txt");
  Touch(a / "readyup_backup_123_map2__round974.txt");
  Touch(b / "readyup_backup_123_map1__round05.txt");
  Touch(a / "readyup_backup_1234_map1__round00.txt");  // another match id that starts the same
  Touch(a / "readyup_resume_123_map1__round03.txt");   // the resume file a failover just wrote
  Touch(a / "notes.txt");

  std::vector<std::string> removed;
  const int n = readyup::stale_backups::RemoveRoundBackups({a.string(), b.string(), (a / "missing").string()},
                                                           "readyup_backup_123_map", &removed);
  CHECK(n == 3);
  CHECK(removed.size() == 3);
  CHECK(!fsys::exists(a / "readyup_backup_123_map1__round00.txt"));
  CHECK(!fsys::exists(a / "readyup_backup_123_map2__round974.txt"));
  CHECK(!fsys::exists(b / "readyup_backup_123_map1__round05.txt"));
  CHECK(fsys::exists(a / "readyup_backup_1234_map1__round00.txt"));
  CHECK(fsys::exists(a / "readyup_resume_123_map1__round03.txt"));
  CHECK(fsys::exists(a / "notes.txt"));

  // Nothing left to remove; an empty prefix removes nothing.
  CHECK(readyup::stale_backups::RemoveRoundBackups({a.string()}, "readyup_backup_123_map", nullptr) == 0);
  CHECK(readyup::stale_backups::RemoveRoundBackups({a.string()}, "", nullptr) == 0);
  CHECK(fsys::exists(a / "notes.txt"));

  for (const auto& d : {a, b}) fsys::remove_all(d);
  if (g_failures) {
    std::fprintf(stderr, "stale_backups_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("stale_backups_test: OK\n");
  return 0;
}
