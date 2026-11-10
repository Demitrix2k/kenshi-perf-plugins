// CharacterDetailCull: RE_Kenshi plugin that skips drawing small character parts
// (hair, beards, hats, masks, backpacks, weapons, ...) when the character is far
// from the camera, and in shadow passes. Bodies and main clothing are always drawn.
//
// Hooks OgreMain's SceneManager::renderSingleObject (one call per object draw) with
// a trampoline. For each SubEntity draw it looks at the mesh name (decided once per
// mesh against the rules) and, for small-part meshes on characters (entity has a
// skeleton, or is attached to a bone), at the distance from the main camera to the
// character's scene node. A skipped draw simply returns.
//
// Rules come from rules.txt next to the DLL (built-in defaults if missing):
//   main_distance=1000     skip small parts beyond this in normal passes (0 = never)
//   shadow_distance=0      skip small parts beyond this in shadow passes (0 = always)
//   small=haircut          case-insensitive substring of a small-part mesh name
//   keep=human_            never skip meshes containing this (checked first)
// Distances are in world units (the same units as the npc range setting).
//
// KENSHI_DETAILCULL=0 starts with the filter off. With KENSHI_PERF_DEBUG=1, Insert
// toggles the filter in game. Status and skip counts go to CharacterDetailCull.log.
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
__declspec(dllimport) int __stdcall GetModuleHandleExA(u32, const char*, HANDLE*);
__declspec(dllimport) u32 __stdcall GetModuleFileNameA(HANDLE, char*, u32);
__declspec(dllimport) void* __stdcall GetProcAddress(HANDLE, const char*);
__declspec(dllimport) int __stdcall VirtualProtect(void*, u64, u32, u32*);
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, u64, u32, u32);
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE, const void*, u64);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) u32 __stdcall GetEnvironmentVariableA(const char*, char*, u32);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall ReadFile(HANDLE, void*, u32, u32*, void*);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) u64 __stdcall GetTickCount64();
__declspec(dllimport) short __stdcall GetAsyncKeyState(int);
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT

// The optimiser turns byte loops into these calls; there is no CRT to provide them.
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

const char* const kRsoName = "?renderSingleObject@SceneManager@Ogre@@IEAAXPEAVRenderable@2@PEBVPass@2@_N2@Z";
const u8 kRsoPrologue[14] = {0x4c, 0x8b, 0xdc, 0x45, 0x88, 0x4b, 0x20, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};

typedef void (*RsoFn)(void* sceneMgr, void* rend, const void* pass, bool scissor, bool lightIter);
typedef void* (*GetParentFn)(const void* subEntity);         // SubEntity::getParent -> Entity*
typedef const void* (*GetMeshFn)(const void* entity);        // Entity::getMesh -> const MeshPtr&
typedef const void* (*GetStrFn)(const void* obj);            // getName -> const String&
typedef bool (*HasSkelFn)(const void* entity);
typedef void* (*GetNodeFn)(const void* movable);             // getParentNode / getParentSceneNode
typedef float* (*NodePosFn)(const void* node, float* out);   // Node::_getDerivedPosition (by value)
typedef const float* (*CamPosFn)(const void* camera);        // Camera::getDerivedPosition
typedef void* (*CamInProgFn)(const void* sceneMgr);

RsoFn gRso = 0;
GetParentFn gGetParent = 0;
GetMeshFn gGetMesh = 0;
GetStrFn gResName = 0;
GetStrFn gMovName = 0;
HasSkelFn gHasSkel = 0;
GetNodeFn gParentNode = 0;
GetNodeFn gParentSceneNode = 0;
NodePosFn gNodePos = 0;
CamPosFn gCamPos = 0;
CamInProgFn gCamInProg = 0;
void* gSubEntityVt = 0;

// Settings
float gMainDist = 1000.0f;
float gShadowDist = 0.0f;
bool gEnabled = true;
bool gHotkeys = false;

const int kMaxPatterns = 64;
char gSmall[kMaxPatterns][32];
int gNumSmall = 0;
char gKeep[kMaxPatterns][32];
int gNumKeep = 0;

const char* const kDefaultSmall[] = {
    "haircut", "hair", "beard", "moustache", "hat", "helmet", "headgear", "mask", "glasses",
    "hood", "bandana", "turban", "backpack", "bag", "katana", "polearm", "club", "sabre",
    "nodachi", "blade", "crossbow", "jitte", "sword", "axe", "hammer", "knife", "spear",
    "naginata", "wakizashi", "staff", "bow", "hoe", "coins", 0};
const char* const kDefaultKeep[] = {"human_", "bone_", "body", "skin", 0};

