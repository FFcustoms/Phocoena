// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common/FilesystemWatcher.h"
#include "Horizon/Log.h"
// No filesystem notification service exists in this port. Browser rescans on
// entry; live texture/resource-pack reload is deliberately unavailable.
namespace wtr { inline namespace watcher { class watch {}; } }
namespace Common
{
FilesystemWatcher::FilesystemWatcher() = default;
FilesystemWatcher::~FilesystemWatcher() = default;
void FilesystemWatcher::Watch(const std::string& path)
{
  Horizon::Log("Filesystem", "live watch unavailable: %s", path.c_str());
}
void FilesystemWatcher::Unwatch(const std::string&) {}
}
