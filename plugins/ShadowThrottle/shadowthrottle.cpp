// ShadowThrottle: RE_Kenshi plugin that re-renders Kenshi's shadow maps only
// every Nth frame instead of every frame.
//
// Kenshi's DeferredLightRenderer renders shadows from one call site
// (Kenshi_x64.exe 0x1402d8634), branching on its shadow mode:
//   mode 2 (RTW):  0x140865ca0 prepare, 0x140864d80 setup, 0x140865010 render pass
//   mode 1 (CSM):  0x140863cb0 renders all cascades into one atlas
// Each is reached through its own incremental-link thunk with no other callers.
//
// On a skipped frame none of these run, so the shadow texture, the light/warp
// matrices and the shader parameters all stay together from the last update.
//
// Kenshi's deferred shader looks shadows up from a camera-relative position
// (worldPos = viewPos * inverseView, camera at the origin), so the shadow
// matrix (shadowViewMat for CSM, shadow_matrix for RTW) has the camera position
// baked in. On a skipped frame the plugin rewrites only its translation:
//   M' = M_saved * Translate(camNow - camAtRender)
// which re-aligns the old shadow map with the moved camera. The matrix pointer
// is found by hooking Kenshi's imports of GpuSharedParameters::
// getConstantDefinition/getFloatPointer, which is how Kenshi writes it.
// Skipping only part of the update would not be safe: the CSM cascades share
// one view matrix and Ogre's D3D11 clear wipes the whole shadow atlas.
//
// Target: RE_Kenshi's Kenshi_x64.exe 1.0.65 (Steam)
//   sha256 504b362cde850d56afb1cea6f5b7b0ee014d9dd7b47e188599d91c804502cd3e
// Every patched byte is checked
// first; on any mismatch nothing is touched.
//
// Built without a CRT (see build.sh).
// KENSHI_SHADOW_INTERVAL=N renders shadows every N frames (default 3, 1 = start with
// throttling off). KENSHI_SHADOW_CORRECT=0 disables the camera-translation fix.
// At runtime, perfctl.ini (see tools/perfctl.py) can change both:
// "shadow_interval=N" (1 = throttling off) and "shadow_correct=0/1". Every change is
// written with a local timestamp to ShadowThrottle.log, to line up with MangoHud
// logs, and reported in perfctl-status-shadow.txt.

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
__declspec(dllimport) void* __stdcall GetProcAddress(HANDLE, const char*);
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
}

#include "../../common/perfctl.h"

namespace {

// RVAs in Kenshi_x64.exe
const u64 kStubPadRva = 0x860392;  // 174 bytes of int3 padding; 4 stubs x 16 bytes
const u64 kIatGetConstDef = 0x2247048;   // GpuSharedParameters::getConstantDefinition
const u64 kIatGetFloatPtr = 0x2247040;   // GpuSharedParameters::getFloatPointer
const u64 kIatCamDerivedPos = 0x2247238; // Camera::getDerivedPosition

struct Target {
    const char* name;
    u64 thunkRva;     // e9 rel32 -> function
    u64 funcRva;
    u8 funcBytes[8];  // first bytes of the function, checked before patching
};

const Target kTargets[4] = {
    {"RTW prepare", 0x30698, 0x865ca0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55}},
    {"RTW setup",   0x0462e, 0x864d80, {0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x41, 0x55}},
    {"RTW pass",    0x3c867, 0x865010, {0x4c, 0x8b, 0xdc, 0x57, 0x41, 0x54, 0x41, 0x55}},
    {"CSM render",  0x14a2e, 0x863cb0, {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57}},
};

typedef void (*Fn3)(void*, void*, void*);
typedef void (*Fn1)(void*);
typedef void* (*GetConstDefFn)(void* params, const void* name);  // returns GpuConstantDefinition*
typedef float* (*GetFloatPtrFn)(void* params, u64 index);
typedef const float* (*CamPosFn)(void* camera);
typedef void (*MarkDirtyFn)(void* params);

GetConstDefFn gGetConstDef = 0;
GetFloatPtrFn gGetFloatPtr = 0;
CamPosFn gCamPos = 0;
MarkDirtyFn gMarkDirty = 0;

// One per shadow mode: 0 = RTW (shadow_matrix), 1 = CSM (shadowViewMat).
struct MatState {
    float* ptr;        // live 4x4 row-major matrix in the shared params
    void* params;      // GpuSharedParameters owning it
    float saved[16];   // matrix as Kenshi wrote it on the last rendered frame
    float cam[3];      // camera position on that frame
    bool valid;
};
MatState gMat[2] = {};

