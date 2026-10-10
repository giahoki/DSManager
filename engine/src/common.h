#pragma once
// DSU Engine: native render mod for DARK SOULS REMASTERED (d3d11.dll proxy).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

namespace dsu {

// ---------------------------------------------------------------- paths, log

fs::path gameDir();       // folder of DarkSoulsRemastered.exe
fs::path engineDir();     // gameDir()/DSUEngine

void log(const char* fmt, ...);

// ---------------------------------------------------------------- settings (DSUEngine.ini)

struct Settings {
    bool enabled = true;
    int keyToggle = VK_F10;   // effects on/off
    int keyReload = VK_F11;   // re-read ini and recompile shaders
    int debugView = 0;        // 0 off, 1 linear depth, 2 normals, 3 reflections only, 4 bloom only

    bool ssr = true;
    float ssrIntensity = 0.6f;
    float ssrMaxDistance = 40.0f;   // metres along the ray
    int ssrSteps = 48;
    float ssrThickness = 0.6f;      // metres
    float ssrWallAmount = 0.0f;     // 0..1: reflections on walls, characters and other non-floor surfaces
    float ssrFresnelF0 = 0.04f;

    bool bloom = true;
    float bloomIntensity = 0.06f;
    float bloomThreshold = 1.5f;    // exposed HDR brightness where glow starts
    float bloomKnee = 0.5f;
    float bloomRadius = 0.85f;      // 0..1, how far glow spreads

    // used only until the game's own camera constants are seen (SAO disabled in settings)
    float fallbackFovY = 43.0f;
    float fallbackNear = 0.1f;
    float fallbackFar = 2000.0f;

    void load();
};

extern Settings g_cfg;

}  // namespace dsu
