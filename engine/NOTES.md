# DSU Engine: developer notes

DSU Engine adds screen-space reflections and HDR bloom to **Dark Souls Remastered**. It ships as the
"DS Upgrader Engine" mod in DSM and works with or without ReShade.

These notes explain how it works and record facts about the game's rendering that took reverse
engineering to find. Read them before changing the engine. Update them when you learn something new.

## How it works, in short

1. **We pretend to be `d3d11.dll`.** The game loads `d3d11.dll` from its own folder before the system one.
   Ours forwards every call to the real Windows `d3d11.dll` and keeps the game working as usual.
2. **We watch the game draw.** When the game creates its Direct3D device, we hook a few of its functions
   (create shader, set shader, draw, present). Now we see every shader the game binds.
3. **We recognise the game's passes.** Each compiled game shader has a checksum. A prebuilt table maps
   checksums to shader names, so we know when the game draws the tonemap, the AO pass, the menu, and so on.
   No exe addresses are used, so the engine survives most game patches.
4. **We collect what the effects need.** The depth buffer, the camera matrix and the exposure are read
   from the game's own shaders and constant buffers.
5. **We run our effects right before the tonemap.** Reflections and bloom are applied to the HDR scene,
   and the game's tonemap reads our result instead of the original image.

## Files

| Path | What it is |
|---|---|
| `src/proxy.cpp`, `src/stubs.asm`, `src/d3d11.def`, `src/exports.h` | The fake `d3d11.dll`: loads the real one and forwards all 51 exports to it. |
| `src/engine.cpp`, `src/engine.h` | Hooks, recognising passes, collecting depth and camera, calling the effects. |
| `src/effects.cpp` | Our render passes: normals from depth, reflections, bloom, final composite. |
| `src/common.cpp`, `src/common.h` | Paths, the log file, reading `DSUEngine.ini`. |
| `src/game_shaders.h` | Generated table "shader checksum → name". Don't edit by hand. |
| `shaders/dsu.hlsl` | Our shaders. Compiled at runtime, so they can be changed without rebuilding. |
| `DSUEngine.ini` | Settings the player can change. Every option is commented. |
| `tools/gen_shader_table.py` | Regenerates `src/game_shaders.h` from the game's files. |

## Build and run

- Open `DSM.slnx` and build `DSUEngine` in **Release x64**.
- A Release build copies `d3d11.dll`, `DSUEngine.ini` and `shaders/dsu.hlsl` straight into
  `mods/Dark Souls Remastered/DS Upgrader Engine/files/`, so DSM installs exactly what you built.
- Then enable the mod in DSM, apply, and start the game.

## Debugging in the game

- **Log:** `DSUEngine.log` in the game folder. It records which hooks were installed, which game passes
  were recognised, the camera that was found and whether the effects initialised.
- **F10** turns the effects on and off, for quick before/after comparisons.
- **F11** re-reads `DSUEngine.ini` and recompiles `dsu.hlsl`. Edit the shader in the game folder,
  press F11, see the result right away.
- **`DebugView`** in the ini shows intermediate images: `1` depth, `2` normals, `3` reflections only, `4` bloom only.

## After a game update

If the effects stop appearing and the log no longer says the tonemap was recognised, the game's shaders
have changed. Regenerate the table:

```bash
pip install soulstruct
python tools/gen_shader_table.py "<game folder>\shader" src/game_shaders.h
```

The table holds only checksums and names, never the game's shader code, so it's fine to commit.

## Game rendering facts

These are not documented anywhere; they were found by inspecting the game's shaders and frames.
In the tables, `cbN` means register `N` of the shader's constant buffer 0 (one register = 4 floats),
and `tN` means the texture in slot `N`.

### Frame layout (2560×1440, no ReShade)

- Scene colour: RT0 `R11G11B10_FLOAT` (HDR) + RT1 `B8G8R8A8`. Depth: `D24S8` at full resolution.
- Shadow map: 2048×2048 `D24S8`. There's also an early 1280×720 `R16_FLOAT` pass.

### Tonemap: `FRPG_Fil_HDR_PBL` (also `_ColAdj`, `FRPG_Fil_HDR`)

