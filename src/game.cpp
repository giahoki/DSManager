#include "game.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <regex>

static Col c3(float r, float g, float b) { return {r, g, b, 1}; }

Theme mix(const Theme& a, const Theme& b, float t)
{
    return {lerp(a.parch, b.parch, t), lerp(a.parchDim, b.parchDim, t), lerp(a.ash, b.ash, t),
            lerp(a.gold, b.gold, t),   lerp(a.goldDim, b.goldDim, t),   lerp(a.ember, b.ember, t),
            lerp(a.emberHot, b.emberHot, t), lerp(a.blood, b.blood, t), mix(a.bg, b.bg, t)};
}

// Ash and stone, the faintest fire: the hub between worlds.
const Theme& selectionTheme()
{
    static const Theme t{rgb(214, 208, 196), rgb(142, 137, 128), rgb(96, 93, 90),
                         rgb(174, 156, 120), rgb(104, 95, 78),
                         rgb(190, 146, 98),  rgb(226, 198, 152), rgb(168, 72, 60),
                         {c3(0.019f, 0.020f, 0.024f), c3(0.034f, 0.031f, 0.029f), c3(0.070f, 0.071f, 0.077f),
                          c3(0.62f, 0.40f, 0.21f), c3(0.85f, 0.58f, 0.32f), 0.f, -0.62f, 0.7f, 0.6f, 0.9f, 1.f, 0.35f, 0.6f, 0.f}};
    return t;
}

// Lordran: cold grey-blue stone, a small amber bonfire.
static Theme themeDSR()
{
    return {rgb(216, 211, 200), rgb(144, 141, 134), rgb(96, 96, 96),
            rgb(176, 160, 124), rgb(104, 98, 82),
            rgb(206, 152, 92),  rgb(236, 206, 154), rgb(170, 70, 58),
            {c3(0.017f, 0.020f, 0.026f), c3(0.031f, 0.030f, 0.031f), c3(0.068f, 0.074f, 0.086f),
             c3(0.70f, 0.44f, 0.21f), c3(0.92f, 0.62f, 0.30f), 0.f, -0.62f, 0.8f, 0.75f, 1.f, 1.f, 1.f, 0.f, 0.f}};
}

// Lothric: ash, smoke and a fire that is going out red.
static Theme themeDS3()
{
    return {rgb(214, 204, 194), rgb(146, 134, 126), rgb(102, 92, 88),
            rgb(170, 138, 108), rgb(102, 82, 66),
            rgb(186, 78, 56),   rgb(228, 134, 100), rgb(188, 60, 46),
            {c3(0.021f, 0.018f, 0.019f), c3(0.040f, 0.026f, 0.023f), c3(0.084f, 0.074f, 0.071f),
             c3(0.64f, 0.22f, 0.11f), c3(0.95f, 0.36f, 0.16f), 0.f, -0.64f, 0.95f, 1.0f, 0.75f, 1.25f, 0.3f, 1.f, 0.f}};
}

// The Lands Between: golden haze, light from above, slow motes of grace.
static Theme themeER()
{
    return {rgb(232, 224, 204), rgb(162, 152, 130), rgb(112, 106, 94),
            rgb(214, 188, 124), rgb(132, 114, 74),
            rgb(222, 192, 118), rgb(250, 232, 178), rgb(178, 80, 62),
            {c3(0.034f, 0.031f, 0.024f), c3(0.024f, 0.022f, 0.021f), c3(0.088f, 0.082f, 0.064f),
             c3(0.78f, 0.64f, 0.34f), c3(0.98f, 0.86f, 0.54f), 0.f, 0.60f, 0.65f, 0.85f, 0.55f, 1.3f, 0.f, 0.f, 1.f}};
}

const std::vector<GameDef>& games()
{
    static const std::vector<GameDef> list = {
        {"dsr", "Dark Souls Remastered", L"DarkSoulsRemastered.exe", L"DARK SOULS REMASTERED", L"", "570940",
         "Лордран  ·  2018", "Где всё началось.", "", themeDSR(), "LOGO_DSR",
         // old parchment catching from a bonfire: an orange edge, sparks in the char
         {rgb(232, 110, 36), rgb(255, 214, 140), 5.f, 0.10f, 1.f}},
        {"ds3", "Dark Souls III", L"DarkSoulsIII.exe", L"DARK SOULS III", L"Game", "374320",
         "Лотрик  ·  2016", "Где всё догорает.",
         "Онлайн защищён EasyAntiCheat: моды только для игры оффлайн.", themeDS3(), "",
         // a page that has been burning for an age: flaky, a dull red edge, a wide band of ash
         {rgb(168, 50, 30), rgb(214, 160, 128), 8.5f, 0.17f, 0.45f}},
        {"er", "Elden Ring", L"eldenring.exe", L"ELDEN RING", L"Game", "1245620",
         "Междуземье  ·  2022", "Где всё разбито.",
         "EasyAntiCheat: только оффлайн. Файлы игра читает через ModEngine 2 (тестово).", themeER(), "",
         // gold leaf in grace: smooth sweeping edge, golden light, glittering
         {rgb(236, 180, 80), rgb(255, 244, 205), 3.2f, 0.05f, 1.5f}},
    };
    return list;
}

static std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    return {};
}

bool isGameFolder(const GameDef& g, const fs::path& dir)
{
    std::error_code ec;
    return !dir.empty() && fs::exists(dir / g.exe, ec);
}

fs::path locateGame(const GameDef& g)
{
    std::vector<fs::path> steamRoots;
    for (auto [root, key, value] : {
             std::tuple{HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
             std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
             std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"}}) {
        std::wstring v = regString(root, key, value);
        if (!v.empty()) steamRoots.emplace_back(fs::path(v).make_preferred());
    }
    steamRoots.emplace_back(L"C:\\Program Files (x86)\\Steam");

    // Library paths from libraryfolders.vdf come first: they keep the real letter case,
    // while the registry stores SteamPath in lowercase.
    std::vector<fs::path> libraries;
    const std::regex pathRe(R"re("path"\s+"([^"]+)")re");
    for (const auto& root : steamRoots) {
        std::string vdf = readTextFile(root / "steamapps" / "libraryfolders.vdf");
        for (std::sregex_iterator it(vdf.begin(), vdf.end(), pathRe), end; it != end; ++it) {
            std::string p = (*it)[1].str();
            p = std::regex_replace(p, std::regex(R"(\\\\)"), "\\");
            libraries.push_back(pathFromUtf8(p));
        }
    }
    libraries.insert(libraries.end(), steamRoots.begin(), steamRoots.end());

    const std::regex dirRe(R"re("installdir"\s+"([^"]+)")re");
    for (const auto& lib : libraries) {
        fs::path dirName = g.steamDir;
        std::string acf = readTextFile(lib / "steamapps" / ("appmanifest_" + g.steamAppId + ".acf"));
        std::smatch m;
        if (std::regex_search(acf, m, dirRe)) dirName = pathFromUtf8(m[1].str());
        fs::path candidate = lib / "steamapps" / "common" / dirName / g.subDir;
        if (isGameFolder(g, candidate)) return realCase(candidate);
    }
    return {};
}
