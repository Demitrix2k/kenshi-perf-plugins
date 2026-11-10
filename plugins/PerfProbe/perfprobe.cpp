// PerfProbe: diagnostic RE_Kenshi plugin that times every Ogre scene pass by camera.
//
// Hooks OgreMain's Camera::_cullScenePhase01 and Camera::_renderScenePhase02 (every
// scene render, whether main view, shadows, reflections or render-to-texture, goes
// through them) with trampolines, and records per camera name and render-queue
// range (Kenshi renders the main camera in several queue-range passes per frame):
// calls, cull time and render time.
// Times are exclusive: a pass started inside another pass is subtracted from its parent.
// Every 5 seconds a table is appended to PerfProbe.log in the game folder with each
// pass's share of wall time and ms per frame (frame = the most frequent pass).
//
// 1.2 also hooks SceneManager::renderSingleObject (one call per object draw) and
// groups its time by what is drawn: SubEntity draws by mesh name, everything else
// by its MSVC RTTI class name. Split by shadow passes vs all other passes.
//
// Does nothing unless KENSHI_PERF_PROBE=1 is set. Target: OgreMain_x64.dll shipped
// with Kenshi 1.0.65; prologue bytes are checked before patching.
// Built without a CRT (see build.sh).

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) void* __stdcall GetProcAddress(HANDLE, const char*);
__declspec(dllimport) int __stdcall VirtualProtect(void*, u64, u32, u32*);
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, u64, u32, u32);
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE, const void*, u64);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(i64*);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(i64*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT

// The optimiser turns byte loops into these calls; there is no CRT to provide them.
// volatile keeps it from turning these bodies back into calls to themselves.
void* memcpy(void* dst, const void* src, u64 n) {
    volatile u8* d = (volatile u8*)dst;
    const u8* s = (const u8*)src;
    for (u64 i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
void* memset(void* dst, int v, u64 n) {
    volatile u8* d = (volatile u8*)dst;
    for (u64 i = 0; i < n; ++i) d[i] = (u8)v;
    return dst;
}
}

namespace {

const char* const kCullName = "?_cullScenePhase01@Camera@Ogre@@QEAAXPEBV12@PEAVViewport@2@EE@Z";
const char* const kRenderName = "?_renderScenePhase02@Camera@Ogre@@QEAAXPEBV12@PEAVViewport@2@EE_N@Z";
const char* const kGetNameName =
    "?getName@MovableObject@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ";

// Prologues to relocate into the trampolines (position independent, verified).
const u8 kCullPrologue[18] = {0x48, 0x8b, 0xc4, 0x41, 0x54, 0x48, 0x83, 0xec, 0x60,
                              0x48, 0xc7, 0x44, 0x24, 0x30, 0xfe, 0xff, 0xff, 0xff};
const u8 kRenderPrologue[15] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x70,
                                0x48, 0xc7, 0x44, 0x24, 0x40, 0xfe, 0xff, 0xff, 0xff};

typedef void (*CullFn)(void* cam, const void* lodCam, void* vp, u8 firstRq, u8 lastRq);
typedef void (*RenderFn)(void* cam, const void* lodCam, void* vp, u8 firstRq, u8 lastRq, bool includeOverlays);
typedef const void* (*GetNameFn)(const void* movable);
typedef void (*RsoFn)(void* sceneMgr, void* rend, const void* pass, bool scissor, bool lightIter);
typedef void* (*GetParentFn)(const void* subEntity);
typedef const void* (*GetMeshFn)(const void* entity);       // returns const MeshPtr&
typedef const void* (*ResGetNameFn)(const void* resource);  // returns const String&

const char* const kRsoName = "?renderSingleObject@SceneManager@Ogre@@IEAAXPEAVRenderable@2@PEBVPass@2@_N2@Z";
const u8 kRsoPrologue[14] = {0x4c, 0x8b, 0xdc, 0x45, 0x88, 0x4b, 0x20, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};

RsoFn gRso = 0;
GetParentFn gGetParent = 0;
GetMeshFn gGetMesh = 0;
ResGetNameFn gResGetName = 0;
void* gSubEntityVt = 0;
bool gInShadowPass = false;

CullFn gCull = 0;      // trampoline -> original
RenderFn gRender = 0;  // trampoline -> original
GetNameFn gGetName = 0;

HANDLE gLog = 0;
i64 gFreq = 1;

struct Entry {
    void* cam;
    u8 firstRq, lastRq;
    char name[40];
    u32 cullCalls, renderCalls;
    i64 cullTicks, renderTicks;  // exclusive
};
const int kMaxEntries = 64;
Entry gEntries[kMaxEntries];
int gNumEntries = 0;

// Nesting: time spent in child passes is subtracted from the parent.
const int kMaxDepth = 16;
i64 gChildTicks[kMaxDepth];
int gDepth = 0;

i64 gWindowStart = 0;

// Per-object stats, keyed by Mesh* (SubEntity draws) or vtable (other renderables).
struct Obj {
    const void* key;
    char name[48];
    u32 calls[2];   // [0] non-shadow passes, [1] shadow passes
    i64 ticks[2];
};
const int kMaxObjs = 2048;
Obj gObjs[kMaxObjs];
int gNumObjs = 0;
int gObjIndex[4096];  // open addressing: index+1, 0 = empty

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

// Fixed-point milliseconds with 3 decimals.
void logMs(i64 ticks, u64 divisor) {
    if (divisor == 0) divisor = 1;
    u64 us = (u64)(ticks * 1000000 / gFreq) / divisor;  // microseconds
    logUint(us / 1000);
    logStr(".");
    u64 frac = us % 1000;
    char b[4] = {(char)('0' + frac / 100), (char)('0' + frac / 10 % 10), (char)('0' + frac % 10), 0};
    logStr(b);
}

// Percentage with one decimal.
void logPct(i64 part, i64 whole) {
    if (whole <= 0) whole = 1;
    u64 p10 = (u64)(part * 1000 / whole);
    logUint(p10 / 10);
    char b[4] = {'.', (char)('0' + p10 % 10), '%', 0};
    logStr(b);
}

void logPadded(const char* s, int width) {
    int n = 0;
    while (s[n]) ++n;
    logStr(s);
    char sp[2] = {' ', 0};
    for (; n < width; ++n) logStr(sp);
}

// MSVC 2010 std::string: 16-byte buffer/pointer, size at +0x10, capacity at +0x18.
void copyName(const void* str, char* out, int outSize) {
    const u8* s = (const u8*)str;
    u64 size = *(const u64*)(s + 0x10);
    u64 cap = *(const u64*)(s + 0x18);
    const char* chars = cap >= 16 ? *(const char* const*)s : (const char*)s;
    int i = 0;
    for (; i < outSize - 1 && (u64)i < size; ++i) out[i] = chars[i] ? chars[i] : '?';
    if (i == 0) { out[0] = '('; out[1] = 'n'; out[2] = 'o'; out[3] = ')'; i = 4; }
    out[i] = 0;
}

void copyCStr(const char* src, char* out, int outSize) {
    int i = 0;
    for (; i < outSize - 1 && src[i]; ++i) out[i] = src[i];
    out[i] = 0;
}

// MSVC x64 RTTI: vtable[-1] -> CompleteObjectLocator {sig=1, off, cdOff, typeDescRva,
// classDescRva, selfRva}; TypeDescriptor name at +0x10 (".?AVName@Ns@@").
void rttiName(const void* vt, char* out, int outSize) {
    const u8* col = *((const u8* const*)vt - 1);
    if (!col || *(const u32*)col != 1) { copyCStr("(no rtti)", out, outSize); return; }
    const u8* base = col - *(const u32*)(col + 0x14);
    const char* nm = (const char*)(base + *(const u32*)(col + 0xc) + 0x10);
    if (nm[0] == '.' && nm[1] == '?' && nm[2] == 'A' && (nm[3] == 'V' || nm[3] == 'U')) nm += 4;
    copyCStr(nm, out, outSize);
}

Obj* objFor(void* rend) {
    const void* vt = *(const void* const*)rend;
    const void* key = vt;
    const void* mesh = 0;
    if (vt == gSubEntityVt) {
        void* ent = gGetParent(rend);
        const void* meshPtr = ent ? gGetMesh(ent) : 0;
        mesh = meshPtr ? *(const void* const*)meshPtr : 0;  // SharedPtr::pRep
        if (mesh) key = mesh;
    }
    u64 h = ((u64)key >> 4) * 0x9e3779b97f4a7c15ull;
    u32 slot = (u32)(h >> 52);  // 12 bits
    for (int probe = 0; probe < 4096; ++probe, slot = (slot + 1) & 4095) {
        int idx = gObjIndex[slot];
        if (idx == 0) {
            if (gNumObjs == kMaxObjs) return &gObjs[kMaxObjs - 1];
            Obj* o = &gObjs[gNumObjs++];
            gObjIndex[slot] = gNumObjs;
            o->key = key;
            if (mesh) {
                copyName(gResGetName(mesh), o->name, sizeof o->name);
            } else {
                o->name[0] = '[';
                rttiName(vt, o->name + 1, sizeof o->name - 2);
                int n = 0;
                while (o->name[n]) ++n;
                o->name[n] = ']';
                o->name[n + 1] = 0;
            }
            return o;
        }
        if (gObjs[idx - 1].key == key) return &gObjs[idx - 1];
    }
    return &gObjs[kMaxObjs - 1];
}

void rsoHook(void* sceneMgr, void* rend, const void* pass, bool scissor, bool lightIter) {
    i64 t0, t1;
    QueryPerformanceCounter(&t0);
    gRso(sceneMgr, rend, pass, scissor, lightIter);
    QueryPerformanceCounter(&t1);
    Obj* o = objFor(rend);
    int k = gInShadowPass ? 1 : 0;
    ++o->calls[k];
    o->ticks[k] += t1 - t0;
}

Entry* entryFor(void* cam, u8 firstRq, u8 lastRq) {
    for (int i = 0; i < gNumEntries; ++i)
        if (gEntries[i].cam == cam && gEntries[i].firstRq == firstRq && gEntries[i].lastRq == lastRq)
            return &gEntries[i];
    if (gNumEntries == kMaxEntries) return &gEntries[kMaxEntries - 1];  // overflow bucket
    Entry* e = &gEntries[gNumEntries++];
    e->cam = cam;
    e->firstRq = firstRq;
    e->lastRq = lastRq;
    copyName(gGetName(cam), e->name, sizeof e->name);
    e->cullCalls = e->renderCalls = 0;
    e->cullTicks = e->renderTicks = 0;
    return e;
}

void report(i64 now) {
    u32 frames = 0;
    for (int i = 0; i < gNumEntries; ++i)
        if (gEntries[i].renderCalls > frames) frames = gEntries[i].renderCalls;
    i64 wall = now - gWindowStart;
    i64 total = 0;
    for (int i = 0; i < gNumEntries; ++i) total += gEntries[i].cullTicks + gEntries[i].renderTicks;

    logStr("---- window ");
    logMs(wall, 1);
    logStr(" ms, frames ");
    logUint(frames);
    logStr(", avg frame ");
    logMs(wall, frames);
    logStr(" ms, scene passes ");
    logPct(total, wall);
    logStr(" of wall time (");
    logMs(total, frames);
    logStr(" ms/frame)\r\n");
    logStr("camera                    rq range   calls/frame  cull %   render %   total %   total ms/f\r\n");
    for (int i = 0; i < gNumEntries; ++i) {
        const Entry& e = gEntries[i];
        if (!e.cullCalls && !e.renderCalls) continue;
        logPadded(e.name, 26);
        char rq[12];
        int n = 0;
        u32 a = e.firstRq, b = e.lastRq;
        rq[n++] = (char)('0' + a / 100); rq[n++] = (char)('0' + a / 10 % 10); rq[n++] = (char)('0' + a % 10);
        rq[n++] = '-';
        rq[n++] = (char)('0' + b / 100); rq[n++] = (char)('0' + b / 10 % 10); rq[n++] = (char)('0' + b % 10);
        rq[n] = 0;
        logPadded(rq, 11);
        u32 calls = e.renderCalls > e.cullCalls ? e.renderCalls : e.cullCalls;
        u64 cpf100 = frames ? (u64)calls * 100 / frames : 0;
        logUint(cpf100 / 100);
        logStr(".");
        char c2[3] = {(char)('0' + cpf100 % 100 / 10), (char)('0' + cpf100 % 10), 0};
        logStr(c2);
        logStr("         ");
        logPct(e.cullTicks, wall);
        logStr("    ");
        logPct(e.renderTicks, wall);
        logStr("     ");
        logPct(e.cullTicks + e.renderTicks, wall);
        logStr("    ");
        logMs(e.cullTicks + e.renderTicks, frames);
        logStr("\r\n");
    }
    // Objects: every row with time (top 40 every window, all rows every 4th window).
    i64 objTotal[2] = {0, 0};
    for (int i = 0; i < gNumObjs; ++i) { objTotal[0] += gObjs[i].ticks[0]; objTotal[1] += gObjs[i].ticks[1]; }
    logStr("object draws (renderSingleObject): main passes ");
    logPct(objTotal[0], wall);
    logStr(", shadow passes ");
    logPct(objTotal[1], wall);
    logStr(" of wall time; ");
    logUint(gNumObjs);
    logStr(" distinct meshes/types\r\n");
    logStr("mesh or [type]                                  main %   shadow %  draws/s main  draws/s shadow\r\n");
    bool shown[kMaxObjs];
    for (int i = 0; i < gNumObjs; ++i) shown[i] = false;
    static u32 sWindow = 0;
    int rows = (++sWindow % 4 == 0) ? kMaxObjs : 40;
    if (rows > 40) logStr("(full list)\r\n");
    for (int r = 0; r < rows; ++r) {
        int best = -1;
        for (int i = 0; i < gNumObjs; ++i)
            if (!shown[i] && (best < 0 || gObjs[i].ticks[0] + gObjs[i].ticks[1] > gObjs[best].ticks[0] + gObjs[best].ticks[1]))
                best = i;
        if (best < 0 || gObjs[best].ticks[0] + gObjs[best].ticks[1] == 0) break;
        shown[best] = true;
        const Obj& o = gObjs[best];
        logPadded(o.name, 48);
        logPct(o.ticks[0], wall);
        logStr("    ");
        logPct(o.ticks[1], wall);
        logStr("     ");
        logUint((u64)o.calls[0] * gFreq / (u64)(wall > 0 ? wall : 1));
        logStr("          ");
        logUint((u64)o.calls[1] * gFreq / (u64)(wall > 0 ? wall : 1));
        logStr("\r\n");
    }
    for (int i = 0; i < gNumObjs; ++i) {
        gObjs[i].calls[0] = gObjs[i].calls[1] = 0;
        gObjs[i].ticks[0] = gObjs[i].ticks[1] = 0;
    }

    // Reset counters, keep camera identities.
    for (int i = 0; i < gNumEntries; ++i) {
        gEntries[i].cullCalls = gEntries[i].renderCalls = 0;
        gEntries[i].cullTicks = gEntries[i].renderTicks = 0;
    }
    gWindowStart = now;
}

void enter() {
    if (gDepth < kMaxDepth) gChildTicks[gDepth] = 0;
    ++gDepth;
}

// Returns exclusive ticks for the pass that just ended and credits the parent.
i64 leave(i64 inclusive) {
    --gDepth;
    i64 child = gDepth < kMaxDepth ? gChildTicks[gDepth] : 0;
    if (gDepth > 0 && gDepth - 1 < kMaxDepth) gChildTicks[gDepth - 1] += inclusive;
    return inclusive - child;
}

void cullHook(void* cam, const void* lodCam, void* vp, u8 firstRq, u8 lastRq) {
    i64 t0, t1;
    QueryPerformanceCounter(&t0);
    enter();
    gCull(cam, lodCam, vp, firstRq, lastRq);
    QueryPerformanceCounter(&t1);
    i64 excl = leave(t1 - t0);
    Entry* e = entryFor(cam, firstRq, lastRq);
    ++e->cullCalls;
    e->cullTicks += excl;
}

void renderHook(void* cam, const void* lodCam, void* vp, u8 firstRq, u8 lastRq, bool includeOverlays) {
    i64 t0, t1;
    Entry* pe = entryFor(cam, firstRq, lastRq);
    bool prevShadow = gInShadowPass;
    gInShadowPass = (pe->name[0] == 'r' && pe->name[1] == 't' && pe->name[2] == 'w') ||
                    (pe->name[0] == 'c' && pe->name[1] == 's' && pe->name[2] == 'm');
    QueryPerformanceCounter(&t0);
    enter();
    gRender(cam, lodCam, vp, firstRq, lastRq, includeOverlays);
    gInShadowPass = prevShadow;
    QueryPerformanceCounter(&t1);
    i64 excl = leave(t1 - t0);
    Entry* e = entryFor(cam, firstRq, lastRq);
    ++e->renderCalls;
    e->renderTicks += excl;
    if (gDepth == 0 && t1 - gWindowStart >= 5 * gFreq) report(t1);
}

bool same(const u8* p, const u8* expected, int n) {
    for (int i = 0; i < n; ++i)
        if (p[i] != expected[i]) return false;
    return true;
}

// Builds "original prologue + jmp back" in executable memory, then replaces the
// function start with "jmp hook". Returns the trampoline.
void* hook(u8* func, const u8* prologue, int len, void* target, u8* tramp) {
    for (int i = 0; i < len; ++i) tramp[i] = prologue[i];
    u8* back = tramp + len;
    back[0] = 0xff; back[1] = 0x25; back[2] = back[3] = back[4] = back[5] = 0;
    u64 ret = (u64)(func + len);
    for (int i = 0; i < 8; ++i) back[6 + i] = (u8)(ret >> (8 * i));

    u8 patch[18];
    patch[0] = 0xff; patch[1] = 0x25; patch[2] = patch[3] = patch[4] = patch[5] = 0;
    u64 t = (u64)target;
    for (int i = 0; i < 8; ++i) patch[6 + i] = (u8)(t >> (8 * i));
    for (int i = 14; i < len; ++i) patch[i] = 0xcc;
    u32 old;
    if (!VirtualProtect(func, len, 0x40, &old)) return 0;
    for (int i = 0; i < len; ++i) func[i] = patch[i];
    VirtualProtect(func, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), func, len);
    return tramp;
}

void install() {
    char env[8];
    u32 len = GetEnvironmentVariableA("KENSHI_PERF_PROBE", env, sizeof env);
    if (!(len == 1 && env[0] == '1')) {
        logStr("KENSHI_PERF_PROBE is not 1, not patching\r\n");
        return;
    }
    HANDLE ogre = GetModuleHandleA("OgreMain_x64.dll");
    u8* cull = ogre ? (u8*)GetProcAddress(ogre, kCullName) : 0;
    u8* render = ogre ? (u8*)GetProcAddress(ogre, kRenderName) : 0;
    gGetName = ogre ? (GetNameFn)GetProcAddress(ogre, kGetNameName) : 0;
    if (!cull || !render || !gGetName) {
        logStr("OgreMain exports not found, not patching\r\n");
        return;
    }
    if (!same(cull, kCullPrologue, sizeof kCullPrologue) || !same(render, kRenderPrologue, sizeof kRenderPrologue)) {
        logStr("prologue bytes differ (different OgreMain build?), not patching\r\n");
        return;
    }
    u8* mem = (u8*)VirtualAlloc(0, 4096, 0x3000 /* MEM_COMMIT|MEM_RESERVE */, 0x40 /* PAGE_EXECUTE_READWRITE */);
    if (!mem) {
        logStr("VirtualAlloc failed\r\n");
        return;
    }
    QueryPerformanceFrequency(&gFreq);
    QueryPerformanceCounter(&gWindowStart);
    u8* rso = (u8*)GetProcAddress(ogre, kRsoName);
    gGetParent = (GetParentFn)GetProcAddress(ogre, "?getParent@SubEntity@Ogre@@QEBAPEAVEntity@2@XZ");
    gGetMesh = (GetMeshFn)GetProcAddress(ogre, "?getMesh@Entity@Ogre@@QEBAAEBV?$SharedPtr@VMesh@Ogre@@@2@XZ");
    gResGetName = (ResGetNameFn)GetProcAddress(ogre, "?getName@Resource@Ogre@@UEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    gSubEntityVt = GetProcAddress(ogre, "??_7SubEntity@Ogre@@6B@");
    bool objOk = rso && gGetParent && gGetMesh && gResGetName && gSubEntityVt && same(rso, kRsoPrologue, sizeof kRsoPrologue);
    gCull = (CullFn)hook(cull, kCullPrologue, sizeof kCullPrologue, (void*)&cullHook, mem);
    gRender = (RenderFn)hook(render, kRenderPrologue, sizeof kRenderPrologue, (void*)&renderHook, mem + 64);
    if (!gCull || !gRender) {
        logStr("VirtualProtect failed\r\n");
        return;
    }
    logStr("hooked Camera::_cullScenePhase01 and Camera::_renderScenePhase02; reporting every 5 s\r\n");
    if (!objOk) {
        logStr("renderSingleObject or classification exports not found/differ, object table off\r\n");
        return;
    }
    gRso = (RsoFn)hook(rso, kRsoPrologue, sizeof kRsoPrologue, (void*)&rsoHook, mem + 128);
    logStr(gRso ? "hooked SceneManager::renderSingleObject (object table on)\r\n" : "VirtualProtect failed on renderSingleObject\r\n");
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("PerfProbe.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("PerfProbe 1.3\r\n");
    install();
    // The log stays open for the periodic reports.
}
