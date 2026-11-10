// BarrierSpin: RE_Kenshi plugin that makes Ogre's worker-thread barrier spin
// briefly before sleeping.
//
// Ogre::Barrier::sync (OgreMain, exported) is a pure sleeping barrier: every thread
// that is not last does WaitForSingleObject on a semaphore; the last one flips the
// generation index and releases the semaphore N-1 times. Kenshi's main thread goes
// through it ~70 times per frame (culling per pass, LOD, animation, node updates)
// and under Wine each sleep/wake costs a syscall plus a scheduler wake-up, which is
// a large share of these tiny parallel phases (main thread off-CPU ~15% of the
// frame at this barrier in the 60+ NPC save).
//
// This replacement keeps the exact same protocol (same fields, same semaphores,
// always consumes its semaphore count with the original blocking wait), but before
// that wait it spins for up to N microseconds watching the generation index. If the
// last thread arrives during the spin, the wait that follows returns immediately
// instead of putting the thread to sleep. Long gaps still sleep as before.
//
// Barrier layout (from the constructor): +0x00 u64 numThreads, +0x08 u64 index,
// +0x10 u32 arrived, +0x18 HANDLE semaphores[2].
//
// KENSHI_BARRIER_SPIN_US=N spin length in microseconds (default 50, 0 = no spin).
// KENSHI_BARRIER_SPIN=0 starts with spinning off. With KENSHI_PERF_DEBUG=1, Insert
// toggles spinning in game. Toggles and per-10-s stats go to BarrierSpin.log.
//
// Target: OgreMain_x64.dll shipped with Kenshi 1.0.65; prologue bytes are checked.
// Built without a CRT (see build.sh).

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) void* __stdcall GetProcAddress(HANDLE, const char*);
__declspec(dllimport) int __stdcall VirtualProtect(void*, u64, u32, u32*);
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE, const void*, u64);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) u32 __stdcall WaitForSingleObject(HANDLE, u32);
__declspec(dllimport) int __stdcall ReleaseSemaphore(HANDLE, int, int*);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(i64*);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(i64*);
__declspec(dllimport) u64 __stdcall GetTickCount64();
__declspec(dllimport) short __stdcall GetAsyncKeyState(int);
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
}

namespace {

const char* const kSyncName = "?sync@Barrier@Ogre@@QEAAXXZ";
const u8 kSyncPrologue[16] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x41, 0x08,
                              0x4c, 0x8b, 0xc9, 0x48, 0x89, 0x44, 0x24, 0x30};

struct Barrier {
    u64 numThreads;
    volatile u64 index;
    volatile u32 arrived;
    u32 pad;
    HANDLE sem[2];
};

u64 gSpinTicks = 0;      // TSC ticks to spin before sleeping
u32 gSpinUs = 50;
volatile bool gSpinOn = true;
bool gHotkeys = false;

// Stats (racy counters are fine for diagnostics)
volatile u64 gSyncs = 0, gSpinHits = 0, gSpinMisses = 0;
u64 gLastReport = 0;
bool gKeyDown = false;
volatile u64 gNextPoll = 0;   // TSC time of the next key poll / stats check
u64 gPollTicks = 0;           // ~50 ms

HANDLE gLog = 0;

inline u64 rdtsc() { return __builtin_ia32_rdtsc(); }

void logStr(const char* s) {
    if (!gLog) return;
    u32 n = 0, w;
    while (s[n]) ++n;
    WriteFile(gLog, s, n, &w, 0);
}

void logUint(u64 v) {
    char buf[21];
    int i = 20;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    logStr(buf + i);
}

void logPad(u32 v, int width) {
    char buf[8];
    for (int i = width - 1; i >= 0; --i) { buf[i] = (char)('0' + v % 10); v /= 10; }
    buf[width] = 0;
    logStr(buf);
}

void logTime() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    logPad(t.h, 2); logStr(":"); logPad(t.mi, 2); logStr(":"); logPad(t.s, 2); logStr(".");
    logPad(t.ms, 3);
    logStr(" ");
}

__declspec(noinline) void housekeeping() {
    if (gHotkeys) {
        const int VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_INSERT = 0x2d;
        bool mod = (GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL)) & 0x8000;
        bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (down && !gKeyDown && !mod) {
            gSpinOn = !gSpinOn;
            logTime();
            logStr(gSpinOn ? "Insert: spin ON\r\n" : "Insert: spin OFF\r\n");
        }
        gKeyDown = down;
    }
    u64 now = GetTickCount64();
    if (now - gLastReport >= 10000) {
        if (gLastReport) {
            logTime();
            logStr("last 10 s: syncs ");
            logUint(gSyncs);
            logStr(", waits avoided by spin ");
            logUint(gSpinHits);
            logStr(", spins that timed out ");
            logUint(gSpinMisses);
            logStr(gSpinOn ? " (spin on)\r\n" : " (spin off)\r\n");
        }
        gSyncs = gSpinHits = gSpinMisses = 0;
        gLastReport = now;
    }
}

