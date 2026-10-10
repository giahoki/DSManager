#pragma once
#include "common.h"

namespace dsu {

// Called once the game created its device (and maybe its swap chain): installs every hook.
void onDevice(ID3D11Device* device, IDXGISwapChain* swapChain);

// What a pixel shader of the game is, recognised by its DXBC checksum (see game_shaders.h).
enum class Role : uint8_t {
    Other,
    Tonemap,     // FRPG_Fil_HDR*: HDR scene -> LDR. Our effects run right before it.
    SaoDepth,    // FRPG_Fil_SAO_Depth: t0 = hardware depth, cb0[8] = depth linearisation
    SaoMain,     // FRPG_Fil_SAO_Main: cb0[72..73] = projection info
    Menu,        // FRPG_Menu_*: HUD and menus
    SceneVS,     // FRPG_Phn/Inst/Wind/Ivy vertex shaders: cb0 rows 0-3 = world -> clip, row 4 = camera
};

// Camera constants: from the scene's world -> clip matrix when seen, else from the game's AO pass.
struct Camera {
    float depthParams[4] = {};  // linear = w / (d * z + y)
    float projInfo[4] = {};     // per uv: viewXY = linear depth * (uv * xy + zw)
    float worldUp[3] = {0, 1, 0};  // world +Y in view space (only from the matrix)
    float farZ = 0;             // view distance of the far plane (0 = unknown)
    bool valid = false;
    bool fromMatrix = false;
    float exposure = 1.0f;      // tonemap key / adapted luminance (cb0 rows 54 and 56 of the tonemap)
};

// Per-frame inputs the effects need, gathered by the hooks.
struct FrameInputs {
    ID3D11ShaderResourceView* depth = nullptr;  // game's hardware depth SRV (AddRef'd)
    Camera cam;
};

// effects.cpp
bool effectsInit(ID3D11Device* device);
void effectsReload();
// Runs reflections + bloom on `scene` (HDR) and returns an SRV to use in its place, or null.
ID3D11ShaderResourceView* effectsRun(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* scene, const FrameInputs& in);

}  // namespace dsu
