#include "modmanager.h"

#include "i18n.h"

#include <algorithm>
#include <cstring>
#include <json.hpp>

using json = nlohmann::json;

void ModManager::init(const GameDef& g, const fs::path& appRoot)
{
    game = &g;
    modsDir = appRoot / "mods" / pathFromUtf8(g.name);
    dataDir = appRoot / "data" / g.id;
    backupDir = dataDir / "backup";
    std::error_code ec;
    fs::create_directories(modsDir, ec);
    fs::create_directories(dataDir, ec);
    load();
    // A folder picked by hand wins; otherwise look the game up through Steam every start.
    gamePath.clear();
    if (!manualPath.empty() && isGameFolder(g, pathFromUtf8(manualPath))) gamePath = pathFromUtf8(manualPath);
    if (gamePath.empty()) gamePath = locateGame(g);
    scan();
}

void ModManager::load()
{
    std::string text = readTextFile(dataDir / "state.json");
    if (text.empty()) return;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return;
    manualPath = j.value("manualPath", "");
    for (auto& m : j.value("order", json::array())) order_.push_back(m.get<std::string>());
    for (auto& m : j.value("enabled", json::array())) enabled.insert(m.get<std::string>());
    // keep the json objects alive: items() of a temporary would dangle inside the loop
    json dep = j.value("deployed", json::object()), orig = j.value("originals", json::object());
    for (auto& [rel, d] : dep.items()) deployed_[rel] = {d.value("mod", ""), hashFromHex(d.value("hash", ""))};
    for (auto& [rel, o] : orig.items()) originals_[rel] = {o.value("existed", false), o.value("backup", false)};
    json inst = j.value("installed", json::object());
    for (auto& [folder, v] : inst.items()) installed_[folder] = v.get<std::string>();
    appliedDebug_ = j.value("appliedDebug", false);
}

void ModManager::save()
{
    std::lock_guard lock(mutex_);
    json j;
    j["manualPath"] = manualPath;
    j["gamePath"] = pathUtf8(gamePath);
    j["order"] = json::array();
    for (auto& m : mods) j["order"].push_back(m.folder);
    j["enabled"] = json::array();
    for (auto& e : enabled) j["enabled"].push_back(e);
    j["deployed"] = json::object();
    for (auto& [rel, d] : deployed_) j["deployed"][rel] = {{"mod", d.mod}, {"hash", hashHex(d.hash)}};
    j["originals"] = json::object();
    for (auto& [rel, o] : originals_) j["originals"][rel] = {{"existed", o.existed}, {"backup", o.backedUp}};
    j["installed"] = installed_;
    j["appliedDebug"] = appliedDebug_;
    writeTextFile(dataDir / "state.json", j.dump(2));
}

