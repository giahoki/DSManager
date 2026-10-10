// The engine's own passes: G-buffer from depth, ambient occlusion, screen-space reflections, HDR bloom, final composite.
// Shaders are compiled at runtime from DSUEngine\shaders\dsu.hlsl, so they can be tuned live (F11).
#include "engine.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dsu {

using PFN_Compile = decltype(&D3DCompileFromFile);
static PFN_Compile g_compile = nullptr;

static ID3D11Device* g_dev = nullptr;
static std::mutex g_fxMutex;  // effectsRun vs. reload

struct Shaders {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader *prep = nullptr, *ao = nullptr, *aoBlur = nullptr, *ssr = nullptr, *bloomPre = nullptr, *bloomDown = nullptr,
                      *bloomUp = nullptr, *final = nullptr;
    void release()
    {
        for (IUnknown* p : std::initializer_list<IUnknown*>{vs, prep, ao, aoBlur, ssr, bloomPre, bloomDown, bloomUp, final})
            if (p) p->Release();
        *this = {};
    }
    bool ok() const { return vs && prep && ao && aoBlur && ssr && bloomPre && bloomDown && bloomUp && final; }
};
static Shaders g_sh;

struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    void release()
    {
        if (srv) srv->Release();
        if (rtv) rtv->Release();
        if (tex) tex->Release();
        *this = {};
    }
    bool create(UINT width, UINT height, DXGI_FORMAT fmt)
    {
        release();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = width;
        d.Height = height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &tex))) return false;
        if (FAILED(g_dev->CreateRenderTargetView(tex, nullptr, &rtv))) return false;
        if (FAILED(g_dev->CreateShaderResourceView(tex, nullptr, &srv))) return false;
        w = width;
        h = height;
        return true;
    }
};

static constexpr int kBloomLevels = 7;
static Target g_prep, g_ao, g_aoTmp, g_ssr, g_final;
static Target g_down[kBloomLevels], g_up[kBloomLevels];
static UINT g_w = 0, g_h = 0;

static ID3D11Buffer* g_cb = nullptr;
static ID3D11SamplerState *g_point = nullptr, *g_linear = nullptr;
static ID3D11BlendState* g_noBlend = nullptr;
static ID3D11DepthStencilState* g_noDepth = nullptr;
static ID3D11RasterizerState* g_raster = nullptr;

struct CB {
    float screen[4];
    float depthParams[4];
    float projUV[4];
    float ssr[4];
    float ssr2[4];
    float bloom[4];
    float src[4];
    float worldUp[4];  // world up in view space, w = view distance treated as sky
    float ao[4];       // intensity (power), radius (m), slices, steps per side
    float ao2[4];      // light protect * exposure, max radius (px), blur direction (0 x, 1 y), 0
};

static constexpr UINT kSlots = 5;  // texture slots our passes use (t0..t4)

// ---------------------------------------------------------------- shaders

static ID3DBlob* compile(const wchar_t* file, const char* entry, const char* target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = g_compile((engineDir() / L"shaders" / file).c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) {
        log("%s %s: %s", entry, FAILED(hr) ? "FAILED" : "warnings", (const char*)err->GetBufferPointer());
        err->Release();
    }
    return SUCCEEDED(hr) ? code : nullptr;
}

static bool loadShaders()
{
    Shaders s;
    if (ID3DBlob* b = compile(L"dsu.hlsl", "FullscreenVS", "vs_5_0")) {
        g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vs);
        b->Release();
    }
    auto ps = [&](const char* entry, ID3D11PixelShader** out) {
        if (ID3DBlob* b = compile(L"dsu.hlsl", entry, "ps_5_0")) {
            g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out);
            b->Release();
        }
    };
    ps("PrepPS", &s.prep);
    ps("AoPS", &s.ao);
    ps("AoBlurPS", &s.aoBlur);
    ps("SsrPS", &s.ssr);
    ps("BloomPrefilterPS", &s.bloomPre);
    ps("BloomDownPS", &s.bloomDown);
    ps("BloomUpPS", &s.bloomUp);
    ps("FinalPS", &s.final);
    if (!s.ok()) {
        log("shaders: compile failed, keeping the previous set");
        s.release();
        return false;
    }
    g_sh.release();
    g_sh = s;
    log("shaders: compiled");
    return true;
}

bool effectsInit(ID3D11Device* device)
{
    g_dev = device;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    g_compile = dc ? (PFN_Compile)GetProcAddress(dc, "D3DCompileFromFile") : nullptr;
    if (!g_compile) {
        log("d3dcompiler_47.dll not found");
        return false;
    }

    D3D11_BUFFER_DESC b = {};
    b.ByteWidth = sizeof(CB);
    b.Usage = D3D11_USAGE_DYNAMIC;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&b, nullptr, &g_cb);

    D3D11_SAMPLER_DESC s = {};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&s, &g_point);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device->CreateSamplerState(&s, &g_linear);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &g_noBlend);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    device->CreateDepthStencilState(&dd, &g_noDepth);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &g_raster);

    return g_cb && g_point && g_linear && g_noBlend && g_noDepth && g_raster && loadShaders();
}

