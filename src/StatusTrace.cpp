#include "StatusTrace.h"
#include "detours.h"
#include <intrin.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace StatusTrace {
namespace {
// Addresses are for the reference client build 622e1068... (see docs/COLLECTION_BOOK_RE.md).
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0;   // pointer to the UI window manager
constexpr UINT_PTR FindWindowFn = 0x00585580;    // thiscall(manager, id) -> window* (nullptr when absent)
constexpr UINT_PTR ShowWindowFn = 0x0058C600;    // thiscall(manager, id, show), ret 8
constexpr UINT_PTR SetTextFn = 0x004F9610;       // thiscall(textControl, const char*), ret 4
constexpr int StatusWindowId = 7;

using FindFn = void*(__thiscall*)(void*, int);
using ShowFn = void(__thiscall*)(void*, int, int);
using SetFn = void(__thiscall*)(void*, const char*);

ShowFn originalShow = reinterpret_cast<ShowFn>(ShowWindowFn);
SetFn originalSetText = reinterpret_cast<SetFn>(SetTextFn);
HANDLE logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION lock;
volatile LONG capturing = 0;
bool keyWasDown = false;

void Log(const char* format, ...) {
    if (logFile == INVALID_HANDLE_VALUE) return;
    char line[600];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(line, sizeof line - 2, format, args);
    va_end(args);
    if (n <= 0) return;
    if (n > static_cast<int>(sizeof line) - 3) n = static_cast<int>(sizeof line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    EnterCriticalSection(&lock);
    DWORD written = 0;
    WriteFile(logFile, line, static_cast<DWORD>(n), &written, nullptr);
    LeaveCriticalSection(&lock);
}

bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

// Printable copy of a (possibly non-text) C string; stops at 90 characters.
void Printable(const char* text, char* out, size_t cap) {
    size_t i = 0;
    if (Readable(text, 1)) {
        for (; i + 1 < cap && i < 90 && text[i]; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            out[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
        }
    }
    out[i] = 0;
}

void __fastcall DetourSetText(void* self, void*, const char* text) {
    if (capturing) {
        char shown[100];
        Printable(text, shown, sizeof shown);
        const unsigned id = Readable(self, 12) ? *reinterpret_cast<const unsigned*>(static_cast<const char*>(self) + 8) : 0xFFFFFFFFu;
        Log("SetText caller=%p control=%p id=%u text=\"%s\"", _ReturnAddress(), self, id, shown);
    }
    originalSetText(self, text);
}

void __fastcall DetourShow(void* manager, void*, int id, int show) {
    Log("ShowWindow caller=%p manager=%p id=%d show=%d", _ReturnAddress(), manager, id, show);
    originalShow(manager, id, show);
}

// Dumps the first 0x800 bytes of a window object as dwords. A dword that points to readable memory is followed by the
// dword it points at, which shows vtables (addresses in the exe image) and child objects at a glance.
void DumpWindow(void* window) {
    Log("---- window object %p (id %d)", window, StatusWindowId);
    for (unsigned offset = 0; offset < 0x800; offset += 4) {
        const unsigned value = *reinterpret_cast<const unsigned*>(static_cast<const char*>(window) + offset);
        if (value == 0) continue;
        if (value >= 0x10000 && Readable(reinterpret_cast<const void*>(static_cast<UINT_PTR>(value)), 4)) {
            const unsigned pointee = *reinterpret_cast<const unsigned*>(static_cast<UINT_PTR>(value));
            Log("  +%03X = %08X -> %08X", offset, value, pointee);
        } else {
            Log("  +%03X = %08X", offset, value);
        }
    }
}


// ---- Window list ----------------------------------------------------------------------------------------------------
// The manager keeps its windows in a list whose head is at manager+4; each node is {next, prev, window*} and the window
// id is at window+0x34, the visible byte at window+0x38 (see docs/COLLECTION_BOOK_RE.md).
constexpr UINT_PTR TextVtable = 0x00C2EB54;
struct WindowInfo { void* window; int id; unsigned vtable; bool visible; };
int CollectWindows(WindowInfo* out, int cap) {
    int count = 0;
    __try {
        const auto manager = *reinterpret_cast<char**>(ManagerGlobal);
        if (!manager) return 0;
        const UINT_PTR head = reinterpret_cast<UINT_PTR>(manager + 4);
        UINT_PTR node = *reinterpret_cast<const UINT_PTR*>(head);
        for (int guard = 0; node && node != head && guard < 600 && count < cap; ++guard) {
            char* window = *reinterpret_cast<char**>(node + 8);
            if (window && Readable(window, 0x40)) {
                out[count++] = {window, *reinterpret_cast<int*>(window + 0x34), *reinterpret_cast<unsigned*>(window),
                                *reinterpret_cast<unsigned char*>(window + 0x38) != 0};
            }
            node = *reinterpret_cast<const UINT_PTR*>(node);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// Text controls of one window, found like UiTrace.cpp does (vector triples among the first members). The text is stored at
// control+0x5C: both the dword there and an inline reading are logged.
void DumpTexts(void* window) {
    __try {
        const auto base = reinterpret_cast<UINT_PTR>(window);
        for (UINT_PTR off = 0x30; off < 0x400; off += 4) {
            const UINT_PTR first = *reinterpret_cast<const UINT_PTR*>(base + off);
            const UINT_PTR last = *reinterpret_cast<const UINT_PTR*>(base + off + 4);
            if (first < 0x10000 || last < first || ((last - first) & 3) || last - first > 4 * 512) continue;
            for (UINT_PTR e = first; e < last; e += 4) {
                const UINT_PTR p = *reinterpret_cast<const UINT_PTR*>(e);
                if (p < 0x10000 || !Readable(reinterpret_cast<void*>(p), 0x80)) continue;
                if (*reinterpret_cast<const UINT_PTR*>(p) != TextVtable) continue;
                const unsigned word = *reinterpret_cast<const unsigned*>(p + 0x5C);
                char inlineText[100], pointedText[100];
                Printable(reinterpret_cast<const char*>(p + 0x5C), inlineText, sizeof inlineText);
                Printable(reinterpret_cast<const char*>(static_cast<UINT_PTR>(word)), pointedText, sizeof pointedText);
                Log("    text control=%p id=%d +5C=%08X inline=\"%s\" pointed=\"%s\"", reinterpret_cast<void*>(p),
                    *reinterpret_cast<const int*>(p + 8), word, inlineText, pointedText);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("    (text scan faulted)"); }
}

void DumpWindowList() {
    WindowInfo windows[600];
    const int n = CollectWindows(windows, 600);
    Log("---- %d windows in the manager list", n);
    for (int i = 0; i < n; ++i) {
        Log("  window=%p id=%d vtable=%08X visible=%d", windows[i].window, windows[i].id, windows[i].vtable, windows[i].visible ? 1 : 0);
        if (windows[i].visible) DumpTexts(windows[i].window);
    }
}

// Logs every visibility change; opening Character Information may not go through ShowWindow at all.
void WatchVisibility() {
    static int frame = 0;
    static bool known[4096];
    static bool primed = false;
    if (++frame % 6) return;
    WindowInfo windows[600];
    const int n = CollectWindows(windows, 600);
    for (int i = 0; i < n; ++i) {
        const int id = windows[i].id;
        if (id < 0 || id >= 4096) continue;
        if (primed && known[id] != windows[i].visible)
            Log("VISIBILITY id=%d vtable=%08X now %s window=%p", id, windows[i].vtable, windows[i].visible ? "SHOWN" : "hidden", windows[i].window);
        known[id] = windows[i].visible;
    }
    primed = true;
}


// ---- Memory scan (F11) ----------------------------------------------------------------------------------------------
// The Character Information labels never go through SetText, so the player's stat block is found by value instead. The
// default values are the character in the reference screenshot (Seijuro, Lv.115); change them with statustrace_values.txt
// (one decimal number per line, the first four: Strength, Vitality, Intelligence, Dexterity). A match needs the
// first two within 0x100 bytes of each other; the surrounding bytes are logged as hex.
struct Anchor { unsigned a, b, c; };
Anchor LoadAnchor() {
    Anchor values{30013, 30008, 30001};
    FILE* file = nullptr;
    if (fopen_s(&file, "statustrace_values.txt", "r") == 0 && file) {
        unsigned x[3] = {0, 0, 0};
        if (std::fscanf(file, "%u %u %u", &x[0], &x[1], &x[2]) == 3) values = {x[0], x[1], x[2]};
        std::fclose(file);
    }
    return values;
}

void HexLine(const unsigned char* p, UINT_PTR address, size_t n) {
    char text[200];
    int len = std::snprintf(text, sizeof text, "      %08X:", static_cast<unsigned>(address));
    for (size_t i = 0; i < n && len < 190; i += 4)
        len += std::snprintf(text + len, sizeof text - len, " %08X", *reinterpret_cast<const unsigned*>(p + i));
    Log("%s", text);
}

void ScanStats() {
    const Anchor anchor = LoadAnchor();
    Log("==== stat scan: %u %u %u", anchor.a, anchor.b, anchor.c);
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    UINT_PTR cursor = reinterpret_cast<UINT_PTR>(info.lpMinimumApplicationAddress);
    const UINT_PTR end = reinterpret_cast<UINT_PTR>(info.lpMaximumApplicationAddress);
    int found = 0;
    while (cursor < end && found < 40) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof mbi)) break;
        const UINT_PTR base = reinterpret_cast<UINT_PTR>(mbi.BaseAddress);
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                              (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY));
        if (readable && mbi.RegionSize >= 0x100) {
            const auto* data = reinterpret_cast<const unsigned char*>(base);
            const size_t size = mbi.RegionSize;
            __try {
                for (size_t off = 0; off + 4 <= size && found < 40; off += 4) {
                    if (*reinterpret_cast<const unsigned*>(data + off) != anchor.a) continue;
                    const size_t from = off > 0x100 ? off - 0x100 : 0;
                    const size_t to = off + 0x100 < size ? off + 0x100 : size;
                    bool second = false;
                    for (size_t i = from; i + 4 <= to; i += 4) second = second || *reinterpret_cast<const unsigned*>(data + i) == anchor.b;
                    if (!second) continue;
                    ++found;
                    Log("  match at %08X (region %08X size %X protect %X type %X)", static_cast<unsigned>(base + off),
                        static_cast<unsigned>(base), static_cast<unsigned>(size), mbi.Protect, mbi.Type);
                    const size_t start = off >= 0x40 ? off - 0x40 : 0;
                    for (size_t row = start; row < start + 0x180 && row + 16 <= size; row += 16) HexLine(data + row, base + row, 16);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        cursor = base + mbi.RegionSize;
    }
    Log("==== stat scan done, %d matches", found);
}

void StartCapture() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        void* window = manager ? reinterpret_cast<FindFn>(FindWindowFn)(manager, StatusWindowId) : nullptr;
        Log("==== capture ON  manager=%p statusWindow=%p", manager, window);
        if (window) DumpWindow(window);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("==== capture ON  (window lookup faulted)");
    }
    InterlockedExchange(&capturing, 1);
}

void StopCapture() {
    InterlockedExchange(&capturing, 0);
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        void* window = manager ? reinterpret_cast<FindFn>(FindWindowFn)(manager, StatusWindowId) : nullptr;
        Log("==== capture OFF statusWindow=%p", window);
        if (window) DumpWindow(window); // second dump: shows which members the open window filled in
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("==== capture OFF (window lookup faulted)");
    }
}

bool ImageMatches() {
    // First bytes of the two hooked functions are only checked for readability; Detours relocates the real prologue.
    return Readable(reinterpret_cast<const void*>(ShowWindowFn), 16) && Readable(reinterpret_cast<const void*>(SetTextFn), 16);
}
} // namespace

LONG Attach() {
    if (GetFileAttributesW(L"statustrace.on") == INVALID_FILE_ATTRIBUTES) return NO_ERROR;
    InitializeCriticalSection(&lock);
    logFile = CreateFileW(L"novaclient_statustrace.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile == INVALID_HANDLE_VALUE || !ImageMatches()) return NO_ERROR;
    Log("status trace armed; F10 toggles capture");
    LONG result = DetourAttach(reinterpret_cast<PVOID*>(&originalSetText), reinterpret_cast<PVOID>(DetourSetText));
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&originalShow), reinterpret_cast<PVOID>(DetourShow));
    return result;
}

void Frame() {
    if (logFile == INVALID_HANDLE_VALUE) return;
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !keyWasDown) {
        DumpWindowList(); // every window with its visibility and, for the visible ones, the text controls
        if (capturing) StopCapture();
        else StartCapture();
    }
    const bool scanDown = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    static bool scanWasDown = false;
    if (scanDown && !scanWasDown) ScanStats();
    scanWasDown = scanDown;
    WatchVisibility();
    keyWasDown = down;
}
} // namespace StatusTrace
