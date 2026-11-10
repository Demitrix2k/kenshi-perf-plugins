// perfctl.h: shared runtime control for our RE_Kenshi plugins, no hotkeys needed.
//
// A background thread checks "perfctl.ini" in the game folder 4 times a second.
// When the file changes it reads every "key=value" line (integer values, '#'
// comments) and calls the plugin's callback for each one, so the plugin can pick
// the keys it owns. A plugin reports what it actually applied by writing
// "perfctl-status-<name>.txt", which the control panel (tools/perfctl.py)
// shows next to the requested value.
//
// Header-only and CRT-free; include it once from the plugin's .cpp and add the
// kernel32 imports listed below to build.sh.

#ifndef PERFCTL_H
#define PERFCTL_H

extern "C" {
struct PerfCtlFileTime { unsigned int lo, hi; };
struct PerfCtlFileInfo {
    unsigned int attrs;
    PerfCtlFileTime created, accessed, written;
    unsigned int sizeHi, sizeLo;
};
__declspec(dllimport) int __stdcall GetFileAttributesExA(const char*, int, void*);
__declspec(dllimport) void* __stdcall CreateFileA(const char*, unsigned int, unsigned int, void*,
                                                  unsigned int, unsigned int, void*);
__declspec(dllimport) int __stdcall ReadFile(void*, void*, unsigned int, unsigned int*, void*);
__declspec(dllimport) int __stdcall WriteFile(void*, const void*, unsigned int, unsigned int*, void*);
__declspec(dllimport) int __stdcall CloseHandle(void*);
__declspec(dllimport) void* __stdcall CreateThread(void*, unsigned long long,
                                                   unsigned int(__stdcall*)(void*), void*,
                                                   unsigned int, unsigned int*);
__declspec(dllimport) void __stdcall Sleep(unsigned int);
}

namespace perfctl {

typedef void (*Callback)(const char* key, int value);

const char* const kFile = "perfctl.ini";
Callback gCallback = 0;

inline bool keyIs(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

// Parses "key=value" lines. Keys up to 31 chars, values optional sign + digits.
inline void parse(const char* p, const char* end) {
    while (p < end) {
        char key[32];
        int n = 0;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p < end && *p == '#') { while (p < end && *p != '\n') ++p; continue; }
        while (p < end && *p != '=' && *p != '\n' && *p != '\r' && *p != ' ' && n < 31) key[n++] = *p++;
        key[n] = 0;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (n && p < end && *p == '=') {
            ++p;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            bool neg = p < end && *p == '-';
            if (neg) ++p;
            bool any = false;
            int v = 0;
            while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; any = true; }
            if (any) gCallback(key, neg ? -v : v);
        }
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    }
}

inline unsigned int __stdcall pollThread(void*) {
    PerfCtlFileTime last = {0, 0};
    unsigned int lastSize = 0xffffffff;
    for (;;) {
        PerfCtlFileInfo info;
        if (GetFileAttributesExA(kFile, 0 /* GetFileExInfoStandard */, &info) &&
            (info.written.lo != last.lo || info.written.hi != last.hi || info.sizeLo != lastSize)) {
            void* h = CreateFileA(kFile, 0x80000000 /* GENERIC_READ */, 3 /* share read|write */, 0,
                                  3 /* OPEN_EXISTING */, 0x80, 0);
            if (h != (void*)-1) {
                static char buf[4096];
                unsigned int got = 0;
                if (ReadFile(h, buf, sizeof buf, &got, 0)) {
                    last = info.written;
                    lastSize = info.sizeLo;
                    parse(buf, buf + got);
                }
                CloseHandle(h);
            }
        }
        Sleep(250);
    }
}

// Starts polling; the callback runs on the poll thread (also once at start if the
// file exists), so the values it writes must be plain words the game threads read.
inline void start(Callback cb) {
    gCallback = cb;
    CreateThread(0, 0, pollThread, 0, 0, 0);
}

// Replaces perfctl-status-<name>.txt with one line of text.
inline void status(const char* name, const char* text) {
    char path[96] = "perfctl-status-";
    int n = 15;
    for (const char* s = name; *s && n < 86; ++s) path[n++] = *s;
    path[n++] = '.'; path[n++] = 't'; path[n++] = 'x'; path[n++] = 't'; path[n] = 0;
    void* h = CreateFileA(path, 0x40000000 /* GENERIC_WRITE */, 3, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    if (h == (void*)-1) return;
    unsigned int len = 0, w;
    while (text[len]) ++len;
    WriteFile(h, text, len, &w, 0);
    CloseHandle(h);
}

// Small text builder for status lines: Text t; t.add("quadfix=").num(1); status(..., t.b)
struct Text {
    char b[256];
    int n;
    Text() : n(0) { b[0] = 0; }
    Text& add(const char* s) {
        while (*s && n < 255) b[n++] = *s++;
        b[n] = 0;
        return *this;
    }
    Text& num(int v) {
        char t[12];
        int i = 11;
        t[i] = 0;
        bool neg = v < 0;
        unsigned int u = neg ? 0u - (unsigned int)v : (unsigned int)v;
        do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u);
        if (neg) t[--i] = '-';
        return add(t + i);
    }
};

}  // namespace perfctl

#endif
