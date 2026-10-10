// Hooks on the game's D3D11 device, contexts and swap chain.
//
// The game records most of its frame on deferred contexts from worker threads and recreates them on
// every area load, so every hook works on whatever context it is called with, and our own passes are
// recorded into that same context right before the game's tonemap draw. Pass recognition uses the DXBC
// checksum of the game's shaders (game_shaders.h): nothing depends on addresses inside the exe.
#include "engine.h"
#include "game_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dsu {

// ---------------------------------------------------------------- hook classes
//
// d3d11 gives every deferred context its own heap copy of the vtable (and ReShade wraps them in yet
// another class), but the copies hold the same functions. A "class" is one set of original functions;
// each class gets its own instantiation of the hook templates, so a hook knows its original without
// any lookup, and any number of contexts can be hooked.

constexpr int kClasses = 4;
struct HookClass {
    bool used = false;
    void* orig[160] = {};
};
struct HookEntry {
    int idx;
    void* fn[kClasses];
};

static std::mutex g_hookMutex;

static void install(void* obj, HookClass* classes, std::initializer_list<HookEntry> entries, const char* what)
{
    std::lock_guard lock(g_hookMutex);
    void** vt = *(void***)obj;
    for (auto& e : entries)
        for (void* f : e.fn)
            if (vt[e.idx] == f) return;  // this vtable is already ours

    int k = 0;
    for (; k < kClasses; ++k) {
        if (!classes[k].used) break;
        bool same = true;
        for (auto& e : entries) same &= classes[k].orig[e.idx] == vt[e.idx];
        if (same) break;
    }
    if (k == kClasses) {
        log("ERROR: too many distinct %s classes, not hooked", what);
        return;
    }
    bool fresh = !classes[k].used;
    if (fresh) {
        for (auto& e : entries) classes[k].orig[e.idx] = vt[e.idx];
        classes[k].used = true;
    }
    for (auto& e : entries) {
        DWORD old;
        VirtualProtect(&vt[e.idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vt[e.idx] = e.fn[k];
        VirtualProtect(&vt[e.idx], sizeof(void*), old, &old);
    }
    if (fresh) log("hooked %s: new class %d (vtable %p)", what, k, (void*)vt);
}

#define DSU_HOOK(idx, tmpl) HookEntry{idx, {(void*)&tmpl<0>, (void*)&tmpl<1>, (void*)&tmpl<2>, (void*)&tmpl<3>}}

static HookClass g_ctxClass[kClasses], g_devClass[kClasses], g_scClass[kClasses], g_facClass[kClasses];

// ---------------------------------------------------------------- shader roles

// Open-addressing table: shader pointer -> (role, index into kGameShaders + 1). Every created shader
// is written (also unknown ones), so a reused address always carries the current shader's role.
struct ShaderInfo { Role role = Role::Other; int16_t name = 0; };
static constexpr size_t kShaderSlots = 1 << 17;
static std::atomic<void*> g_shaderKey[kShaderSlots];
static std::atomic<uint32_t> g_shaderVal[kShaderSlots];

static size_t slotOf(void* p, size_t mask) { return size_t((uint64_t(size_t(p) >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & mask; }

static void setShaderInfo(void* p, ShaderInfo info)
{
    uint32_t v = uint32_t(info.role) | (uint32_t(uint16_t(info.name)) << 8);
    for (size_t i = slotOf(p, kShaderSlots - 1), n = 0; n < kShaderSlots; i = (i + 1) & (kShaderSlots - 1), ++n) {
        void* k = g_shaderKey[i].load();
        if (k == p) { g_shaderVal[i] = v; return; }
        if (!k) {
            void* expected = nullptr;
            if (g_shaderKey[i].compare_exchange_strong(expected, p) || expected == p) { g_shaderVal[i] = v; return; }
        }
    }
}

static ShaderInfo shaderInfo(void* p)
{
    if (!p) return {};
    for (size_t i = slotOf(p, kShaderSlots - 1), n = 0; n < 256; i = (i + 1) & (kShaderSlots - 1), ++n) {
        void* k = g_shaderKey[i].load(std::memory_order_relaxed);
        if (k == p) { uint32_t v = g_shaderVal[i].load(std::memory_order_relaxed); return {Role(v & 0xff), int16_t(v >> 8)}; }
        if (!k) break;
    }
    return {};
}

static bool startsWith(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

static ShaderInfo identify(const void* bytecode, SIZE_T len)
{
    if (len < 20 || std::memcmp(bytecode, "DXBC", 4) != 0) return {};
    const uint32_t* sum = (const uint32_t*)((const char*)bytecode + 4);
    for (size_t i = 0; i < std::size(kGameShaders); ++i) {
        auto& g = kGameShaders[i];
        if (std::memcmp(g.sum, sum, 16) != 0) continue;
        const char* n = g.name;
        Role r = Role::Other;
        if (!std::strcmp(n, "FRPG_Fil_HDR_PBL.fpo") || !std::strcmp(n, "FRPG_Fil_HDR_PBL_ColAdj.fpo") || !std::strcmp(n, "FRPG_Fil_HDR.fpo")
            || !std::strcmp(n, "FRPG_Fil_HDR_ColAdj.fpo"))
            r = Role::Tonemap;
        else if (!std::strcmp(n, "FRPG_Fil_SAO_Depth.fpo") || !std::strcmp(n, "FRPG_Compute_SAO_Depth.cpo")
                 || startsWith(n, "FRPG_Compute_SSAO_PrepareDepthBuffers1"))
            r = Role::SaoDepth;
        else if (!std::strcmp(n, "FRPG_Fil_SAO_Main.fpo") || !std::strcmp(n, "FRPG_Compute_SAO_Main.cpo"))
            r = Role::SaoMain;
        else if (startsWith(n, "FRPG_Menu_"))
            r = Role::Menu;
        else if (startsWith(n, "FRPG_Phn_") || startsWith(n, "FRPG_Inst_") || startsWith(n, "FRPG_Wind_") || startsWith(n, "FRPG_Ivy_"))
            r = Role::SceneVS;  // cb0 rows 0-3 = world -> clip, row 4 = camera position
        return {r, int16_t(i + 1)};
    }
    return {};
}

static const char* shaderName(ShaderInfo s) { return s.name > 0 ? kGameShaders[s.name - 1].name : "?"; }

// ---------------------------------------------------------------- per-context state

// A context is used by one thread at a time, so its state needs no lock. Contexts are found through an
// open-addressing table; a recreated context at a reused address is reset in hookContext.
struct CtxState {
    ShaderInfo ps, cs, vs;
};
static constexpr size_t kCtxSlots = 1024;
static std::atomic<void*> g_ctxKey[kCtxSlots];
static CtxState g_ctxVal[kCtxSlots];
static CtxState g_ctxOverflow;

static CtxState& ctxState(void* ctx)
{
    for (size_t i = slotOf(ctx, kCtxSlots - 1), n = 0; n < kCtxSlots; i = (i + 1) & (kCtxSlots - 1), ++n) {
        void* k = g_ctxKey[i].load(std::memory_order_relaxed);
        if (k == ctx) return g_ctxVal[i];
        if (!k) {
            void* expected = nullptr;
            if (g_ctxKey[i].compare_exchange_strong(expected, ctx) || expected == ctx) return g_ctxVal[i];
        }
    }
    return g_ctxOverflow;
}

// ---------------------------------------------------------------- frame state

static ID3D11Device* g_device = nullptr;
static thread_local bool t_inside = false;  // true while our own passes call the context
static std::atomic<uint64_t> g_frame{0};
static std::atomic<uint64_t> g_effectsFrame{~0ull};
static std::atomic<uint64_t> g_camFrame{~0ull};
static std::atomic<bool> g_effectsReady{false};
static std::atomic<uint32_t> g_sceneW{0}, g_sceneH{0};

static std::mutex g_inMutex;
static FrameInputs g_inputs;  // depth SRV + camera, guarded by g_inMutex

// ---------------------------------------------------------------- constant buffer readback
//
// Constant buffers of the game's passes are copied into staging buffers (a small ring per kind) and read
// one or two frames later on the presenting thread: works however the game fills them.

enum class CbKind : uint8_t { PsSaoDepth, PsSaoMain, CsSaoDepth, CsSaoMain, CsSsaoDepth, Tonemap, ViewClip, Count };
struct CbSlot {
    ID3D11Buffer* staging = nullptr;
    UINT size = 0;
    uint64_t frame = 0;
    bool pending = false;
};
static constexpr int kRing = 3;
static std::mutex g_cbMutex;
static CbSlot g_cbRing[int(CbKind::Count)][kRing];

static void queueCbCopy(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, CbKind kind)
{
    if (!cb) return;
    D3D11_BUFFER_DESC d;
    cb->GetDesc(&d);
    CbSlot* slot = nullptr;
    {
        std::lock_guard lock(g_cbMutex);
        for (auto& s : g_cbRing[int(kind)])
            if (!s.pending) { slot = &s; break; }
        if (!slot) return;  // all copies still in flight
        if (slot->size != d.ByteWidth) {
            if (slot->staging) slot->staging->Release();
            slot->staging = nullptr;
            slot->size = 0;
            D3D11_BUFFER_DESC s = {};
            s.ByteWidth = d.ByteWidth;
            s.Usage = D3D11_USAGE_STAGING;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(g_device->CreateBuffer(&s, nullptr, &slot->staging))) return;
            slot->size = d.ByteWidth;
        }
        slot->frame = g_frame;
        slot->pending = true;
    }
    t_inside = true;
    ctx->CopyResource(slot->staging, cb);
    t_inside = false;
}

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Camera from the scene's world -> clip matrix (registers = clip x, y, z, w) and camera position.
static void cameraFromViewClip(const float* f, Camera& c)
{
    float r[4][4];
    std::memcpy(r, f, sizeof r);
    float s = std::sqrt(dot3(r[3], r[3]));
    if (s < 1e-6f) return;
    for (auto& row : r)
        for (float& v : row) v /= s;
    const float* fwd = r[3];
    float p02 = dot3(r[0], fwd), p12 = dot3(r[1], fwd);
    float rx[3], ry[3];
    for (int i = 0; i < 3; ++i) {
        rx[i] = r[0][i] - p02 * fwd[i];
        ry[i] = r[1][i] - p12 * fwd[i];
    }
    float p00 = std::sqrt(dot3(rx, rx)), p11 = std::sqrt(dot3(ry, ry));
    if (p00 < 1e-6f || p11 < 1e-6f) return;
    float a = dot3(r[2], fwd);
    float b = r[2][3] - a * r[3][3];
    // viewZ = b / (d - a)  ->  w / (d * z + y) with w = b, z = 1, y = -a
    float dp[4] = {0, -a, 1, b};
    float pj[4] = {2 / p00, -2 / p11, (-1 - p02) / p00, (1 - p12) / p11};
    std::memcpy(c.depthParams, dp, 16);
    std::memcpy(c.projInfo, pj, 16);
    // world up (+Y) in view space (x = right, y = up, z = forward)
    c.worldUp[0] = rx[1] / p00;
    c.worldUp[1] = ry[1] / p11;
    c.worldUp[2] = fwd[1];
    c.farZ = std::fabs(1 - a) > 1e-6f ? b / (1 - a) : 0;
    if (!c.fromMatrix)
        log("camera from scene matrix: fovY %.1f deg, aspect %.3f, far %.0f, world up in view (%.2f %.2f %.2f)",
            2 * std::atan(1 / p11) * 57.29578f, p11 / p00, c.farZ, c.worldUp[0], c.worldUp[1], c.worldUp[2]);
    c.valid = c.fromMatrix = true;
}

// Called on the presenting thread with the immediate context.
static void pollCbReadbacks(ID3D11DeviceContext* imm)
{
    std::lock_guard lock(g_cbMutex);
    for (int k = 0; k < int(CbKind::Count); ++k) {
        // read finished copies oldest first, so the newest one wins
        CbSlot* ready[kRing];
        int n = 0;
        for (auto& s : g_cbRing[k])
            if (s.pending && g_frame >= s.frame + 1) ready[n++] = &s;
        std::sort(ready, ready + n, [](CbSlot* a, CbSlot* b) { return a->frame < b->frame; });
        for (int i = 0; i < n; ++i) {
            CbSlot& r = *ready[i];
            D3D11_MAPPED_SUBRESOURCE m;
            t_inside = true;
            HRESULT hr = imm->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            t_inside = false;
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            r.pending = false;
            if (FAILED(hr)) continue;
            const float* f = (const float*)m.pData;
            {
                std::lock_guard l2(g_inMutex);
                Camera& c = g_inputs.cam;
                switch (CbKind(k)) {
                case CbKind::ViewClip:
                    if (r.size >= 80) cameraFromViewClip(f, c);
                    break;
                case CbKind::PsSaoMain:
                    if (r.size >= 74 * 16 && !c.fromMatrix) {
                        // projInfo is per pixel of the AO buffer (size in row 12): convert to per uv
                        std::memcpy(c.depthParams, f + 8 * 4, 16);
                        const float* pi = f + 73 * 4;
                        const float* sz = f + 12 * 4;
                        float uv[4] = {pi[0] * sz[0], pi[1] * sz[1], pi[2], pi[3]};
                        std::memcpy(c.projInfo, uv, 16);
                        c.valid = true;
                    }
                    break;
                case CbKind::CsSaoMain:
                    if (r.size >= 48 && !c.fromMatrix) {
                        // cbuffer { uint2 screenSize; float projScale, radius; float4 clipInfo; float4 projInfo; }
                        std::memcpy(c.depthParams, f + 4, 16);
                        const uint32_t* sz = (const uint32_t*)f;
                        const float* pi = f + 8;
                        float uv[4] = {pi[0] * float(sz[0]), pi[1] * float(sz[1]), pi[2], pi[3]};
                        std::memcpy(c.projInfo, uv, 16);
                        c.valid = true;
                    }
                    break;
                case CbKind::Tonemap:
                    // Hable tonemap: exposure = key (row 54.x) / adapted luminance (row 56.z)
                    if (r.size >= 57 * 16 && f[54 * 4] > 0) c.exposure = f[54 * 4] / (f[56 * 4 + 2] + 0.0001f);
                    break;
                default:
                    break;
                }
            }
            t_inside = true;
            imm->Unmap(r.staging, 0);
            t_inside = false;
        }
    }
}

static void setDepth(ID3D11ShaderResourceView* srv)
{
    if (!srv) return;
    std::lock_guard lock(g_inMutex);
    if (g_inputs.depth == srv) return;
    if (g_inputs.depth) g_inputs.depth->Release();
    g_inputs.depth = srv;
    srv->AddRef();
    log("scene depth SRV: %p", (void*)srv);
}

// ---------------------------------------------------------------- render targets

// Size of the first bound render target (0 if none).
static void targetSize(ID3D11DeviceContext* ctx, UINT& w, UINT& h)
{
    w = h = 0;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* r;
    rtv->GetResource(&r);
    D3D11_TEXTURE2D_DESC d = {};
    ((ID3D11Texture2D*)r)->GetDesc(&d);
    w = d.Width;
    h = d.Height;
    r->Release();
    rtv->Release();
}

// ---------------------------------------------------------------- draw-time logic

// Everything the engine does around one of the game's draws. Returns an SRV that must be restored into
// PS slot 0 after the draw, if we replaced it.
static ID3D11ShaderResourceView* beforeDraw(ID3D11DeviceContext* ctx, CtxState& s)
{
    // camera: first scene-geometry draw of the frame into a full-size target
    if (s.vs.role == Role::SceneVS && g_camFrame.load(std::memory_order_relaxed) != g_frame.load(std::memory_order_relaxed)) {
        UINT w = 0, h = 0;
        targetSize(ctx, w, h);
        if (w && w == g_sceneW && h == g_sceneH) {
            uint64_t f = g_frame;
            if (g_camFrame.exchange(f) != f) {
                ID3D11Buffer* cb = nullptr;
                ctx->VSGetConstantBuffers(0, 1, &cb);
                queueCbCopy(ctx, cb, CbKind::ViewClip);
                if (cb) cb->Release();
            }
        }
    }

    switch (s.ps.role) {
    case Role::SaoDepth: {
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv);
        setDepth(srv);
        if (srv) srv->Release();
        ID3D11Buffer* cb = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &cb);
        queueCbCopy(ctx, cb, CbKind::PsSaoDepth);
        if (cb) cb->Release();
        break;
    }
    case Role::SaoMain: {
        ID3D11Buffer* cb = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &cb);
        queueCbCopy(ctx, cb, CbKind::PsSaoMain);
        if (cb) cb->Release();
        break;
    }
    case Role::Tonemap: {
        ID3D11ShaderResourceView* scene = nullptr;
        ctx->PSGetShaderResources(0, 1, &scene);
        if (!scene) break;
        {
            ID3D11Resource* res;
            scene->GetResource(&res);
            D3D11_TEXTURE2D_DESC d = {};
            ((ID3D11Texture2D*)res)->GetDesc(&d);
            res->Release();
            g_sceneW = d.Width;
            g_sceneH = d.Height;
        }
        uint64_t f = g_frame;
        if (!g_cfg.enabled || !g_effectsReady || g_effectsFrame.exchange(f) == f) {  // once per frame
            scene->Release();
            break;
        }
        ID3D11Buffer* cb = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &cb);
        queueCbCopy(ctx, cb, CbKind::Tonemap);
        if (cb) cb->Release();

        FrameInputs in;
        {
            std::lock_guard lock(g_inMutex);
            in = g_inputs;
            if (in.depth) in.depth->AddRef();
        }
        t_inside = true;
        ID3D11ShaderResourceView* out = effectsRun(ctx, scene, in);
        t_inside = false;
        if (in.depth) in.depth->Release();
        if (out) {
            ctx->PSSetShaderResources(0, 1, &out);
            return scene;  // keep the reference: released after the draw restores it
        }
        scene->Release();
        break;
    }
    default:
        break;
    }
    return nullptr;
}

static void afterDraw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* restore)
{
    if (!restore) return;
    ctx->PSSetShaderResources(0, 1, &restore);
    restore->Release();
}

