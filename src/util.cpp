#include "util.h"

#include "i18n.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

std::string toUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string pathUtf8(const fs::path& p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path pathFromUtf8(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

uint64_t hashFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return 0;
    uint64_t h = 1469598103934665603ULL;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= (unsigned char)buf[(size_t)i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

std::string hashHex(uint64_t h)
{
    char b[17];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return b;
}

uint64_t hashFromHex(const std::string& s)
{
    return s.empty() ? 0 : std::stoull(s, nullptr, 16);
}

bool isProcessRunning(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = true; break; }
    }
    CloseHandle(snap);
    return found;
}

void shellOpen(const std::wstring& target)
{
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

fs::path exeDir()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::vector<unsigned char> resourceBytes(const wchar_t* name)
{
    HRSRC res = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return {};
    HGLOBAL data = LoadResource(nullptr, res);
    const unsigned char* ptr = data ? static_cast<const unsigned char*>(LockResource(data)) : nullptr;
    if (!ptr) return {};
    return {ptr, ptr + SizeofResource(nullptr, res)};
}

fs::path realCase(const fs::path& p)
{
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return p;
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleW(h, buf, (DWORD)std::size(buf), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= std::size(buf)) return p;
    std::wstring w(buf, n);
    if (w.rfind(L"\\\\?\\", 0) == 0) w = w.substr(4);
    return w;
}

std::string readTextFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeTextFile(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), (std::streamsize)text.size());
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string clockTime()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[16];
    strftime(b, sizeof(b), "%H:%M:%S", &tm);
    return b;
}

std::string formatSize(uint64_t bytes)
{
    char b[32];
    if (bytes >= 1024ull * 1024 * 1024) snprintf(b, sizeof(b), tr("%.1f ГБ", "%.1f GB"), bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) snprintf(b, sizeof(b), tr("%.1f МБ", "%.1f MB"), bytes / (1024.0 * 1024));
    else if (bytes >= 1024) snprintf(b, sizeof(b), tr("%.0f КБ", "%.0f KB"), bytes / 1024.0);
    else snprintf(b, sizeof(b), tr("%llu Б", "%llu B"), (unsigned long long)bytes);
    return b;
}
