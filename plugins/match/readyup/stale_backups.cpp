#include "readyup/stale_backups.h"

#include <filesystem>
#include <system_error>

namespace readyup::stale_backups {

int RemoveRoundBackups(const std::vector<std::string>& dirs, const std::string& prefix,
                       std::vector<std::string>* removed) {
  if (prefix.empty()) return 0;
  int n = 0;
  for (const auto& dir : dirs) {
    std::error_code ec;
    std::vector<std::filesystem::path> hits;
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      const std::string fn = it.path().filename().string();
      if (fn.compare(0, prefix.size(), prefix) == 0 && it.path().extension() == ".txt") hits.push_back(it.path());
    }
    for (const auto& p : hits) {
      std::error_code rm;
      if (!std::filesystem::remove(p, rm) || rm) continue;
      ++n;
      if (removed) removed->push_back(p.filename().string());
    }
  }
  return n;
}

}  // namespace readyup::stale_backups
