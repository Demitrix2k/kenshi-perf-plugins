// StackProbe: diagnostic RE_Kenshi plugin that samples real call stacks of Kenshi's
// main thread. perf can't do this under Wine: MSVC code has no frame pointers.
//
// When perfctl.ini sets "stackprobe=1", a sampler thread runs for 10 seconds at
// ~1 kHz: it suspends the main thread, reads its registers, walks the stack with the
// x64 unwind data (.pdata) of the loaded PE modules, resumes it, and counts each
// distinct call chain. Afterwards it writes StackProbe.log (module table + chains as
// module+RVA, leaf first) and sets itself idle (status "stackprobe=0"); set it to 1
// again for another run. Symbolize with tools/analysis/stacksym.py.
//
// While the main thread is suspended the sampler does no allocation, no logging and
// no loader calls (it could hold their locks): the module table is built beforehand,
// the function entry is found by our own binary search, and the stack is unwound by
// our own x64 unwind-code interpreter, which checks every stack read against the
// thread's TEB stack bounds and simply ends the chain on anything unexpected.
// (1.0 used Wine's RtlVirtualUnwind, which dereferenced a null pointer on some frame
// and crashed the game; 1.1 never calls it.) Built without a CRT (see build.sh).

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef void* HANDLE;

struct RuntimeFunction { u32 begin, end, unwind; };

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
__declspec(dllimport) u32 __stdcall GetCurrentThreadId();
__declspec(dllimport) HANDLE __stdcall OpenThread(u32, int, u32);
__declspec(dllimport) u32 __stdcall SuspendThread(HANDLE);
__declspec(dllimport) u32 __stdcall ResumeThread(HANDLE);
__declspec(dllimport) int __stdcall GetThreadContext(HANDLE, void*);
__declspec(dllimport) int __stdcall K32EnumProcessModules(HANDLE, HANDLE*, u32, u32*);
__declspec(dllimport) u32 __stdcall GetModuleFileNameA(HANDLE, char*, u32);
__declspec(dllimport) u64 __stdcall GetTickCount64();
__declspec(dllimport) int __stdcall SetThreadPriority(HANDLE, int);
__declspec(dllimport) HANDLE __stdcall GetCurrentThread();
__declspec(dllimport) int __stdcall NtQueryInformationThread(HANDLE, int, void*, u32, u32*);
struct SYSTEMTIME { unsigned short y, mo, dow, d, h, mi, s, ms; };
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME*);
int _fltused = 0;  // MSVC ABI marker normally provided by the CRT
void* memset(void* dst, int c, unsigned long long n) {
    volatile unsigned char* d = (volatile unsigned char*)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
void* memcpy(void* dst, const void* src, unsigned long long n) {
    volatile unsigned char* d = (volatile unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}
}

#include "../../common/perfctl.h"

namespace {

// ---- module table (built while the main thread runs) ----
struct Module {
    u64 base, end;
    const RuntimeFunction* pdata;
    u32 count;
    char name[48];
};
const int kMaxModules = 256;
Module gMods[kMaxModules];
int gNumMods = 0;

void copyBaseName(const char* path, char* out, int outSize) {
    const char* s = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') s = p + 1;
    int n = 0;
    while (s[n] && n < outSize - 1) { out[n] = s[n]; ++n; }
    out[n] = 0;
}

void buildModuleTable() {
    static HANDLE mods[kMaxModules];
    u32 needed = 0;
    gNumMods = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    int n = (int)(needed / sizeof(HANDLE));
    if (n > kMaxModules) n = kMaxModules;
    for (int i = 0; i < n; ++i) {
        const u8* b = (const u8*)mods[i];
        u32 pe = *(const u32*)(b + 0x3c);
        if (*(const u32*)(b + pe) != 0x4550) continue;           // "PE\0\0"
        const u8* opt = b + pe + 24;
        if (*(const u16*)opt != 0x20b) continue;                  // PE32+
        Module& m = gMods[gNumMods];
        m.base = (u64)b;
        m.end = m.base + *(const u32*)(opt + 56);                 // SizeOfImage
        u32 pdRva = *(const u32*)(opt + 112 + 8 * 3);             // exception directory
        u32 pdSize = *(const u32*)(opt + 112 + 8 * 3 + 4);
        m.pdata = pdRva ? (const RuntimeFunction*)(b + pdRva) : 0;
        m.count = pdRva ? pdSize / 12 : 0;
        char path[260];
        path[0] = 0;
        GetModuleFileNameA(mods[i], path, sizeof path);
        copyBaseName(path, m.name, sizeof m.name);
        ++gNumMods;
    }
    // sort by base (insertion sort; small n)
    for (int i = 1; i < gNumMods; ++i) {
        Module t = gMods[i];
        int j = i - 1;
        while (j >= 0 && gMods[j].base > t.base) { gMods[j + 1] = gMods[j]; --j; }
        gMods[j + 1] = t;
    }
}

const Module* findModule(u64 pc) {
    int lo = 0, hi = gNumMods - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (pc < gMods[mid].base) hi = mid - 1;
        else if (pc >= gMods[mid].end) lo = mid + 1;
        else return &gMods[mid];
    }
    return 0;
}

RuntimeFunction* findFunction(const Module* m, u64 pc) {
    if (!m->pdata) return 0;
    u32 rva = (u32)(pc - m->base);
    u32 lo = 0, hi = m->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const RuntimeFunction& f = m->pdata[mid];
        if (rva < f.begin) hi = mid;
        else if (rva >= f.end) lo = mid + 1;
        else return (RuntimeFunction*)&f;
    }
    return 0;
}

