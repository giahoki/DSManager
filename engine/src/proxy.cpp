// d3d11.dll proxy: loads the system d3d11.dll, forwards every export through stubs.asm, and
// intercepts device creation so the engine can hook the game's device and contexts.
#include "common.h"
#include "engine.h"
#include "exports.h"

extern "C" void* g_real[std::size(kForwardedExports)] = {};

static HMODULE g_system = nullptr;
static PFN_D3D11_CREATE_DEVICE g_createDevice = nullptr;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_createDeviceAndSwapChain = nullptr;

static bool loadSystem()
{
    if (g_system) return true;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    g_system = LoadLibraryW((std::wstring(sys) + L"\\d3d11.dll").c_str());
    if (!g_system) {
        dsu::log("FATAL: cannot load system d3d11.dll (%lu)", GetLastError());
        return false;
    }
    for (size_t i = 0; i < std::size(kForwardedExports); ++i)
        g_real[i] = (void*)GetProcAddress(g_system, kForwardedExports[i]);
    g_createDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(g_system, "D3D11CreateDevice");
    g_createDeviceAndSwapChain = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(g_system, "D3D11CreateDeviceAndSwapChain");
    return true;
}

extern "C" HRESULT WINAPI DSU_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level,
    ID3D11DeviceContext** ctx)
{
    if (!loadSystem()) return E_FAIL;
    HRESULT hr = g_createDevice(adapter, type, sw, flags, levels, numLevels, sdk, device, level, ctx);
    dsu::log("D3D11CreateDevice -> 0x%08x", hr);
    if (SUCCEEDED(hr) && device && *device) dsu::onDevice(*device, nullptr);
    return hr;
}

extern "C" HRESULT WINAPI DSU_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdk, const DXGI_SWAP_CHAIN_DESC* scDesc,
    IDXGISwapChain** swapChain, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx)
{
    if (!loadSystem()) return E_FAIL;
    HRESULT hr = g_createDeviceAndSwapChain(adapter, type, sw, flags, levels, numLevels, sdk, scDesc, swapChain, device, level, ctx);
    dsu::log("D3D11CreateDeviceAndSwapChain -> 0x%08x", hr);
    if (SUCCEEDED(hr) && device && *device) dsu::onDevice(*device, swapChain ? *swapChain : nullptr);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        // Only DarkSoulsRemastered.exe gets the engine; anything else just gets plain d3d11.
        loadSystem();
        dsu::log("DSU Engine loaded into %ls", dsu::gameDir().c_str());
    }
    return TRUE;
}
