// ShaderConstCache: RE_Kenshi plugin that caches the D3D11 render system's
// shader-constant name lookups.
//
// For every draw and shader stage, RenderSystem_Direct3D11 copies each shader
// variable's value into the constant buffer, and to find it calls
// Ogre::GpuProgramParameters::getConstantDefinition(name) per variable: a
// std::map<String> search (string compares, memcmp) behind a 0x1c8- and a 0x240-byte
// stack frame with security cookies. The answer never changes for a given program.
//
// This plugin swaps RenderSystem's import slot for getConstantDefinition (RVA
// 0x717f0; Kenshi's own calls use their own slot and are untouched) for a cached
// version keyed by (params->mNamedConstants at +0x128, address of the name string).
// The name strings sit in the program's reflected variable array, so their address
// is stable. Every hit is re-checked: the cached definition lives in a map node whose
// key string is 0x28 bytes before it, and that key must equal the requested name.
// The whole cache is dropped whenever Ogre destroys any GpuNamedConstants (a
// trampoline on its destructor), so freed tables are never used.
// On a miss the plugin asks _findNamedConstantDefinition(name, false) (no throw);
// if the name does not exist it tail-calls the original, which throws as before.
//
// perfctl.ini key "shadercache": 1 = on (default), 0 = every lookup goes to Ogre.
// Every 10 s ShaderConstCache.log reports lookups, hits and misses per frame.
// Built without a CRT (see build.sh).

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) void* __stdcall GetProcAddress(HANDLE, const char*);
__declspec(dllimport) int __stdcall VirtualProtect(void*, u64, u32, u32*);
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, u64, u32, u32);
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE, const void*, u64);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) u64 __stdcall GetTickCount64();
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
}

#include "../../common/perfctl.h"

namespace {

const u32 kIatRva = 0x717f0;  // RenderSystem_Direct3D11_x64.dll import slot
const char* const kGetDef =
    "?getConstantDefinition@GpuProgramParameters@Ogre@@QEBAAEBUGpuConstantDefinition@2@"
    "AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z";
const char* const kFindDef =
    "?_findNamedConstantDefinition@GpuProgramParameters@Ogre@@QEBAPEBUGpuConstantDefinition@2@"
    "AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z";
const char* const kDtor = "??1GpuNamedConstants@Ogre@@QEAA@XZ";
// GpuNamedConstants::~GpuNamedConstants prologue: push rbx (REX form); sub rsp,30h;
// mov qword [rsp+20h],-2 (15 bytes, position independent).
const int kDtorLen = 15;
const u8 kDtorBytes[kDtorLen] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7,
                                 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff};

typedef const void* (*GetDefFn)(const u8* params, const u8* name);
typedef const void* (*FindDefFn)(const u8* params, const u8* name, bool throwIfMissing);
typedef void (*DtorFn)(void* self);
GetDefFn gGetDef = 0;
FindDefFn gFindDef = 0;
DtorFn gDtorTramp = 0;
u8** gRootPtr = 0;  // &Ogre::Root::msSingleton; next frame number at +0x190

volatile bool gOn = true;
volatile u32 gGen = 1;  // bumped whenever a GpuNamedConstants is destroyed

struct Entry { const void* nc; const u8* name; const void* def; u32 gen; };
const u32 kEntries = 16384;
Entry gCache[kEntries];