static bool interesting(const CtxState& s)
{
    return s.ps.role != Role::Other || s.vs.role == Role::SceneVS;
}

// ---------------------------------------------------------------- context hooks

enum CtxIdx {
    kPSSetShader = 9, kVSSetShader = 11, kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21,
    kDispatch = 41, kCSSetShader = 69
};
using PFN_SetPS = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using PFN_SetVS = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
using PFN_SetCS = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_Dispatch = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);

template <int K> static void STDMETHODCALLTYPE hkPSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n)
{
    if (!t_inside) ctxState(c).ps = shaderInfo(ps);
    ((PFN_SetPS)g_ctxClass[K].orig[kPSSetShader])(c, ps, ci, n);
}

template <int K> static void STDMETHODCALLTYPE hkVSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n)
{
    if (!t_inside) ctxState(c).vs = shaderInfo(vs);
    ((PFN_SetVS)g_ctxClass[K].orig[kVSSetShader])(c, vs, ci, n);
}

template <int K> static void STDMETHODCALLTYPE hkCSSetShader(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci, UINT n)
{
    if (!t_inside) ctxState(c).cs = shaderInfo(cs);
    ((PFN_SetCS)g_ctxClass[K].orig[kCSSetShader])(c, cs, ci, n);
}

template <int K> static void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* c, UINT count, UINT start)
{
    auto fn = (PFN_Draw)g_ctxClass[K].orig[kDraw];
    if (t_inside) return fn(c, count, start);
    auto& s = ctxState(c);
    if (!interesting(s)) return fn(c, count, start);
    ID3D11ShaderResourceView* restore = beforeDraw(c, s);
    fn(c, count, start);
    afterDraw(c, restore);
}

