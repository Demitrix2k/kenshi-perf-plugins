// CharProbe: diagnostic RE_Kenshi plugin that measures Kenshi's per-character
// update cost on the main thread. It only times calls; game behaviour is unchanged.
//
// Kenshi's world update calls a character-budget function every frame
// (RE_Kenshi Kenshi_x64.exe 0x1407862f0, reached through thunk 0x14004b628 from
// 0x140788467; Meitou's notes call the 1.0.68 version FUN_140786e30):
//   - 8 characters per frame (round robin) get the logic tick, Character vtable
//     slot 29 (0x1405cc300, thunk 0x14000b91a);
//   - characters with the "priority" byte +0xe4 set get the full update, slot 28
//     (0x1405ce6a0, thunk 0x1400073d8), EVERY frame;
//   - the rest get slot 28 only 6 per frame (round robin); in between, their
//     elapsed time is added to the accumulator at +0xc0, which slot 28 consumes.
// Both slots are only reached through vtables, i.e. through those thunks.
//
// Every 10 s CharProbe.log reports per frame: the budget function's time, slot 28
// calls and time split by the priority flag, slot 28 calls made outside the budget,
// and slot 29 calls and time. Built without a CRT (see build.sh).

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) int __stdcall VirtualProtect(void*, u64, u32, u32*);
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE, const void*, u64);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(i64*);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(i64*);
__declspec(dllimport) u64 __stdcall GetTickCount64();
__declspec(dllimport) u32 __stdcall GetCurrentThreadId();
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
}