void effectsReload()
{
    std::lock_guard lock(g_fxMutex);
    loadShaders();
}

// ---------------------------------------------------------------- state backup

struct StateBackup {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11InputLayout* il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT numVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencilRef = 0;
    ID3D11ShaderResourceView* srv[kSlots] = {};
    ID3D11SamplerState* smp[2] = {};
    ID3D11Buffer* cb = nullptr;

    void save(ID3D11DeviceContext* c)
    {
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
        c->RSGetState(&rs);
        c->RSGetViewports(&numVp, vp);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&dss, &stencilRef);
        c->PSGetShaderResources(0, kSlots, srv);
        c->PSGetSamplers(0, 2, smp);
        c->PSGetConstantBuffers(0, 1, &cb);
    }
    void restore(ID3D11DeviceContext* c)
    {
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        c->RSSetState(rs);
        c->RSSetViewports(numVp, vp);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(dss, stencilRef);
        c->PSSetShaderResources(0, kSlots, srv);
        c->PSSetSamplers(0, 2, smp);
        c->PSSetConstantBuffers(0, 1, &cb);
        for (IUnknown* p : std::initializer_list<IUnknown*>{vs, ps, gs, hs, ds, il, rs, dsv, blend, dss, cb})
            if (p) p->Release();
        for (auto* p : rtv) if (p) p->Release();
        for (auto* p : srv) if (p) p->Release();
        for (auto* p : smp) if (p) p->Release();
    }
};

// ---------------------------------------------------------------- run

static bool ensureTargets(UINT w, UINT h)
{
    if (w == g_w && h == g_h && g_final.srv) return true;
    bool ok = g_prep.create(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT) && g_ao.create(w, h, DXGI_FORMAT_R8_UNORM)
              && g_aoTmp.create(w, h, DXGI_FORMAT_R8_UNORM) && g_ssr.create(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT)
              && g_final.create(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    UINT bw = w, bh = h;
    for (int i = 0; i < kBloomLevels && ok; ++i) {
        bw = std::max(1u, bw / 2);
        bh = std::max(1u, bh / 2);
        ok = g_down[i].create(bw, bh, DXGI_FORMAT_R11G11B10_FLOAT) && g_up[i].create(bw, bh, DXGI_FORMAT_R11G11B10_FLOAT);
    }
    g_w = ok ? w : 0;
    g_h = ok ? h : 0;
    log("render targets %ux%u: %s", w, h, ok ? "ok" : "FAILED");
    return ok;
}

static void setCB(ID3D11DeviceContext* c, const CB& data)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &data, sizeof data);
        c->Unmap(g_cb, 0);
    }
}

static void pass(ID3D11DeviceContext* c, ID3D11PixelShader* ps, Target& out, std::initializer_list<ID3D11ShaderResourceView*> in)
{
    ID3D11ShaderResourceView* none[kSlots] = {};
    c->PSSetShaderResources(0, kSlots, none);  // nothing may be bound as input and output at once
    c->OMSetRenderTargets(1, &out.rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, float(out.w), float(out.h), 0, 1};
    c->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* srv[kSlots] = {};
    UINT i = 0;
    for (auto* s : in)
        if (i < kSlots) srv[i++] = s;
    c->PSSetShaderResources(0, kSlots, srv);
    c->PSSetShader(ps, nullptr, 0);
    c->Draw(3, 0);
}