// Set while a real (non-skipped) shadow update runs, so the hooks only
// capture the matrix pointer from Kenshi's own shadow code.
int gInUpdate = -1;
int gPendingMode = -1;
void* gPendingParams = 0;
u64 gPendingIndex = 0;
u32 gOutOfBandWrites = 0;  // matrix fetched outside a shadow update (diagnostic)

// MSVC 2010 std::string: 16-byte buffer/pointer, size at +0x10, capacity at +0x18.
bool strEquals(const void* str, const char* lit) {
    const u8* s = (const u8*)str;
    u64 size = *(const u64*)(s + 0x10);
    u64 cap = *(const u64*)(s + 0x18);
    const char* chars = cap >= 16 ? *(const char* const*)s : (const char*)s;
    u64 i = 0;
    for (; lit[i]; ++i)
        if (i >= size || chars[i] != lit[i]) return false;
    return i == size;
}

void* hookGetConstDef(void* params, const void* name) {
    void* def = gGetConstDef(params, name);
    int mode = strEquals(name, "shadow_matrix") ? 0 : strEquals(name, "shadowViewMat") ? 1 : -1;
    if (mode >= 0) {
        gPendingMode = mode;
        gPendingParams = params;
        gPendingIndex = *(const u64*)((const u8*)def + 8);  // GpuConstantDefinition::physicalIndex
    }
    return def;
}

float* hookGetFloatPtr(void* params, u64 index) {
    float* p = gGetFloatPtr(params, index);
    if (gPendingMode >= 0 && params == gPendingParams && index == gPendingIndex) {
        if (gInUpdate == gPendingMode) {
            gMat[gPendingMode].ptr = p;
            gMat[gPendingMode].params = params;
        } else {
            ++gOutOfBandWrites;
        }
        gPendingMode = -1;
    }
    return p;
}

// After a real update: remember what Kenshi wrote and where the camera was.
void saveMatrix(int mode, void* camera) {
    MatState& m = gMat[mode];
    if (!m.ptr || !camera) { m.valid = false; return; }
    for (int i = 0; i < 16; ++i) m.saved[i] = m.ptr[i];
    const float* c = gCamPos(camera);
    m.cam[0] = c[0]; m.cam[1] = c[1]; m.cam[2] = c[2];
    m.valid = true;
}

// On a skipped frame: M' = M_saved * Translate(camNow - camAtRender).
void correctMatrix(int mode, void* camera) {
    MatState& m = gMat[mode];
    if (!m.valid || !camera) return;
    const float* c = gCamPos(camera);
    float d0 = c[0] - m.cam[0], d1 = c[1] - m.cam[1], d2 = c[2] - m.cam[2];
    for (int r = 0; r < 4; ++r) {
        const float* row = m.saved + 4 * r;
        m.ptr[4 * r + 0] = row[0];
        m.ptr[4 * r + 1] = row[1];
        m.ptr[4 * r + 2] = row[2];
        m.ptr[4 * r + 3] = row[0] * d0 + row[1] * d1 + row[2] * d2 + row[3];
    }
    if (gMarkDirty) gMarkDirty(m.params);
}

u8* gExe = 0;
// Written by the perfctl thread, read once per frame by the wrappers.
volatile u32 gInterval = 3;
volatile bool gActive = true;  // throttling on; when off every frame renders

Fn3 gRtwPrepare = 0;
Fn1 gRtwSetup = 0;
Fn1 gRtwPass = 0;
Fn3 gCsmRender = 0;

u64 gRtwFrame = 0, gCsmFrame = 0;
void* gRtwLast = 0;
void* gCsmLast = 0;
bool gRtwRender = true;
void* gRtwCamera = 0;
volatile bool gCorrect = true;  // KENSHI_SHADOW_CORRECT=0 turns the matrix fix off

void logStatus(const char* why);