// Replacement for Ogre::Barrier::sync (this in rcx).
void barrierSync(Barrier* b) {
    ++gSyncs;
    u64 now = rdtsc();
    if (now >= gNextPoll) {  // time-based: racy across threads, at worst polls twice
        gNextPoll = now + gPollTicks;
        housekeeping();
    }

    u64 idx = b->index;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    u32 before = __atomic_fetch_add(&b->arrived, 1u, __ATOMIC_SEQ_CST);
    if ((u64)before == b->numThreads - 1) {
        // Last to arrive: flip the generation and wake everyone (as the original).
        b->index = idx == 0 ? 1 : 0;
        b->arrived = 0;
        if (b->numThreads > 1) ReleaseSemaphore(b->sem[idx], (int)(b->numThreads - 1), 0);
        return;
    }
    if (gSpinOn && gSpinTicks) {
        u64 start = rdtsc();
        for (;;) {
            if (b->index != idx) { ++gSpinHits; break; }
            if (rdtsc() - start > gSpinTicks) { ++gSpinMisses; break; }
            __builtin_ia32_pause();
        }
    }
    // Always consume our semaphore count; returns at once if already released.
    WaitForSingleObject(b->sem[idx], 0xffffffff);
}

bool same(const u8* p, const u8* expected, int n) {
    for (int i = 0; i < n; ++i)
        if (p[i] != expected[i]) return false;
    return true;
}

void calibrate() {
    i64 f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    u64 t0 = rdtsc();
    do { QueryPerformanceCounter(&q1); } while (q1 - q0 < f / 200);  // ~5 ms
    u64 t1 = rdtsc();
    u64 ticksPerUs = (t1 - t0) * (u64)f / (u64)(q1 - q0) / 1000000;
    gSpinTicks = ticksPerUs * gSpinUs;
    gPollTicks = ticksPerUs * 50000;
    logStr("TSC ");
    logUint(ticksPerUs);
    logStr(" ticks/us, spin ");
    logUint(gSpinUs);
    logStr(" us\r\n");
}

void install() {
    char env[16];
    u32 len = GetEnvironmentVariableA("KENSHI_BARRIER_SPIN", env, sizeof env);
    if (len == 1 && env[0] == '0') gSpinOn = false;
    len = GetEnvironmentVariableA("KENSHI_BARRIER_SPIN_US", env, sizeof env);
    if (len > 0 && len < sizeof env) {
        u32 v = 0;
        for (u32 i = 0; i < len && env[i] >= '0' && env[i] <= '9'; ++i) v = v * 10 + (env[i] - '0');
        if (v <= 2000) gSpinUs = v;
    }
    len = GetEnvironmentVariableA("KENSHI_PERF_DEBUG", env, sizeof env);
    gHotkeys = len == 1 && env[0] == '1';
    logStr(gHotkeys ? "hotkey Insert: on (KENSHI_PERF_DEBUG=1)\r\n" : "hotkey Insert: off\r\n");
    calibrate();

    HANDLE ogre = GetModuleHandleA("OgreMain_x64.dll");
    u8* sync = ogre ? (u8*)GetProcAddress(ogre, kSyncName) : 0;
    if (!sync) { logStr("Barrier::sync not found, not patching\r\n"); return; }
    if (!same(sync, kSyncPrologue, sizeof kSyncPrologue)) {
        logStr("Barrier::sync bytes differ (different build or already hooked), not patching\r\n");
        return;
    }
    // Full replacement: jmp barrierSync at the function start (never returns into it).
    u8 patch[14] = {0xff, 0x25, 0, 0, 0, 0};
    u64 target = (u64)&barrierSync;
    for (int i = 0; i < 8; ++i) patch[6 + i] = (u8)(target >> (8 * i));
    u32 old;
    if (!VirtualProtect(sync, 14, 0x40, &old)) { logStr("VirtualProtect failed\r\n"); return; }
    for (int i = 0; i < 14; ++i) sync[i] = patch[i];
    VirtualProtect(sync, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), sync, 14);
    logTime();
    logStr(gSpinOn ? "replaced Ogre::Barrier::sync, spin ON\r\n"
                   : "replaced Ogre::Barrier::sync, spin OFF (KENSHI_BARRIER_SPIN=0)\r\n");
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("BarrierSpin.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("BarrierSpin 1.1\r\n");
    install();
    // The log stays open for toggles and stats.
}
