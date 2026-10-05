// dsr30.cpp - EXPERIMENTAL 30 FPS cap + timestep scanner/patcher (generic build, no game-specific offsets).
// dinput8.dll proxy. No third-party libraries. OFFLINE USE ONLY.
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>
#include <stdint.h>

static double TIME_SCALE = 1.0;
static double TARGET_FPS = 30.0;
static char gDir[MAX_PATH];

static void Log(const char* fmt, ...) {
    char p[MAX_PATH]; lstrcpyA(p, gDir); lstrcatA(p, "dsr30.log");
    FILE* f = fopen(p, "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

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
static int gSyncInterval = 0;   // >0: let the display hold each frame for N vblanks (perfect pacing)

static HRESULT STDMETHODCALLTYPE hPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    static int n = 0; if (++n == 1) Log("Present hook is running");
    static LONGLONG t0 = 0; static int frames = 0;
    { LARGE_INTEGER q, qf; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&qf);
      if (!t0) t0 = q.QuadPart; frames++;
      double el = (double)(q.QuadPart - t0) / qf.QuadPart;
      if (el >= 10.0) { Log("measured FPS: %.2f", frames / el); t0 = q.QuadPart; frames = 0; } }
    HRESULT hr = gSyncInterval > 0 ? oPresent(sc, (UINT)gSyncInterval, flags & ~0x200u)
                                   : oPresent(sc, sync, flags);
    if (gSyncInterval > 0) return hr;   // vblank pacing replaces the software limiter
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


// ---- timestep scanner / patcher ----
static float gDtScale = 2.0f;      // multiplier for Patch= spots (2.0 turns 1/60 into 1/30)
static float gDtScale2 = 2.0f;     // multiplier for Patch2= spots
static CRITICAL_SECTION gCS;
struct Lock { Lock() { EnterCriticalSection(&gCS); } ~Lock() { LeaveCriticalSection(&gCS); } };
static volatile bool gFreezeOn = false;     // keep re-writing patched values (ini: Freeze=1)
static bool gFreezeEnabled = true;
struct Cand { BYTE* addr; bool isDouble; };
static std::vector<Cand> gCands;
static bool gPatched = false;
static double gScanValueD = 1.0 / 60.0;   // timestep value to search for (ini: ScanValue)
static UINT32 gScanF = 0;                  // float bits of ScanValue
static UINT64 gScanD = 0;                  // double bits of ScanValue
#define F60 gScanF
#define D60 gScanD
static void SetScan(double v) {
    gScanValueD = v; float f = (float)v;
    memcpy(&gScanF, &f, 4); memcpy(&gScanD, &v, 8);
}

static bool Readable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void Scan(bool wholeProcess) {
    Lock lk;
    Log("Scan started (%s)...", wholeProcess ? "whole process" : "exe only");
    gCands.clear(); gPatched = false;
    BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    SIZE_T exeSize = nt->OptionalHeader.SizeOfImage;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&Scan, &self);
    BYTE* sb = (BYTE*)self;
    SIZE_T ss = ((IMAGE_NT_HEADERS*)(sb + ((IMAGE_DOS_HEADER*)sb)->e_lfanew))->OptionalHeader.SizeOfImage;

    BYTE* p = wholeProcess ? (BYTE*)0x10000 : exe;
    BYTE* end = wholeProcess ? (BYTE*)0x7FFFFFFE0000ull : exe + exeSize;
    while (p < end) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(p, &m, sizeof(m))) break;
        BYTE* rb = (BYTE*)m.BaseAddress; SIZE_T rs = m.RegionSize;
        bool isSelf = rb < sb + ss && rb + rs > sb;
        if (Readable(m) && !isSelf && !(m.Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) && m.Type != MEM_MAPPED) {
            size_t step = wholeProcess ? 4 : 1;
            const SIZE_T CH = 1 << 20;
            static std::vector<BYTE> buf(CH + 16);
            for (SIZE_T off = 0; off < rs; off += CH) {
                SIZE_T want = rs - off < CH + 8 ? rs - off : CH + 8, got = 0;
                // ReadProcessMemory on ourselves fails safely instead of crashing the game
                if (!ReadProcessMemory(GetCurrentProcess(), rb + off, buf.data(), want, &got) || got < 8) continue;
                SIZE_T lim = (rs - off > CH) ? CH : got;
                for (SIZE_T i = 0; i + 8 <= got && i < lim; i += step) {
                    UINT32 f; memcpy(&f, &buf[i], 4);
                    if (f == F60) { gCands.push_back({ rb + off + i, false }); continue; }
                    UINT64 d; memcpy(&d, &buf[i], 8);
                    if (d == D60) gCands.push_back({ rb + off + i, true });
                }
            }
        }
        p = rb + rs;
    }
    Log("Scan (%s): %u candidates", wholeProcess ? "whole process" : "exe only", (unsigned)gCands.size());
    for (size_t i = 0; i < gCands.size() && i < 400; i++) {
        BYTE* a = gCands[i].addr;
        if (a >= exe && a < exe + exeSize)
            Log("  %s at exe+0x%llX", gCands[i].isDouble ? "double" : "float", (unsigned long long)(a - exe));
        else
            Log("  %s at 0x%p (outside exe)", gCands[i].isDouble ? "double" : "float", a);
    }
}