// The three RTW calls run back to back from the same call site, so the
// decision is made in the first one and reused by the other two.
// Arguments: self = shadow renderer, light, camera (the main view camera).
void rtwPrepare(void* self, void* light, void* camera) {
    gRtwCamera = camera;
    gRtwRender = !gActive || !gMat[0].valid || (gRtwFrame++ % gInterval) == 0 || self != gRtwLast;
    gRtwLast = self;
    if (gRtwRender) {
        gInUpdate = 0;
        gRtwPrepare(self, light, camera);
    } else if (gCorrect) {
        correctMatrix(0, camera);
    }
}
void rtwSetup(void* self) {
    if (gRtwRender) gRtwSetup(self);
}
void rtwPass(void* self) {
    if (!gRtwRender) return;
    gRtwPass(self);
    gInUpdate = -1;
    saveMatrix(0, gRtwCamera);
}
void csmRender(void* self, void* light, void* camera) {
    bool render = !gActive || !gMat[1].valid || (gCsmFrame++ % gInterval) == 0 || self != gCsmLast;
    gCsmLast = self;
    if (render) {
        gInUpdate = 1;
        gCsmRender(self, light, camera);
        gInUpdate = -1;
        saveMatrix(1, camera);
    } else if (gCorrect) {
        correctMatrix(1, camera);
    }
}

void* const kWrappers[4] = {(void*)&rtwPrepare, (void*)&rtwSetup, (void*)&rtwPass, (void*)&csmRender};

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

void logUint(u32 v) {
    char buf[11];
    int i = 10;
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

void logStatus(const char* why) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    logPad(t.h, 2); logStr(":"); logPad(t.mi, 2); logStr(":"); logPad(t.s, 2); logStr(".");
    logPad(t.ms, 3);
    logStr(" ");
    logStr(why);
    logStr(": throttle ");
    logStr(gActive ? "ON" : "OFF");
    logStr(", interval ");
    logUint(gInterval);
    logStr(", matrix RTW ");
    logStr(gMat[0].valid ? "ok" : "-");
    logStr(" CSM ");
    logStr(gMat[1].valid ? "ok" : "-");
    logStr(", correction ");
    logStr(gCorrect ? "on" : "off");
    logStr(", out-of-band fetches ");
    logUint(gOutOfBandWrites);
    logStr("\r\n");
}

void reportStatus() {
    perfctl::Text t;
    t.add("shadow_interval=").num(gActive ? (int)gInterval : 1).add("\n");
    t.add("shadow_correct=").num(gCorrect ? 1 : 0).add("\n");
    perfctl::status("shadow", t.b);
}

// perfctl thread: "shadow_interval=N" (1 = off, 2-16) and "shadow_correct=0/1".
void onControl(const char* key, int value) {
    if (perfctl::keyIs(key, "shadow_interval")) {
        if (value < 1 || value > 16) return;
        bool active = value >= 2;
        if (active == gActive && (!active || (u32)value == gInterval)) return;
        if (active) gInterval = (u32)value;
        gActive = active;
    } else if (perfctl::keyIs(key, "shadow_correct")) {
        if ((value != 0) == gCorrect) return;
        gCorrect = value != 0;
    } else {
        return;
    }
    logStatus("perfctl");
    reportStatus();
}

