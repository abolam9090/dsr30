// dsr30.cpp - EXPERIMENTAL 30 FPS cap + 2x time scale for Dark Souls Remastered.
// dinput8.dll proxy. No third-party libraries. OFFLINE USE ONLY.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string.h>

static const double TIME_SCALE = 2.0;
static const double TARGET_FPS = 30.0;

// ---- dinput8 proxy ----
typedef HRESULT(WINAPI* DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE h, DWORD v, REFIID r, LPVOID* out, LPUNKNOWN u) {
    static DI8Create_t real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\dinput8.dll");
        real = (DI8Create_t)GetProcAddress(LoadLibraryA(path), "DirectInput8Create");
    }
    return real(h, v, r, out, u);
}

// ---- IAT hook: patch every import of `fn` in the main exe ----
static bool HookIAT(const char* fn, void* hook, void** orig) {
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool done = false;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        auto thunk = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        auto names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, fn) != 0) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            if (!*orig) *orig = (void*)thunk->u1.Function;
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            done = true;
        }
    }
    return done;
}

// ---- scaled clocks ----
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
typedef ULONGLONG(WINAPI* GTC64_t)();
typedef DWORD(WINAPI* TGT_t)();
static QPC_t oQPC; static GTC64_t oGTC64; static TGT_t oTGT;
static LONGLONG baseQpc = 0; static ULONGLONG baseTick = 0; static DWORD baseTgt = 0;

static BOOL WINAPI hQPC(LARGE_INTEGER* p) {
    BOOL r = oQPC(p);
    if (r) {
        if (!baseQpc) baseQpc = p->QuadPart;
        p->QuadPart = baseQpc + (LONGLONG)((p->QuadPart - baseQpc) * TIME_SCALE);
    }
    return r;
}
static ULONGLONG WINAPI hGTC64() {
    ULONGLONG t = oGTC64();
    if (!baseTick) baseTick = t;
    return baseTick + (ULONGLONG)((t - baseTick) * TIME_SCALE);
}
static DWORD WINAPI hTGT() {
    DWORD t = oTGT();
    if (!baseTgt) baseTgt = t;
    return baseTgt + (DWORD)((t - baseTgt) * TIME_SCALE);
}

// ---- 30 FPS limiter on IDXGISwapChain::Present (real time) ----
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
static Present_t oPresent;
static LONGLONG nextFrame = 0;

static HRESULT STDMETHODCALLTYPE hPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    HRESULT hr = oPresent(sc, sync, flags);
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    LONGLONG step = (LONGLONG)(f.QuadPart / TARGET_FPS);
    QueryPerformanceCounter(&now);          // real clock (not via IAT)
    if (!nextFrame || now.QuadPart - nextFrame > step * 4) nextFrame = now.QuadPart;
    nextFrame += step;
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = nextFrame - now.QuadPart;
        if (left <= 0) break;
        if (left > f.QuadPart / 500) Sleep(1); else YieldProcessor();
    }
    return hr;
}

static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void HookPresentVtable() {
    WNDCLASSA wc = {}; wc.lpfnWndProc = DummyProc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "dsr30dummy";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("dsr30dummy", "", WS_OVERLAPPED, 0, 0, 100, 100, 0, 0, wc.hInstance, 0);
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 1; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = w;
    d.SampleDesc.Count = 1; d.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0,
            D3D11_SDK_VERSION, &d, &sc, &dev, 0, &ctx))) {
        void** vt = *(void***)sc;           // vtable is shared by all swap chains of this class
        DWORD old;
        VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &old);
        oPresent = (Present_t)vt[8];
        vt[8] = (void*)hPresent;
        VirtualProtect(&vt[8], sizeof(void*), old, &old);
        // keep sc/dev alive on purpose so the vtable module stays loaded
    }
    DestroyWindow(w);
}

static DWORD WINAPI Init(LPVOID) {
    Sleep(2000);
    HookIAT("QueryPerformanceCounter", (void*)hQPC, (void**)&oQPC);
    HookIAT("GetTickCount64", (void*)hGTC64, (void**)&oGTC64);
    HookIAT("timeGetTime", (void*)hTGT, (void**)&oTGT);
    HookPresentVtable();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(m);
        CreateThread(0, 0, Init, 0, 0, 0);
    }
    return TRUE;
}
