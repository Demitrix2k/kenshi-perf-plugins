// RenderQueueTrim: RE_Kenshi plugin that turns on Ogre's
// Root::setRemoveRenderQueueStructuresOnClear(true), which Kenshi never calls.
//
// SceneManager::prepareRenderQueue (OgreMain 0x1802bcc..) reads Root+0x198 and passes
// it to RenderQueue::clear. With false (Kenshi's default), every material pass ever
// queued keeps an entry, and each render pass walks all of them, mostly to skip
// empty ones: QueuedRenderableCollection::clear and acceptVisitorGrouped grow ~2.5x
// from the light save to the 60+ NPC save. With true, the per-pass structures are
// removed on clear, so only passes used this frame are walked, at the price of
// rebuilding them every frame. Which way wins is what the A/B is for.
//
// No code is patched: the plugin calls Ogre's own exported setter from a small
// background thread once Root exists, and again on every toggle.
// KENSHI_RQ_TRIM=0 starts with the flag off (Kenshi's default). At runtime
// "rqtrim=0/1" in perfctl.ini (see tools/perfctl.py) switches it; changes
// go to RenderQueueTrim.log and perfctl-status-rqtrim.txt.
//
// Built without a CRT (see build.sh).

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) void* __stdcall GetProcAddress(HANDLE, const char*);
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
}

#include "../../common/perfctl.h"

namespace {

typedef void (*SetFlagFn)(void* root, bool on);
typedef bool (*GetFlagFn)(const void* root);

SetFlagFn gSet = 0;
GetFlagFn gGet = 0;
void** gRootPtr = 0;  // &Root::msSingleton
volatile bool gOn = true;  // wanted state, written by the perfctl thread
HANDLE gLog = 0;

void logStr(const char* s) {
    if (!gLog) return;
    u32 n = 0, w;
    while (s[n]) ++n;
    WriteFile(gLog, s, n, &w, 0);
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

void apply(void* root, const char* why);

// perfctl thread: "rqtrim=0/1".
void onControl(const char* key, int value) {
    if (!perfctl::keyIs(key, "rqtrim") || (value != 0) == gOn) return;
    gOn = value != 0;
    if (void* root = *gRootPtr) apply(root, "perfctl");
}

void apply(void* root, const char* why) {
    gSet(root, gOn);
    bool now = gGet(root);
    logTime();
    logStr(why);
    logStr(now ? ": removeRenderQueueStructuresOnClear = true\r\n"
               : ": removeRenderQueueStructuresOnClear = false\r\n");
    perfctl::status("rqtrim", now ? "rqtrim=1\n" : "rqtrim=0\n");
}

// Waits for Ogre's Root, applies the starting value, then hands over to perfctl.
u32 __stdcall worker(void*) {
    void* root;
    while (!(root = *gRootPtr)) Sleep(100);
    logTime();
    logStr(gGet(root) ? "Kenshi's value: true\r\n" : "Kenshi's value: false\r\n");
    apply(root, "start");
    perfctl::start(onControl);
    return 0;
}

bool envIs(const char* name, char v) {
    char env[8];
    u32 len = GetEnvironmentVariableA(name, env, sizeof env);
    return len == 1 && env[0] == v;
}

void install() {
    gOn = !envIs("KENSHI_RQ_TRIM", '0');
    logStr(gOn ? "starts ON\r\n" : "starts OFF (KENSHI_RQ_TRIM=0)\r\n");
    HANDLE ogre = GetModuleHandleA("OgreMain_x64.dll");
    if (!ogre) { logStr("OgreMain not loaded, doing nothing\r\n"); return; }
    gSet = (SetFlagFn)GetProcAddress(ogre, "?setRemoveRenderQueueStructuresOnClear@Root@Ogre@@QEAAX_N@Z");
    gGet = (GetFlagFn)GetProcAddress(ogre, "?getRemoveRenderQueueStructuresOnClear@Root@Ogre@@QEBA_NXZ");
    gRootPtr = (void**)GetProcAddress(ogre, "?msSingleton@?$Singleton@VRoot@Ogre@@@Ogre@@1PEAVRoot@2@EA");
    if (!gSet || !gGet || !gRootPtr) { logStr("OgreMain exports not found, doing nothing\r\n"); return; }
    if (!CreateThread(0, 0, worker, 0, 0, 0)) logStr("CreateThread failed\r\n");
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("RenderQueueTrim.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("RenderQueueTrim 1.1\r\n");
    install();
}