static void Apply(bool toThirty) {
    Lock lk;
    int n = 0;
    for (auto& c : gCands) {
        DWORD old;
        if (!VirtualProtect(c.addr, 8, PAGE_EXECUTE_READWRITE, &old)) continue;
        double nd = gScanValueD * gDtScale; float nf = (float)nd;
        if (c.isDouble) { UINT64 v = D60; if (toThirty) memcpy(&v, &nd, 8); memcpy(c.addr, &v, 8); }
        else { UINT32 v = F60; if (toThirty) memcpy(&v, &nf, 4); memcpy(c.addr, &v, 4); }
        VirtualProtect(c.addr, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), c.addr, 8);
        n++;
    }
    gPatched = toThirty;
    gFreezeOn = toThirty && gFreezeEnabled;
    Log("%s %d locations", toThirty ? "PATCHED to 1/30:" : "RESTORED to 1/60:", n);
}



// ---- narrow candidates by checking their CURRENT value (for games with a 30fps mode and a 60fps mode) ----
static double gAltRatio = 2.0;   // value seen in the 30 fps mode = ScanValue * AltRatio

static void Filter(bool wantAlt) {
    Lock lk;
    Log("Filter started (%s)...", wantAlt ? "30fps-mode" : "60fps-mode");
    double td = wantAlt ? gScanValueD * gAltRatio : gScanValueD;
    float tf = (float)td; UINT32 tfb; memcpy(&tfb, &tf, 4); UINT64 tdb; memcpy(&tdb, &td, 8);
    BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
    std::vector<Cand> kept;
    for (auto& c : gCands) {
        UINT64 buf = 0; SIZE_T got = 0;
        SIZE_T need = c.isDouble ? 8 : 4;
        if (!ReadProcessMemory(GetCurrentProcess(), c.addr, &buf, need, &got) || got != need) continue;
        bool ok = c.isDouble ? (buf == tdb) : ((UINT32)buf == tfb);
        if (ok) kept.push_back(c);
    }
    gCands = kept; gPatched = false;
    Log("Filter (%s value %.9g): %u candidates left", wantAlt ? "30fps-mode" : "60fps-mode", td, (unsigned)gCands.size());
    for (size_t i = 0; i < gCands.size() && i < 100; i++) {
        BYTE* a = gCands[i].addr;
        if (a >= exe && a < exe + 0x10000000)
            Log("  %s at exe+0x%llX", gCands[i].isDouble ? "double" : "float", (unsigned long long)(a - exe));
        else
            Log("  %s at 0x%p", gCands[i].isDouble ? "double" : "float", a);
    }
}

