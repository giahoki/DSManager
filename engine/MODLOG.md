# MODLOG: DSU Engine (own render passes for DARK SOULS REMASTERED)

## Route (2026-10-10)
Native render mod = `d3d11.dll` proxy in the game folder (the game imports d3d11.dll and dinput8.dll; ReShade sits in dxgi.dll).
- The proxy forwards all 51 d3d11 exports (stubs.asm) and intercepts D3D11CreateDevice(AndSwapChain).
- vtable hooks: device (CreatePixelShader 15, CreateComputeShader 18, CreateDeferredContext 27),
  context (PSSetShader 9, DrawIndexed 12, Draw 13, Dispatch 41, CSSetShader 69), factory (CreateSwapChain 10,
  CreateSwapChainForHwnd 15), swap chain (Present 8).
- With ReShade the hooks land on ReShade's wrapper classes (one vtable shared by immediate and deferred contexts). Without it, d3d11 has separate vtables (the immediate one is on the heap).
- Passes are recognised by the DXBC MD5 checksum of the game's shaders (src/game_shaders.h, generated from shader\*.shaderbnd.dcx by tools/gen_shader_table.py). No exe addresses are used.

## Frame facts
- Tonemap = FRPG_Fil_HDR_PBL(.fpo) [/_ColAdj, FRPG_Fil_HDR]: t0 HDR scene, t1 game bloom (added as cb56.x * t1), t2 overlay (cb68.x).
  Hable curve: cb54 = (key, W, E, F), cb55 = (A, B, C, D), cb56.z = adapted luminance. Exposure = cb54.x / cb56.z.
- Depth + camera: PS FRPG_Fil_SAO_Depth: t0 = hardware depth, linear = cb8.w / (d*cb8.z + cb8.y).
  PS FRPG_Fil_SAO_Main: viewXY = z * ((pixel+0.5) * cb73.xy + cb73.zw), pixels at AO size cb12.xy.
  CS FRPG_Compute_SAO_*: cbuffer {uint2 screenSize; float projScale, radius; float4 clipInfo; float4 projInfo;...}.
  CS FRPG_Compute_SSAO_PrepareDepthBuffers1: {ZNear, ZFar}, t0 = depth (no projInfo: fallback FOV).
- Constant buffers are read via CopyResource to staging, then mapped 3 frames later on the Present thread.

## Effects (shaders/dsu.hlsl, compiled at runtime, F11 reload)
Inserted right before the tonemap draw on the same (often deferred) context; the tonemap's t0 is swapped to our output.
Prep (normals from depth) -> SSR (view-space march, binary refine) -> bloom (13-tap Karis prefilter, 7 levels, tent up)
-> final (depth-aware 3x3 SSR filter, Fresnel and floor boost, bloom add).

## Verification
- test/harness.cpp records a frame like the game (deferred context: SAO depth, SAO main, tonemap) with the game's
  own shader bytecode read from local extracted files, through the built proxy, and writes the tonemapped frame.
  Checked: hooks, roles, depth capture, camera readback, SSR geometry, bloom, debug views, and the same with ReShade's dxgi.dll.
- Not yet checked in the real game.

## First in-game run (2026-10-10, 2560x1440, no ReShade)
- Real d3d11 gives every deferred context its own heap vtable, and the game recreates its contexts on every area load.
  A fixed table of 12 vtables ran out and post-processing contexts stayed unhooked. Fix: hook classes are grouped by
  their original functions (template hooks per class, <= 4 classes), so any number of contexts can be hooked.
- Scene: RT0 R11G11B10_FLOAT + RT1 B8G8R8A8, depth D24S8 at full res; shadow map 2048 D24S8; a 1280x720 R16_FLOAT pass first.
- AO in this setup is compute SSAO (MiniEngine style): PrepareDepthBuffers1 gives depth (t0) and {ZNear, ZFar}, but no projection.
  Linear depth there = 2n / (n + f - d (f - n)).
- Camera now comes from the scene vertex shaders (FRPG_Phn/Inst/Wind/Ivy, all 580 have it): cb0 regs 0-3 = world -> clip,
  reg 4 = camera position. FOV, depth mapping, far plane and world up are derived from it (cameraFromViewClip).