template <int K> static void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
{
    auto fn = (PFN_DrawIndexed)g_ctxClass[K].orig[kDrawIndexed];
    if (t_inside) return fn(c, count, start, base);
    auto& s = ctxState(c);
    if (!interesting(s)) return fn(c, count, start, base);
    ID3D11ShaderResourceView* restore = beforeDraw(c, s);
    fn(c, count, start, base);
    afterDraw(c, restore);
}

template <int K> static void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, INT base, UINT startInst)
{
    auto fn = (PFN_DrawIndexedInstanced)g_ctxClass[K].orig[kDrawIndexedInstanced];
    if (t_inside) return fn(c, count, inst, start, base, startInst);
    auto& s = ctxState(c);
    if (!interesting(s)) return fn(c, count, inst, start, base, startInst);
    ID3D11ShaderResourceView* restore = beforeDraw(c, s);
    fn(c, count, inst, start, base, startInst);
    afterDraw(c, restore);
}

template <int K> static void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, UINT startInst)
{
    auto fn = (PFN_DrawInstanced)g_ctxClass[K].orig[kDrawInstanced];
    if (t_inside) return fn(c, count, inst, start, startInst);
    auto& s = ctxState(c);
    if (!interesting(s)) return fn(c, count, inst, start, startInst);
    ID3D11ShaderResourceView* restore = beforeDraw(c, s);
    fn(c, count, inst, start, startInst);
    afterDraw(c, restore);
}