// ---- x64 CONTEXT (1232 bytes, 16-byte aligned) ----
struct alignas(16) Context { u8 raw[1232]; };
inline u32& ctxFlags(Context& c) { return *(u32*)(c.raw + 0x30); }
inline u64& ctxRsp(Context& c) { return *(u64*)(c.raw + 0x98); }
inline u64& ctxRip(Context& c) { return *(u64*)(c.raw + 0xf8); }
inline u64* ctxRegs(Context& c) { return (u64*)(c.raw + 0x78); }  // rax..r15 in unwind-code order
const u32 CONTEXT_FULL = 0x10000b;  // CONTROL | INTEGER | FLOATING_POINT

// ---- chain table ----
const int kDepth = 24;
struct Chain { u64 hash; u32 count; u8 depth; u64 pc[kDepth]; };
const u32 kChains = 16384;
Chain gChains[kChains];
u32 gSamples = 0, gDropped = 0, gUnwindStops = 0;

HANDLE gMain = 0;
u64 gStackBase = 0, gStackLimit = 0;
volatile int gRequest = 0;  // perfctl "stackprobe"

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
void logHex(u64 v) {
    char t[17];
    int n = 0;
    do { u32 nib = v & 0xf; t[n++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10); v >>= 4; } while (v);
    char buf[20] = {'0', 'x'};
    for (int i = 0; i < n; ++i) buf[2 + i] = t[n - 1 - i];
    buf[2 + n] = 0;
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

// Bounded stack read: false if the address is outside the main thread's stack.
inline bool readStack(u64 a, u64* out) {
    if (a < gStackLimit || a + 8 > gStackBase || (a & 7)) return false;
    *out = *(const u64*)a;
    return true;
}

// Unwinds one frame using the function's x64 unwind codes (the same rules as
// Windows/Wine RtlVirtualUnwind, without epilogue detection). regs = rax..r15
// (index 4 = rsp). Returns false if anything is out of bounds or unexpected.
bool unwindFrame(const Module* m, const RuntimeFunction* f, u64 pc, u64* regs, u64* rip) {
    u64 pcOff = pc - (m->base + f->begin);
    bool first = true;
    for (int chain = 0; chain < 8; ++chain) {
        u32 uw = f->unwind;
        if (uw & 1) {  // indirect: points at another RUNTIME_FUNCTION
            f = (const RuntimeFunction*)(m->base + (uw & ~1u));
            if ((u64)f < m->base || (u64)f + 12 > m->end) return false;
            uw = f->unwind;
        }
        const u8* info = (const u8*)(m->base + uw);
        if ((u64)info < m->base || (u64)info + 4 > m->end) return false;
        u32 version = info[0] & 7, flags = info[0] >> 3, prolog = info[1], count = info[2];
        u32 frameReg = info[3] & 0xf, frameOff = info[3] >> 4;
        if (version < 1 || version > 2 || (u64)info + 4 + 2 * count + 12 > m->end) return false;
        const u16* codes = (const u16*)(info + 4);
        u64 limit = first && pcOff < prolog ? pcOff : ~0ull;  // inside the prologue: only executed codes
        u64 frame = frameReg ? regs[frameReg] - 16ull * frameOff : regs[4];
        for (u32 i = 0; i < count;) {
            u32 codeOff = codes[i] & 0xff, op = (codes[i] >> 8) & 0xf, opInfo = codes[i] >> 12;
            u32 slots = 1;
            switch (op) {
                case 1: slots = opInfo ? 3 : 2; break;
                case 4: case 6: case 8: slots = 2; break;
                case 5: case 7: case 9: slots = 3; break;
            }
            if (version == 2 && op == 6) slots = 1;  // UWOP_EPILOG (v2) markers
            if (i + slots > count) return false;
            if (codeOff > limit) { i += slots; continue; }
            u64 v;
            switch (op) {
                case 0:  // PUSH_NONVOL
                    if (!readStack(regs[4], &v)) return false;
                    regs[opInfo] = v;
                    regs[4] += 8;
                    break;
                case 1:  // ALLOC_LARGE
                    regs[4] += opInfo ? (codes[i + 1] | ((u64)codes[i + 2] << 16)) : 8ull * codes[i + 1];
                    break;
                case 2:  // ALLOC_SMALL
                    regs[4] += 8ull * opInfo + 8;
                    break;
                case 3:  // SET_FPREG
                    regs[4] = frame;
                    break;
                case 4:  // SAVE_NONVOL
                    if (!readStack(frame + 8ull * codes[i + 1], &v)) return false;
                    regs[opInfo] = v;
                    break;
                case 5:  // SAVE_NONVOL_FAR
                    if (!readStack(frame + (codes[i + 1] | ((u64)codes[i + 2] << 16)), &v)) return false;
                    regs[opInfo] = v;
                    break;
                case 10: {  // PUSH_MACHFRAME: rip and rsp come from the machine frame
                    u64 base = regs[4] + (opInfo ? 8 : 0), sp;
                    if (!readStack(base, &v) || !readStack(base + 24, &sp)) return false;
                    *rip = v;
                    regs[4] = sp;
                    return true;
                }
                default:  // XMM saves / epilog markers: nothing to restore for walking
                    if (op > 10) return false;
                    break;
            }
            i += slots;
        }
        if (!(flags & 4)) break;  // UNW_FLAG_CHAININFO: continue with the parent entry
        f = (const RuntimeFunction*)(codes + ((count + 1) & ~1u));
        if ((u64)f < m->base || (u64)f + 12 > m->end) return false;
        first = false;
    }
    u64 ret;
    if (!readStack(regs[4], &ret)) return false;
    *rip = ret;
    regs[4] += 8;
    return true;
}

// One sample. Runs while the main thread is suspended: no allocation, no locks.
void sampleOnce() {
    static Context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctxFlags(ctx) = CONTEXT_FULL;
    if (SuspendThread(gMain) == (u32)-1) return;
    u64 pcs[kDepth];
    int depth = 0;
    if (GetThreadContext(gMain, &ctx)) {
        u64 regs[16];
        for (int r = 0; r < 16; ++r) regs[r] = ctxRegs(ctx)[r];
        u64 pc = ctxRip(ctx);
        for (; depth < kDepth;) {
            u64 sp = regs[4];
            if (!pc || sp < gStackLimit || sp >= gStackBase) break;
            pcs[depth++] = pc;
            const Module* m = findModule(pc);
            const RuntimeFunction* f = m ? findFunction(m, pc) : 0;
            u64 next = 0;
            if (f) {
                if (!unwindFrame(m, f, pc, regs, &next)) { ++gUnwindStops; break; }
            } else if (depth == 1) {
                // leaf without unwind data at the top: return address at [rsp]
                if (!readStack(sp, &next)) break;
                regs[4] = sp + 8;
            } else {
                ++gUnwindStops;
                break;
            }
            if (regs[4] <= sp) { ++gUnwindStops; break; }  // the stack must grow toward the base
            pc = next;
        }
    }
    ResumeThread(gMain);
    if (!depth) return;

    u64 h = 1469598103934665603ull;
    for (int i = 0; i < depth; ++i) h = (h ^ pcs[i]) * 1099511628211ull;
    h ^= (u64)depth;
    u32 slot = (u32)(h >> 40) & (kChains - 1);
    for (u32 probe = 0; probe < 32; ++probe) {
        Chain& c = gChains[(slot + probe) & (kChains - 1)];
        if (c.count && c.hash == h) { ++c.count; ++gSamples; return; }
        if (!c.count) {
            c.hash = h;
            c.count = 1;
            c.depth = (u8)depth;
            for (int i = 0; i < depth; ++i) c.pc[i] = pcs[i];
            ++gSamples;
            return;
        }
    }
    ++gDropped;
}

void writeAddr(u64 pc) {
    const Module* m = findModule(pc);
    if (m) { logStr(m->name); logStr("+"); logHex(pc - m->base); }
    else logHex(pc);
}

void writeReport(u64 ms) {
    logTime();
    logStr("run: ");
    logUint(gSamples);
    logStr(" samples in ");
    logUint(ms);
    logStr(" ms, ");
    logUint(gDropped);
    logStr(" dropped (table full), ");
    logUint(gUnwindStops);
    logStr(" stacks cut at code without unwind data\r\n");
    logStr("modules:\r\n");
    for (int i = 0; i < gNumMods; ++i) {
        logStr("  "); logStr(gMods[i].name); logStr(" "); logHex(gMods[i].base);
        logStr(" "); logHex(gMods[i].end - gMods[i].base); logStr("\r\n");
    }
    logStr("chains (count|leaf>caller>...):\r\n");
    for (u32 i = 0; i < kChains; ++i) {
        Chain& c = gChains[i];
        if (!c.count) continue;
        logUint(c.count);
        logStr("|");
        for (int d = 0; d < c.depth; ++d) {
            if (d) logStr(">");
            writeAddr(c.pc[d]);
        }
        logStr("\r\n");
    }
    logStr("end of run\r\n");
}

u32 __stdcall sampler(void*) {
    SetThreadPriority(GetCurrentThread(), 2 /* THREAD_PRIORITY_HIGHEST */);
    for (;;) {
        while (gRequest != 1) Sleep(200);
        buildModuleTable();  // main thread running: loader calls are safe here
        memset(gChains, 0, sizeof gChains);
        gSamples = gDropped = gUnwindStops = 0;
        logTime();
        logStr("sampling the main thread for 10 s\r\n");
        perfctl::status("stackprobe", "stackprobe=1\n");
        u64 t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 10000) {
            sampleOnce();
            Sleep(1);
        }
        writeReport(GetTickCount64() - t0);
        gRequest = 0;
        perfctl::status("stackprobe", "stackprobe=0\n");
    }
}

