#pragma once

#include <string>
#include <vector>

namespace readyup::stale_backups {

// Removes the CS2 round backups (`<prefix>*.txt`) in `dirs`: what an earlier match with the same
// numeric id left behind (platform slugs repeat across tournaments, so their ids do too). A fresh
// assignment calls it so those files are never sent, nor restored, as this match's rounds.
// Missing directories are skipped; an empty prefix removes nothing. Returns how many files were
// removed; their names go to *removed when given.
int RemoveRoundBackups(const std::vector<std::string>& dirs, const std::string& prefix,
                       std::vector<std::string>* removed);

}  // namespace readyup::stale_backups