void ModManager::scan()
{
    std::vector<ModInfo> found;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(modsDir, ec)) {
        if (!entry.is_directory()) continue;
        ModInfo m;
        m.root = entry.path();
        m.folder = pathUtf8(entry.path().filename());
        m.name = m.folder;
        std::string mj = readTextFile(m.root / "mod.json");
        if (!mj.empty()) {
            json j = json::parse(mj, nullptr, false);
            if (!j.is_discarded()) {
                m.name = j.value("name", m.folder);
                m.version = j.value("version", "");
                m.author = j.value("author", "");
                m.description = j.value("description", "");
                if (j.contains("keys") && j["keys"].is_array())
                    for (auto& k : j["keys"])
                        if (k.is_object()) m.keys.push_back({k.value("key", ""), k.value("action", "")});
                if (j.contains("incompatible") && j["incompatible"].is_array())
                    for (auto& s : j["incompatible"])
                        if (s.is_string()) m.incompatible.push_back(s.get<std::string>());
                if (j.contains("cleanup") && j["cleanup"].is_array())
                    for (auto& c : j["cleanup"])
                        if (c.is_string()) m.cleanup.push_back(c.get<std::string>());
                if (j.contains("debug") && j["debug"].is_array())
                    for (auto& d : j["debug"])
                        if (d.is_object() && d.contains("file") && d.contains("key"))
                            m.debug.push_back({d.value("file", ""), d.value("section", ""), d.value("key", ""),
                                               d.value("on", "1"), d.value("off", "0")});
            }
        }
        m.baseName = m.name;

        // translation into the interface language: replaces what it has, the rest stays from mod.json
        std::string lj = readTextFile(m.root / "lang" / (std::string(langCode()) + ".json"));
        if (!lj.empty()) {
            json j = json::parse(lj, nullptr, false);
            if (j.is_object()) {
                m.name = j.value("name", m.name);
                m.description = j.value("description", m.description);
                if (j.contains("keys") && j["keys"].is_array()) {
                    m.keys.clear();
                    for (auto& k : j["keys"])
                        if (k.is_object()) m.keys.push_back({k.value("key", ""), k.value("action", "")});
                }
            }
        }
        fs::path filesDir = m.root / "files";
        for (auto& f : fs::recursive_directory_iterator(filesDir, ec)) {
            if (!f.is_regular_file()) continue;
            m.files.push_back(pathUtf8(fs::relative(f.path(), filesDir).generic_u8string()));
            m.totalSize += f.file_size();
        }
        fs::path patchDir = m.root / "patches";
        for (auto& f : fs::recursive_directory_iterator(patchDir, ec)) {
            std::string rel = pathUtf8(fs::relative(f.path(), patchDir).generic_u8string());
            if (!f.is_regular_file() || !rel.ends_with(kPatchExt)) continue;
            rel.resize(rel.size() - std::strlen(kPatchExt));
            if (std::find(m.files.begin(), m.files.end(), rel) == m.files.end()) m.files.push_back(rel);
            m.patched.insert(rel);
            m.totalSize += f.file_size();
        }
        std::sort(m.files.begin(), m.files.end());
        found.push_back(std::move(m));
    }

    // Keep the saved load order, new mods go to the end alphabetically.
    std::sort(found.begin(), found.end(), [&](const ModInfo& a, const ModInfo& b) {
        auto ia = std::find(order_.begin(), order_.end(), a.folder);
        auto ib = std::find(order_.begin(), order_.end(), b.folder);
        if (ia != ib) return ia < ib;
        return a.folder < b.folder;
    });
    mods = std::move(found);
    order_.clear();
    for (auto& m : mods) order_.push_back(m.folder);
    for (auto it = enabled.begin(); it != enabled.end();)
        it = findMod(*it) ? std::next(it) : enabled.erase(it);
    {
        // installs from before versions were tracked: take what is in the folder now as installed
        std::lock_guard lock(mutex_);
        for (auto& [rel, d] : deployed_)
            if (!installed_.count(d.mod))
                if (const ModInfo* m = findMod(d.mod)) installed_[d.mod] = m->version;
    }
    refresh();
}

std::string ModManager::displayName(const std::string& folder) const
{
    const ModInfo* m = findMod(folder);
    return m ? m->name : folder;
}

std::vector<CompatIssue> ModManager::compatibility(const std::string& folder) const
{
    std::vector<CompatIssue> out;
    const ModInfo* a = findMod(folder);
    if (!a) return out;
    auto lists = [](const ModInfo& x, const ModInfo& y) {
        return std::any_of(x.incompatible.begin(), x.incompatible.end(),
                           [&](const std::string& s) { return s == y.folder || s == y.baseName || s == y.name; });
    };
    for (auto& b : mods) {
        if (b.folder == a->folder) continue;
        CompatIssue c{b.folder, lists(*a, b) || lists(b, *a), 0};
        // both lists are sorted (scan)
        for (size_t i = 0, j = 0; i < a->files.size() && j < b.files.size();) {
            int cmp = a->files[i].compare(b.files[j]);
            if (cmp == 0) { ++c.sharedFiles; ++i; ++j; }
            else if (cmp < 0) ++i;
            else ++j;
        }
        if (c.declared || c.sharedFiles) out.push_back(c);
    }
    return out;
}

