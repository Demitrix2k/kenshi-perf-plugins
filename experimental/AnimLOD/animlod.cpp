// AnimLOD: RE_Kenshi plugin that updates the skeletal pose of distant characters
// at a lower rate.
//
// Ogre's worker threads evaluate every skinned entity's skeleton every frame
// (SceneManager::updateAllOldAnimations01Thread / 02Thread -> Entity::updateAnimation),
// visible or not, near or far. That is ~33% of all worker time in the 60+ NPC save,
// and the main thread waits for it at the barrier.
//
// Entity::updateAnimation already has a fast path: when the entity's
// mFrameAnimationLastUpdated (+0x284) equals the animation-state set's dirty frame
// ([+0x1a8]+0x18), it skips keyframe evaluation and only re-applies the existing bone
// pose to the node's current transform (so the mesh still follows a walking
// character). For entities beyond a distance, on N-1 of every N frames, this plugin
// sets +0x284 to the dirty frame before calling the original, so Ogre takes its own
// fast path. Kenshi keeps advancing animation time, so the next full update lands at
// the right point of the animation. Manually controlled bones still force a full
// update (Ogre checks getManualBonesDirty after our write).
//
// Entities that share a skeleton (body, armour, head...) share the animation-state
// set, so the frame stagger is keyed by that set: all parts skip on the same frames.
//
// Only the two worker call sites are redirected; Kenshi's own explicit
// _updateAnimation/_updateAnimationForce calls are untouched.
//
// Distances are in Kenshi world units (~10 cm; a human is ~19.5 units tall).
// KENSHI_ANIMLOD=0            start disabled (stats are still logged)
// KENSHI_ANIMLOD_MID=400      distance where pose updates drop to 1/MIDN (default 40 m)
// KENSHI_ANIMLOD_FAR=1200     distance where pose updates drop to 1/FARN (default 120 m)
// KENSHI_ANIMLOD_MIDN=2, KENSHI_ANIMLOD_FARN=4
// With KENSHI_PERF_DEBUG=1, Home toggles it in game. Stats every 10 s in AnimLOD.log.
//
// Target: OgreMain_x64.dll shipped with Kenshi 1.0.65; every patched byte is checked.
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
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE, const void*, u64);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(i64*);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(i64*);
__declspec(dllimport) u64 __stdcall GetTickCount64();
__declspec(dllimport) short __stdcall GetAsyncKeyState(int);
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
}