template <int K> static void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
{
    if (!t_inside) {
        auto& s = ctxState(c);
        if (s.cs.role == Role::SaoDepth || s.cs.role == Role::SaoMain) {
            const char* n = shaderName(s.cs);
            if (s.cs.role == Role::SaoDepth) {
                ID3D11ShaderResourceView* srv = nullptr;
                c->CSGetShaderResources(0, 1, &srv);
                setDepth(srv);
                if (srv) srv->Release();
            }
            ID3D11Buffer* cb = nullptr;
            c->CSGetConstantBuffers(0, 1, &cb);
            CbKind k = s.cs.role == Role::SaoMain ? CbKind::CsSaoMain : std::strstr(n, "SSAO") ? CbKind::CsSsaoDepth : CbKind::CsSaoDepth;
            queueCbCopy(c, cb, k);
            if (cb) cb->Release();
        }
    }
    ((PFN_Dispatch)g_ctxClass[K].orig[kDispatch])(c, x, y, z);
}

static void hookContext(ID3D11DeviceContext* ctx, const char* what)
{
    install(ctx, g_ctxClass,
            {DSU_HOOK(kPSSetShader, hkPSSetShader), DSU_HOOK(kVSSetShader, hkVSSetShader), DSU_HOOK(kDrawIndexed, hkDrawIndexed),
             DSU_HOOK(kDraw, hkDraw), DSU_HOOK(kDrawIndexedInstanced, hkDrawIndexedInstanced), DSU_HOOK(kDrawInstanced, hkDrawInstanced),
             DSU_HOOK(kDispatch, hkDispatch), DSU_HOOK(kCSSetShader, hkCSSetShader)},
            what);
    ctxState(ctx) = {};  // a new context may reuse the address of a destroyed one
}