// Per-mesh decision cache (open addressing on Mesh*).
struct MeshInfo { const void* mesh; bool small; };
const int kMeshSlots = 4096;
MeshInfo gMeshes[kMeshSlots];

// Camera kind cache: shadow cameras are named rtw*/csm*.
struct CamInfo { const void* cam; bool shadow; };
CamInfo gCams[16];
int gNumCams = 0;
float gMainCamPos[3] = {0, 0, 0};

u64 gSkipMain = 0, gSkipShadow = 0, gDraws = 0;
u64 gLastReport = 0;
bool gKeyDown = false;

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

char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// MSVC 2010 std::string: 16-byte buffer/pointer, size at +0x10, capacity at +0x18.
const char* strChars(const void* str, u64* size) {
    const u8* s = (const u8*)str;
    *size = *(const u64*)(s + 0x10);
    u64 cap = *(const u64*)(s + 0x18);
    return cap >= 16 ? *(const char* const*)s : (const char*)s;
}

bool contains(const char* hay, u64 n, const char* needle) {
    for (u64 i = 0; i < n; ++i) {
        u64 j = 0;
        while (needle[j] && i + j < n && lower(hay[i + j]) == needle[j]) ++j;
        if (!needle[j]) return true;
    }
    return false;
}

bool isSmallMesh(const void* mesh) {
    u64 h = ((u64)mesh >> 4) * 0x9e3779b97f4a7c15ull;
    u32 slot = (u32)(h >> 52);
    for (int probe = 0; probe < kMeshSlots; ++probe, slot = (slot + 1) & (kMeshSlots - 1)) {
        MeshInfo& m = gMeshes[slot];
        if (m.mesh == mesh) return m.small;
        if (!m.mesh) {
            u64 n;
            const char* name = strChars(gResName(mesh), &n);
            bool keep = false, small = false;
            for (int i = 0; i < gNumKeep && !keep; ++i) keep = contains(name, n, gKeep[i]);
            for (int i = 0; i < gNumSmall && !keep && !small; ++i) small = contains(name, n, gSmall[i]);
            m.mesh = mesh;
            m.small = small && !keep;
            return m.small;
        }
    }
    return false;  // table full: never skip
}

bool isShadowCam(const void* cam) {
    for (int i = 0; i < gNumCams; ++i)
        if (gCams[i].cam == cam) return gCams[i].shadow;
    u64 n;
    const char* name = strChars(gMovName(cam), &n);
    bool shadow = n >= 3 && ((lower(name[0]) == 'r' && lower(name[1]) == 't' && lower(name[2]) == 'w') ||
                             (lower(name[0]) == 'c' && lower(name[1]) == 's' && lower(name[2]) == 'm'));
    if (gNumCams < 16) { gCams[gNumCams].cam = cam; gCams[gNumCams].shadow = shadow; ++gNumCams; }
    return shadow;
}

void pollKey() {
    const int VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_INSERT = 0x2d;
    bool mod = (GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL)) & 0x8000;
    bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (down && !gKeyDown && !mod) {
        gEnabled = !gEnabled;
        logTime();
        logStr(gEnabled ? "Insert: filter ON\r\n" : "Insert: filter OFF\r\n");
    }
    gKeyDown = down;
}

void report() {
    u64 now = GetTickCount64();
    if (now - gLastReport < 10000) return;
    if (gLastReport) {
        logTime();
        logStr("last 10 s: draws ");
        logUint(gDraws);
        logStr(", skipped main ");
        logUint(gSkipMain);
        logStr(", skipped shadow ");
        logUint(gSkipShadow);
        logStr(gEnabled ? " (filter on)\r\n" : " (filter off)\r\n");
    }
    gDraws = gSkipMain = gSkipShadow = 0;
    gLastReport = now;
}

bool shouldSkip(void* sceneMgr, void* rend) {
    if (*(void* const*)rend != gSubEntityVt) return false;
    void* ent = gGetParent(rend);
    if (!ent) return false;
    const void* meshPtr = gGetMesh(ent);
    const void* mesh = meshPtr ? *(const void* const*)meshPtr : 0;
    if (!mesh || !isSmallMesh(mesh)) return false;

    // Only characters: skinned entities, or objects attached to a bone.
    void* sceneNode = gParentSceneNode(ent);
    if (!sceneNode) return false;
    if (!gHasSkel(ent) && gParentNode(ent) == sceneNode) return false;

    void* cam = gCamInProg(sceneMgr);
    if (!cam) return false;
    bool shadow = isShadowCam(cam);
    float limit = shadow ? gShadowDist : gMainDist;
    if (!shadow && limit <= 0.0f) return false;          // main: 0 = never skip
    if (shadow && limit <= 0.0f) { ++gSkipShadow; return true; }  // shadow: 0 = always skip

    float pos[3];
    gNodePos(sceneNode, pos);
    float dx = pos[0] - gMainCamPos[0], dy = pos[1] - gMainCamPos[1], dz = pos[2] - gMainCamPos[2];
    if (dx * dx + dy * dy + dz * dz <= limit * limit) return false;
    if (shadow) ++gSkipShadow; else ++gSkipMain;
    return true;
}