// ---- patch only specific exe offsets (from dsr30.ini "Patch=") ----
static std::vector<size_t> gOffsets;

static std::vector<size_t> gOffsets2;
static UINT32 gCurBits = 0, gCurBits2 = 0;   // values we last wrote

static UINT32 BitsFor(float scale) { float f = (float)(gScanValueD * scale); UINT32 b; memcpy(&b, &f, 4); return b; }

static int PatchList(const std::vector<size_t>& list, UINT32 want, UINT32& lastWritten) {
    BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
    int n = 0;
    for (size_t off : list) {
        BYTE* a = exe + off;
        UINT32 cur; memcpy(&cur, a, 4);
        if (cur != F60 && cur != lastWritten && cur != want) { Log("  skip exe+0x%llX (value changed)", (unsigned long long)off); continue; }
        DWORD old;
        if (!VirtualProtect(a, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        memcpy(a, &want, 4);
        VirtualProtect(a, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), a, 4);
        n++;
    }
    lastWritten = want;
    return n;
}

static void ApplyList(bool toThirty) {
    UINT32 w1 = toThirty ? BitsFor(gDtScale) : F60;
    UINT32 w2 = toThirty ? BitsFor(gDtScale2) : F60;
    int n1 = PatchList(gOffsets, w1, gCurBits);
    int n2 = PatchList(gOffsets2, w2, gCurBits2);
    gPatched = toThirty;
    Log("%s speed dtScale=%.3f at %d spots, camera dtScale=%.3f at %d spots",
        toThirty ? "PATCHED" : "RESTORED", toThirty ? gDtScale : 1.0f, n1, toThirty ? gDtScale2 : 1.0f, n2);
}


// ---- direct pokes into the exe (ini: Poke=OFFSET=TYPEvalue,... e.g. 25E82D8=i30,25E82F8=f30 ; Peek=OFFSET:TYPE,...) ----
struct Poke { size_t off; char type; double val; BYTE orig[8]; bool haveOrig; };
static std::vector<Poke> gPokes;
static std::vector<std::pair<size_t, char>> gPeeks;
static bool gPokesOn = false;

static size_t PokeLen(char t) { return t == 'd' ? 8 : 4; }
static void PokeBytes(const Poke& p, BYTE* out) {
    if (p.type == 'f') { float f = (float)p.val; memcpy(out, &f, 4); }
    else if (p.type == 'i') { int v = (int)p.val; memcpy(out, &v, 4); }
    else { double d = p.val; memcpy(out, &d, 8); }
}
static void LogValue(size_t off, char type, const char* tag) {
    BYTE* a = (BYTE*)GetModuleHandleA(nullptr) + off; BYTE b[8] = {}; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), a, b, PokeLen(type), &got) || got != PokeLen(type)) { Log("%s exe+0x%llX unreadable", tag, (unsigned long long)off); return; }
    if (type == 'f') { float f; memcpy(&f, b, 4); Log("%s exe+0x%llX = %.9g (float)", tag, (unsigned long long)off, f); }
    else if (type == 'i') { int v; memcpy(&v, b, 4); Log("%s exe+0x%llX = %d (int)", tag, (unsigned long long)off, v); }
    else { double d; memcpy(&d, b, 8); Log("%s exe+0x%llX = %.12g (double)", tag, (unsigned long long)off, d); }
}
static void ApplyPokes(bool on) {
    Lock lk;
    BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
    for (auto& p : gPokes) {
        BYTE* a = exe + p.off; SIZE_T n = PokeLen(p.type), w = 0, got = 0;
        if (!p.haveOrig) { if (ReadProcessMemory(GetCurrentProcess(), a, p.orig, n, &got) && got == n) p.haveOrig = true; }
        LogValue(p.off, p.type, on ? "before poke:" : "before restore:");
        BYTE nb[8]; if (on) PokeBytes(p, nb); else if (p.haveOrig) memcpy(nb, p.orig, 8); else continue;
        DWORD old; VirtualProtect(a, n, PAGE_EXECUTE_READWRITE, &old);
        WriteProcessMemory(GetCurrentProcess(), a, nb, n, &w);
        VirtualProtect(a, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), a, n);
    }
    gPokesOn = on;
    Log("%s %u pokes", on ? "POKES APPLIED:" : "POKES RESTORED:", (unsigned)gPokes.size());
}