const ModInfo* ModManager::findMod(const std::string& folder) const
{
    for (auto& m : mods)
        if (m.folder == folder) return &m;
    return nullptr;
}

std::map<std::string, std::string> ModManager::desired() const
{
    std::map<std::string, std::string> d;
    for (auto& m : mods)
        if (enabled.count(m.folder))
            for (auto& f : m.files) d[f] = m.folder;
    return d;
}

void ModManager::refresh()
{
    conflicts.clear();
    std::map<std::string, std::vector<std::string>> owners;
    for (auto& m : mods)
        if (enabled.count(m.folder))
            for (auto& f : m.files) owners[f].push_back(m.folder);
    for (auto& [f, list] : owners)
        if (list.size() > 1) conflicts[f] = list;

    auto want = desired();
    bool debugChanged = usesDebug() && debug != appliedDebug_;
    std::lock_guard lock(mutex_);
    dirty = debugChanged || want.size() != deployed_.size();
    if (!dirty)
        for (auto& [rel, mod] : want) {
            auto it = deployed_.find(rel);
            if (it == deployed_.end() || it->second.mod != mod) { dirty = true; break; }
        }
}

void ModManager::setEnabled(const std::string& folder, bool on)
{
    if (on) enabled.insert(folder);
    else enabled.erase(folder);
    refresh();
    save();
}

void ModManager::move(int index, int delta)
{
    int to = index + delta;
    if (index < 0 || to < 0 || to >= (int)mods.size()) return;
    std::swap(mods[index], mods[to]);
    order_.clear();
    for (auto& m : mods) order_.push_back(m.folder);
    refresh();
    save();
}

void ModManager::setGamePath(const fs::path& p)
{
    gamePath = p;
    manualPath = pathUtf8(p);
    save();
}

void ModManager::autoGamePath()
{
    manualPath.clear();
    gamePath = locateGame(*game);
    save();
}

void ModManager::setDebug(bool on)
{
    debug = on;
    refresh();
}

bool ModManager::usesDebug() const
{
    return std::any_of(mods.begin(), mods.end(), [&](const ModInfo& m) { return !m.debug.empty() && enabled.count(m.folder); });
}

// Sets key=value in [section] of an ini, keeping everything else (comments, order, line endings) as it is.
static void setIniValue(std::string& text, const std::string& section, const std::string& key, const std::string& value)
{
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    bool inSection = section.empty();
    size_t sectionEnd = std::string::npos;  // where to add the key if the section lacks it
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        size_t next = end == std::string::npos ? text.size() : end + 1;
        std::string line = trim(text.substr(pos, next - pos));
        if (!line.empty() && line[0] == '[') {
            if (inSection && !section.empty()) break;  // the section ended without the key
            inSection = lower(line) == "[" + lower(section) + "]";
            if (inSection) sectionEnd = next;
        } else if (inSection && !line.empty() && line[0] != ';' && line[0] != '#') {
            size_t eq = line.find('=');
            if (eq != std::string::npos && lower(trim(line.substr(0, eq))) == lower(key)) {
                size_t lineEnd = end == std::string::npos ? text.size() : (end > pos && text[end - 1] == '\r' ? end - 1 : end);
                text.replace(pos, lineEnd - pos, key + "=" + value);
                return;
            }
            sectionEnd = next;
        }
        pos = next;
    }
    std::string add = key + "=" + value + nl;
    if (sectionEnd != std::string::npos) {
        text.insert(sectionEnd, add);
    } else {
        if (!text.empty() && text.back() != '\n') text += nl;
        if (!section.empty()) text += "[" + section + "]" + nl;
        text += add;
    }
}