void rsoHook(void* sceneMgr, void* rend, const void* pass, bool scissor, bool lightIter) {
    ++gDraws;
    if ((gDraws & 1023) == 0) {
        if (gHotkeys) pollKey();
        report();
    }
    // Track the main camera's position for distance checks in every pass.
    void* cam = gCamInProg(sceneMgr);
    if (cam && !isShadowCam(cam)) {
        const float* c = gCamPos(cam);
        gMainCamPos[0] = c[0]; gMainCamPos[1] = c[1]; gMainCamPos[2] = c[2];
    }
    if (gEnabled && shouldSkip(sceneMgr, rend)) return;
    gRso(sceneMgr, rend, pass, scissor, lightIter);
}

bool same(const u8* p, const u8* expected, int n) {
    for (int i = 0; i < n; ++i)
        if (p[i] != expected[i]) return false;
    return true;
}

void addPattern(char (*list)[32], int* count, const char* s, int len) {
    if (*count >= kMaxPatterns || len <= 0) return;
    if (len > 31) len = 31;
    for (int i = 0; i < len; ++i) list[*count][i] = lower(s[i]);
    list[*count][len] = 0;
    ++*count;
}

float parseFloat(const char* s, int len) {
    float v = 0;
    int i = 0;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

bool startsWith(const char* s, int len, const char* p) {
    int i = 0;
    for (; p[i]; ++i)
        if (i >= len || s[i] != p[i]) return false;
    return true;
}

void loadRules() {
    // rules.txt next to this DLL.
    char path[520];
    HANDLE self = 0;
    if (!GetModuleHandleExA(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, (const char*)&loadRules, &self)) self = 0;
    u32 n = self ? GetModuleFileNameA(self, path, 500) : 0;
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') --n;
    const char* name = "rules.txt";
    for (int i = 0; name[i]; ++i) path[n++] = name[i];
    path[n] = 0;

    static char buf[16384];
    u32 got = 0;
    HANDLE f = CreateFileA(path, 0x80000000 /* GENERIC_READ */, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0);
    if (f != (HANDLE)-1) {
        ReadFile(f, buf, sizeof buf - 1, &got, 0);
        CloseHandle(f);
    }
    if (!got) {
        logStr("rules.txt not found, using built-in defaults\r\n");
        for (int i = 0; kDefaultSmall[i]; ++i) {
            int l = 0; while (kDefaultSmall[i][l]) ++l;
            addPattern(gSmall, &gNumSmall, kDefaultSmall[i], l);
        }
        for (int i = 0; kDefaultKeep[i]; ++i) {
            int l = 0; while (kDefaultKeep[i][l]) ++l;
            addPattern(gKeep, &gNumKeep, kDefaultKeep[i], l);
        }
        return;
    }
    buf[got] = 0;
    u32 i = 0;
    while (i < got) {
        u32 start = i;
        while (i < got && buf[i] != '\n' && buf[i] != '\r') ++i;
        const char* line = buf + start;
        int len = (int)(i - start);
        while (i < got && (buf[i] == '\n' || buf[i] == '\r')) ++i;
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) --len;
        if (len == 0 || line[0] == '#') continue;
        if (startsWith(line, len, "main_distance=")) gMainDist = parseFloat(line + 14, len - 14);
        else if (startsWith(line, len, "shadow_distance=")) gShadowDist = parseFloat(line + 16, len - 16);
        else if (startsWith(line, len, "small=")) addPattern(gSmall, &gNumSmall, line + 6, len - 6);
        else if (startsWith(line, len, "keep=")) addPattern(gKeep, &gNumKeep, line + 5, len - 5);
    }
    logStr("rules.txt loaded\r\n");
}