static void DoPatch(bool toThirty) {
    if (!gPokes.empty()) { ApplyPokes(toThirty); return; }
    if (!gOffsets.empty() || !gOffsets2.empty()) ApplyList(toThirty); else Apply(toThirty);
}

static DWORD WINAPI AutoPatchThread(LPVOID p) {
    Sleep((DWORD)(INT_PTR)p * 1000);
    DoPatch(true);
    return 0;
}


// ---- hardware write-watch: find WHICH code writes a value (logged, no game code is changed) ----
struct Hit { BYTE* rip; BYTE b[48]; SIZE_T got; ULONG64 stk[8]; SIZE_T stkGot; ULONG64 rax, rcx, rdx; };
static BYTE* gWatchAddr = nullptr;   // ini Watch=OFFSET (exe-relative)
static Hit gHits[16];
static volatile LONG gNumHits = 0;
static bool gArmed = false;

static LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 0xF)) {
        LONG idx = InterlockedIncrement(&gNumHits) - 1;
        if (idx < 16) {
            BYTE* rip = (BYTE*)ep->ContextRecord->Rip;
            gHits[idx].rip = rip; gHits[idx].got = 0;
            ReadProcessMemory(GetCurrentProcess(), rip - 32, gHits[idx].b, 48, &gHits[idx].got);
            gHits[idx].stkGot = 0;
            ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ep->ContextRecord->Rsp, gHits[idx].stk, 64, &gHits[idx].stkGot);
            gHits[idx].rax = ep->ContextRecord->Rax; gHits[idx].rcx = ep->ContextRecord->Rcx; gHits[idx].rdx = ep->ContextRecord->Rdx;
        }
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->EFlags |= 0x10000;   // resume flag
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void ArmWatch(BYTE* addr, bool quiet = false) {
    AddVectoredExceptionHandler(1, Veh);
    std::vector<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) do {
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                ids.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    int ok = 0;
    for (DWORD id : ids) {   // no allocation or file I/O while a thread is suspended
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &ctx)) {
                ctx.Dr0 = (DWORD64)addr; ctx.Dr6 = 0;
                ctx.Dr7 = (ctx.Dr7 & ~(DWORD64)0xF0003) | 1ull | (1ull << 16) | (3ull << 18);  // write, 4 bytes
                if (SetThreadContext(h, &ctx)) ok++;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    gArmed = true;
    if (!quiet) Log("WATCH armed on 0x%p for %d of %d threads (logging code that writes it)", addr, ok, (int)ids.size());
}

static void LogNewHits() {
    static LONG logged = 0;
    LONG n = gNumHits; if (n > 16) n = 16;
    while (logged < n) {
        Hit& h = gHits[logged];
        HMODULE mod = nullptr; char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)h.rip, &mod);
        if (mod) { GetModuleFileNameA(mod, name, MAX_PATH); char* sl = strrchr(name, '\\'); if (sl) memmove(name, sl + 1, strlen(sl)); }
        Log("WRITER hit #%d: %s+0x%llX (rip after the write)", (int)logged + 1, name, (unsigned long long)(h.rip - (BYTE*)mod));
        char hex[256]; hex[0] = 0;
        for (SIZE_T i = 0; i < h.got && i < 48; i++) sprintf(hex + i * 3, "%02X ", h.b[i]);
        Log("  bytes (rip-32 .. rip+16): %s", hex);
        {
            BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
            Log("  regs: rax=%llX rcx=%llX rdx=%llX", h.rax, h.rcx, h.rdx);
            for (SIZE_T i = 0; i < h.stkGot / 8 && i < 8; i++) {
                ULONG64 v = h.stk[i];
                if (v >= (ULONG64)exe && v < (ULONG64)exe + 0x10000000) Log("  stack[%u]: exe+0x%llX", (unsigned)i, v - (ULONG64)exe);
                else Log("  stack[%u]: %llX", (unsigned)i, v);
            }
        }
        logged++;
    }
}