// ---------------------------------------------------------------- device hooks

enum DevIdx { kCreateVertexShader = 12, kCreatePixelShader = 15, kCreateComputeShader = 18, kCreateDeferredContext = 27 };
using PFN_CreateVS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_CreatePS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PFN_CreateCS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
using PFN_CreateDeferred = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

static void registerShader(void* shader, const void* code, SIZE_T len, const char* kind)
{
    ShaderInfo i = identify(code, len);
    setShaderInfo(shader, i);
    if (i.role != Role::Other && i.role != Role::SceneVS) log("game %s %s -> role %d", kind, shaderName(i), int(i.role));
}

template <int K> static HRESULT STDMETHODCALLTYPE hkCreateVertexShader(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11VertexShader** out)
{
    HRESULT hr = ((PFN_CreateVS)g_devClass[K].orig[kCreateVertexShader])(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out) registerShader(*out, code, len, "vertex shader");
    return hr;
}

template <int K> static HRESULT STDMETHODCALLTYPE hkCreatePixelShader(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11PixelShader** out)
{
    HRESULT hr = ((PFN_CreatePS)g_devClass[K].orig[kCreatePixelShader])(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out) registerShader(*out, code, len, "pixel shader");
    return hr;
}

template <int K> static HRESULT STDMETHODCALLTYPE hkCreateComputeShader(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11ComputeShader** out)
{
    HRESULT hr = ((PFN_CreateCS)g_devClass[K].orig[kCreateComputeShader])(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out) registerShader(*out, code, len, "compute shader");
    return hr;
}

