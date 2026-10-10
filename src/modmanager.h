#pragma once
#include "game.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// A mod is a folder in mods/<game name>/<mod>/ with an optional mod.json and a files/ folder
// that mirrors the game root (files/param/DrawParam/x.dcx -> <game>/param/DrawParam/x.dcx).
struct ModInfo {
    std::string folder;
    std::string name, version, author, description;
    fs::path root;
    std::vector<std::string> files;  // game-relative paths the mod sets, forward slashes, UTF-8
    std::set<std::string> patched;   // those of them built from patches/<path>.dsmpatch instead of files/
    uint64_t totalSize = 0;
};

enum class LogLevel { Info, Good, Warn, Error };
using LogFn = std::function<void(LogLevel, const std::string&)>;

class ModManager {
public:
    void init(const GameDef& game, const fs::path& appRoot);
    void scan();
    void save();

    const GameDef* game = nullptr;
    fs::path modsDir, dataDir, backupDir;
    fs::path gamePath;
    std::string manualPath;

    std::vector<ModInfo> mods;   // in load order: later mods win file conflicts
    std::set<std::string> enabled;

    // file (relative) -> mods that ship it, only for files shipped by more than one enabled mod
    std::map<std::string, std::vector<std::string>> conflicts;
    bool dirty = false;          // enabled set differs from what is deployed

    void setEnabled(const std::string& folder, bool on);
    void move(int index, int delta);
    void setGamePath(const fs::path& p);
    void refresh();              // recompute conflicts + dirty
    int deployedCount(const std::string& folder) const;
    std::string installedVersion(const std::string& folder) const;  // version in the game, empty if none
    bool hasUpdate(const std::string& folder) const;                 // in the game, and mod.json has another version
    size_t deployedTotal() const;
    std::string displayName(const std::string& folder) const;  // name from mod.json, else folder

    bool apply(const LogFn& log, std::atomic<float>* progress);
    bool restoreAll(const LogFn& log, std::atomic<float>* progress);

private:
    struct Deployed { std::string mod; uint64_t hash = 0; };
    struct Original { bool existed = false; bool backedUp = false; };
    std::map<std::string, Deployed> deployed_;
    std::map<std::string, Original> originals_;
    std::map<std::string, std::string> installed_;  // mod folder -> version that was applied
    std::vector<std::string> order_;
    mutable std::mutex mutex_;

    void load();
    std::map<std::string, std::string> desired() const;
    const ModInfo* findMod(const std::string& folder) const;
    bool restoreFile(const std::string& rel, const LogFn& log);
    bool checkReady(const LogFn& log) const;
};