u64 gLookups = 0, gHits = 0, gMisses = 0, gInvalidations = 0, gFrames = 0;
u32 gLastFrame = 0;
u64 gLastReport = 0;

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
void logRatio(u64 v, u64 d) {
    if (!d) { logStr("-"); return; }
    u64 x = (v * 10 + d / 2) / d;
    logUint(x / 10);
    char t[3] = {'.', (char)('0' + x % 10), 0};
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

// MSVC 2010 std::string: inline buffer (capacity < 16) or pointer at +0, size +0x10, capacity +0x18.
inline const char* strData(const u8* s) { return *(const u64*)(s + 0x18) >= 16 ? *(const char* const*)s : (const char*)s; }
inline u64 strSize(const u8* s) { return *(const u64*)(s + 0x10); }

bool sameString(const u8* a, const u8* b) {
    u64 n = strSize(a);
    if (n != strSize(b)) return false;
    const char* pa = strData(a);
    const char* pb = strData(b);
    for (u64 i = 0; i < n; ++i)
        if (pa[i] != pb[i]) return false;
    return true;
}

__declspec(noinline) void housekeeping() {
    u8* root = *gRootPtr;
    u32 frame = root ? *(u32*)(root + 0x190) : 0;
    if (frame == gLastFrame) return;
    gLastFrame = frame;
    ++gFrames;
    u64 now = GetTickCount64();
    if (now - gLastReport < 10000) return;
    if (gLastReport && gFrames) {
        logTime();
        logStr(gOn ? "[cache on]  " : "[cache off] ");
        logStr("frames ");
        logUint(gFrames);
        logStr(", lookups/frame ");
        logRatio(gLookups, gFrames);
        logStr(", hits/frame ");
        logRatio(gHits, gFrames);
        logStr(", misses/frame ");
        logRatio(gMisses, gFrames);
        logStr(", cache resets ");
        logUint(gInvalidations);
        logStr("\r\n");
    }
    gLookups = gHits = gMisses = gInvalidations = gFrames = 0;
    gLastReport = now;
}

// Replacement for GpuProgramParameters::getConstantDefinition (rcx = params, rdx = name).
const void* getDefHook(const u8* params, const u8* name) {
    housekeeping();
    ++gLookups;
    const void* nc = *(const void* const*)(params + 0x128);
    if (!gOn || !nc) return gGetDef(params, name);  // null: the original throws as before
    u64 key = ((u64)nc >> 4) * 0x9e3779b97f4a7c15ull ^ ((u64)name >> 3) * 0xc2b2ae3d27d4eb4full;
    Entry& e = gCache[(u32)(key >> 50) & (kEntries - 1)];
    u32 gen = gGen;
    if (e.nc == nc && e.name == name && e.gen == gen &&
        sameString((const u8*)e.def - 0x28, name)) {  // the map node's key
        ++gHits;
        return e.def;
    }
    ++gMisses;
    const void* def = gFindDef(params, name, false);
    if (!def) return gGetDef(params, name);  // missing name: let Ogre throw
    e.nc = nc;
    e.name = name;
    e.def = def;
    e.gen = gen;
    return def;
}

void dtorHook(void* self) {
    ++gGen;  // drop every cached entry
    ++gInvalidations;
    gDtorTramp(self);
}

void reportStatus() {
    perfctl::Text t;
    t.add("shadercache=").num(gOn ? 1 : 0).add("\n");
    perfctl::status("shadercache", t.b);
}

void onControl(const char* key, int value) {
    if (!perfctl::keyIs(key, "shadercache") || (value != 0) == gOn) return;
    gOn = value != 0;
    ++gGen;
    logTime();
    logStr(gOn ? "perfctl: cache ON\r\n" : "perfctl: cache OFF\r\n");
    reportStatus();
}

bool writeCode(u8* dst, const u8* src, int n, u32 prot) {
    u32 old;
    if (!VirtualProtect(dst, n, prot, &old)) return false;
    for (int i = 0; i < n; ++i) dst[i] = src[i];
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

void install() {
    char env[8];
    u32 len = GetEnvironmentVariableA("KENSHI_SHADERCACHE", env, sizeof env);
    if (len == 1 && env[0] == '0') gOn = false;

    HANDLE ogre = GetModuleHandleA("OgreMain_x64.dll");
    u8* rs = (u8*)GetModuleHandleA("RenderSystem_Direct3D11_x64.dll");
    if (!ogre || !rs) { logStr("OgreMain or RenderSystem_Direct3D11 not loaded, not patching\r\n"); return; }
    gGetDef = (GetDefFn)GetProcAddress(ogre, kGetDef);
    gFindDef = (FindDefFn)GetProcAddress(ogre, kFindDef);
    u8* dtor = (u8*)GetProcAddress(ogre, kDtor);
    gRootPtr = (u8**)GetProcAddress(ogre, "?msSingleton@?$Singleton@VRoot@Ogre@@@Ogre@@1PEAVRoot@2@EA");
    if (!gGetDef || !gFindDef || !dtor || !gRootPtr) { logStr("OgreMain exports missing, not patching\r\n"); return; }
    void** slot = (void**)(rs + kIatRva);
    if (*slot != (void*)gGetDef) { logStr("RenderSystem import slot does not hold getConstantDefinition, not patching\r\n"); return; }
    for (int i = 0; i < kDtorLen; ++i)
        if (dtor[i] != kDtorBytes[i]) { logStr("GpuNamedConstants destructor bytes differ, not patching\r\n"); return; }

    // Destructor trampoline: the 15 original bytes, then jmp back to dtor+15.
    u8* tramp = (u8*)VirtualAlloc(0, 4096, 0x3000, 0x40);
    if (!tramp) { logStr("VirtualAlloc failed\r\n"); return; }
    for (int i = 0; i < kDtorLen; ++i) tramp[i] = kDtorBytes[i];
    u8* back = tramp + kDtorLen;
    back[0] = 0xff; back[1] = 0x25; back[2] = back[3] = back[4] = back[5] = 0;
    u64 ret = (u64)(dtor + kDtorLen);
    for (int i = 0; i < 8; ++i) back[6 + i] = (u8)(ret >> (8 * i));
    gDtorTramp = (DtorFn)tramp;
    u8 jmp[kDtorLen] = {0xff, 0x25, 0, 0, 0, 0};
    u64 target = (u64)&dtorHook;
    for (int i = 0; i < 8; ++i) jmp[6 + i] = (u8)(target >> (8 * i));
    jmp[14] = 0xcc;  // the leftover byte is never executed
    if (!writeCode(dtor, jmp, kDtorLen, 0x40)) { logStr("VirtualProtect failed on destructor\r\n"); return; }

    void* hook = (void*)&getDefHook;
    if (!writeCode((u8*)slot, (const u8*)&hook, 8, 0x04)) { logStr("VirtualProtect failed on import slot\r\n"); return; }
    logTime();
    logStr(gOn ? "hooked RenderSystem getConstantDefinition + GpuNamedConstants destructor, cache ON\r\n"
               : "hooked RenderSystem getConstantDefinition + GpuNamedConstants destructor, cache OFF\r\n");
    reportStatus();
    perfctl::start(onControl);
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("ShaderConstCache.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("ShaderConstCache 1.0\r\n");
    install();
}