template <int K> static HRESULT STDMETHODCALLTYPE hkCreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out)
{
    HRESULT hr = ((PFN_CreateDeferred)g_devClass[K].orig[kCreateDeferredContext])(d, flags, out);
    if (SUCCEEDED(hr) && out && *out) hookContext(*out, "deferred context");
    return hr;
}

// ---------------------------------------------------------------- swap chain

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

static bool keyPressed(int vk)
{
    static bool down[256];
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = now && !down[vk & 0xff];
    down[vk & 0xff] = now;
    return edge;
}

static void onPresent()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) {
        if (keyPressed(g_cfg.keyToggle)) {
            g_cfg.enabled = !g_cfg.enabled;
            log("effects %s", g_cfg.enabled ? "ON" : "OFF");
        }
        if (keyPressed(g_cfg.keyReload)) {
            g_cfg.load();
            effectsReload();
        }
    }

    ID3D11DeviceContext* imm = nullptr;
    g_device->GetImmediateContext(&imm);
    pollCbReadbacks(imm);
    imm->Release();
    ++g_frame;
}

template <int K> static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    onPresent();
    return ((PFN_Present)g_scClass[K].orig[8])(sc, sync, flags);
}

static void hookSwapChain(IDXGISwapChain* sc) { install(sc, g_scClass, {DSU_HOOK(8, hkPresent)}, "swap chain"); }