// ---- re-write patched values continuously, report which one the game keeps reverting ----
static DWORD WINAPI FreezeThread(LPVOID) {
    ULONGLONG last = GetTickCount64(); unsigned long long checks = 0;
    std::vector<unsigned> cnt;
    for (;;) {
        Sleep(1);
        if (gArmed) LogNewHits();
        if (gWatchAddr) { static ULONGLONG lastRe = 0; ULONGLONG nw = GetTickCount64(); if (nw - lastRe > 3000) { lastRe = nw; ArmWatch(gWatchAddr, true); } }
        {   // keep pokes in place and report values
            static ULONGLONG lastPeek = 0; static unsigned pokeReverts = 0;
            if (gPokesOn && gFreezeEnabled) {
                Lock lk;
                BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
                for (auto& p : gPokes) {
                    BYTE want[8], cur[8] = {}; SIZE_T n = PokeLen(p.type), got = 0, w = 0;
                    PokeBytes(p, want);
                    if (!ReadProcessMemory(GetCurrentProcess(), exe + p.off, cur, n, &got) || got != n) continue;
                    if (memcmp(cur, want, n) != 0) { pokeReverts++; WriteProcessMemory(GetCurrentProcess(), exe + p.off, want, n, &w); }
                }
            }
            if ((!gPeeks.empty() || gPokesOn) && GetTickCount64() - lastPeek > 5000) {
                lastPeek = GetTickCount64();
                for (auto& pk : gPeeks) LogValue(pk.first, pk.second, "peek:");
                if (gPokesOn) { Log("pokes: game reverted a poked value %u times in the last 5s", pokeReverts); pokeReverts = 0; }
            }
        }
        if (!gFreezeOn) { last = GetTickCount64(); cnt.clear(); checks = 0; continue; }
        {
            Lock lk;
            if (cnt.size() != gCands.size()) cnt.assign(gCands.size(), 0);
            double nd = gScanValueD * gDtScale; float nf = (float)nd;
            UINT32 nfb; memcpy(&nfb, &nf, 4); UINT64 ndb; memcpy(&ndb, &nd, 8);
            for (size_t i = 0; i < gCands.size(); i++) {
                Cand& c = gCands[i];
                UINT64 cur = 0; SIZE_T got = 0, need = c.isDouble ? 8 : 4;
                if (!ReadProcessMemory(GetCurrentProcess(), c.addr, &cur, need, &got) || got != need) continue;
                checks++;
                bool same = c.isDouble ? (cur == ndb) : ((UINT32)cur == nfb);
                if (!same) {
                    cnt[i]++;
                    SIZE_T wr = 0;
                    if (c.isDouble) WriteProcessMemory(GetCurrentProcess(), c.addr, &ndb, 8, &wr);
                    else WriteProcessMemory(GetCurrentProcess(), c.addr, &nfb, 4, &wr);
                }
            }
            if (GetTickCount64() - last > 5000) {
                size_t best = 0; unsigned total = 0;
                for (size_t i = 0; i < cnt.size(); i++) { total += cnt[i]; if (cnt[i] > cnt[best]) best = i; }
                Log("freeze (last 5s): game reverted the value %u times in total", total);
                for (size_t i = 0; i < cnt.size(); i++) if (cnt[i]) {
                    BYTE* a = gCands[i].addr; BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
                    if (a >= exe && a < exe + 0x10000000) Log("  reverted %u times: exe+0x%llX", cnt[i], (unsigned long long)(a - exe));
                    else Log("  reverted %u times: 0x%p", cnt[i], a);
                }
                if (!gArmed && !gWatchAddr && !cnt.empty() && cnt[best] > 20) {
                    gFreezeOn = false;          // stop writing so only the game's own writes are seen
                    ArmWatch(gCands[best].addr);
                }
                last = GetTickCount64(); std::fill(cnt.begin(), cnt.end(), 0u); checks = 0;
            }
        }
    }
}