// "*" and "?" in a file name, case-insensitive like Windows
static bool wildcardMatch(const std::wstring& pat, const std::wstring& s)
{
    size_t p = 0, i = 0, star = std::wstring::npos, mark = 0;
    auto eq = [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); };
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == L'?' || eq(pat[p], s[i]))) { ++p; ++i; }
        else if (p < pat.size() && pat[p] == L'*') { star = p++; mark = i; }
        else if (star != std::wstring::npos) { p = star + 1; i = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == L'*') ++p;
    return p == pat.size();
}

void ModManager::cleanupMod(const std::string& folder, const LogFn& log)
{
    const ModInfo* mod = findMod(folder);
    if (!mod || mod->cleanup.empty() || gamePath.empty()) return;
    std::error_code ec;
    for (auto& pattern : mod->cleanup) {
        // only inside the game folder: no absolute paths, no "..", wildcards only in the file name
        fs::path rel = pathFromUtf8(pattern);
        bool safe = !pattern.empty() && rel.is_relative() && !rel.has_root_name();
        for (auto& part : rel) safe &= part != "..";
        if (!safe || rel.parent_path().wstring().find_first_of(L"*?") != std::wstring::npos) {
            log(LogLevel::Debug, "cleanup: " + pattern + tr(" — недопустимый путь, пропущен", " — not allowed, skipped"));
            continue;
        }
        fs::path dir = gamePath / rel.parent_path();
        std::wstring name = rel.filename().wstring();
        std::vector<fs::path> found;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file() && wildcardMatch(name, e.path().filename().wstring())) found.push_back(e.path());
        for (auto& p : found) {
            std::string grel = pathUtf8(fs::relative(p, gamePath, ec).generic_u8string());
            {
                std::lock_guard lock(mutex_);
                if (deployed_.count(grel)) continue;  // a file some mod installs, not something left behind
            }
            if (fs::remove(p, ec))
                log(LogLevel::Debug, tr("удалён файл мода: ", "removed the mod's file: ") + grel);
        }
    }
}

void ModManager::setDebugKeys(const ModInfo& mod, const std::string& rel, Bytes& data) const
{
    std::string text(data.begin(), data.end());
    for (auto& d : mod.debug)
        if (d.file == rel) setIniValue(text, d.section, d.key, debug ? d.on : d.off);
    data.assign(text.begin(), text.end());
}

int ModManager::deployedCount(const std::string& folder) const
{
    std::lock_guard lock(mutex_);
    int n = 0;
    for (auto& [rel, d] : deployed_)
        if (d.mod == folder) ++n;
    return n;
}

std::string ModManager::installedVersion(const std::string& folder) const
{
    std::lock_guard lock(mutex_);
    auto it = installed_.find(folder);
    return it == installed_.end() ? std::string() : it->second;
}

bool ModManager::hasUpdate(const std::string& folder) const
{
    const ModInfo* m = findMod(folder);
    if (!m || deployedCount(folder) == 0) return false;
    std::lock_guard lock(mutex_);
    auto it = installed_.find(folder);
    return it != installed_.end() && it->second != m->version;
}

size_t ModManager::deployedTotal() const
{
    std::lock_guard lock(mutex_);
    return deployed_.size();
}

bool ModManager::checkReady(const LogFn& log) const
{
    if (!isGameFolder(*game, gamePath)) {
        log(LogLevel::Error, tr("Папка игры не найдена. Укажи её в настройках.", "Game folder not found. Set it in the settings."));
        return false;
    }
    if (isProcessRunning(game->exe.c_str())) {
        log(LogLevel::Error, tr("Игра запущена. Закрой её и попробуй снова.", "The game is running. Close it and try again."));
        return false;
    }
    return true;
}

static void removeEmptyParents(fs::path dir, const fs::path& stop)
{
    std::error_code ec;
    while (!dir.empty() && dir != stop && fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) {
        fs::remove(dir, ec);
        dir = dir.parent_path();
    }
}