namespace {

// RVAs in OgreMain_x64.dll (1.0.65)
const u32 kUpdateAnimRva = 0xc1620;            // Entity::updateAnimation
const u32 kCallSiteRva[2] = {0x2ca89d, 0x2ca8fd};  // calls in updateAllOldAnimations01/02Thread
const u32 kStubRva = 0x2a1ec0;                 // 16 bytes of int3 padding

// Entity / Ogre field offsets (from the disassembly)
const u32 kEntParentNode = 0x28;   // Node*: +0x38 u8 index, +0x80 Matrix4* derived transforms
const u32 kEntManager = 0x90;      // SceneManager* (MovableObject::_getManager)
const u32 kEntAnimSet = 0x1a8;     // AnimationStateSet*: +0x18 u32 dirty frame
const u32 kEntAnimFrame = 0x284;   // u32 mFrameAnimationLastUpdated
const u32 kEntSkeleton = 0x300;    // OldSkeletonInstance*: vtable +0x248 getManualBonesDirty
const u32 kSmCameras = 0x4c8;      // std::vector<Camera*> (SceneManager::getCameras)
const u32 kCamDerivedPos = 0x564;  // Vector3 mDerivedPosition (cached by updateView)
const u32 kRootNextFrame = 0x190;  // u32, as read by Entity::cacheBoneMatrices

typedef void (*UpdateAnimFn)(u8* entity);
typedef const u8* (*GetNameFn)(const u8* movable);  // returns const std::string&
typedef bool (*ManualDirtyFn)(u8* skeleton);

UpdateAnimFn gOrig = 0;
GetNameFn gGetName = 0;
u8** gRootPtr = 0;  // &Root::msSingleton

volatile bool gOn = true;
bool gHotkeys = false;
float gMid2 = 400.f * 400.f, gFar2 = 1200.f * 1200.f;
u32 gMidN = 2, gFarN = 4;

volatile u32 gFrame = 0xffffffff;  // last frame the per-frame step ran for
volatile float gCam[3];
volatile bool gCamValid = false;
bool gKeyDown = false;

// Racy diagnostics; exact counts don't matter.
const int kBuckets = 7;  // <100, <200, <400, <800, <1600, <3200, >=3200 units
volatile u64 gCalls = 0, gSkips = 0, gManual = 0, gFrames = 0, gTicks = 0, gNoCam = 0;
volatile u64 gHist[kBuckets];
u64 gLastReport = 0;
i64 gFreq = 1;

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

// v / d with one decimal
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

inline u64 rdtsc() { return __builtin_ia32_rdtsc(); }

bool nameIs(const u8* str, const char* want) {
    u64 size = *(const u64*)(str + 0x10);
    u64 cap = *(const u64*)(str + 0x18);
    const char* s = cap >= 16 ? *(const char* const*)str : (const char*)str;
    u64 i = 0;
    for (; want[i]; ++i)
        if (i >= size || s[i] != want[i]) return false;
    return i == size;
}

u64 gTscPerMs = 1;

// Runs once per frame on whichever worker gets there first, while the main thread
// waits at the barrier: refresh camera position, poll the hotkey, log stats.
__declspec(noinline) void perFrame(u8* entity) {
    ++gFrames;
    gCamValid = false;
    u8* sm = *(u8**)(entity + kEntManager);
    if (sm) {
        u8** first = *(u8***)(sm + kSmCameras);
        u8** last = *(u8***)(sm + kSmCameras + 8);
        if (first && last >= first && last - first < 256) {
            for (u8** c = first; c < last; ++c) {
                if (*c && nameIs(gGetName(*c), "mainCamera")) {
                    const float* p = (const float*)(*c + kCamDerivedPos);
                    gCam[0] = p[0]; gCam[1] = p[1]; gCam[2] = p[2];
                    gCamValid = true;
                    break;
                }
            }
        }
    }
    if (gHotkeys) {
        const int VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_HOME = 0x24;
        bool mod = (GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL)) & 0x8000;
        bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (down && !gKeyDown && !mod) {
            gOn = !gOn;
            logTime();
            logStr(gOn ? "Home: animation LOD ON\r\n" : "Home: animation LOD OFF\r\n");
        }
        gKeyDown = down;
    }
    u64 now = GetTickCount64();
    if (now - gLastReport >= 10000) {
        if (gLastReport && gFrames > 1) {
            u64 frames = gFrames - 1;  // this frame has no samples yet
            u64 ms10 = gTicks * 10 / gTscPerMs;  // tenths of ms
            logTime();
            logStr(gOn ? "[on]  " : "[off] ");
            logStr("frames ");
            logUint(frames);
            logStr(", skinned entities/frame ");
            logRatio(gCalls, frames);
            logStr(", pose updates skipped/frame ");
            logRatio(gSkips, frames);
            logStr(", skips blocked by manual bones/frame ");
            logRatio(gManual, frames);
            logStr(", updateAnimation CPU ms/frame (sum of workers) ");
            logRatio(ms10, frames * 10);
            logStr("\r\n    distance per frame (units ~10 cm): <100 ");
            static const char* const lbl[kBuckets] = {"", "  <200 ", "  <400 ", "  <800 ", "  <1600 ", "  <3200 ", "  >=3200 "};
            for (int i = 0; i < kBuckets; ++i) {
                if (i) logStr(lbl[i]);
                logRatio(gHist[i], frames);
            }
            if (gNoCam) { logStr("  no camera/node "); logRatio(gNoCam, frames); }
            logStr("\r\n");
        }
        gCalls = gSkips = gManual = gTicks = gNoCam = 0;
        for (int i = 0; i < kBuckets; ++i) gHist[i] = 0;
        gFrames = 1;
        gLastReport = now;
    }
}

