#pragma once
#include "game.h"
#include "patch.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// mod.json "debug": a key of an ini the mod ships, set by DSM while debug mode is on (e.g. a log switch).
struct DebugIni {
    std::string file;  // game-relative, one of the mod's files
    std::string section, key, on, off;
};

// A mod is a folder in mods/<game name>/<mod>/ with an optional mod.json and a files/ folder
// that mirrors the game root (files/param/DrawParam/x.dcx -> <game>/param/DrawParam/x.dcx).
// lang/<code>.json (en.json, ru.json, ...) translates its name, description and key actions.
struct ModInfo {
    std::string folder;
    std::string name, version, author, description;  // name, description: in the interface language if lang/ has it
    std::string baseName;                             // name in mod.json, untranslated: what "incompatible" refers to
    fs::path root;
    std::vector<std::string> files;  // game-relative paths the mod sets, forward slashes, UTF-8
    std::set<std::string> patched;   // those of them built from patches/<path>.dsmpatch instead of files/
    uint64_t totalSize = 0;
    std::vector<std::pair<std::string, std::string>> keys;  // mod.json "keys": key -> what it does in game
    std::vector<std::string> incompatible;                  // mod.json "incompatible": folders or names of mods
    std::vector<DebugIni> debug;                            // mod.json "debug"
    std::vector<std::string> cleanup;                       // mod.json "cleanup": files it makes in the game (logs), * allowed
};

// Why two mods may not work together: the author said so, and/or they ship the same game files.
struct CompatIssue {
    std::string other;     // folder of the other mod
    bool declared = false; // listed in "incompatible" of either mod
    int sharedFiles = 0;   // game files both of them replace
};

enum class LogLevel { Info, Good, Warn, Error, Debug };  // Debug: details shown only in the journal of debug mode
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
    bool dirty = false;          // enabled set differs from what is deployed, or debug mode from what it was applied with
    bool debug = false;          // debug mode: mods' "debug" ini keys get their "on" value when applied

    void setEnabled(const std::string& folder, bool on);
    void move(int index, int delta);
    void setGamePath(const fs::path& p);
    void autoGamePath();         // forget the folder picked by hand, look the game up through Steam again
    void setDebug(bool on);
    void refresh();              // recompute conflicts + dirty
    int deployedCount(const std::string& folder) const;
    std::string installedVersion(const std::string& folder) const;  // version in the game, empty if none
    bool hasUpdate(const std::string& folder) const;                 // in the game, and mod.json has another version
    size_t deployedTotal() const;
    std::string displayName(const std::string& folder) const;  // name from mod.json, else folder
    std::vector<CompatIssue> compatibility(const std::string& folder) const;  // against every other mod in the list

    bool apply(const LogFn& log, std::atomic<float>* progress);
    bool restoreAll(const LogFn& log, std::atomic<float>* progress);

private:
    struct Deployed { std::string mod; uint64_t hash = 0; };
    struct Original { bool existed = false; bool backedUp = false; };
    std::map<std::string, Deployed> deployed_;
    std::map<std::string, Original> originals_;
    std::map<std::string, std::string> installed_;  // mod folder -> version that was applied
    bool appliedDebug_ = false;                     // debug mode the files in the game were applied with
    std::vector<std::string> order_;
    mutable std::mutex mutex_;

    void load();
    std::map<std::string, std::string> desired() const;
    const ModInfo* findMod(const std::string& folder) const;
    bool restoreFile(const std::string& rel, const LogFn& log);
    bool checkReady(const LogFn& log) const;
    bool usesDebug() const;  // an enabled mod has "debug" keys
    void setDebugKeys(const ModInfo& mod, const std::string& rel, Bytes& data) const;
    void cleanupMod(const std::string& folder, const LogFn& log);  // deletes what the mod left in the game while running
};