ID3D11ShaderResourceView* effectsRun(ID3D11DeviceContext* c, ID3D11ShaderResourceView* scene, const FrameInputs& in)
{
    std::lock_guard lock(g_fxMutex);
    if (!g_sh.ok()) return nullptr;

    ID3D11Resource* res = nullptr;
    scene->GetResource(&res);
    D3D11_TEXTURE2D_DESC sd = {};
    ((ID3D11Texture2D*)res)->GetDesc(&sd);
    res->Release();
    if (sd.SampleDesc.Count > 1) return nullptr;  // MSAA scene: not supported
    if (!ensureTargets(sd.Width, sd.Height)) return nullptr;

    bool haveDepth = in.depth != nullptr;
    CB cb = {};
    cb.screen[0] = float(sd.Width);
    cb.screen[1] = float(sd.Height);
    cb.screen[2] = 1.0f / sd.Width;
    cb.screen[3] = 1.0f / sd.Height;
    if (in.cam.valid) {
        memcpy(cb.depthParams, in.cam.depthParams, 16);
        memcpy(cb.projUV, in.cam.projInfo, 16);
    } else {
        float n = g_cfg.fallbackNear, f = g_cfg.fallbackFar;
        float ty = tanf(g_cfg.fallbackFovY * 0.5f * 3.14159265f / 180.0f), tx = ty * sd.Width / sd.Height;
        float dp[4] = {0, f, -(f - n), n * f};
        float pj[4] = {2 * tx, -2 * ty, -tx, ty};
        memcpy(cb.depthParams, dp, 16);
        memcpy(cb.projUV, pj, 16);
    }
    static uint32_t frame = 0;
    ++frame;
    cb.ssr[0] = (g_cfg.ssr && haveDepth) ? g_cfg.ssrIntensity : 0.0f;
    cb.ssr[1] = g_cfg.ssrMaxDistance;
    cb.ssr[2] = float(std::clamp(g_cfg.ssrSteps, 4, 256));
    cb.ssr[3] = g_cfg.ssrThickness;
    cb.ssr2[0] = std::clamp(g_cfg.ssrWallAmount, 0.0f, 1.0f);
    cb.ssr2[1] = g_cfg.ssrFresnelF0;
    cb.ssr2[2] = float(frame % 64);
    cb.ssr2[3] = float(haveDepth ? g_cfg.debugView : (g_cfg.debugView == 4 ? 4 : 0));
    cb.bloom[0] = g_cfg.bloom ? g_cfg.bloomIntensity : 0.0f;
    cb.bloom[1] = g_cfg.bloomThreshold / std::max(in.cam.exposure, 1e-4f);  // threshold is on the exposed image
    cb.bloom[2] = g_cfg.bloomKnee;
    cb.bloom[3] = g_cfg.bloomRadius;
    // without the matrix: assume a level camera (screen up = world up)
    cb.worldUp[0] = in.cam.fromMatrix ? in.cam.worldUp[0] : 0.0f;
    cb.worldUp[1] = in.cam.fromMatrix ? in.cam.worldUp[1] : 1.0f;
    cb.worldUp[2] = in.cam.fromMatrix ? in.cam.worldUp[2] : 0.0f;
    cb.worldUp[3] = in.cam.farZ > 0 ? in.cam.farZ * 0.95f : g_cfg.fallbackFar * 0.95f;
    bool doAo = g_cfg.ao && haveDepth;
    cb.ao[0] = std::max(g_cfg.aoIntensity, 0.0f);
    cb.ao[1] = std::max(g_cfg.aoRadius, 0.05f);
    cb.ao[2] = float(std::clamp(g_cfg.aoSlices, 1, 8));
    cb.ao[3] = float(std::clamp(g_cfg.aoSteps, 1, 16));
    cb.ao2[0] = std::max(g_cfg.aoLightProtect, 0.0f) * in.cam.exposure;
    cb.ao2[1] = float(sd.Height) * 0.12f;

    StateBackup st;
    st.save(c);

    c->VSSetShader(g_sh.vs, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->RSSetState(g_raster);
    c->OMSetBlendState(g_noBlend, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(g_noDepth, 0);
    ID3D11SamplerState* smp[2] = {g_point, g_linear};
    c->PSSetSamplers(0, 2, smp);
    c->PSSetConstantBuffers(0, 1, &g_cb);

    auto src = [&](const Target& t) {
        cb.src[0] = 1.0f / t.w;
        cb.src[1] = 1.0f / t.h;
        cb.src[2] = float(t.w);
        cb.src[3] = float(t.h);
        setCB(c, cb);
    };

    // 1-2: G-buffer, ambient occlusion and reflections (only with the game's depth)
    src(g_prep);
    float zero[4] = {}, one[4] = {1, 1, 1, 1};
    if (haveDepth) {
        pass(c, g_sh.prep, g_prep, {in.depth});
        if (doAo) {
            pass(c, g_sh.ao, g_ao, {g_prep.srv});
            cb.ao2[2] = 0;
            setCB(c, cb);
            pass(c, g_sh.aoBlur, g_aoTmp, {g_ao.srv, g_prep.srv});
            cb.ao2[2] = 1;
            setCB(c, cb);
            pass(c, g_sh.aoBlur, g_ao, {g_aoTmp.srv, g_prep.srv});
        }
        pass(c, g_sh.ssr, g_ssr, {scene, g_prep.srv});
    } else {
        c->ClearRenderTargetView(g_prep.rtv, zero);
        c->ClearRenderTargetView(g_ssr.rtv, zero);
    }
    if (!doAo) c->ClearRenderTargetView(g_ao.rtv, one);

    // 3: bloom, down then up
    src(g_final);  // texel of the full-res scene for the first 13-tap
    pass(c, g_sh.bloomPre, g_down[0], {scene});
    for (int i = 1; i < kBloomLevels; ++i) {
        src(g_down[i - 1]);
        pass(c, g_sh.bloomDown, g_down[i], {g_down[i - 1].srv});
    }
    // the smallest level of the up chain is the down level itself
    src(g_down[kBloomLevels - 1]);
    pass(c, g_sh.bloomDown, g_up[kBloomLevels - 1], {g_down[kBloomLevels - 2].srv});
    for (int i = kBloomLevels - 2; i >= 0; --i) {
        src(g_up[i + 1]);
        pass(c, g_sh.bloomUp, g_up[i], {g_down[i].srv, g_up[i + 1].srv});
    }

    // 4: final into our own HDR target; the game's tonemap reads it instead of the scene
    src(g_final);
    pass(c, g_sh.final, g_final, {scene, g_prep.srv, g_ssr.srv, g_up[0].srv, g_ao.srv});

    ID3D11ShaderResourceView* none[kSlots] = {};
    c->PSSetShaderResources(0, kSlots, none);
    st.restore(c);
    return g_final.srv;
}

}  // namespace dsu
