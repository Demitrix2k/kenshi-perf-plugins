// MyGUIQuadFix: RE_Kenshi plugin that replaces MyGUI's VertexQuad::set.
//
// The original writes 4 vertices into the mapped vertex buffer, then builds
// the 2 duplicate vertices by reading them back out of that buffer. Under
// DXVK the buffer can be uncached/write-combined memory, so those reads stall
// the main thread. This version computes all 6 vertices from the arguments
// and only ever writes to the buffer.
//
// Target: MyGUIEngine_x64.dll shipped with Kenshi 1.0.65
//   sha256 fea6f1f935703812485bd6606ff4d3ceea172ece131ed4c6dcbf7a7e25b083a8
// Every patched byte is checked first; on any mismatch nothing is touched.
//
// Built without a CRT (see build.sh). MYGUI_QUADFIX=0 starts with the fix off
// (still installed). At runtime, "quadfix=0/1" in perfctl.ini (see
// tools/perfctl.py) switches between the fix and MyGUI's original
// function; each switch is written with a local timestamp to MyGUIQuadFix.log and
// reported in perfctl-status-quadfix.txt.

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) int __stdcall VirtualProtect(void*, u64, u32, u32*);
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE, const void*, u64);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
}

#include "../../common/perfctl.h"

namespace {

const u64 kFuncRva = 0x199460;   // VertexQuad::set
const u64 kThunkRva = 0x6320;    // incremental-link thunk: jmp VertexQuad::set
const u64 kPadRva = 0x199560;    // int3 padding after the function

const u8 kFuncBytes[16] = {0x48, 0x83, 0xec, 0x18, 0x8b, 0x44, 0x24, 0x70,
                           0xf3, 0x0f, 0x10, 0x6c, 0x24, 0x48, 0xf3, 0x0f};
const u8 kThunkBytes[5] = {0xe9, 0x3b, 0x31, 0x19, 0x00};

struct Vertex {
    float x, y, z;
    u32 colour;
    float u, v;
};

typedef void (*QuadSetFn)(Vertex*, float, float, float, float, float,
                          float, float, float, float, u32);
QuadSetFn gOriginal = 0;  // MyGUI's own VertexQuad::set, left intact
volatile bool gEnabled = true;  // written by the perfctl thread

// Store-only quad write: 6 vertices, never reads the buffer back.
// Vertex order matches MyGUI: LT, RT, LB, LB2 (=LB), RT2 (=RT), RB.
inline void writeQuad(Vertex* q, float l, float t, float r, float b, float z,
                      float u1, float v1, float u2, float v2, u32 colour) {
    const Vertex lt = {l, t, z, colour, u1, v1};
    const Vertex rt = {r, t, z, colour, u2, v1};
    const Vertex lb = {l, b, z, colour, u1, v2};
    const Vertex rb = {r, b, z, colour, u2, v2};
    q[0] = lt;
    q[1] = rt;
    q[2] = lb;
    q[3] = lb;
    q[4] = rt;
    q[5] = rb;
}

// Same signature/ABI as MyGUI::VertexQuad::set (this in rcx, floats positional).
// While the fix is off, MyGUI's original runs (sibling call: jmp).
void quadSet(Vertex* q, float l, float t, float r, float b, float z,
             float u1, float v1, float u2, float v2, u32 colour) {
    if (!gEnabled) {
        gOriginal(q, l, t, r, b, z, u1, v1, u2, v2, colour);
        return;
    }
    writeQuad(q, l, t, r, b, z, u1, v1, u2, v2, colour);
}

HANDLE gLog = 0;

void logStr(const char* s) {
    if (!gLog) return;
    u32 n = 0, w;
    while (s[n]) ++n;
    WriteFile(gLog, s, n, &w, 0);
}

void logHex(u64 v) {
    char buf[19] = {'0', 'x'};
    for (int i = 0; i < 16; ++i) {
        u32 nib = (v >> (60 - 4 * i)) & 0xf;
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = 0;
    logStr(buf);
}

void logPad(u32 v, int width) {
    char buf[8];
    for (int i = width - 1; i >= 0; --i) { buf[i] = (char)('0' + v % 10); v /= 10; }
    buf[width] = 0;
    logStr(buf);
}

void logToggle(const char* why) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    logPad(t.h, 2); logStr(":"); logPad(t.mi, 2); logStr(":"); logPad(t.s, 2); logStr(".");
    logPad(t.ms, 3);
    logStr(" ");
    logStr(why);
    logStr(gEnabled ? ": fix ON\r\n" : ": fix OFF (original MyGUI)\r\n");
}

void reportStatus() {
    perfctl::Text t;
    t.add("quadfix=").num(gEnabled ? 1 : 0).add("\n");
    perfctl::status("quadfix", t.b);
}

// perfctl thread: "quadfix=0/1".
void onControl(const char* key, int value) {
    if (!perfctl::keyIs(key, "quadfix") || (value != 0) == gEnabled) return;
    gEnabled = value != 0;
    logToggle("perfctl");
    reportStatus();
}

bool same(const u8* p, const u8* expected, int n) {
    for (int i = 0; i < n; ++i)
        if (p[i] != expected[i]) return false;
    return true;
}

bool writeCode(u8* dst, const u8* src, int n) {
    u32 old;
    if (!VirtualProtect(dst, n, 0x40 /* PAGE_EXECUTE_READWRITE */, &old)) return false;
    for (int i = 0; i < n; ++i) dst[i] = src[i];
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

void install() {
    char env[8];
    u32 len = GetEnvironmentVariableA("MYGUI_QUADFIX", env, sizeof env);
    if (len == 1 && env[0] == '0') gEnabled = false;

    u8* base = (u8*)GetModuleHandleA("MyGUIEngine_x64.dll");
    if (!base) {
        logStr("MyGUIEngine_x64.dll not loaded, not patching\r\n");
        return;
    }
    logStr("MyGUIEngine_x64.dll base ");
    logHex((u64)base);
    logStr("\r\n");

    u8* func = base + kFuncRva;
    gOriginal = (QuadSetFn)func;
    u8* thunk = base + kThunkRva;
    u8* pad = base + kPadRva;

    if (!same(func, kFuncBytes, sizeof kFuncBytes)) {
        logStr("VertexQuad::set bytes differ (different MyGUI build?), not patching\r\n");
        return;
    }
    if (!same(thunk, kThunkBytes, sizeof kThunkBytes)) {
        logStr("thunk bytes differ (already patched?), not patching\r\n");
        return;
    }
    for (int i = 0; i < 14; ++i) {
        if (pad[i] != 0xcc) {
            logStr("padding is not int3, not patching\r\n");
            return;
        }
    }

    // pad: jmp qword ptr [rip+0] ; dq quadSet
    u8 jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    u64 target = (u64)&quadSet;
    for (int i = 0; i < 8; ++i) jump[6 + i] = (u8)(target >> (8 * i));
    if (!writeCode(pad, jump, sizeof jump)) {
        logStr("VirtualProtect failed on padding\r\n");
        return;
    }

    // thunk: jmp pad (rel32)
    int rel = (int)((long long)(pad - (thunk + 5)));
    u8 thunkNew[5] = {0xe9, (u8)rel, (u8)(rel >> 8), (u8)(rel >> 16), (u8)(rel >> 24)};
    if (!writeCode(thunk, thunkNew, sizeof thunkNew)) {
        logStr("VirtualProtect failed on thunk\r\n");
        return;
    }

    logStr("patched: VertexQuad::set -> ");
    logHex(target);
    logStr("\r\n");
    logToggle("start");
    reportStatus();
    perfctl::start(onControl);
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("MyGUIQuadFix.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("MyGUIQuadFix 1.3\r\n");
    install();
    // The log stays open for the perfctl toggle lines.
}