void onControl(const char* key, int value) {
    if (perfctl::keyIs(key, "stackprobe") && value == 1 && gRequest == 0) gRequest = 1;
}

void install() {
    u32 tid = GetCurrentThreadId();
    gMain = OpenThread(0x0002 | 0x0008 | 0x0040 /* SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION */, 0, tid);
    if (!gMain) { logStr("OpenThread failed\r\n"); return; }
    // Stack bounds from the main thread's TEB (NT_TIB: StackBase +8, StackLimit +0x10).
    struct { i64 exitStatus; u8* teb; u64 rest[4]; } tbi;
    if (NtQueryInformationThread(gMain, 0 /* ThreadBasicInformation */, &tbi, sizeof tbi, 0) != 0 || !tbi.teb) {
        logStr("NtQueryInformationThread failed\r\n");
        return;
    }
    gStackBase = *(u64*)(tbi.teb + 8);
    gStackLimit = *(u64*)(tbi.teb + 0x10);
    logStr("main thread ");
    logUint(tid);
    logStr(", stack ");
    logHex(gStackLimit);
    logStr("-");
    logHex(gStackBase);
    logStr("\r\nset stackprobe=1 in perfctl.ini (perfctl.py set stackprobe=1) to sample 10 s\r\n");
    perfctl::status("stackprobe", "stackprobe=0\n");
    CreateThread(0, 0, sampler, 0, 0, 0);
    perfctl::start(onControl);
}

}  // namespace

__declspec(dllexport) void startPlugin() {
    gLog = CreateFileA("StackProbe.log", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */,
                       0, 2 /* CREATE_ALWAYS */, 0x80 /* FILE_ATTRIBUTE_NORMAL */, 0);
    if (gLog == (HANDLE)-1) gLog = 0;
    logStr("StackProbe 1.1\r\n");
    install();
}
