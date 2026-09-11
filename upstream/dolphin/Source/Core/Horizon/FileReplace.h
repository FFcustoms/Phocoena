// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>

namespace Horizon
{
using RenameOperation = void (*)(const std::filesystem::path&, const std::filesystem::path&,
                                std::error_code&);
inline void NativeRename(const std::filesystem::path& from, const std::filesystem::path& to,
                         std::error_code& error)
{
  std::filesystem::rename(from, to, error);
}
// Horizon's filesystem rename does not replace an existing file. Keep the old
// file until replacement succeeds; restore it on a reported failure. A power
// loss between renames can leave .horizon-old, which is retained for recovery.
// Never overwrite a pre-existing backup or apply this fallback to directories.
inline void ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to,
                        std::error_code& error, RenameOperation rename = NativeRename)
{
  namespace fs = std::filesystem;
  rename(from, to, error);
  if (!error) return;
  std::error_code check;
  if (!fs::is_regular_file(from, check) || !fs::is_regular_file(to, check)) return;
  const fs::path backup = to.string() + ".horizon-old";
  if (fs::exists(backup, check) || check) return;
  rename(to, backup, error);
  if (error) return;
  rename(from, to, error);
  if (error)
  {
    const auto failure = error;
    rename(backup, to, check);
    error = failure;
    return;
  }
  // Replacement is complete even if cleanup fails; do not discard the new file.
  fs::remove(backup, check);
}
}