static DWORD WINAPI HotkeyThread(LPVOID) {
    bool k6 = false, k7 = false, k8 = false, k9 = false, k10 = false, k11 = false, k3 = false, k4 = false, k5 = false;
    for (;;) {
        Sleep(50);
        bool n6 = GetAsyncKeyState(VK_F6) & 0x8000, n7 = GetAsyncKeyState(VK_F7) & 0x8000;
        bool n8 = GetAsyncKeyState(VK_F8) & 0x8000, n9 = GetAsyncKeyState(VK_F9) & 0x8000;
        if (n6 && !k6) Scan(false);
        if (n7 && !k7) Scan(true);
        if (n8 && !k8) DoPatch(true);
        if (n9 && !k9) DoPatch(false);
        bool n10 = GetAsyncKeyState(VK_F10) & 0x8000, n11 = GetAsyncKeyState(VK_F11) & 0x8000;
        if (n10 && !k10) { gDtScale -= 0.02f; if (gPatched && (!gOffsets.empty() || !gOffsets2.empty())) ApplyList(true); else Log("dtScale=%.3f", gDtScale); }
        if (n11 && !k11) { gDtScale += 0.02f; if (gPatched && (!gOffsets.empty() || !gOffsets2.empty())) ApplyList(true); else Log("dtScale=%.3f", gDtScale); }
        bool n3 = GetAsyncKeyState(VK_F3) & 0x8000, n4 = GetAsyncKeyState(VK_F4) & 0x8000;
        if (n3 && !k3) Filter(true);    // call while in the 30 fps section
        if (n4 && !k4) Filter(false);   // call while in the 60 fps section
        bool n5 = GetAsyncKeyState(VK_F5) & 0x8000;
        if (n5 && !k5) { Log("MARK (F5 pressed)"); for (auto& pk : gPeeks) LogValue(pk.first, pk.second, "peek:"); }
        k5 = n5;
        k6 = n6; k7 = n7; k8 = n8; k9 = n9; k10 = n10; k11 = n11; k3 = n3; k4 = n4;
    }
}

