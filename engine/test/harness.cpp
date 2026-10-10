// Offline test bench for DSU Engine: loads the built d3d11.dll proxy, records a frame the way the
// game does (deferred context: SAO depth, SAO main, tonemap) with the game's own shaders, and
// saves the tonemapped result. Not shipped.
//
//   harness.exe <proxy d3d11.dll> <folder with extracted .fpo files> <out.ppm>
//
// The game's shader bytecode is read from the user's own extracted files at runtime.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

static std::vector<char> readFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static const int W = 1280, H = 720;
static const float kNear = 0.1f, kFar = 2000.0f, kFov = 43.0f * 3.14159265f / 180.0f;

int main(int argc, char** argv)
{
    if (argc < 4) { printf("usage: harness <d3d11.dll> <shader dir> <out.ppm>\n"); return 1; }
    HMODULE proxy = LoadLibraryA(argv[1]);
    if (!proxy) { printf("cannot load proxy\n"); return 1; }
    auto createDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(proxy, "D3D11CreateDevice");

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* imm = nullptr;
    if (FAILED(createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &imm))) {
        printf("device failed\n");
        return 1;
    }

    // window + swap chain through the factory (exercises the factory hook like the game)
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "dsu_harness";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("dsu_harness", "dsu", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGIDevice* dd;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
    IDXGIAdapter* ad;
    dd->GetAdapter(&ad);
    IDXGIFactory* fac;
    ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc = {W, H, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc = nullptr;
    if (FAILED(fac->CreateSwapChain(dev, &sd, &sc))) { printf("swap chain failed\n"); return 1; }

    // game shaders
    std::string dir = argv[2];
    auto loadPS = [&](const char* name) {
        auto b = readFile(dir + "\\" + name);
        ID3D11PixelShader* ps = nullptr;
        dev->CreatePixelShader(b.data(), b.size(), nullptr, &ps);
        if (!ps) printf("cannot create %s\n", name);
        return ps;
    };
    ID3D11PixelShader* psSaoDepth = loadPS("FRPG_Fil_SAO_Depth.fpo");
    ID3D11PixelShader* psSaoMain = loadPS("FRPG_Fil_SAO_Main.fpo");
    ID3D11PixelShader* psTonemap = loadPS("FRPG_Fil_HDR_PBL.fpo");
    // a scene-geometry vertex shader of the game: its cb0 carries the world -> clip matrix
    ID3D11VertexShader* vsScene = nullptr;
    {
        auto b = readFile(dir + "\\FRPG_Phn_PINTT_DDL_Non.vpo");
        dev->CreateVertexShader(b.data(), b.size(), nullptr, &vsScene);
        if (!vsScene) printf("cannot create scene VS\n");
    }

    const char* vsSrc =
        "struct O { float4 p : SV_Position; float4 t : TEXCOORD1; };"
        "O main(uint id : SV_VertexID) { O o; float2 uv = float2((id << 1) & 2, id & 2);"
        "o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.t = float4(uv, uv); return o; }";
    ID3DBlob* vb = nullptr;
    D3DCompile(vsSrc, strlen(vsSrc), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, nullptr);
    ID3D11VertexShader* vs;
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);

    // procedural scene: floor, a row of pillars with lamps, bright sky and sun
    float ty = tanf(kFov / 2), tx = ty * W / H;
    std::vector<float> scene(W * H * 4), depth(W * H);
    const float eyeY = 1.6f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
            float dx = (u * 2 - 1) * tx, dy = (1 - v * 2) * ty;  // view ray (dx, dy, 1)
            float z = 1e9f;
            float r = 2.0f, g = 2.4f, b = 3.2f;  // sky
            if (dy < 0) {  // floor
                float zf = eyeY / -dy;
                if (zf < z) {
                    z = zf;
                    float wx = dx * z, wz = z;
                    bool tile = (int(floorf(wx)) + int(floorf(wz))) & 1;
                    r = g = b = tile ? 0.25f : 0.12f;
                }
            }
            for (int p = -3; p <= 3; ++p) {  // pillars at z = 8, x = p * 2.5, 0.6 wide, 4 high
                float px = p * 2.5f, zp = 8.0f;
                float wx = dx * zp, wy = eyeY + dy * zp;
                if (zp < z && fabsf(wx - px) < 0.3f && wy > 0 && wy < 4) {
                    z = zp;
                    r = 0.35f; g = 0.3f; b = 0.25f;
                    if (wy > 2.6f && wy < 3.0f && fabsf(wx - px) < 0.15f) { r = 40; g = 22; b = 8; }  // lamp
                }
            }
            if (z > 1e8f) {
                float sx = (u - 0.7f) * W / H, sy = v - 0.2f;
                if (sx * sx + sy * sy < 0.0015f) { r = 60; g = 55; b = 45; }
            }
            float* c = &scene[(y * W + x) * 4];
            c[0] = r; c[1] = g; c[2] = b; c[3] = 1;
            depth[y * W + x] = z > 1e8f ? 1.0f : (kFar - kNear * kFar / z) / (kFar - kNear);
        }

    auto makeTex = [&](DXGI_FORMAT fmt, const void* data, UINT pitch, UINT bind) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
        d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA s = {data, pitch, 0};
        ID3D11Texture2D* t = nullptr;
        dev->CreateTexture2D(&d, data ? &s : nullptr, &t);
        return t;
    };
    ID3D11Texture2D* sceneTex = makeTex(DXGI_FORMAT_R32G32B32A32_FLOAT, scene.data(), W * 16, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* depthTex = makeTex(DXGI_FORMAT_R32_FLOAT, depth.data(), W * 4, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* aoTex = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr, 0, D3D11_BIND_RENDER_TARGET);
    ID3D11Texture2D* ldrTex = makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, D3D11_BIND_RENDER_TARGET);
    std::vector<float> zero(W * H * 4, 0.0f), gray(W * H * 4, 0.5f);
    ID3D11Texture2D* bloomTex = makeTex(DXGI_FORMAT_R32G32B32A32_FLOAT, zero.data(), W * 16, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* overTex = makeTex(DXGI_FORMAT_R32G32B32A32_FLOAT, gray.data(), W * 16, D3D11_BIND_SHADER_RESOURCE);
    ID3D11ShaderResourceView *sceneSrv, *depthSrv, *bloomSrv, *overSrv;
    dev->CreateShaderResourceView(sceneTex, nullptr, &sceneSrv);
    dev->CreateShaderResourceView(depthTex, nullptr, &depthSrv);
    dev->CreateShaderResourceView(bloomTex, nullptr, &bloomSrv);
    dev->CreateShaderResourceView(overTex, nullptr, &overSrv);
    ID3D11RenderTargetView *aoRtv, *ldrRtv;
    dev->CreateRenderTargetView(aoTex, nullptr, &aoRtv);
    dev->CreateRenderTargetView(ldrTex, nullptr, &ldrRtv);

    // the game's $Globals: depth params (row 8), AO size (row 12), Hable tonemap (54-56), projInfo (73)
    std::vector<float> g(80 * 4, 0.0f);
    auto row = [&](int r, float a, float b, float c, float d) { g[r * 4] = a; g[r * 4 + 1] = b; g[r * 4 + 2] = c; g[r * 4 + 3] = d; };
    row(8, 0, kFar, -(kFar - kNear), kNear * kFar);
    row(12, float(W), float(H), 0, 0);
    row(54, 0.18f, 11.2f, 0.02f, 0.30f);
    row(55, 0.15f, 0.50f, 0.10f, 0.20f);
    row(56, 1.0f, 0, 0.18f, 0);
    row(68, 0, 0, 0, 0);
    row(73, 2 * tx / W, -2 * ty / H, -tx, ty);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(g.size() * 4);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA gs = {g.data(), 0, 0};
    ID3D11Buffer* globals;
    dev->CreateBuffer(&bd, &gs, &globals);

    // world -> clip for the scene VS: camera at (0, eyeY, 0) looking down +Z, D3D depth
    float A = kFar / (kFar - kNear), B = -kNear * kFar / (kFar - kNear);
    float viewClipRows[8 * 4] = {1 / tx, 0, 0, 0,   0, 1 / ty, 0, -eyeY / ty,   0, 0, A, B,   0, 0, 1, 0,   0, eyeY, 0, 1};
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = sizeof viewClipRows;
    vbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = {viewClipRows, 0, 0};
    ID3D11Buffer* viewClip;
    dev->CreateBuffer(&vbd, &vsd, &viewClip);
    ID3D11Texture2D* hdrTex = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr, 0, D3D11_BIND_RENDER_TARGET);
    ID3D11RenderTargetView* hdrRtv;
    dev->CreateRenderTargetView(hdrTex, nullptr, &hdrRtv);

    D3D11_SAMPLER_DESC smd = {};
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* smp;
    dev->CreateSamplerState(&smd, &smp);
    ID3D11SamplerState* smps[3] = {smp, smp, smp};

    ID3D11DeviceContext* def;
    dev->CreateDeferredContext(0, &def);
    D3D11_VIEWPORT vp = {0, 0, float(W), float(H), 0, 1};

    for (int frame = 0; frame < 10; ++frame) {
        def->VSSetShader(vs, nullptr, 0);
        def->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        def->RSSetViewports(1, &vp);
        def->PSSetConstantBuffers(0, 1, &globals);
        def->PSSetSamplers(0, 3, smps);

        // scene geometry (nothing is actually drawn: the engine only needs to see the draw)
        def->OMSetRenderTargets(1, &hdrRtv, nullptr);
        def->VSSetShader(vsScene, nullptr, 0);
        def->VSSetConstantBuffers(0, 1, &viewClip);
        def->DrawIndexed(0, 0, 0);
        def->VSSetShader(vs, nullptr, 0);

        def->OMSetRenderTargets(1, &aoRtv, nullptr);
        def->PSSetShaderResources(0, 1, &depthSrv);
        def->PSSetShader(psSaoDepth, nullptr, 0);
        def->Draw(3, 0);
        def->PSSetShader(psSaoMain, nullptr, 0);
        def->Draw(3, 0);

        ID3D11ShaderResourceView* none[3] = {};
        def->PSSetShaderResources(0, 3, none);
        def->OMSetRenderTargets(1, &ldrRtv, nullptr);
        ID3D11ShaderResourceView* in[3] = {sceneSrv, bloomSrv, overSrv};
        def->PSSetShaderResources(0, 3, in);
        def->PSSetShader(psTonemap, nullptr, 0);
        def->Draw(3, 0);

        ID3D11CommandList* cl;
        def->FinishCommandList(FALSE, &cl);
        imm->ExecuteCommandList(cl, FALSE);
        cl->Release();
        sc->Present(0, 0);
        Sleep(20);
    }

    // read back the tonemapped frame
    D3D11_TEXTURE2D_DESC rd = {};
    ldrTex->GetDesc(&rd);
    rd.Usage = D3D11_USAGE_STAGING;
    rd.BindFlags = 0;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st;
    dev->CreateTexture2D(&rd, nullptr, &st);
    imm->CopyResource(st, ldrTex);
    D3D11_MAPPED_SUBRESOURCE m;
    imm->Map(st, 0, D3D11_MAP_READ, 0, &m);
    FILE* f = fopen(argv[3], "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y) {
        const unsigned char* p = (const unsigned char*)m.pData + y * m.RowPitch;
        for (int x = 0; x < W; ++x) fwrite(p + x * 4, 1, 3, f);
    }
    fclose(f);
    imm->Unmap(st, 0);
    printf("wrote %s\n", argv[3]);
    return 0;
}
