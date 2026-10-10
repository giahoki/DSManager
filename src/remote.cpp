#include "remote.h"

#include "patch.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <json.hpp>
#include <map>

using json = nlohmann::json;

namespace {

struct RemoteFile {
    std::string path;  // inside the mod folder, forward slashes
    std::string sha;   // git blob sha1
    uint64_t size = 0;
};

std::string urlEncodePath(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// GET https://<host><path>. Returns the HTTP status (0 if the request didn't go through).
int httpGet(const wchar_t* host, const std::string& path, const wchar_t* headers, Bytes& out, std::string& err)
{
    out.clear();
    std::wstring agent = fromUtf8(std::string("DSM/") + DSM_VERSION);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", fromUtf8(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    auto close = [&] {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    };
    if (session) WinHttpSetTimeouts(session, 10000, 10000, 20000, 60000);

    DWORD status = 0, len = sizeof(status);
    bool ok = req && WinHttpSendRequest(req, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
              && WinHttpReceiveResponse(req, nullptr)
              && WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                     &status, &len, WINHTTP_NO_HEADER_INDEX);
    while (ok) {
        DWORD avail = 0, read = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
        if (avail == 0) break;
        size_t at = out.size();
        out.resize(at + avail);
        if (!WinHttpReadData(req, out.data() + at, avail, &read)) { ok = false; break; }
        out.resize(at + read);
    }
    if (!ok) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ошибка сети %lu", GetLastError());
        err = buf;
        status = 0;
    }
    close();
    return (int)status;
}

// git's object id of a file: sha1("blob <size>\0" + contents)
std::string gitBlobSha(const Bytes& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[20] = {};
    std::string header = "blob " + std::to_string(data.size());
    header.push_back('\0');
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(h, (PUCHAR)header.data(), (ULONG)header.size(), 0);
        if (!data.empty()) BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
        BCryptFinishHash(h, digest, sizeof(digest), 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[41];
    for (int i = 0; i < 20; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// A path from the repository may only point inside the mod folder.
bool safeRelPath(const std::string& p)
{
    if (p.empty() || p.front() == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

std::string versionOf(const std::string& modJson)
{
    json j = json::parse(modJson, nullptr, false);
    return j.is_object() ? j.value("version", "") : "";
}

void removeEmptyDirs(const fs::path& dir)
{
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_directory()) dirs.push_back(e.path());
    std::sort(dirs.rbegin(), dirs.rend());  // deepest first
    for (auto& d : dirs)
        if (fs::is_empty(d, ec)) fs::remove(d, ec);
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b)
{
    auto split = [](const std::string& s) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find('.', start);
            if (end == std::string::npos) end = s.size();
            parts.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    };
    auto digits = [](const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit); };
    std::vector<std::string> pa = split(a), pb = split(b);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        std::string x = i < pa.size() ? pa[i] : "0", y = i < pb.size() ? pb[i] : "0";
        if (digits(x) && digits(y)) {
            x.erase(0, std::min(x.find_first_not_of('0'), x.size() - 1));
            y.erase(0, std::min(y.find_first_not_of('0'), y.size() - 1));
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
        }
        if (int c = x.compare(y)) return c < 0 ? -1 : 1;
    }
    return 0;
}

bool syncMods(const GameDef& game, const fs::path& modsDir, const LogFn& log, std::atomic<float>* progress)
{
    // One API call lists the whole repository; the files themselves come from raw.githubusercontent.com,
    // which doesn't count against GitHub's limit of 60 API requests an hour.
    Bytes body;
    std::string err;
    std::string treePath = std::string("/repos/") + kModsRepo + "/git/trees/" + kModsBranch + "?recursive=1";
    int status = httpGet(L"api.github.com", treePath, L"Accept: application/vnd.github+json\r\n", body, err);
    if (status != 200) {
        log(LogLevel::Warn, status == 0 ? "Нет связи с GitHub (" + err + "), показываю только локальные моды."
                          : status == 403 || status == 429 ? "GitHub временно ограничил запросы, попробуй через час."
                          : "GitHub ответил " + std::to_string(status) + ", обновления не проверены.");
        return false;
    }
    json tree = json::parse(body.begin(), body.end(), nullptr, false);
    if (!tree.is_object() || !tree.contains("tree")) {
        log(LogLevel::Warn, "Не удалось разобрать ответ GitHub.");
        return false;
    }

    // mods/<game name>/<mod>/<path> -> files of each published mod
    std::string prefix = "mods/" + game.name + "/";
    std::map<std::string, std::vector<RemoteFile>> remote;
    for (auto& e : tree["tree"]) {
        if (e.value("type", "") != "blob") continue;
        std::string p = e.value("path", "");
        if (!p.starts_with(prefix)) continue;
        p.erase(0, prefix.size());
        size_t slash = p.find('/');
        if (slash == std::string::npos || !safeRelPath(p)) continue;  // files lying next to the mod folders
        remote[p.substr(0, slash)].push_back({p.substr(slash + 1), e.value("sha", ""), e.value("size", uint64_t(0))});
    }

    // Which mods to take: new here, or a newer version than the local mod.json.
    struct Plan { std::string mod, version, localVersion; Bytes modJson; bool isNew; std::vector<RemoteFile> fetch; std::vector<RemoteFile> all; };
    std::vector<Plan> plans;
    uint64_t totalBytes = 0;
    for (auto& [mod, files] : remote) {
        fs::path dir = modsDir / pathFromUtf8(mod);
        std::error_code ec;
        bool isNew = !fs::exists(dir, ec);
        std::string localVersion = isNew ? "" : versionOf(readTextFile(dir / "mod.json"));

        Plan plan{mod, "", localVersion, {}, isNew, {}, files};
        auto mj = std::find_if(files.begin(), files.end(), [](const RemoteFile& f) { return f.path == "mod.json"; });
        if (mj != files.end()) {
            std::string path = std::string("/") + kModsRepo + "/" + kModsBranch + "/" + urlEncodePath("mods/" + game.name + "/" + mod + "/mod.json");
            if (httpGet(L"raw.githubusercontent.com", path, nullptr, plan.modJson, err) != 200) {
                log(LogLevel::Warn, "Не удалось прочитать mod.json мода " + mod + " на GitHub.");
                continue;
            }
            plan.version = versionOf(std::string(plan.modJson.begin(), plan.modJson.end()));
        }
        if (!isNew && compareVersions(plan.version, localVersion) <= 0) continue;

        for (auto& f : files) {
            if (f.path == "mod.json") continue;  // already here, written last
            fs::path local = dir / pathFromUtf8(f.path);
            if (fs::exists(local, ec) && fs::file_size(local, ec) == f.size && gitBlobSha(readBytes(local)) == f.sha) continue;
            plan.fetch.push_back(f);
            totalBytes += f.size;
        }
        plans.push_back(std::move(plan));
    }

    if (plans.empty()) {
        log(LogLevel::Good, "Новых модов и версий на GitHub нет.");
        return true;
    }

    uint64_t doneBytes = 0;
    bool allOk = true;
    for (auto& plan : plans) {
        fs::path dir = modsDir / pathFromUtf8(plan.mod);
        std::error_code ec;
        bool ok = true;
        for (auto& f : plan.fetch) {
            Bytes data;
            std::string path = std::string("/") + kModsRepo + "/" + kModsBranch + "/" + urlEncodePath("mods/" + game.name + "/" + plan.mod + "/" + f.path);
            int st = httpGet(L"raw.githubusercontent.com", path, nullptr, data, err);
            if (st != 200 || gitBlobSha(data) != f.sha) {
                log(LogLevel::Error, "Не удалось скачать " + plan.mod + "/" + f.path + (st == 200 ? ": файл повреждён" : st ? ": код " + std::to_string(st) : ": " + err));
                ok = false;
                break;
            }
            // written next to the old file and swapped in, so a broken download never leaves half a file
            fs::path dst = dir / pathFromUtf8(f.path), part = dst;
            part += ".part";
            fs::create_directories(dst.parent_path(), ec);
            if (!writeBytes(part, data) || (fs::rename(part, dst, ec), ec)) {
                fs::remove(part, ec);
                log(LogLevel::Error, "Не удалось записать " + plan.mod + "/" + f.path);
                ok = false;
                break;
            }
            doneBytes += f.size;
            if (progress) *progress = float(doneBytes) / float(std::max<uint64_t>(totalBytes, 1));
        }
        if (!ok) {
            allOk = false;
            continue;  // mod.json keeps the old version: the next check takes the rest
        }

        // files the new version no longer ships (only in files/ and patches/: notes of the user stay)
        for (const char* sub : {"files", "patches"}) {
            std::vector<fs::path> stale;
            for (auto& e : fs::recursive_directory_iterator(dir / sub, ec)) {
                if (!e.is_regular_file()) continue;
                std::string rel = std::string(sub) + "/" + pathUtf8(fs::relative(e.path(), dir / sub).generic_u8string());
                if (std::none_of(plan.all.begin(), plan.all.end(), [&](const RemoteFile& f) { return f.path == rel; }))
                    stale.push_back(e.path());
            }
            for (auto& p : stale) fs::remove(p, ec);
            removeEmptyDirs(dir / sub);
        }
        if (!plan.modJson.empty()) writeBytes(dir / "mod.json", plan.modJson);

        std::string name = plan.mod;
        json j = json::parse(plan.modJson.begin(), plan.modJson.end(), nullptr, false);
        if (j.is_object()) name = j.value("name", name);
        std::string v = plan.version.empty() ? "" : " v" + plan.version;
        log(LogLevel::Good, plan.isNew ? "Скачан новый мод" + v + ": " + name
                                       : "Загружена" + v + (plan.localVersion.empty() ? "" : " (было v" + plan.localVersion + ")") + ": " + name);
    }
    if (progress) *progress = 1.f;
    return allOk;
}