// Called instead of Entity::updateAnimation from the two worker loops (rcx = entity).
void animHook(u8* e) {
    ++gCalls;
    u32 frame = *(const u32*)(*gRootPtr + kRootNextFrame);
    u32 seen = gFrame;
    if (frame != seen && __atomic_compare_exchange_n(&gFrame, &seen, frame, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        perFrame(e);

    u8* node = *(u8**)(e + kEntParentNode);
    u8* set = *(u8**)(e + kEntAnimSet);
    if (gCamValid && node && set) {
        const float* m = (const float*)(*(u8**)(node + 0x80) + (u64)*(node + 0x38) * 64);
        float dx = m[3] - gCam[0], dy = m[7] - gCam[1], dz = m[11] - gCam[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        int b = 0;
        for (float lim = 100.f * 100.f; b < kBuckets - 1 && d2 >= lim; lim *= 4.f) ++b;
        ++gHist[b];
        u32 n = d2 >= gFar2 ? gFarN : d2 >= gMid2 ? gMidN : 1;
        if (gOn && n > 1) {
            u32 h = (u32)((((u64)set >> 4) * 0x9e3779b97f4a7c15ull) >> 40);
            u32 dirty = *(const u32*)(set + 0x18);
            if ((frame + h) % n != 0 && *(u32*)(e + kEntAnimFrame) != dirty) {
                u8* skel = *(u8**)(e + kEntSkeleton);
                if (skel && (*(ManualDirtyFn*)(*(u8**)skel + 0x248))(skel)) {
                    ++gManual;
                } else {
                    *(u32*)(e + kEntAnimFrame) = dirty;  // Ogre takes its fast path
                    ++gSkips;
                }
            }
        }
    } else {
        ++gNoCam;
    }
    u64 t0 = rdtsc();
    gOrig(e);
    __atomic_fetch_add(&gTicks, rdtsc() - t0, __ATOMIC_RELAXED);
}

bool allBytes(const u8* p, u8 v, int n) {
    for (int i = 0; i < n; ++i)
        if (p[i] != v) return false;
    return true;
}

bool write(u8* dst, const u8* src, int n) {
    u32 old;
    if (!VirtualProtect(dst, n, 0x40, &old)) return false;
    for (int i = 0; i < n; ++i) dst[i] = src[i];
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

u32 envUint(const char* name, u32 def, u32 lo, u32 hi) {
    char env[16];
    u32 len = GetEnvironmentVariableA(name, env, sizeof env);
    if (len == 0 || len >= sizeof env) return def;
    u32 v = 0;
    for (u32 i = 0; i < len; ++i) {
        if (env[i] < '0' || env[i] > '9') return def;
        v = v * 10 + (env[i] - '0');
    }
    return v >= lo && v <= hi ? v : def;
}

void install() {
    gOn = envUint("KENSHI_ANIMLOD", 1, 0, 1) == 1;
    u32 mid = envUint("KENSHI_ANIMLOD_MID", 400, 0, 100000);
    u32 far = envUint("KENSHI_ANIMLOD_FAR", 1200, 0, 100000);
    if (far < mid) far = mid;
    gMid2 = (float)mid * (float)mid;
    gFar2 = (float)far * (float)far;
    gMidN = envUint("KENSHI_ANIMLOD_MIDN", 2, 1, 16);
    gFarN = envUint("KENSHI_ANIMLOD_FARN", 4, 1, 16);
    gHotkeys = envUint("KENSHI_PERF_DEBUG", 0, 0, 1) == 1;
    logStr("mid "); logUint(mid); logStr(" units -> 1/"); logUint(gMidN);
    logStr(", far "); logUint(far); logStr(" units -> 1/"); logUint(gFarN);
    logStr(gOn ? ", starts ON" : ", starts OFF (KENSHI_ANIMLOD=0)");
    logStr(gHotkeys ? ", hotkey Home: on\r\n" : ", hotkey Home: off\r\n");

    QueryPerformanceFrequency(&gFreq);
    i64 q0, q1;
    QueryPerformanceCounter(&q0);
    u64 t0 = rdtsc();
    do { QueryPerformanceCounter(&q1); } while (q1 - q0 < gFreq / 200);  // ~5 ms
    u64 t1 = rdtsc();
    gTscPerMs = (t1 - t0) * (u64)gFreq / (u64)(q1 - q0) / 1000;
    if (!gTscPerMs) gTscPerMs = 1;

    u8* ogre = (u8*)GetModuleHandleA("OgreMain_x64.dll");
    if (!ogre) { logStr("OgreMain not loaded, not patching\r\n"); return; }
    u8* upd = (u8*)GetProcAddress(ogre, "?updateAnimation@Entity@Ogre@@IEAAXXZ");
    gGetName = (GetNameFn)GetProcAddress(ogre,
        "?getName@MovableObject@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    gRootPtr = (u8**)GetProcAddress(ogre, "?msSingleton@?$Singleton@VRoot@Ogre@@@Ogre@@1PEAVRoot@2@EA");
    if (upd != ogre + kUpdateAnimRva || !gGetName || !gRootPtr) {
        logStr("OgreMain exports differ (different build?), not patching\r\n");
        return;
    }
    static const u8 kProlog[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9,
                                   0x08, 0x03, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9};
    for (int i = 0; i < 16; ++i)
        if (upd[i] != kProlog[i]) { logStr("updateAnimation bytes differ, not patching\r\n"); return; }
    u8* stub = ogre + kStubRva;
    if (!allBytes(stub, 0xcc, 16)) { logStr("stub padding not free, not patching\r\n"); return; }
    for (int s = 0; s < 2; ++s) {
        u8* c = ogre + kCallSiteRva[s];
        i64 target = (i64)(c + 5) + *(const int*)(c + 1);
        if (c[0] != 0xe8 || (u8*)target != upd) { logStr("call site bytes differ, not patching\r\n"); return; }
    }
    gOrig = (UpdateAnimFn)upd;

    u8 jmp[14] = {0xff, 0x25, 0, 0, 0, 0};
    u64 hook = (u64)&animHook;
    for (int i = 0; i < 8; ++i) jmp[6 + i] = (u8)(hook >> (8 * i));
    if (!write(stub, jmp, 14)) { logStr("VirtualProtect failed\r\n"); return; }
    for (int s = 0; s < 2; ++s) {
        u8* c = ogre + kCallSiteRva[s];
        int rel = (int)(stub - (c + 5));
        u8 call[5] = {0xe8, (u8)rel, (u8)(rel >> 8), (u8)(rel >> 16), (u8)(rel >> 24)};
        if (!write(c, call, 5)) { logStr("VirtualProtect failed\r\n"); return; }
    }
    logTime();
    logStr("redirected both worker calls of Entity::updateAnimation\r\n");
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("AnimLOD.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("AnimLOD 1.0\r\n");
    install();
    // The log stays open for toggles and stats.
}
