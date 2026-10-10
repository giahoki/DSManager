#pragma once
#include "modmanager.h"

#include <atomic>

// Mods published in DSM's GitHub repository, mods/<game name>/<mod>/ on the main branch.
// syncMods() downloads only the mods that are new or whose mod.json version there is newer than
// the local one, and inside them only the files that differ (compared by git blob hash).
// Runs on a worker thread: it touches nothing but the files in modsDir; rescan the list afterwards.
constexpr const char* kModsRepo = "giahoki/DSManager";
constexpr const char* kModsBranch = "main";

bool syncMods(const GameDef& game, const fs::path& modsDir, const LogFn& log, std::atomic<float>* progress);

// The latest DSM release on GitHub: its tag ("1.2.0" or "v1.2.0", returned without the "v") and page.
struct ReleaseInfo {
    std::string version, url;
};
bool latestRelease(ReleaseInfo& out, std::string& err);

// "1.10" > "1.9": numeric parts compare as numbers, the rest as text. <0, 0, >0 like strcmp.
int compareVersions(const std::string& a, const std::string& b);