namespace {

struct Target { u32 thunkRva, funcRva; u8 funcBytes[8]; const char* name; };
const Target kTargets[3] = {
    {0x4b628, 0x7862f0, {0x48, 0x8b, 0xc4, 0x48, 0x83, 0xec, 0x58, 0x48}, "character budget"},
    {0x073d8, 0x5ce6a0, {0x40, 0x56, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b}, "slot 28 full update"},
    {0x0b91a, 0x5cc300, {0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x54, 0x41}, "slot 29 logic tick"},
};
const u32 kPadRva = 0x6c139;  // 71 bytes of int3 padding; 3 stubs x 16 bytes

typedef void (*Fn1)(u8*);
Fn1 gBudget = 0, gFull = 0, gTick = 0;

inline u64 rdtsc() { return __builtin_ia32_rdtsc(); }

u32 gMainTid = 0;
bool gInBudget = false;
u64 gFrames = 0, gBudgetTicks = 0;
u64 gFullCalls[3] = {0, 0, 0}, gFullTicks[3] = {0, 0, 0};  // [priority, normal, outside budget]
u64 gTickCalls = 0, gTickTicks = 0, gOtherThread = 0;
u64 gLastReport = 0, gTscPerMs = 1;
u32 gMaxPriority = 0, gPriorityThisFrame = 0;

HANDLE gLog = 0;
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
// v / d with two decimals
void logFrac(u64 v, u64 d) {
    if (!d) { logStr("-"); return; }
    u64 x = (v * 100 + d / 2) / d;
    logUint(x / 100);
    char t[4] = {'.', (char)('0' + x / 10 % 10), (char)('0' + x % 10), 0};
    logStr(t);
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

__declspec(noinline) void report() {
    u64 f = gFrames, ms = gTscPerMs;
    logTime();
    logStr("frames ");
    logUint(f);
    logStr(", character budget ms/frame ");
    logFrac(gBudgetTicks * 100 / ms, f * 100);
    logStr("\r\n    full update (slot 28), priority:     calls/frame ");
    logFrac(gFullCalls[0], f);
    logStr(", ms/frame ");
    logFrac(gFullTicks[0] * 100 / ms, f * 100);
    logStr(" (max priority chars in a frame ");
    logUint(gMaxPriority);
    logStr(")\r\n    full update (slot 28), round robin:  calls/frame ");
    logFrac(gFullCalls[1], f);
    logStr(", ms/frame ");
    logFrac(gFullTicks[1] * 100 / ms, f * 100);
    logStr("\r\n    full update (slot 28), other callers: calls/frame ");
    logFrac(gFullCalls[2], f);
    logStr(", ms/frame ");
    logFrac(gFullTicks[2] * 100 / ms, f * 100);
    logStr("\r\n    logic tick (slot 29):                calls/frame ");
    logFrac(gTickCalls, f);
    logStr(", ms/frame ");
    logFrac(gTickTicks * 100 / ms, f * 100);
    if (gOtherThread) { logStr("\r\n    calls from other threads (not timed): "); logUint(gOtherThread); }
    logStr("\r\n");
    gFrames = gBudgetTicks = gTickCalls = gTickTicks = gOtherThread = 0;
    for (int i = 0; i < 3; ++i) gFullCalls[i] = gFullTicks[i] = 0;
    gMaxPriority = 0;
}

void budgetWrap(u8* world) {
    if (!gMainTid) gMainTid = GetCurrentThreadId();
    gInBudget = true;
    gPriorityThisFrame = 0;
    u64 t0 = rdtsc();
    gBudget(world);
    gBudgetTicks += rdtsc() - t0;
    gInBudget = false;
    if (gPriorityThisFrame > gMaxPriority) gMaxPriority = gPriorityThisFrame;
    ++gFrames;
    u64 now = GetTickCount64();
    if (now - gLastReport >= 10000) {
        if (gLastReport) report();
        gLastReport = now;
    }
}

void fullWrap(u8* ch) {
    if (GetCurrentThreadId() != gMainTid) { ++gOtherThread; gFull(ch); return; }
    int k = !gInBudget ? 2 : (ch[0xe4] ? 0 : 1);
    if (k == 0) ++gPriorityThisFrame;
    u64 t0 = rdtsc();
    gFull(ch);
    gFullTicks[k] += rdtsc() - t0;
    ++gFullCalls[k];
}

void tickWrap(u8* ch) {
    if (GetCurrentThreadId() != gMainTid) { ++gOtherThread; gTick(ch); return; }
    u64 t0 = rdtsc();
    gTick(ch);
    gTickTicks += rdtsc() - t0;
    ++gTickCalls;
}

void* const kWrappers[3] = {(void*)&budgetWrap, (void*)&fullWrap, (void*)&tickWrap};

bool writeCode(u8* dst, const u8* src, int n) {
    u32 old;
    if (!VirtualProtect(dst, n, 0x40, &old)) return false;
    for (int i = 0; i < n; ++i) dst[i] = src[i];
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

void install() {
    i64 f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    u64 t0 = rdtsc();
    do { QueryPerformanceCounter(&q1); } while (q1 - q0 < f / 200);
    gTscPerMs = (rdtsc() - t0) * (u64)f / (u64)(q1 - q0) / 1000;
    if (!gTscPerMs) gTscPerMs = 1;

    u8* exe = (u8*)GetModuleHandleA(0);
    if (!exe) { logStr("no exe module\r\n"); return; }
    u8* pad = exe + kPadRva;
    for (int i = 0; i < 48; ++i)
        if (pad[i] != 0xcc) { logStr("stub padding is not int3 (different build?), not patching\r\n"); return; }
    for (int t = 0; t < 3; ++t) {
        const Target& tg = kTargets[t];
        u8* thunk = exe + tg.thunkRva;
        u8* func = exe + tg.funcRva;
        int rel = (int)(thunk[1] | (thunk[2] << 8) | (thunk[3] << 16) | ((u32)thunk[4] << 24));
        if (thunk[0] != 0xe9 || thunk + 5 + rel != func) {
            logStr(tg.name); logStr(": thunk does not jump to the expected function, not patching\r\n");
            return;
        }
        for (int i = 0; i < 8; ++i)
            if (func[i] != tg.funcBytes[i]) { logStr(tg.name); logStr(": function bytes differ, not patching\r\n"); return; }
    }
    gBudget = (Fn1)(exe + kTargets[0].funcRva);
    gFull = (Fn1)(exe + kTargets[1].funcRva);
    gTick = (Fn1)(exe + kTargets[2].funcRva);
    for (int t = 0; t < 3; ++t) {
        u8 stub[16] = {0xff, 0x25, 0, 0, 0, 0};
        u64 target = (u64)kWrappers[t];
        for (int i = 0; i < 8; ++i) stub[6 + i] = (u8)(target >> (8 * i));
        stub[14] = stub[15] = 0xcc;
        if (!writeCode(pad + 16 * t, stub, 16)) { logStr("VirtualProtect failed\r\n"); return; }
    }
    for (int t = 0; t < 3; ++t) {
        u8* thunk = exe + kTargets[t].thunkRva;
        int rel = (int)((i64)(pad + 16 * t - (thunk + 5)));
        u8 jmp[5] = {0xe9, (u8)rel, (u8)(rel >> 8), (u8)(rel >> 16), (u8)(rel >> 24)};
        if (!writeCode(thunk, jmp, 5)) { logStr("VirtualProtect failed\r\n"); return; }
    }
    logTime();
    logStr("timing the character budget and Character slots 28/29; reporting every 10 s\r\n");
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("CharProbe.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("CharProbe 1.0\r\n");
    install();
}