template <int K> static HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    HRESULT hr = ((PFN_CreateSwapChain)g_facClass[K].orig[10])(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) {
        log("CreateSwapChain %ux%u", d ? d->BufferDesc.Width : 0, d ? d->BufferDesc.Height : 0);
        hookSwapChain(*out);
    }
    return hr;
}

template <int K> static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    HRESULT hr = ((PFN_CreateSwapChainForHwnd)g_facClass[K].orig[15])(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out && *out) {
        log("CreateSwapChainForHwnd %ux%u", d ? d->Width : 0, d ? d->Height : 0);
        hookSwapChain(*out);
    }
    return hr;
}

// ---------------------------------------------------------------- entry

void onDevice(ID3D11Device* device, IDXGISwapChain* swapChain)
{
    if (g_device) {  // a second device (e.g. a probe by some library): leave it alone
        log("additional device %p ignored", (void*)device);
        return;
    }
    g_device = device;
    g_cfg.load();

    install(device, g_devClass,
            {DSU_HOOK(kCreateVertexShader, hkCreateVertexShader), DSU_HOOK(kCreatePixelShader, hkCreatePixelShader),
             DSU_HOOK(kCreateComputeShader, hkCreateComputeShader), DSU_HOOK(kCreateDeferredContext, hkCreateDeferredContext)},
            "device");

    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    hookContext(imm, "immediate context");
    imm->Release();

    if (swapChain) hookSwapChain(swapChain);

    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory))) {
                IDXGIFactory2* f2 = nullptr;
                if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
                    install(f2, g_facClass, {DSU_HOOK(10, hkCreateSwapChain), DSU_HOOK(15, hkCreateSwapChainForHwnd)}, "dxgi factory");
                    f2->Release();
                } else {
                    install(factory, g_facClass, {DSU_HOOK(10, hkCreateSwapChain)}, "dxgi factory");
                }
                factory->Release();
            }
            adapter->Release();
        }
        dxgiDev->Release();
    }

    g_effectsReady = effectsInit(device);
    log("effects %s", g_effectsReady ? "ready" : "FAILED to initialise");
}

}  // namespace dsu