static DWORD WINAPI Init(LPVOID) {
    InitializeCriticalSection(&gCS);
    GetModuleFileNameA(nullptr, gDir, MAX_PATH);
    char* sl = strrchr(gDir, '\\'); if (sl) sl[1] = 0;
    char ini[MAX_PATH]; lstrcpyA(ini, gDir); lstrcatA(ini, "dsr30.ini");
    char b[64];
    GetPrivateProfileStringA("dsr30", "ScanValue", "1/60", b, 64, ini);
    { char* sl = strchr(b, '/');           // allows exact values like 1/60 or 1000/60
      double v = sl ? atof(b) / atof(sl + 1) : atof(b);
      SetScan(v); }
    GetPrivateProfileStringA("dsr30", "AltRatio", "2.0", b, 64, ini); gAltRatio = atof(b);
    gCurBits = gCurBits2 = gScanF;
    GetPrivateProfileStringA("dsr30", "TimeScale", "1.0", b, 64, ini); TIME_SCALE = atof(b);
    GetPrivateProfileStringA("dsr30", "TargetFPS", "30", b, 64, ini); TARGET_FPS = atof(b);
    Log("---- loaded (generic build v8 watch-exe). ScanValue=%.9f", gScanValueD); Log("---- settings. TimeScale=%.3f TargetFPS=%.1f", TIME_SCALE, TARGET_FPS);
    timeBeginPeriod(1);   // precise Sleep() so the limiter does not overshoot frames
    Sleep(2000);
    if (TIME_SCALE != 1.0) {
        Log("IAT QueryPerformanceCounter: %d", HookIAT("QueryPerformanceCounter", (void*)hQPC, (void**)&oQPC));
        Log("IAT GetTickCount64: %d", HookIAT("GetTickCount64", (void*)hGTC64, (void**)&oGTC64));
        Log("IAT timeGetTime: %d", HookIAT("timeGetTime", (void*)hTGT, (void**)&oTGT));
    }
    GetPrivateProfileStringA("dsr30", "DtScale", "2.0", b, 64, ini); gDtScale = (float)atof(b);
    char lst[2048];
    GetPrivateProfileStringA("dsr30", "Patch", "", lst, 2048, ini);
    for (char* t = strtok(lst, ", "); t; t = strtok(nullptr, ", ")) gOffsets.push_back((size_t)strtoull(t, nullptr, 16));
    GetPrivateProfileStringA("dsr30", "DtScale2", "2.0", b, 64, ini); gDtScale2 = (float)atof(b);
    char patch2str[2048];
    GetPrivateProfileStringA("dsr30", "Patch2", "", patch2str, 2048, ini);
    for (char* t = strtok(patch2str, ", "); t; t = strtok(nullptr, ", ")) gOffsets2.push_back((size_t)strtoull(t, nullptr, 16));
    Log("Speed spots: %u (scale %.3f), camera spots: %u (scale %.3f)", (unsigned)gOffsets.size(), gDtScale, (unsigned)gOffsets2.size(), gDtScale2);
    gFreezeEnabled = GetPrivateProfileIntA("dsr30", "Freeze", 1, ini) != 0;
    { char pk[1024]; GetPrivateProfileStringA("dsr30", "Poke", "", pk, 1024, ini);
      for (char* t = strtok(pk, ", "); t; t = strtok(nullptr, ", ")) {
          char* eq = strchr(t, '='); if (!eq || !eq[1]) continue;
          Poke p = {}; p.off = (size_t)strtoull(t, nullptr, 16); p.type = eq[1]; p.val = atof(eq + 2);
          if (p.type == 'f' || p.type == 'i' || p.type == 'd') gPokes.push_back(p);
      }
      char pe[1024]; GetPrivateProfileStringA("dsr30", "Peek", "", pe, 1024, ini);
      for (char* t = strtok(pe, ", "); t; t = strtok(nullptr, ", ")) {
          char* c = strchr(t, ':'); if (!c || !c[1]) continue;
          gPeeks.push_back({ (size_t)strtoull(t, nullptr, 16), c[1] });
      }
      Log("Pokes: %u, peeks: %u", (unsigned)gPokes.size(), (unsigned)gPeeks.size());
      char wa[64]; GetPrivateProfileStringA("dsr30", "Watch", "", wa, 64, ini);
      if (wa[0]) { gWatchAddr = (BYTE*)GetModuleHandleA(nullptr) + (size_t)strtoull(wa, nullptr, 16); ArmWatch(gWatchAddr); } }
    int autoSec = GetPrivateProfileIntA("dsr30", "AutoPatchSeconds", 15, ini);
    if (autoSec > 0 && (!gOffsets.empty() || !gOffsets2.empty() || !gPokes.empty())) CreateThread(0, 0, AutoPatchThread, (LPVOID)(INT_PTR)autoSec, 0, 0);
    CreateThread(0, 0, FreezeThread, 0, 0, 0);
    CreateThread(0, 0, HotkeyThread, 0, 0, 0);
    Log("Hotkeys: F6 scan exe, F7 scan all, F8 patch, F9 restore, F10 dt -2%%, F11 dt +2%%, F3 filter(30fps section), F4 filter(60fps section)");
    if (GetPrivateProfileIntA("dsr30", "Vsync30", 1, ini)) {
        DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
        int hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
        if (hz >= 30 && hz % 30 == 0) gSyncInterval = hz / 30;
        Log("Display %d Hz -> SyncInterval %d (0 = software limiter only)", hz, gSyncInterval);
    }
    HookPresentVtable();
    Log("Present vtable hooked: %d", oPresent != nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(m);
        CreateThread(0, 0, Init, 0, 0, 0);
    }
    return TRUE;
}