This is where HDR becomes the final image. Our effects are inserted right before it.

| Input | Meaning |
|---|---|
| `t0` | HDR scene. We replace it with our output. |
| `t1` | The game's own bloom, added as `cb56.x * t1`. |
| `t2` | Overlay, strength `cb68.x`. |
| `cb54` | `(key, W, E, F)`: Hable tonemap curve. |
| `cb55` | `(A, B, C, D)`: Hable tonemap curve. |
| `cb56.z` | Adapted (average) scene brightness. |

Exposure = `cb54.x / cb56.z`. Our bloom threshold works on exposed brightness, so it matches what the player sees.

### Camera: scene vertex shaders `FRPG_Phn`, `FRPG_Inst`, `FRPG_Wind`, `FRPG_Ivy`

All 580 scene vertex shaders share this layout:

| Register | Meaning |
|---|---|
| `cb0`–`cb3` | World → clip matrix. |
| `cb4` | Camera position in the world. |

Field of view, depth mapping, far plane and the world "up" direction are all derived from this matrix
(`cameraFromViewClip` in `engine.cpp`). This is the main camera source.

### Depth: the game's ambient occlusion pass

Used for depth, and for the camera when the matrix above hasn't been seen yet.

- **SAO (pixel shaders):** `FRPG_Fil_SAO_Depth` has the hardware depth in `t0`;
  linear depth = `cb8.w / (d * cb8.z + cb8.y)`. In `FRPG_Fil_SAO_Main`,
  view-space XY = `z * ((pixel + 0.5) * cb73.xy + cb73.zw)`, where pixel is at the AO resolution `cb12.xy`.
- **SSAO (compute, MiniEngine style):** `FRPG_Compute_SSAO_PrepareDepthBuffers1` has depth in `t0` and
  `{ZNear, ZFar}` in its constant buffer, but no projection.
  Linear depth = `2n / (n + f - d * (f - n))`.
- **SAO (compute):** `FRPG_Compute_SAO_*` constant buffer is
  `{uint2 screenSize; float projScale, radius; float4 clipInfo; float4 projInfo; ...}`.

If the player turns ambient occlusion off in the game settings, none of this is available and the engine
falls back to the `[Camera]` values in the ini until it sees the scene matrix.

### Reading constant buffers

The GPU owns the game's constant buffers, so we can't read them directly. We copy them into a staging
buffer (`CopyResource`) and read that copy 3 frames later, on the thread that calls `Present`.
Reading it sooner would stall the GPU.

## Hooking details

- **What is hooked.** The device (`CreatePixelShader`, `CreateComputeShader`, `CreateDeferredContext`),
  device contexts (`PSSetShader`, `DrawIndexed`, `Draw`, `Dispatch`, `CSSetShader`),
  the DXGI factory (`CreateSwapChain`, `CreateSwapChainForHwnd`) and the swap chain (`Present`).
  Hooks replace entries in the object's virtual function table.
- **The game draws on deferred contexts.** It records command lists on separate contexts, so our passes
  are also recorded there, right before the tonemap draw on the same context.
- **Without ReShade**, every deferred context has its own function table, and the game recreates its
  contexts on every area load. A fixed table of 12 slots ran out and post-processing stopped working.
  The fix groups contexts by their original functions (at most 4 groups), so any number of contexts can be hooked.
- **With ReShade** (`dxgi.dll`), the hooks land on ReShade's wrapper objects, which share one table for
  all contexts. Both cases work.

## Our effects (`shaders/dsu.hlsl`)

In the order they run:

1. **Normals from depth.** The game has no normal buffer we can use, so normals are rebuilt from depth.
2. **Reflections (SSR).** A ray is marched in view space through the depth buffer, then refined with a
   binary search. By default only floors, puddles and steps reflect (`WallAmount=0`).
3. **Bloom.** 7 levels down with a 13-tap filter (Karis average against fireflies), then back up with a tent filter.
4. **Final.** Reflections are smoothed with a depth-aware 3×3 filter, Fresnel and the floor boost are
   applied, bloom is added. The result goes to the tonemap instead of the original scene.