void install() {
    char env[8];
    u32 len = GetEnvironmentVariableA("KENSHI_DETAILCULL", env, sizeof env);
    if (len == 1 && env[0] == '0') gEnabled = false;
    len = GetEnvironmentVariableA("KENSHI_PERF_DEBUG", env, sizeof env);
    gHotkeys = len == 1 && env[0] == '1';

    loadRules();
    logStr("main_distance ");
    logUint((u64)gMainDist);
    logStr(", shadow_distance ");
    logUint((u64)gShadowDist);
    logStr(gShadowDist <= 0 ? " (always skip in shadows)" : "");
    logStr(", small patterns ");
    logUint(gNumSmall);
    logStr(", keep patterns ");
    logUint(gNumKeep);
    logStr(gHotkeys ? ", hotkey Insert: on\r\n" : ", hotkey Insert: off\r\n");

    HANDLE ogre = GetModuleHandleA("OgreMain_x64.dll");
    if (!ogre) { logStr("OgreMain not loaded, not patching\r\n"); return; }
    u8* rso = (u8*)GetProcAddress(ogre, kRsoName);
    gGetParent = (GetParentFn)GetProcAddress(ogre, "?getParent@SubEntity@Ogre@@QEBAPEAVEntity@2@XZ");
    gGetMesh = (GetMeshFn)GetProcAddress(ogre, "?getMesh@Entity@Ogre@@QEBAAEBV?$SharedPtr@VMesh@Ogre@@@2@XZ");
    gResName = (GetStrFn)GetProcAddress(ogre, "?getName@Resource@Ogre@@UEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    gMovName = (GetStrFn)GetProcAddress(ogre, "?getName@MovableObject@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    gHasSkel = (HasSkelFn)GetProcAddress(ogre, "?hasSkeleton@Entity@Ogre@@QEBA_NXZ");
    gParentNode = (GetNodeFn)GetProcAddress(ogre, "?getParentNode@MovableObject@Ogre@@QEBAPEAVNode@2@XZ");
    gParentSceneNode = (GetNodeFn)GetProcAddress(ogre, "?getParentSceneNode@MovableObject@Ogre@@QEBAPEAVSceneNode@2@XZ");
    gNodePos = (NodePosFn)GetProcAddress(ogre, "?_getDerivedPosition@Node@Ogre@@QEBA?AVVector3@2@XZ");
    gCamPos = (CamPosFn)GetProcAddress(ogre, "?getDerivedPosition@Camera@Ogre@@QEBAAEBVVector3@2@XZ");
    gCamInProg = (CamInProgFn)GetProcAddress(ogre, "?getCameraInProgress@SceneManager@Ogre@@QEBAPEAVCamera@2@XZ");
    gSubEntityVt = GetProcAddress(ogre, "??_7SubEntity@Ogre@@6B@");
    if (!rso || !gGetParent || !gGetMesh || !gResName || !gMovName || !gHasSkel || !gParentNode ||
        !gParentSceneNode || !gNodePos || !gCamPos || !gCamInProg || !gSubEntityVt) {
        logStr("OgreMain exports not found, not patching\r\n");
        return;
    }
    if (!same(rso, kRsoPrologue, sizeof kRsoPrologue)) {
        logStr("renderSingleObject prologue differs (other hook or different build), not patching\r\n");
        return;
    }

    // Trampoline: original 14-byte prologue + jmp back; then jmp hook at the function start.
    u8* tramp = (u8*)VirtualAlloc(0, 4096, 0x3000, 0x40);
    if (!tramp) { logStr("VirtualAlloc failed\r\n"); return; }
    for (int i = 0; i < 14; ++i) tramp[i] = kRsoPrologue[i];
    u8* back = tramp + 14;
    back[0] = 0xff; back[1] = 0x25; back[2] = back[3] = back[4] = back[5] = 0;
    u64 ret = (u64)(rso + 14);
    for (int i = 0; i < 8; ++i) back[6 + i] = (u8)(ret >> (8 * i));
    gRso = (RsoFn)tramp;

    u8 patch[14] = {0xff, 0x25, 0, 0, 0, 0};
    u64 target = (u64)&rsoHook;
    for (int i = 0; i < 8; ++i) patch[6 + i] = (u8)(target >> (8 * i));
    u32 old;
    if (!VirtualProtect(rso, 14, 0x40, &old)) { logStr("VirtualProtect failed\r\n"); return; }
    for (int i = 0; i < 14; ++i) rso[i] = patch[i];
    VirtualProtect(rso, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), rso, 14);
    logTime();
    logStr(gEnabled ? "hooked SceneManager::renderSingleObject, filter ON\r\n"
                    : "hooked SceneManager::renderSingleObject, filter OFF (KENSHI_DETAILCULL=0)\r\n");
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("CharacterDetailCull.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("CharacterDetailCull 1.0\r\n");
    install();
    // The log stays open for the periodic skip counts and toggles.
}
