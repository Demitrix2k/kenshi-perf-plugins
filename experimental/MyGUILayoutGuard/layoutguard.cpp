// MyGUILayoutGuard: RE_Kenshi plugin that skips Kenshi's repeated no-op MyGUI
// layout calls, the safer way.
//
// Kenshi's UI code calls MyGUI::Widget::setPosition/setSize/setCoord/setRealPosition/
// setRealSize every frame on widgets that are not moving. Kenshi's MyGUI fork has no
// "unchanged, do nothing" check, so each call re-lays-out the widget and marks its
// whole render batch out of date (~222 RenderItem::outOfDate per frame measured in
// the 60+ NPC save). Micromanner's fix added the equality check inside MyGUI's
// generic setters; it worked but hid some buttons (vendor window X), because MyGUI's
// own layout code also goes through those setters.
//
// This plugin only sees Kenshi's own calls: it swaps the exe's import-table slots of
// the five non-virtual wrappers (MyGUI-internal layout never goes through them). And
// it skips a call only when all of these hold:
//   1. the same widget got the identical call (same function, same arguments) from
//      the same call site in the previous frame or earlier this frame, and
//   2. the widget's current coordinates already equal the result (for the "Real"
//      float variants, computed exactly as MyGUI does: (int)(parentSize * f)).
// One-off layout calls (opening a window, creating a widget) are never skipped.
//
// perfctl.ini key "uiguard": 0 = count only (default; everything passes through),
// 1 = guard on. Every 10 s MyGUILayoutGuard.log lists calls per frame, how many were
// no-ops and skipped, and the busiest call sites (exe RVAs).
//
// Target: RE_Kenshi's Kenshi_x64.exe + MyGUIEngine_x64.dll (Kenshi 1.0.65). Each
// import slot must hold the expected MyGUIEngine export before it is replaced.
// Built without a CRT (see build.sh).

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) void* __stdcall GetProcAddress(HANDLE, const char*);
__declspec(dllimport) int __stdcall VirtualProtect(void*, u64, u32, u32*);
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) u64 __stdcall GetTickCount64();
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT

// The optimiser turns small copy loops into memcpy calls; no CRT provides one.
void* memcpy(void* dst, const void* src, unsigned long long n) {
    volatile unsigned char* d = (volatile unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}
}

#include "../../common/perfctl.h"

