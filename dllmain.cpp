// WWE 2K19 "cinematic 30 FPS look":
// game keeps simulating at 60 (normal speed), but only every 2nd frame is shown.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include "MinHook.h"

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn oPresent = nullptr;
static unsigned long long g_frame = 0;
static LARGE_INTEGER g_freq, g_next;

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    // Pace the game to 60 FPS with a hybrid sleep/spin wait
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    while (now.QuadPart < g_next.QuadPart) {
        long long remainMs = (g_next.QuadPart - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (remainMs > 2) Sleep(1);
        QueryPerformanceCounter(&now);
    }
    long long base = g_next.QuadPart > now.QuadPart ? g_next.QuadPart : now.QuadPart;
    g_next.QuadPart = base + g_freq.QuadPart / 60;

    // Show every 2nd frame only
    if ((g_frame++ & 1) == 0) return S_OK;
    return oPresent(sc, 0, flags);
}

static DWORD WINAPI Init(LPVOID) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_next);
    timeBeginPeriod(1);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "wwe30dummy";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("wwe30dummy", "", 0, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 1;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;

    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx))) return 1;

    void** vtbl = *(void***)sc;
    MH_Initialize();
    MH_CreateHook(vtbl[8], (void*)hkPresent, (void**)&oPresent); // 8 = Present
    MH_EnableHook(MH_ALL_HOOKS);

    sc->Release(); dev->Release(); ctx->Release();
    DestroyWindow(hwnd);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
