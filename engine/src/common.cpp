#include "common.h"

#include <cstdarg>
#include <cstdio>

namespace dsu {

Settings g_cfg;

fs::path gameDir()
{
    static fs::path dir = [] {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path();
    }();
    return dir;
}

fs::path engineDir() { return gameDir() / L"DSUEngine"; }

void log(const char* fmt, ...)
{
    static std::mutex m;
    static FILE* f = [] {
        FILE* fp = nullptr;
        _wfopen_s(&fp, (gameDir() / L"DSUEngine.log").c_str(), L"w");
        return fp;
    }();
    if (!f) return;

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(m);
    fprintf(f, "%02d:%02d:%02d.%03d [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), buf);
    fflush(f);
}

// ---------------------------------------------------------------- ini

static std::wstring iniPath() { return (gameDir() / L"DSUEngine.ini").wstring(); }

static float readF(const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64];
    wchar_t d[64];
    swprintf_s(d, L"%g", def);
    GetPrivateProfileStringW(sec, key, d, buf, 64, iniPath().c_str());
    return (float)_wtof(buf);
}

static int readI(const wchar_t* sec, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, iniPath().c_str());
}

void Settings::load()
{
    Settings d;
    enabled = readI(L"General", L"Enabled", d.enabled) != 0;
    keyToggle = readI(L"General", L"KeyToggle", d.keyToggle);
    keyReload = readI(L"General", L"KeyReload", d.keyReload);
    debugView = readI(L"General", L"DebugView", d.debugView);

    ssr = readI(L"Reflections", L"Enabled", d.ssr) != 0;
    ssrIntensity = readF(L"Reflections", L"Intensity", d.ssrIntensity);
    ssrMaxDistance = readF(L"Reflections", L"MaxDistance", d.ssrMaxDistance);
    ssrSteps = readI(L"Reflections", L"Steps", d.ssrSteps);
    ssrThickness = readF(L"Reflections", L"Thickness", d.ssrThickness);
    ssrWallAmount = readF(L"Reflections", L"WallAmount", d.ssrWallAmount);
    ssrFresnelF0 = readF(L"Reflections", L"FresnelF0", d.ssrFresnelF0);

    bloom = readI(L"Bloom", L"Enabled", d.bloom) != 0;
    bloomIntensity = readF(L"Bloom", L"Intensity", d.bloomIntensity);
    bloomThreshold = readF(L"Bloom", L"Threshold", d.bloomThreshold);
    bloomKnee = readF(L"Bloom", L"Knee", d.bloomKnee);
    bloomRadius = readF(L"Bloom", L"Radius", d.bloomRadius);

    fallbackFovY = readF(L"Camera", L"FallbackFovY", d.fallbackFovY);
    fallbackNear = readF(L"Camera", L"FallbackNear", d.fallbackNear);
    fallbackFar = readF(L"Camera", L"FallbackFar", d.fallbackFar);

    log("settings: enabled=%d ssr=%d (%.2f, %d steps) bloom=%d (%.3f, thr %.2f) debug=%d",
        enabled, ssr, ssrIntensity, ssrSteps, bloom, bloomIntensity, bloomThreshold, debugView);
}

}  // namespace dsu