namespace {

// Import slots in RE_Kenshi/Kenshi_x64.exe (RVA) and the export each must hold.
enum { F_POS, F_SIZE, F_COORD, F_RPOS, F_RSIZE, F_COUNT };
struct Slot { u32 iatRva; const char* exportName; const char* label; };
const Slot kSlots[F_COUNT] = {
    {0x2245a68, "?setPosition@Widget@MyGUI@@QEAAXHH@Z", "setPosition"},
    {0x2245a38, "?setSize@Widget@MyGUI@@QEAAXHH@Z", "setSize"},
    {0x2245a28, "?setCoord@Widget@MyGUI@@QEAAXHHHH@Z", "setCoord"},
    {0x2245af0, "?setRealPosition@Widget@MyGUI@@QEAAXMM@Z", "setRealPosition"},
    {0x22459e0, "?setRealSize@Widget@MyGUI@@QEAAXMM@Z", "setRealSize"},
};

// Widget layout (outer Widget*, from the wrappers: they forward to this+8, whose
// mCoord is at +0x18): left +0x20, top +0x24, width +0x28, height +0x2c.
// Cropped parent (sub-object pointer) at +0x40, its width/height at +0x20/+0x24.
inline int* coord(u8* w) { return (int*)(w + 0x20); }

typedef void (*Fn2i)(u8*, int, int);
typedef void (*Fn4i)(u8*, int, int, int, int);
typedef void (*Fn2f)(u8*, float, float);
Fn2i gPos = 0, gSize = 0;
Fn4i gCoord = 0;
Fn2f gRPos = 0, gRSize = 0;

volatile int gMode = 0;  // perfctl "uiguard": 0 count only, 1 guard
u8* gExe = 0;
u8** gRootPtr = 0;       // &Ogre::Root::msSingleton; next frame number at +0x190

inline u32 frameNow() {
    u8* root = *gRootPtr;
    return root ? *(u32*)(root + 0x190) : 0;
}

// Per (widget, site, function): last arguments and the frame they were seen.
struct Entry { u8* widget; u64 site; int a[4]; u32 frame; u32 fn; };
const u32 kEntries = 8192;
Entry gCache[kEntries];

// Per call site statistics.
struct SiteStat { u64 site; u32 fn; u32 calls, noops, skips; };
const u32 kSites = 512;
SiteStat gSites[kSites];
u32 gFnCalls[F_COUNT], gFnNoops[F_COUNT], gFnSkips[F_COUNT];
u32 gFrames = 0, gLastFrame = 0;
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
void logHex(u64 v) {
    char buf[11] = {'0', 'x'};
    int n = 0;
    char t[16];
    do { u32 nib = v & 0xf; t[n++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10); v >>= 4; } while (v);
    for (int i = 0; i < n && i < 8; ++i) buf[2 + i] = t[n - 1 - i];
    buf[2 + (n < 8 ? n : 8)] = 0;
    logStr(buf);
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
    u32 frames = gFrames ? gFrames : 1;
    logTime();
    logStr(gMode ? "[guard on]  frames " : "[count only] frames ");
    logUint(gFrames);
    logStr("\r\n");
    for (int f = 0; f < F_COUNT; ++f) {
        if (!gFnCalls[f]) continue;
        logStr("    ");
        logStr(kSlots[f].label);
        logStr(": calls/frame ");
        logRatio(gFnCalls[f], frames);
        logStr(", no-ops/frame ");
        logRatio(gFnNoops[f], frames);
        logStr(", skipped/frame ");
        logRatio(gFnSkips[f], frames);
        logStr("\r\n");
    }
    // Top 12 call sites by calls.
    for (int k = 0; k < 12; ++k) {
        int best = -1;
        for (u32 i = 0; i < kSites; ++i)
            if (gSites[i].site && gSites[i].calls && (best < 0 || gSites[i].calls > gSites[best].calls)) best = (int)i;
        if (best < 0) break;
        SiteStat& s = gSites[best];
        logStr("      site exe+");
        logHex(s.site - (u64)gExe);
        logStr(" ");
        logStr(kSlots[s.fn].label);
        logStr(": calls/frame ");
        logRatio(s.calls, frames);
        logStr(", no-ops ");
        logRatio(s.noops, frames);
        logStr(", skipped ");
        logRatio(s.skips, frames);
        logStr("\r\n");
        s.calls = 0;  // excluded from the next pick; everything is reset below anyway
    }
    for (u32 i = 0; i < kSites; ++i) gSites[i].calls = gSites[i].noops = gSites[i].skips = 0;
    for (int f = 0; f < F_COUNT; ++f) gFnCalls[f] = gFnNoops[f] = gFnSkips[f] = 0;
    gFrames = 0;
}

// Bookkeeping shared by all five hooks. Returns true if the call should be skipped.
// noop: the widget's current coordinates already equal what the call would set.
__declspec(noinline) bool decide(u32 fn, u8* w, u64 site, const int* args, int nargs, bool noop) {
    u32 frame = frameNow();
    if (frame != gLastFrame) {
        gLastFrame = frame;
        ++gFrames;
        u64 now = GetTickCount64();
        if (now - gLastReport >= 10000) {
            if (gLastReport) report();
            gLastReport = now;
        }
    }

    // Call-site statistics (open addressing on the return address).
    u32 h = (u32)((site * 0x9e3779b97f4a7c15ull) >> 55) & (kSites - 1);
    SiteStat* st = 0;
    for (u32 i = 0; i < 8; ++i) {
        SiteStat& s = gSites[(h + i) & (kSites - 1)];
        if (s.site == site) { st = &s; break; }
        if (!s.site) { s.site = site; s.fn = fn; st = &s; break; }
    }
    ++gFnCalls[fn];
    if (st) ++st->calls;
    if (noop) { ++gFnNoops[fn]; if (st) ++st->noops; }

    // Repeat check: same widget, same site, same function, same arguments, seen in
    // this or the previous frame.
    u64 key = (u64)w * 0x9e3779b97f4a7c15ull ^ site * 0xc2b2ae3d27d4eb4full ^ fn;
    Entry& e = gCache[(u32)(key >> 51) & (kEntries - 1)];
    bool same = e.widget == w && e.site == site && e.fn == fn && frame - e.frame <= 1;
    for (int i = 0; same && i < nargs; ++i) same = e.a[i] == args[i];
    e.widget = w;
    e.site = site;
    e.fn = fn;
    e.frame = frame;
    for (int i = 0; i < nargs; ++i) e.a[i] = args[i];

    bool skip = gMode == 1 && noop && same;
    if (skip) { ++gFnSkips[fn]; if (st) ++st->skips; }
    return skip;
}

void posHook(u8* w, int l, int t) {
    u64 site = (u64)__builtin_return_address(0);
    int a[2] = {l, t};
    const int* c = coord(w);
    if (decide(F_POS, w, site, a, 2, c[0] == l && c[1] == t)) return;
    gPos(w, l, t);
}

void sizeHook(u8* w, int wd, int ht) {
    u64 site = (u64)__builtin_return_address(0);
    int a[2] = {wd, ht};
    const int* c = coord(w);
    if (decide(F_SIZE, w, site, a, 2, c[2] == wd && c[3] == ht)) return;
    gSize(w, wd, ht);
}

void coordHook(u8* w, int l, int t, int wd, int ht) {
    u64 site = (u64)__builtin_return_address(0);
    int a[4] = {l, t, wd, ht};
    const int* c = coord(w);
    if (decide(F_COORD, w, site, a, 4, c[0] == l && c[1] == t && c[2] == wd && c[3] == ht)) return;
    gCoord(w, l, t, wd, ht);
}

// MyGUI's setRealPosition/setRealSize: pixels = (int)((float)parentSize * f), with the
// render target size when there is no parent (then we never skip).
union FloatBits { float f; int i; };

void rposHook(u8* w, float fx, float fy) {
    u64 site = (u64)__builtin_return_address(0);
    FloatBits bx, by;
    bx.f = fx; by.f = fy;
    int a[2] = {bx.i, by.i};
    const u8* parent = *(u8**)(w + 0x40);
    bool noop = false;
    if (parent) {
        int px = (int)((float)*(const int*)(parent + 0x20) * fx);
        int py = (int)((float)*(const int*)(parent + 0x24) * fy);
        const int* c = coord(w);
        noop = c[0] == px && c[1] == py;
    }
    if (decide(F_RPOS, w, site, a, 2, noop)) return;
    gRPos(w, fx, fy);
}

void rsizeHook(u8* w, float fw, float fh) {
    u64 site = (u64)__builtin_return_address(0);
    FloatBits bw, bh;
    bw.f = fw; bh.f = fh;
    int a[2] = {bw.i, bh.i};
    const u8* parent = *(u8**)(w + 0x40);
    bool noop = false;
    if (parent) {
        int pw = (int)((float)*(const int*)(parent + 0x20) * fw);
        int ph = (int)((float)*(const int*)(parent + 0x24) * fh);
        const int* c = coord(w);
        noop = c[2] == pw && c[3] == ph;
    }
    if (decide(F_RSIZE, w, site, a, 2, noop)) return;
    gRSize(w, fw, fh);
}

void* const kHooks[F_COUNT] = {(void*)&posHook, (void*)&sizeHook, (void*)&coordHook,
                               (void*)&rposHook, (void*)&rsizeHook};

void reportStatus() {
    perfctl::Text t;
    t.add("uiguard=").num(gMode).add("\n");
    perfctl::status("uiguard", t.b);
}

void onControl(const char* key, int value) {
    if (!perfctl::keyIs(key, "uiguard") || value < 0 || value > 1 || value == gMode) return;
    gMode = value;
    logTime();
    logStr(gMode ? "perfctl: guard ON\r\n" : "perfctl: guard OFF (count only)\r\n");
    reportStatus();
}

void install() {
    char env[8];
    u32 len = GetEnvironmentVariableA("KENSHI_UIGUARD", env, sizeof env);
    if (len == 1 && env[0] == '1') gMode = 1;

    gExe = (u8*)GetModuleHandleA(0);
    HANDLE mygui = GetModuleHandleA("MyGUIEngine_x64.dll");
    HANDLE ogre = GetModuleHandleA("OgreMain_x64.dll");
    gRootPtr = ogre ? (u8**)GetProcAddress(ogre, "?msSingleton@?$Singleton@VRoot@Ogre@@@Ogre@@1PEAVRoot@2@EA") : 0;
    if (!gExe || !mygui || !gRootPtr) { logStr("modules or exports missing, not patching\r\n"); return; }

    void* orig[F_COUNT];
    for (int f = 0; f < F_COUNT; ++f) {
        void* exp = GetProcAddress(mygui, kSlots[f].exportName);
        void** slot = (void**)(gExe + kSlots[f].iatRva);
        if (!exp || *slot != exp) {
            logStr(kSlots[f].label);
            logStr(": import slot does not hold the MyGUI export (different build?), not patching\r\n");
            return;
        }
        orig[f] = exp;
    }
    gPos = (Fn2i)orig[F_POS];
    gSize = (Fn2i)orig[F_SIZE];
    gCoord = (Fn4i)orig[F_COORD];
    gRPos = (Fn2f)orig[F_RPOS];
    gRSize = (Fn2f)orig[F_RSIZE];

    for (int f = 0; f < F_COUNT; ++f) {
        void** slot = (void**)(gExe + kSlots[f].iatRva);
        u32 old;
        if (!VirtualProtect(slot, 8, 0x04 /* PAGE_READWRITE */, &old)) { logStr("VirtualProtect failed\r\n"); return; }
        *slot = kHooks[f];  // aligned 8-byte write
        VirtualProtect(slot, 8, old, &old);
    }
    logTime();
    logStr(gMode ? "hooked 5 MyGUI setter imports, guard ON\r\n"
                 : "hooked 5 MyGUI setter imports, count only (uiguard=0)\r\n");
    reportStatus();
    perfctl::start(onControl);
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("MyGUILayoutGuard.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("MyGUILayoutGuard 1.0\r\n");
    install();
}