bool writeCode(u8* dst, const u8* src, int n) {
    u32 old;
    if (!VirtualProtect(dst, n, 0x40 /* PAGE_EXECUTE_READWRITE */, &old)) return false;
    for (int i = 0; i < n; ++i) dst[i] = src[i];
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

// The thunk must be exactly "jmp func".
bool thunkPointsTo(const u8* thunk, const u8* func) {
    if (thunk[0] != 0xe9) return false;
    int rel = (int)(thunk[1] | (thunk[2] << 8) | (thunk[3] << 16) | ((u32)thunk[4] << 24));
    return thunk + 5 + rel == func;
}

void install() {
    char env[16];
    u32 len = GetEnvironmentVariableA("KENSHI_SHADOW_INTERVAL", env, sizeof env);
    if (len > 0 && len < sizeof env) {
        u32 v = 0;
        for (u32 i = 0; i < len && env[i] >= '0' && env[i] <= '9'; ++i) v = v * 10 + (env[i] - '0');
        if (v == 1) gActive = false;
        else if (v >= 2 && v <= 16) gInterval = v;
    }

    gExe = (u8*)GetModuleHandleA(0);
    if (!gExe) {
        logStr("no exe module, not patching\r\n");
        return;
    }
    logStr("exe base ");
    logHex((u64)gExe);
    logStr("\r\n");

    // Verify everything before writing anything.
    u8* pad = gExe + kStubPadRva;
    for (int i = 0; i < 16 * 4; ++i) {
        if (pad[i] != 0xcc) {
            logStr("stub padding is not int3 (different Kenshi build?), not patching\r\n");
            return;
        }
    }
    for (int t = 0; t < 4; ++t) {
        const Target& tg = kTargets[t];
        u8* thunk = gExe + tg.thunkRva;
        u8* func = gExe + tg.funcRva;
        if (!thunkPointsTo(thunk, func)) {
            logStr(tg.name);
            logStr(": thunk does not jump to the expected function (different build or already patched), not patching\r\n");
            return;
        }
        {
            for (int i = 0; i < 8; ++i) {
                if (func[i] != tg.funcBytes[i]) {
                    logStr(tg.name);
                    logStr(": function bytes differ, not patching\r\n");
                    return;
                }
            }
        }
    }

    len = GetEnvironmentVariableA("KENSHI_SHADOW_CORRECT", env, sizeof env);
    if (len == 1 && env[0] == '0') gCorrect = false;

    HANDLE ogre = GetModuleHandleA("OgreMain_x64.dll");
    gMarkDirty = ogre ? (MarkDirtyFn)GetProcAddress(ogre, "?_markDirty@GpuSharedParameters@Ogre@@QEAAXXZ") : 0;
    void** iatConstDef = (void**)(gExe + kIatGetConstDef);
    void** iatFloatPtr = (void**)(gExe + kIatGetFloatPtr);
    void** iatCamPos = (void**)(gExe + kIatCamDerivedPos);
    void* expConstDef = ogre ? GetProcAddress(ogre, "?getConstantDefinition@GpuSharedParameters@Ogre@@QEBAAEBUGpuConstantDefinition@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z") : 0;
    void* expFloatPtr = ogre ? GetProcAddress(ogre, "?getFloatPointer@GpuSharedParameters@Ogre@@QEAAPEAM_K@Z") : 0;
    void* expCamPos = ogre ? GetProcAddress(ogre, "?getDerivedPosition@Camera@Ogre@@QEBAAEBVVector3@2@XZ") : 0;
    if (!gMarkDirty || !expConstDef || !expFloatPtr || !expCamPos ||
        *iatConstDef != expConstDef || *iatFloatPtr != expFloatPtr || *iatCamPos != expCamPos) {
        logStr("import table entries don't match OgreMain exports (different build?), not patching\r\n");
        return;
    }
    gGetConstDef = (GetConstDefFn)expConstDef;
    gGetFloatPtr = (GetFloatPtrFn)expFloatPtr;
    gCamPos = (CamPosFn)expCamPos;

    gRtwPrepare = (Fn3)(gExe + kTargets[0].funcRva);
    gRtwSetup = (Fn1)(gExe + kTargets[1].funcRva);
    gRtwPass = (Fn1)(gExe + kTargets[2].funcRva);
    gCsmRender = (Fn3)(gExe + kTargets[3].funcRva);

    // Stubs first (never executed until a thunk points at them), then thunks.
    for (int t = 0; t < 4; ++t) {
        u8 stub[16] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword ptr [rip+0]
        u64 target = (u64)kWrappers[t];
        for (int i = 0; i < 8; ++i) stub[6 + i] = (u8)(target >> (8 * i));
        stub[14] = 0xcc;
        stub[15] = 0xcc;
        if (!writeCode(pad + 16 * t, stub, sizeof stub)) {
            logStr("VirtualProtect failed on stub padding\r\n");
            return;
        }
    }
    // Import hooks: one aligned 8-byte pointer write each.
    void* hookConst = (void*)&hookGetConstDef;
    void* hookFloat = (void*)&hookGetFloatPtr;
    if (!writeCode((u8*)iatConstDef, (const u8*)&hookConst, 8) ||
        !writeCode((u8*)iatFloatPtr, (const u8*)&hookFloat, 8)) {
        logStr("VirtualProtect failed on import table\r\n");
        return;
    }
    logStr("hooked: getConstantDefinition, getFloatPointer\r\n");

    for (int t = 0; t < 4; ++t) {
        u8* thunk = gExe + kTargets[t].thunkRva;
        u8* stub = pad + 16 * t;
        int rel = (int)((long long)(stub - (thunk + 5)));
        u8 jmp[5] = {0xe9, (u8)rel, (u8)(rel >> 8), (u8)(rel >> 16), (u8)(rel >> 24)};
        if (!writeCode(thunk, jmp, sizeof jmp)) {
            logStr("VirtualProtect failed on thunk\r\n");
            return;
        }
        logStr("patched: ");
        logStr(kTargets[t].name);
        logStr("\r\n");
    }
    logStatus("start");
    reportStatus();
    perfctl::start(onControl);
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("ShadowThrottle.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("ShadowThrottle 1.5\r\n");
    install();
    // The log stays open for the perfctl status lines.
}