// Puts the original back (or deletes a file the mods added). Caller holds no lock.
bool ModManager::restoreFile(const std::string& rel, const LogFn& log)
{
    std::error_code ec;
    fs::path dst = gamePath / pathFromUtf8(rel);
    fs::path bak = backupDir / pathFromUtf8(rel);
    Deployed d;
    Original o;
    {
        std::lock_guard lock(mutex_);
        d = deployed_[rel];
        o = originals_[rel];
    }

    bool ok = true;
    if (!o.existed) {
        // the game never had it: it goes even if it changed since (ReShade rewrites its ini, settings edited by hand)
        bool changed = fs::exists(dst, ec) && hashFile(dst) != d.hash;
        fs::remove(dst, ec);
        removeEmptyParents(dst.parent_path(), gamePath);
        log(LogLevel::Debug, tr("удалён (в игре его не было): ", "removed (not in the original game): ") + rel +
                                 (changed ? tr(", был изменён во время игры", ", it had changed while playing") : ""));
    } else if (fs::exists(dst, ec) && hashFile(dst) != d.hash) {
        // Steam update or file integrity check replaced it: the game file is the new original.
        log(LogLevel::Warn, tr("Файл изменён не менеджером, оставляю как есть: ", "Changed outside DSM, leaving it as it is: ") + rel);
        fs::remove(bak, ec);
    } else if (o.backedUp) {
        fs::copy_file(bak, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { log(LogLevel::Error, tr("Не удалось вернуть ", "Couldn't restore ") + rel + ": " + ec.message()); return false; }
        fs::remove(bak, ec);
        removeEmptyParents(bak.parent_path(), backupDir);
        log(LogLevel::Debug, tr("оригинал возвращён: ", "original restored: ") + rel);
    } else {
        log(LogLevel::Warn, tr("Нет оригинала для ", "No original of ") + rel +
                                tr(". Проверь целостность файлов в Steam.", ". Verify the game files in Steam."));
        ok = false;
    }

    std::lock_guard lock(mutex_);
    deployed_.erase(rel);
    originals_.erase(rel);
    return ok;
}

bool ModManager::apply(const LogFn& log, std::atomic<float>* progress)
{
    if (!checkReady(log)) return false;
    auto want = desired();
    std::vector<std::string> stale;
    std::set<std::string> before, touched;  // mods in the game before / mods whose files were copied
    {
        std::lock_guard lock(mutex_);
        for (auto& [rel, d] : deployed_) {
            before.insert(d.mod);
            if (!want.count(rel)) stale.push_back(rel);
        }
    }

    size_t total = stale.size() + want.size(), done = 0;
    int restored = 0, copied = 0;
    bool ok = true;
    std::error_code ec;
    log(LogLevel::Debug, tr("игра: ", "game: ") + pathUtf8(gamePath));
    log(LogLevel::Debug, tr("файлов от модов: ", "files from mods: ") + std::to_string(want.size()) +
                             tr(", вернуть оригиналов: ", ", originals to restore: ") + std::to_string(stale.size()) +
                             tr(", режим отладки: ", ", debug mode: ") + (debug ? tr("вкл", "on") : tr("выкл", "off")));

    for (auto& rel : stale) {
        if (restoreFile(rel, log)) ++restored;
        if (progress) *progress = float(++done) / float(std::max<size_t>(total, 1));
    }

    for (auto& [rel, folder] : want) {
        const ModInfo* mod = findMod(folder);
        fs::path src = mod->root / "files" / pathFromUtf8(rel);
        fs::path dst = gamePath / pathFromUtf8(rel);
        fs::path bak = backupDir / pathFromUtf8(rel);
        uint64_t dstHash = fs::exists(dst, ec) ? hashFile(dst) : 0;

        Deployed cur;
        Original orig;
        bool isDeployed = false, hasOriginal = false;
        {
            std::lock_guard lock(mutex_);
            auto it = deployed_.find(rel);
            if (it != deployed_.end()) { cur = it->second; isDeployed = true; }
            auto ot = originals_.find(rel);
            if (ot != originals_.end()) { orig = ot->second; hasOriginal = true; }
        }

        // A patched file is built from the game's original: the backup once we have one, else the game file.
        // If the expected base doesn't fit (the game file was modded outside DSM, or the backup is stale), try the other one.
        Bytes built;
        bool fromPatch = mod->patched.count(rel) > 0, gameFitsPatch = true;
        if (fromPatch) {
            bool gameIsOriginal = !hasOriginal || (isDeployed && dstHash != 0 && dstHash != cur.hash);
            std::vector<fs::path> bases;
            if (gameIsOriginal && dstHash) bases.push_back(dst);
            if (hasOriginal && orig.backedUp) bases.push_back(bak);
            if (!gameIsOriginal && dstHash) bases.push_back(dst);
            std::string patch = readTextFile(mod->root / "patches" / pathFromUtf8(rel + kPatchExt)), err, firstErr;
            bool builtOk = false;
            for (auto& base : bases) {
                if (applyPatch(patch, readBytes(base), built, err)) {
                    builtOk = true;
                    gameFitsPatch = base == dst && hashBytes(built) != dstHash;
                    break;
                }
                if (firstErr.empty()) firstErr = err;
            }
            if (!builtOk) {
                log(LogLevel::Error, tr("Патч ", "Patch ") + rel + ": " + (bases.empty() ? tr("нет оригинального файла", "no original file") : firstErr));
                ok = false;
                break;
            }
            log(LogLevel::Debug, tr("патч собран: ", "patch built: ") + rel + " (" + formatSize(built.size()) + ")");
        }
        // an ini with "debug" keys is written with them set for the current mode
        bool withDebug = std::any_of(mod->debug.begin(), mod->debug.end(), [&](const DebugIni& d) { return d.file == rel; });
        if (withDebug) {
            if (!fromPatch) built = readBytes(src);
            setDebugKeys(*mod, rel, built);
            log(LogLevel::Debug, std::string(tr("отладочные ключи ", "debug keys ")) + (debug ? tr("включены: ", "on: ") : tr("выключены: ", "off: ")) + rel);
        }
        bool fromBytes = fromPatch || withDebug;
        uint64_t srcHash = fromBytes ? hashBytes(built) : hashFile(src);

        if (isDeployed && cur.mod == folder && cur.hash == srcHash && dstHash == srcHash) {
            log(LogLevel::Debug, tr("без изменений: ", "unchanged: ") + rel);
            if (progress) *progress = float(++done) / float(total);
            continue;
        }

        // A game file the patch doesn't fit is not a new original: keep the old backup.
        // Neither is a file the game never had: the mod itself (or the player) changed it while playing.
        bool externalChange = isDeployed && dstHash != 0 && dstHash != cur.hash && gameFitsPatch && !(hasOriginal && !orig.existed);
        if (!hasOriginal || externalChange) {
            Original o;
            o.existed = dstHash != 0;
            if (o.existed) {
                // Never back up a file that already is a mod file: it would become a fake "original".
                if (!hasOriginal && dstHash == srcHash)
                    log(LogLevel::Warn, tr("В игре уже лежит модовая версия ", "The game already has the modded ") + rel +
                                            tr(", оригинал неизвестен.", ", the original is unknown."));
                else {
                    fs::create_directories(bak.parent_path(), ec);
                    fs::copy_file(dst, bak, fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        log(LogLevel::Error, tr("Не удалось сделать бэкап ", "Couldn't back up ") + rel + ": " + ec.message());
                        ok = false;
                        break;
                    }
                    o.backedUp = true;
                    log(LogLevel::Debug, tr("бэкап оригинала: ", "original backed up: ") + rel);
                }
            } else {
                log(LogLevel::Debug, tr("новый файл (в игре его нет): ", "new file (not in the game): ") + rel);
            }
            if (externalChange)
                log(LogLevel::Warn, tr("Игра обновила ", "The game updated ") + rel + tr(", бэкап заменён новой версией.", ", the backup is now the new version."));
            std::lock_guard lock(mutex_);
            originals_[rel] = o;
        }

        fs::create_directories(dst.parent_path(), ec);
        if (fromBytes) {
            if (!writeBytes(dst, built)) ec = std::make_error_code(std::errc::io_error);
        } else {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            log(LogLevel::Error, tr("Не удалось скопировать ", "Couldn't copy ") + rel + ": " + ec.message());
            ok = false;
            break;
        }
        log(LogLevel::Debug, tr("записан: ", "written: ") + rel + "  ←  " + mod->name +
                                 (fromBytes ? "" : " (" + formatSize(fs::file_size(dst, ec)) + ")"));
        {
            std::lock_guard lock(mutex_);
            deployed_[rel] = {folder, srcHash};
        }
        touched.insert(folder);
        ++copied;
        if (progress) *progress = float(++done) / float(total);
    }

    std::set<std::string> after, newVersion;  // mods in the game now / those whose version changed
    {
        std::lock_guard lock(mutex_);
        for (auto& [rel, d] : deployed_) after.insert(d.mod);
        if (ok) {
            for (auto it = installed_.begin(); it != installed_.end();)
                it = after.count(it->first) ? std::next(it) : installed_.erase(it);
            for (auto& m : after)
                if (const ModInfo* mi = findMod(m)) {
                    auto it = installed_.find(m);
                    if (it != installed_.end() && it->second != mi->version) newVersion.insert(m);
                    installed_[m] = mi->version;
                }
            appliedDebug_ = debug;
        }
    }
    save();
    refresh();
    if (!ok) return false;

    int reported = 0;
    for (auto& m : before)
        if (!after.count(m)) {
            cleanupMod(m, log);
            log(LogLevel::Good, tr("Мод удалён: ", "Mod removed: ") + displayName(m));
            ++reported;
        }
    for (auto& m : after) {
        if (!touched.count(m) && !newVersion.count(m)) continue;
        const ModInfo* mi = findMod(m);
        std::string v = mi && !mi->version.empty() ? tr(" до v", " to v") + mi->version : "";
        log(LogLevel::Good, before.count(m) ? tr("Мод обновлён", "Mod updated") + v + ": " + displayName(m)
                                            : tr("Мод установлен: ", "Mod installed: ") + displayName(m));
        ++reported;
    }
    if (!reported)
        log(LogLevel::Good, after.empty() ? tr("Нечего применять: ни один мод не выбран.", "Nothing to apply: no mod is lit.")
                                          : tr("Всё уже установлено.", "Everything is already installed."));
    log(LogLevel::Debug, tr("записано файлов: ", "files written: ") + std::to_string(copied) + tr(", возвращено: ", ", restored: ") +
                             std::to_string(restored));
    return true;
}

bool ModManager::restoreAll(const LogFn& log, std::atomic<float>* progress)
{
    if (deployedTotal() == 0) {  // nothing of ours in the game: done, even if the game isn't installed
        log(LogLevel::Good, tr("Игра в оригинале.", "The game is original."));
        return true;
    }
    if (!checkReady(log)) return false;
    std::vector<std::string> all;
    std::set<std::string> before;
    {
        std::lock_guard lock(mutex_);
        for (auto& [rel, d] : deployed_) {
            all.push_back(rel);
            before.insert(d.mod);
        }
    }
    int restored = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (restoreFile(all[i], log)) ++restored;
        if (progress) *progress = float(i + 1) / float(all.size());
    }
    enabled.clear();
    {
        std::lock_guard lock(mutex_);
        installed_.clear();
    }
    save();
    refresh();
    for (auto& m : before) {
        cleanupMod(m, log);
        log(LogLevel::Good, tr("Мод удалён: ", "Mod removed: ") + displayName(m));
    }
    log(LogLevel::Good, tr("Игра в оригинале.", "The game is original."));
    return true;
}
