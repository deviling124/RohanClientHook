#include "UiTrace.h"
#include "detours.h"
#include "Bridge.h"
#include "CollectionBook/CollectionBookClient.h"
#include <algorithm>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace UiTrace {
namespace {
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn originalCreateW = CreateFileW;
CreateFileAFn originalCreateA = CreateFileA;
HANDLE logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION lock;
thread_local bool inHook = false;

// The engine's generic file-to-buffer loader (reference client build 622e1068...). Its prologue is a plain SEH
// frame, so the real caller is the return address on entry. The stub records it and `this`, then continues into
// the Detours trampoline without touching any argument.
constexpr UINT_PTR LoaderAddress = 0x00A1B730;
constexpr unsigned char LoaderPrologue[] = {0x6A, 0xFF, 0x68, 0xE0, 0x1C, 0xC1, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
volatile UINT_PTR lastCaller = 0;
volatile UINT_PTR lastThis = 0;
PVOID originalLoader = reinterpret_cast<PVOID>(LoaderAddress);

__declspec(naked) void LoaderStub() {
    __asm {
        mov eax, [esp]
        mov lastCaller, eax
        mov lastThis, ecx
        jmp dword ptr [originalLoader]
    }
}

bool EndsWithUi(const char* text) {
    const std::size_t n = text ? std::strlen(text) : 0;
    return n >= 3 && text[n - 3] == '.' && (text[n - 2] | 0x20) == 'u' && (text[n - 1] | 0x20) == 'i';
}

void Record(const char* path) {
    char text[900];
    const int n = std::snprintf(text, sizeof text, "[%llu] [%lu] caller=%08X this=%08X %s\r\n", GetTickCount64(),
                                GetCurrentThreadId(), static_cast<unsigned>(lastCaller),
                                static_cast<unsigned>(lastThis), path);
    if (n <= 0 || n >= static_cast<int>(sizeof text)) return;
    EnterCriticalSection(&lock);
    DWORD written = 0;
    WriteFile(logFile, text, static_cast<DWORD>(n), &written, nullptr);
    LeaveCriticalSection(&lock);
}

// CreateFileA is entered first (from the CRT), then forwards to CreateFileW; `inHook` stops the second log line.
HANDLE WINAPI HookCreateW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                          DWORD flags, HANDLE tmpl) {
    if (!inHook && name) {
        inHook = true;
        char narrow[600]{};
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr) && EndsWithUi(narrow))
            Record(narrow);
        inHook = false;
    }
    return originalCreateW(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI HookCreateA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                          DWORD flags, HANDLE tmpl) {
    if (inHook) return originalCreateA(name, access, share, sa, disp, flags, tmpl);
    const bool ui = EndsWithUi(name);
    if (ui) Record(name);
    inHook = ui; // suppresses the duplicate line from the W call that A makes internally
    const HANDLE result = originalCreateA(name, access, share, sa, disp, flags, tmpl);
    inHook = false;
    return result;
}

// ---- Window manager dump -------------------------------------------------------------------------------------
// The UI manager is `*(void**)0xD4C5E0` (the object whose method 0x0058C600 is ShowWindow). Its window list is the
// circular list whose sentinel pointer is at `mgr+4`; each node is {next, prev, window*}; a window's id is `win+0x34`
// and its first dword is the vtable. Every few seconds this writes the list to novaclient_windows.log whenever it
// changed, so the stock window ids and vtables are known without finding the registration code.
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0; // ECX that the stock code loads before calling 0x00585580 (lookup)

void Dump(HANDLE file, const char* title, const UINT_PTR* sentinelSlot, unsigned& countOut) {
    char line[1400];
    countOut = 0;
    __try {
        const auto sentinel = *reinterpret_cast<const UINT_PTR*>(sentinelSlot);
        if (!sentinel) return;
        int n = std::snprintf(line, sizeof line, "== %s (sentinel %08X)\r\n", title, static_cast<unsigned>(sentinel));
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
        auto node = *reinterpret_cast<const UINT_PTR*>(sentinel);
        for (unsigned guard = 0; node && node != sentinel && guard < 2000; ++guard) {
            const auto win = reinterpret_cast<const UINT_PTR*>(reinterpret_cast<const UINT_PTR*>(node)[2]);
            if (win) {
                const auto vtable = reinterpret_cast<const UINT_PTR*>(win[0]);
                n = std::snprintf(line, sizeof line,
                                  "id=0x%X win=%08X vtable=%08X vis=%u en=%u rect=%d,%d,%d,%d slots:",
                                  static_cast<unsigned>(win[0x34 / 4]), static_cast<unsigned>(reinterpret_cast<UINT_PTR>(win)),
                                  static_cast<unsigned>(reinterpret_cast<UINT_PTR>(vtable)),
                                  static_cast<unsigned>(*reinterpret_cast<const unsigned char*>(&win[0x38 / 4])),
                                  static_cast<unsigned>(*reinterpret_cast<const unsigned char*>(
                                      reinterpret_cast<const unsigned char*>(win) + 0x39)),
                                  static_cast<int>(win[0x3C / 4]), static_cast<int>(win[0x40 / 4]),
                                  static_cast<int>(win[0x44 / 4]), static_cast<int>(win[0x48 / 4]));
                for (int s = 0; s < 40 && n > 0 && n < static_cast<int>(sizeof line) - 12; ++s)
                    n += std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), " %08X",
                                       static_cast<unsigned>(vtable[s]));
                line[n++] = '\r';
                line[n++] = '\n';
                WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
            }
            ++countOut;
            node = *reinterpret_cast<const UINT_PTR*>(node);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

DWORD WINAPI WindowDumpThread(void*) {
    HANDLE file = CreateFileW(L"novaclient_windows.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    unsigned lastA = ~0u, lastB = ~0u;
    for (;;) {
        Sleep(8000);
        UINT_PTR manager = 0;
        __try { manager = *reinterpret_cast<const UINT_PTR*>(ManagerGlobal); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!manager) continue;
        // Count first, then rewrite the file only when something changed.
        unsigned a = 0, b = 0;
        HANDLE probe = CreateFileW(L"NUL", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;
        Dump(probe, "list +4", reinterpret_cast<const UINT_PTR*>(manager + 4), a);
        Dump(probe, "list +0x10", reinterpret_cast<const UINT_PTR*>(manager + 0x10), b);
        CloseHandle(probe);
        if (a == lastA && b == lastB) continue;
        lastA = a;
        lastB = b;
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        char head[96];
        const int n = std::snprintf(head, sizeof head, "manager=%08X tick=%llu counts=%u/%u\r\n",
                                    static_cast<unsigned>(manager), GetTickCount64(), a, b);
        DWORD written = 0;
        WriteFile(file, head, static_cast<DWORD>(n), &written, nullptr);
        unsigned ignored = 0;
        Dump(file, "list +4", reinterpret_cast<const UINT_PTR*>(manager + 4), ignored);
        Dump(file, "list +0x10", reinterpret_cast<const UINT_PTR*>(manager + 0x10), ignored);
        FlushFileBuffers(file);
    }
}

// ---- Registration watch --------------------------------------------------------------------------------------
// Finds the code that adds a window to the manager list. As soon as the manager exists, a hardware write watch is set
// on the list sentinel's `prev` dword (`sentinel+4`), which every push_back rewrites. A vectored handler catches the
// single-step after each write and logs the instruction address, a few caller addresses and the window just added.
DWORD mainThreadId = 0;
volatile LONG watchHits = 0;
HANDLE watchLog = INVALID_HANDLE_VALUE;
UINT_PTR watchedAddress = 0;

bool LooksLikeReturn(UINT_PTR value) {
    if (value < 0x00401010 || value >= 0x00C25000) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(value);
    __try {
        return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] == 0x15 || p[-5] == 0x95)) ||
               (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) || (p[-3] == 0xFF && p[-2] == 0x55);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG CALLBACK WatchHandler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    info->ContextRecord->Dr6 = 0;
    const LONG hit = InterlockedIncrement(&watchHits);
    if (hit > 400) { // enough samples; switch the watch off
        info->ContextRecord->Dr7 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    char text[700];
    int n = std::snprintf(text, sizeof text, "#%ld eip=%08X frames:", hit,
                          static_cast<unsigned>(info->ContextRecord->Eip));
    const auto* stack = reinterpret_cast<const UINT_PTR*>(info->ContextRecord->Esp);
    int found = 0;
    __try {
        for (int i = 0; i < 256 && found < 8; ++i)
            if (LooksLikeReturn(stack[i])) {
                n += std::snprintf(text + n, sizeof text - static_cast<std::size_t>(n), " %08X",
                                   static_cast<unsigned>(stack[i]));
                ++found;
            }
        const auto sentinel = watchedAddress - 4;
        const auto node = *reinterpret_cast<const UINT_PTR*>(sentinel + 4);
        const auto win = *reinterpret_cast<const UINT_PTR*>(node + 8);
        const auto id = *reinterpret_cast<const unsigned*>(win + 0x34);
        n += std::snprintf(text + n, sizeof text - static_cast<std::size_t>(n), " | node=%08X win=%08X id=0x%X vtable=%08X",
                           static_cast<unsigned>(node), static_cast<unsigned>(win), id,
                           static_cast<unsigned>(*reinterpret_cast<const UINT_PTR*>(win)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    text[n++] = '\r';
    text[n++] = '\n';
    DWORD written = 0;
    WriteFile(watchLog, text, static_cast<DWORD>(n), &written, nullptr);
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI RegistrationWatchThread(void*) {
    watchLog = CreateFileW(L"novaclient_register.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (watchLog == INVALID_HANDLE_VALUE || !mainThreadId) return 0;
    UINT_PTR sentinel = 0;
    for (int tries = 0; tries < 120000 && !sentinel; ++tries) { // poll every millisecond for up to two minutes
        __try {
            const auto manager = *reinterpret_cast<const UINT_PTR*>(ManagerGlobal);
            if (manager) sentinel = *reinterpret_cast<const UINT_PTR*>(manager + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!sentinel) Sleep(1);
    }
    if (!sentinel) return 0;
    watchedAddress = sentinel + 4;
    AddVectoredExceptionHandler(1, WatchHandler);
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, mainThreadId);
    if (!thread) return 0;
    SuspendThread(thread);
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(thread, &context)) {
        context.Dr0 = static_cast<DWORD>(watchedAddress);
        context.Dr7 = 0x000D0001; // local enable 0, write-only, 4 bytes
        SetThreadContext(thread, &context);
    }
    ResumeThread(thread);
    CloseHandle(thread);
    char head[96];
    const int n = std::snprintf(head, sizeof head, "watching %08X (sentinel %08X)\r\n",
                                static_cast<unsigned>(watchedAddress), static_cast<unsigned>(sentinel));
    DWORD written = 0;
    WriteFile(watchLog, head, static_cast<DWORD>(n), &written, nullptr);
    return 0;
}

// ---- Prototype native window ----------------------------------------------------------------------------------
// Builds window id 0x300 with the stock generic popup constructor 0x00806790(this, id, uiPath) (reference client build),
// registers it with the manager's push_back 0x00526B30 and shows it with 0x0058C600. Runs on the render thread from
// Frame(). Everything is wrapped in SEH so a wrong guess logs instead of crashing the game.
constexpr unsigned PrototypeId = 0x300;
bool prototypeEnabled = false;
bool diagnostics = false;           // uitrace.on exists: tracing and logging are active
bool prototypeBuilt = false;
bool inspectLayout = false;        // the inspect layout file is installed in the client folder: the icon resolver is needed for it too
// Layout used by the prototype: the first line of uitrace_proto.on names a .ui file in bitmaps\interface4\ui
// (default: the stock mail-delete popup).
char prototypePath[260] = "\\Bitmaps\\Interface4\\UI\\post_pop_postdelete.ui";

void ReadPrototypeName() {
    HANDLE file = CreateFileW(L"uitrace_proto.on", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char name[120]{};
    DWORD got = 0;
    ReadFile(file, name, sizeof name - 1, &got, nullptr);
    CloseHandle(file);
    std::size_t n = 0;
    while (n < got && name[n] != '\r' && name[n] != '\n' && name[n] != ' ' && name[n] != '\t') ++n;
    if (n == 0 || n > 100) return;
    for (std::size_t i = 0; i < n; ++i)
        if (name[i] == '\\' || name[i] == '/' || name[i] == ':' || name[i] == '"') return; // file name only
    name[n] = 0;
    std::snprintf(prototypePath, sizeof prototypePath, "\\Bitmaps\\Interface4\\UI\\%s", name);
}

void ProtoLog(const char* text) {
    if (!diagnostics) return;
    HANDLE file = CreateFileW(L"novaclient_proto.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[420];
    const int n = std::snprintf(line, sizeof line, "[%llu] %s\r\n", GetTickCount64(), text);
    DWORD written = 0;
    if (n > 0) WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
}

// Handler for the Register button (control id 4 in collection_book.ui). Stock handlers are `thiscall(window, arg)`
// with `ret 4`; __fastcall with an unused edx has the same register/stack shape.
volatile LONG registerClicks = 0;
void __fastcall RegisterHandler(void* self, void*, int arg) {
    const LONG n = InterlockedIncrement(&registerClicks);
    char text[120];
    std::snprintf(text, sizeof text, "Register clicked #%ld window=%08X arg=%d", n,
                  static_cast<unsigned>(reinterpret_cast<UINT_PTR>(self)), arg);
    ProtoLog(text);
}

struct HandlerEntry { int key; int sub; void* fn; int pad; };
using MapInsertFn = void(__thiscall*)(void*, void*, void*);

// ---- Item icons through native image controls ------------------------------------------------------------------
// A native image control (type 1) draws by resolving `imageId -> handle` with 0x005C9900 (thiscall, (out*, id), ret 8),
// then `handle -> texture` with 0x00A08390. Item icons use the same second step; their handle comes from the item
// manager tree (same walk as EventShop/ItemIcons.cpp). This detour answers a reserved id range with the icon handle of
// the item type assigned to that slot, so a layout can show item icons with ordinary image controls.
volatile LONG selectedSet = 0;  // set row currently selected (0..4); drives the glow below and the centre pane
// Active tab: 1 Sets, 2 Collection, 3 Decompose. Layout images wrapped by gate() in the layout script (AliasFlag | mask << 16 |
// real id, mask bit n = tab n+1) are drawn only while their tab is active.
volatile LONG activeMode = 1;
// The results popup (what a disassembly gave) is an overlay of the book window: layout images with mask bit 8 are drawn only while it is open.
volatile LONG resultsOpen = 0;
constexpr int OverlayBit = 8;
int ActiveBits() { return (1 << (activeMode - 1)) | (resultsOpen ? OverlayBit : 0); }
constexpr int AliasFlag = 0x100000;
// Inspect window pages: an image id | InspectAlias | page mask << 16 resolves only while that page is shown (1 Battle, 2 Costume).
constexpr int InspectAlias = 0x200000;
volatile LONG inspectPage = 1;
// Button hover glow: 0x7050 + key (fill) and 0x7070 + key (border) resolve to the glow textures only for the hovered button.
volatile LONG hoverKey = -1;
constexpr int FirstItemBoxKey = 22;   // hover keys from here on are the item boxes of the Collection tab (glow textures); below: buttons
constexpr int HoverFillIds = 0x7050;
constexpr int HoverEdgeIds = 0x7070;
// Selection glow: five fill ids (0x7030 + row) and five border ids (0x7040 + row) resolve to the glow textures only while
// that row is selected, otherwise to "no image". Texture ids match FIXED_TEX in tools/make_collection_book_layout.py.
constexpr int GlowFillIds = 0x7030;
constexpr int GlowEdgeIds = 0x7040;
constexpr int GlowFillTex = 0x7121;
constexpr int GlowEdgeTex = 0x7120;
constexpr int IconIdBase = 0x7000;
constexpr int IconIdCount = 48;
// Slots 0..29 = five set rows of six items, 30..35 = the six slots of the centre pane. Test items (EventShop docs):
// 2621591 Split Sword, 2884093 Guild Stone. Row 4 stays empty to show the empty-slot case.
unsigned iconItem[IconIdCount] = {
    2621591u, 2884093u, 2621591u, 2884093u, 2621591u, 2884093u,
    2884093u, 2621591u, 2884093u, 2621591u, 2884093u, 2621591u,
    2621591u, 2621591u, 2884093u, 2884093u, 2621591u, 2884093u,
    2884093u, 2884093u, 2621591u, 2621591u, 2884093u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u,
    2621591u, 2884093u, 2621591u, 2884093u, 2621591u, 2884093u};
using ImageResolveFn = void(__thiscall*)(void*, std::uint32_t*, int);
ImageResolveFn originalResolve = reinterpret_cast<ImageResolveFn>(0x005C9900);
constexpr unsigned char ResolvePrologue[] = {0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8B, 0xF1, 0x8D, 0x44, 0x24, 0x20};

bool LookupItemHandle(unsigned type, std::uint32_t out[2]) {
    __try {
        const auto manager = *reinterpret_cast<const UINT_PTR*>(0x00D3B07C);
        if (!manager) return false;
        auto node = *reinterpret_cast<const UINT_PTR*>(*reinterpret_cast<const UINT_PTR*>(manager + 0x18) + 4);
        const auto head = *reinterpret_cast<const UINT_PTR*>(manager + 0x18);
        for (unsigned depth = 0; node && node != head && depth != 64; ++depth) {
            if (*reinterpret_cast<const unsigned char*>(node + 0x19)) return false;
            const auto key = *reinterpret_cast<const unsigned*>(node + 0x0C);
            if (key != type) {
                node = *reinterpret_cast<const UINT_PTR*>(node + (type < key ? 0 : 8));
                continue;
            }
            out[0] = *reinterpret_cast<const std::uint32_t*>(node + 0x10);
            out[1] = *reinterpret_cast<const std::uint32_t*>(node + 0x14);
            return !(out[0] == 0xFFFFFFFFu && out[1] == 0xFFFFFFFFu);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void __fastcall DetourImageResolve(void* self, void* edx, std::uint32_t* out, int id) {
    if (id & InspectAlias) {
        const int mask = (id >> 16) & 15;
        if (mask & (1 << (inspectPage - 1))) DetourImageResolve(self, edx, out, id & 0xFFFF);
        else out[0] = out[1] = 0xFFFFFFFFu;
        return;
    }
    if (id & AliasFlag) {
        const int mask = (id >> 16) & 15;
        if (mask & ActiveBits()) DetourImageResolve(self, edx, out, id & 0xFFFF);
        else out[0] = out[1] = 0xFFFFFFFFu;
        return;
    }
    if (id >= IconIdBase && id < IconIdBase + IconIdCount) {
        std::uint32_t handle[2]{};
        const unsigned type = iconItem[id - IconIdBase];
        if (type & 0x80000000u) {   // a rune picture (.gtx registered in the texture table): resolve its image id like our own textures
            out[0] = out[1] = 0xFFFFFFFFu;
            originalResolve(self, out, static_cast<int>(type & 0xFFFFu));
            return;
        }
        if (type && LookupItemHandle(type, handle)) {
            out[0] = handle[0];
            out[1] = handle[1];
        } else {
            out[0] = out[1] = 0xFFFFFFFFu; // "no image": the native draw skips a null resource
        }
        return;
    }
    if ((id >= HoverFillIds && id < HoverFillIds + 0x20) || (id >= HoverEdgeIds && id < HoverEdgeIds + 0x20)) {
        const bool fill = id < HoverEdgeIds;
        const int key = id - (fill ? HoverFillIds : HoverEdgeIds);
        if (key < FirstItemBoxKey) {
            // The action buttons use the stock button pictures: focused while the mouse is over, pushed while it is down (the normal
            // picture is drawn by the layout underneath). 0x875/0x876 = etc_btn_1, 0x878/0x879 = etc_btn_2 (the wide buttons).
            if (fill && key == hoverKey) {
                const bool wide = key == 16 || key == 19;
                const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                originalResolve(self, out, wide ? (down ? 0x879 : 0x878) : (down ? 0x876 : 0x875));
            } else {
                out[0] = out[1] = 0xFFFFFFFFu;
            }
            return;
        }
        if (key == hoverKey) originalResolve(self, out, fill ? GlowFillTex : GlowEdgeTex);
        else out[0] = out[1] = 0xFFFFFFFFu;
        return;
    }
    if ((id >= GlowFillIds && id < GlowFillIds + 5) || (id >= GlowEdgeIds && id < GlowEdgeIds + 5)) {
        const bool fill = id < GlowEdgeIds;
        const int row = id - (fill ? GlowFillIds : GlowEdgeIds);
        if (row == selectedSet) originalResolve(self, out, fill ? GlowFillTex : GlowEdgeTex);
        else out[0] = out[1] = 0xFFFFFFFFu;
        return;
    }
    if (id >= 0x7100 && id < 0x7200) {
        // Diagnostic for our own textures: does the image table know the id, and what does the handle resolve to?
        static unsigned lastTexture[0x100];
        static unsigned char seen[0x100];
        static LONG logged = 0;
        out[0] = out[1] = 0xDEADBEEFu;
        originalResolve(self, out, id);
        {
            char text[220];
            void* resource = nullptr;
            unsigned texture = 0, w = 0, h = 0;
            __try {
                const auto manager = *reinterpret_cast<void**>(0x00D3D3C4);
                if (manager && !(out[0] == 0xDEADBEEFu && out[1] == 0xDEADBEEFu) &&
                    !(out[0] == 0xFFFFFFFFu && out[1] == 0xFFFFFFFFu))
                    resource = reinterpret_cast<void*(__thiscall*)(void*, const std::uint32_t*)>(0x00A08390)(manager, out);
                if (resource) {
                    texture = *reinterpret_cast<unsigned*>(static_cast<char*>(resource) + 0x70);
                    w = *reinterpret_cast<unsigned*>(static_cast<char*>(resource) + 0x68);
                    h = *reinterpret_cast<unsigned*>(static_cast<char*>(resource) + 0x6C);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            // Log the first request and every later change of the texture pointer (async loads finish later).
            if ((!seen[id - 0x7100] || texture != lastTexture[id - 0x7100]) && InterlockedIncrement(&logged) <= 120) {
                std::snprintf(text, sizeof text, "image id 0x%X: handle=%08X,%08X resource=%08X texture=%08X size=%ux%u%s", id,
                              out[0], out[1], static_cast<unsigned>(reinterpret_cast<UINT_PTR>(resource)), texture, w, h,
                              seen[id - 0x7100] ? " (changed)" : "");
                ProtoLog(text);
            }
            seen[id - 0x7100] = 1;
            lastTexture[id - 0x7100] = texture;
        }
        return;
    }
    originalResolve(self, out, id);
}

bool ResolveMatches() {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(0x005C9900), ResolvePrologue, sizeof ResolvePrologue) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Diagnostic: the popup class's button dispatcher (vtable +0xB4 = 0x007C5370, `bool __thiscall(key, kind, arg)`, ret 0xC)
// looks {key, kind} up in the map at window+0x168. Logging its calls shows what a click really produces.
using DispatchFn = unsigned char(__thiscall*)(void*, int, int, int);
DispatchFn originalDispatch = reinterpret_cast<DispatchFn>(0x007C5370);
constexpr unsigned char DispatchPrologue[] = {0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x0C, 0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x14};
volatile LONG dispatchLogged = 0;

unsigned char __fastcall DetourDispatch(void* self, void*, int key, int kind, int arg) {
    const unsigned char result = originalDispatch(self, key, kind, arg);
    if (InterlockedIncrement(&dispatchLogged) <= 60) {
        char text[160];
        std::snprintf(text, sizeof text, "dispatch window=%08X key=%d kind=%d arg=%08X -> handled=%u",
                      static_cast<unsigned>(reinterpret_cast<UINT_PTR>(self)), key, kind, static_cast<unsigned>(arg),
                      static_cast<unsigned>(result));
        ProtoLog(text);
    }
    return result;
}

bool DispatchMatches() {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(0x007C5370), DispatchPrologue, sizeof DispatchPrologue) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Runtime text -------------------------------------------------------------------------------------------------
// Text controls (type 10) are objects with vtable 0x00C2EB54; `SetText(const char*)` is 0x004F9610 (thiscall, ret 4: it
// stores the string at control+0x5C) and the control id sits at +0x08. The window keeps its controls in std::vector-like
// triples (first, last, end) among its members, so the controls are found by scanning those and matching the vtable.
// Ids mirror DYN in tools/make_collection_book_layout.py.
enum TextId {
    TitleText = 1000, PageText = 1001, SelName = 1002, SelInfo = 1003, EffectText = 1004, BtnText = 1030, ViewTitle = 1031,
    RowTitle0 = 1010,  // 1010..1014
    CollBtn = 1032,    // Collection tab: label of the centred Unlock button
    Counter0 = 1020,   // 1020..1025
    UpgradeLabel = 1040, RewardsLabel = 1041, ClaimLabel = 1042,
    CollRow0 = 1050,   // 1050..1054: Collection tab row names
    CollCounter = 1065, DecompMsg = 1070, DecompEmpty = 1071, DecompBtn = 1072, DecompBtn30 = 1073, PrevLabel = 1080, NextLabel = 1081,
    ScrollUpLabel = 1090, ScrollDownLabel = 1091,
    PopCount0 = 1100,  // 1100..1107: the counts under the eight reward slots of the results popup
    PopTitle = 1110, PopConfirm = 1111,
};
constexpr UINT_PTR TextVtable = 0x00C2EB54;
using SetTextFn = void(__thiscall*)(void*, const char*);
struct TextRef { int id; void* control; };
TextRef textControls[64];
int textCount = 0;

bool IsTextControl(UINT_PTR p) {
    __try { return p > 0x10000 && *reinterpret_cast<const UINT_PTR*>(p) == TextVtable; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void CollectTextControls(void* window) {
    textCount = 0;
    __try {
        const auto base = reinterpret_cast<UINT_PTR>(window);
        for (UINT_PTR off = 0x30; off < 0x1C0 && textCount < 64; off += 4) {
            const UINT_PTR first = *reinterpret_cast<const UINT_PTR*>(base + off);
            const UINT_PTR last = *reinterpret_cast<const UINT_PTR*>(base + off + 4);
            if (first < 0x10000 || last < first || ((last - first) & 3) || last - first > 4 * 512) continue;
            for (UINT_PTR e = first; e < last && textCount < 64; e += 4) {
                const UINT_PTR p = *reinterpret_cast<const UINT_PTR*>(e);
                if (!IsTextControl(p)) continue;
                const int id = *reinterpret_cast<const int*>(p + 8);
                bool known = false;
                for (int i = 0; i < textCount; ++i) known = known || textControls[i].id == id;
                if (!known) textControls[textCount++] = {id, reinterpret_cast<void*>(p)};
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool RawSetText(int id, const char* text) {
    for (int i = 0; i < textCount; ++i) {
        if (textControls[i].id != id) continue;
        __try {
            reinterpret_cast<SetTextFn>(0x004F9610)(textControls[i].control, text);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    return false;
}

// Text for the active tab: the layout has separate text controls per tab where the position differs, so a write is routed to
// the control of the active tab (-1 = this text does not exist on the tab).
int RouteText(int id) {
    const int m = static_cast<int>(activeMode);
    if (id >= RowTitle0 && id < RowTitle0 + 5) return m == 1 ? id : (m == 2 || m == 3) ? CollRow0 + (id - RowTitle0) : -1;
    if (id >= Counter0 && id < Counter0 + 6) return m <= 2 ? id : -1;   // Sets: the runes; Collection: the amounts of the items a rune needs
    if (id == BtnText) return m == 1 ? id : m == 2 ? static_cast<int>(CollBtn) : -1;
    if (id == EffectText || id == ViewTitle || id == PageText) return m <= 2 ? id : -1;
    return id;
}

bool SetControlText(int id, const char* text) {
    const int target = RouteText(id);
    return target >= 0 && RawSetText(target, text);
}

// Demo data used when no server is connected (F9 without a `.cbook` session, or the server module is off).
struct SetInfo { const char* name; const char* effect; unsigned items[6]; unsigned owned; };
SetInfo sets[5] = {
    {"Greenhouse Administration Set",
     "2 Set Effect\nDamage vs. Formless +1%\n\n3 Set Effect\nDamage vs. Formless +2%\n\n5 Set Effect\nATK +7%, MATK +7%",
     {2621591u, 2884093u, 2621591u, 2884093u, 2621591u, 2884093u}, 0x07},
    {"Malfunction Set", "2 Set Effect\nMax HP +2%\n\n3 Set Effect\nMax HP +4%\n\n5 Set Effect\nDEF +10%",
     {2884093u, 2621591u, 2884093u, 2621591u, 2884093u, 2621591u}, 0x00},
    {"Wing Set", "2 Set Effect\nSpeed +1\n\n3 Set Effect\nSpeed +2\n\n5 Set Effect\nEvasion +5%",
     {2621591u, 2621591u, 2884093u, 2884093u, 2621591u, 2884093u}, 0x3F},
    {"Toxic Symptom Set", "2 Set Effect\nPoison resist +5%\n\n3 Set Effect\nPoison resist +10%",
     {2884093u, 2884093u, 2621591u, 2621591u, 2884093u, 0u}, 0x15},
    {"Strongest Set", "Not available yet.", {0u, 0u, 0u, 0u, 0u, 0u}, 0x00},
};

// ---- Live data (server) ---------------------------------------------------------------------------------------------
// The window shows what CollectionBookClient (src/CollectionBook) received from the server once the player has a session
// (typed `.cbook`, which also teaches the Say header). Without a session or before the first reply it shows the demo data.
unsigned currentCategory = 1;       // tab: 1 Sets, 2 Collection, 3 Decompose (sets carry the same category number)
bool showRewards = false;           // the text area shows the rewards instead of the effect and refine text
std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) { if (i) out += '\n'; out += lines[i]; }
    return out;
}

int pageIdx = 0;                    // the section shown in the text area (the left/right buttons move it)
int lastTextSet = -1;
bool lastTextRewards = false;

struct Section { std::string title; std::vector<std::string> lines; };

// Splits server text into sections: a line like "[Bonus Reward]" starts a new one; leading and trailing blank lines are dropped.
void AddSections(std::vector<Section>& out, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            out.push_back({line.substr(1, line.size() - 2), {}});
            continue;
        }
        if (out.empty()) out.push_back({"Set Effect", {}});
        auto& body = out.back().lines;
        if (line.empty() && body.empty()) continue;
        body.push_back(line);
    }
    for (auto& section : out) while (!section.lines.empty() && section.lines.back().empty()) section.lines.pop_back();
}

// Shows one section in the text area with its title (and "page/pages") in the title line; the page resets when the set or the view changed.
void ShowSections(const std::vector<Section>& sections, int setId) {
    if (setId != lastTextSet || showRewards != lastTextRewards) { pageIdx = 0; lastTextSet = setId; lastTextRewards = showRewards; }
    const int total = static_cast<int>(sections.size());
    if (total == 0) { SetControlText(ViewTitle, showRewards ? "Rewards" : "Set Effect"); SetControlText(EffectText, ""); return; }
    if (pageIdx >= total) pageIdx = total - 1;
    if (pageIdx < 0) pageIdx = 0;
    const Section& section = sections[static_cast<std::size_t>(pageIdx)];
    std::string title = section.title;
    if (total > 1) title += "  (" + std::to_string(pageIdx + 1) + "/" + std::to_string(total) + ")";
    SetControlText(ViewTitle, title.c_str());
    SetControlText(EffectText, Join(section.lines).c_str());
}

int infoRequestedFor = 0;           // set id whose effect text was asked for (avoids repeated requests)
unsigned lastVersion = ~0u;

bool Live() {
    const auto& page = CollectionBookClient::Shared().CurrentPage();
    return Bridge::SessionMatches() && page.loaded && page.category == currentCategory;
}


void ApplyRows() {
    if (activeMode == 3) {  // Decompose: the decomposable items of the bag
        const auto& d = CollectionBookClient::Shared().Decomp();
        const bool shown = Bridge::SessionMatches() && d.loaded;
        for (int r = 0; r < 5; ++r) {
            const bool have = shown && static_cast<std::size_t>(r) < d.items.size();
            const std::string label = have ? "[" + std::to_string(d.items[static_cast<std::size_t>(r)].count) + "] " + d.items[static_cast<std::size_t>(r)].name : std::string();
            SetControlText(RowTitle0 + r, label.c_str());
            for (int c = 0; c < 6; ++c) iconItem[r * 6 + c] = (have && c == 0) ? d.items[static_cast<std::size_t>(r)].item : 0u;
        }
        char buf[48]{};
        if (shown) std::snprintf(buf, sizeof buf, "Page %u / %u", d.page + 1, d.pages);
        SetControlText(PageText, buf);
        return;
    }
    if (Bridge::SessionMatches() && !Live()) {  // the list of this tab has not arrived yet
        for (int r = 0; r < 5; ++r) {
            SetControlText(RowTitle0 + r, "");
            for (int c = 0; c < 6; ++c) iconItem[r * 6 + c] = 0u;
        }
        SetControlText(PageText, "");
        return;
    }
    if (Live()) {
        const auto& page = CollectionBookClient::Shared().CurrentPage();
        for (int r = 0; r < 5; ++r) {
            const bool have = static_cast<std::size_t>(r) < page.sets.size();
            SetControlText(RowTitle0 + r, have ? page.sets[r].name.c_str() : "");
            for (int c = 0; c < 6; ++c) iconItem[r * 6 + c] = have ? page.sets[r].items[c] : 0u;
        }
        char buf[48];
        std::snprintf(buf, sizeof buf, "Page %u / %u", page.page + 1, page.pages);
        SetControlText(PageText, buf);
        return;
    }
    for (int r = 0; r < 5; ++r) {
        SetControlText(RowTitle0 + r, sets[r].name);
        for (int c = 0; c < 6; ++c) iconItem[r * 6 + c] = sets[r].items[c];
    }
}

void ApplySelection(int row) {
    if (row < 0 || row > 4) return;
    if (activeMode == 3) {  // Decompose
        auto& client = CollectionBookClient::Shared();
        const auto& d = client.Decomp();
        const std::string status = client.Status();
        const bool shown = Bridge::SessionMatches() && d.loaded;
        for (int i = 0; i < 6; ++i) iconItem[30 + i] = 0;
        if (!shown || d.items.empty()) {
            selectedSet = 0;
            SetControlText(SelName, "Disassemble");
            SetControlText(SelInfo, !status.empty() ? status.c_str() : shown ? "Nothing in your bag can be disassembled" : "");
            RawSetText(DecompMsg, shown ? "There are no items to disassemble" : "");
            RawSetText(DecompEmpty, shown ? "E M P T Y" : "");
            return;
        }
        if (static_cast<std::size_t>(row) >= d.items.size()) row = static_cast<int>(d.items.size()) - 1;
        selectedSet = row;
        const auto& chosen = d.items[static_cast<std::size_t>(row)];
        iconItem[30] = chosen.item;
        char info[80];
        std::snprintf(info, sizeof info, "You have %u", chosen.count);
        SetControlText(SelName, chosen.name.c_str());
        SetControlText(SelInfo, status.empty() ? info : status.c_str());
        RawSetText(DecompMsg, "");
        RawSetText(DecompEmpty, "");
        return;
    }
    if (Bridge::SessionMatches() && !Live()) {  // waiting for the list of this tab
        for (int i = 0; i < 6; ++i) { iconItem[30 + i] = 0; SetControlText(Counter0 + i, ""); }
        SetControlText(SelName, "");
        SetControlText(SelInfo, CollectionBookClient::Shared().Status().c_str());
        SetControlText(EffectText, "");
        SetControlText(BtnText, "");
        return;
    }
    if (Live()) {
        auto& client = CollectionBookClient::Shared();
        const auto& page = client.CurrentPage();
        const std::string status = client.Status();
        if (page.sets.empty()) {
            selectedSet = 0;
            for (int i = 0; i < 6; ++i) { iconItem[30 + i] = 0; SetControlText(Counter0 + i, ""); }
            SetControlText(BtnText, "Register");
            SetControlText(SelName, "No sets in this category");
            SetControlText(SelInfo, status.c_str());
            SetControlText(EffectText, "");
            return;
        }
        if (static_cast<std::size_t>(row) >= page.sets.size()) row = static_cast<int>(page.sets.size()) - 1;
        selectedSet = row;
        const auto& set = page.sets[row];
        int owned = 0;
        const bool isRune = CollectionBookClient::IsRune(set);
        bool hasNeeds = false;
        for (int i = 0; i < 5; ++i) hasNeeds = hasNeeds || set.needItem[i];
        if (isRune && hasNeeds) {
            // Collection: one box per item the rune needs (the item's own picture) with the amount underneath; the sixth stays empty.
            owned = (set.owned & 1u) ? 1 : 0;
            for (int i = 0; i < 6; ++i) {
                const bool used = i < 5 && set.needItem[i];
                iconItem[30 + i] = used ? set.needItem[i] : 0u;
                SetControlText(Counter0 + i, used ? ("x" + std::to_string(set.needAmount[i])).c_str() : "");
            }
        } else {
            for (int i = 0; i < 6; ++i) {
                iconItem[30 + i] = set.items[i];
                const bool have = set.items[i] && (set.owned & (1u << i));
                owned += have ? 1 : 0;
                SetControlText(Counter0 + i, set.items[i] ? (have ? "1 / 1" : "0 / 1") : "");
            }
        }
        const bool active = CollectionBookClient::Active(set);
        char info[100];
        if (isRune) std::snprintf(info, sizeof info, "%s", owned ? "Rune unlocked" : "Rune locked");
        else std::snprintf(info, sizeof info, "Runes unlocked: %d of %u%s", owned, CollectionBookClient::Slots(set),
                           active ? " - Equipped" : CollectionBookClient::Owns(set) ? " - Activated" : "");
        if (!isRune && CollectionBookClient::Owns(set)) {
            const std::size_t used = std::strlen(info);
            std::snprintf(info + used, sizeof info - used, "  Lv +%u", CollectionBookClient::Level(set));
        }
        SetControlText(BtnText, isRune ? (owned ? "Unlocked" : "Unlock") : (active ? "Unequip" : CollectionBookClient::Owns(set) ? "Equip" : "Activate"));
        SetControlText(SelName, set.name.c_str());
        SetControlText(SelInfo, status.empty() ? info : status.c_str());
        // Effect/requirement text and, once loaded, the upgrade lines share the one text area under the slots.
        SetControlText(ViewTitle, showRewards ? "Rewards" : "Set Effect");
        if (showRewards) {
            if (set.rewardsLoaded) { std::vector<Section> sections; sections.push_back({"Rewards", set.rewards}); ShowSections(sections, set.id); }
            else SetControlText(EffectText, "Loading...");
        } else if (set.effectLoaded) {
            std::vector<Section> sections;
            AddSections(sections, set.effect);
            if (isRune) { if (sections.size() > 1) sections.resize(1); }   // a rune only shows its requirement
            else if (set.upgradeLoaded) AddSections(sections, set.upgrade);
            ShowSections(sections, set.id);
        } else SetControlText(EffectText, "Loading...");
        if ((!set.effectLoaded || !set.upgradeLoaded || !set.rewardsLoaded) && infoRequestedFor != set.id) { infoRequestedFor = set.id; client.RequestInfo(set.id); }
        return;
    }
    selectedSet = row;
    SetControlText(BtnText, "Register");
    const SetInfo& set = sets[row];
    int owned = 0;
    for (int i = 0; i < 6; ++i) {
        iconItem[30 + i] = set.items[i];
        const bool have = set.items[i] && (set.owned & (1u << i));
        owned += have ? 1 : 0;
        SetControlText(Counter0 + i, set.items[i] ? (have ? "1 / 1" : "0 / 1") : "");
    }
    char info[100];
    std::snprintf(info, sizeof info, "Collected %d of 6 items", owned);
    SetControlText(SelName, set.name);
    SetControlText(SelInfo, info);
    SetControlText(EffectText, set.effect);
}

// Switches the visible content to a tab: clears the texts of the other tabs, writes this tab's fixed labels and refills the
// rows and the selection. The images follow activeMode on their own (see DetourImageResolve).
void OpenResults(const std::vector<CollectionBookClient::Reward>& rewards) {
    for (int k = 0; k < 8; ++k) {
        const bool have = static_cast<std::size_t>(k) < rewards.size();
        iconItem[36 + k] = have ? rewards[static_cast<std::size_t>(k)].item : 0u;
        RawSetText(PopCount0 + k, have ? std::to_string(rewards[static_cast<std::size_t>(k)].amount).c_str() : "");
    }
    RawSetText(PopTitle, "Decomposition Results");
    RawSetText(PopConfirm, "Confirm");
    resultsOpen = 1;
}

void CloseResults() {
    resultsOpen = 0;
    for (int k = 0; k < 8; ++k) { iconItem[36 + k] = 0u; RawSetText(PopCount0 + k, ""); }
    RawSetText(PopTitle, "");
    RawSetText(PopConfirm, "");
}

void ApplyMode(unsigned mode) {
    currentCategory = mode;
    activeMode = static_cast<LONG>(mode);
    selectedSet = 0;
    infoRequestedFor = 0;
    showRewards = false;
    static const int tabSpecific[] = {
        1010, 1011, 1012, 1013, 1014, 1020, 1021, 1022, 1023, 1024, 1025, 1050, 1051, 1052, 1053, 1054, 1065,
        1001, 1004, 1030, 1031, 1032, 1040, 1041, 1042, 1070, 1071, 1072, 1073, 1080, 1081, 1090, 1091,
        1100, 1101, 1102, 1103, 1104, 1105, 1106, 1107, 1110, 1111};
    for (int id : tabSpecific) RawSetText(id, "");
    if (mode <= 3) {
        RawSetText(PrevLabel, "Prev");
        RawSetText(NextLabel, "Next");
    }
    if (mode == 1) {   // the section arrows exist on the Sets tab only
        RawSetText(ScrollUpLabel, "<");
        RawSetText(ScrollDownLabel, ">");
    }
    if (mode == 1) {
        RawSetText(UpgradeLabel, "Upgrade");
        RawSetText(RewardsLabel, "Rewards");
        RawSetText(ClaimLabel, "Claim");
    }
    if (mode == 3) {
        RawSetText(DecompMsg, "There are no items to disassemble");
        RawSetText(DecompEmpty, "E M P T Y");
        RawSetText(DecompBtn, "Disassemble");
        RawSetText(DecompBtn30, "Disassemble x30");
    }
    ApplyRows();
    ApplySelection(0);
}

void* prototypeWindow = nullptr;

// Dispatcher installed in our window's private vtable. Buttons are keyed by their position in the layout
// (tools/make_collection_book_layout.py, KEYS): 0 drag bar, 1 close X, 2..4 category tabs, 5..9 set rows, 10 prev,
// 11 next, 12 Register, 13 Upgrade, 14 Rewards view, 15 Claim. Close (1) keeps the stock handler.
unsigned char __fastcall DetourWindowDispatch(void* self, void*, int key, int kind, int arg) {
    if (self == prototypeWindow && kind == 0 && key >= 2 && key <= 21) {
        char text[100];
        std::snprintf(text, sizeof text, "click key=%d", key);
        ProtoLog(text);
        auto& client = CollectionBookClient::Shared();
        const bool online = Bridge::SessionMatches();
        const int mode = static_cast<int>(activeMode);
        // Which controls exist on which tab: rows, paging and Register on Sets and Collection; Upgrade/Rewards/Claim on Sets;
        // Disassemble on Decompose. The layout hides the others, so their clicks are ignored.
        if (resultsOpen) {  // the results popup is modal: only its Confirm button works
            if (key == 20) CloseResults();
            return 1;
        }
        // 12 Register / 13 Upgrade / 14 Rewards / 15 Claim / 17-18 section arrows: Sets only; 21 Unlock: Collection only.
        const bool allowed = key == 20 ? false : key <= 4 || (key <= 11 ? mode <= 3 : key == 21 ? mode == 2 : key <= 15 ? mode == 1 : (key == 16 || key == 19) ? mode == 3 : mode == 1);
        if (!allowed) return 1;
        if (key >= 2 && key <= 4) {
            ApplyMode(static_cast<unsigned>(key - 1));
            if (online && key <= 3) client.RequestList(currentCategory, 0);
            else if (online && key == 4) client.RequestDList(0);
        } else if (key == 16 || key == 19) {
            const auto& d = client.Decomp();
            if (!online) SetControlText(SelInfo, "Disassemble works only when connected to the server.");
            else if (!d.loaded || static_cast<std::size_t>(selectedSet) >= d.items.size()) client.Notify("Select an item first.");
            else client.RequestDisassemble(d.items[static_cast<std::size_t>(selectedSet)].item, key == 19 ? 2 : 1);
        } else if (key >= 5 && key <= 9) {
            // Selecting a row fills the centre pane (icons resolve at draw time, texts through SetText).
            ApplySelection(key - 5);
        } else if (key == 10 || key == 11) {
            if (mode == 3) {
                const auto& d = client.Decomp();
                if (online && d.loaded) {
                    const unsigned next = key == 10 ? (d.page > 0 ? d.page - 1 : 0) : std::min(d.page + 1, d.pages - 1);
                    if (next != d.page) { selectedSet = 0; client.RequestDList(next); }
                }
            } else if (online && Live()) {
                const auto& page = client.CurrentPage();
                const unsigned next = key == 10 ? (page.page > 0 ? page.page - 1 : 0) : std::min(page.page + 1, page.pages - 1);
                if (next != page.page) {
                    selectedSet = 0;
                    infoRequestedFor = 0;
                    client.RequestList(page.category, next);
                }
            }
        } else if (key == 17 || key == 18) {
            pageIdx += key == 17 ? -1 : 1;
            ApplySelection(selectedSet);
        } else if (key == 14) {
            showRewards = !showRewards;
            ApplySelection(selectedSet);
        } else if (key == 15) {
            if (online && Live()) {
                const auto& page = client.CurrentPage();
                if (static_cast<std::size_t>(selectedSet) < page.sets.size()) {
                    const auto& chosen = page.sets[selectedSet];
                    if (CollectionBookClient::IsRune(chosen)) client.Notify("Runes have no rewards.");
                    else if (!CollectionBookClient::Owns(chosen)) client.Notify("Activate this set first.");
                    else client.RequestClaim(chosen.id);
                }
            }
        } else if (key == 13) {
            if (online && Live()) {
                const auto& page = client.CurrentPage();
                if (static_cast<std::size_t>(selectedSet) < page.sets.size()) {
                    const auto& chosen = page.sets[selectedSet];
                    if (CollectionBookClient::IsRune(chosen)) client.Notify("Runes cannot be upgraded.");
                    else if (!CollectionBookClient::Owns(chosen)) client.Notify("Activate this set first.");
                    else client.RequestUpgrade(chosen.id);
                }
            }
        } else if (key == 12 || key == 21) {
            if (online && Live()) {
                const auto& page = client.CurrentPage();
                if (static_cast<std::size_t>(selectedSet) < page.sets.size()) {
                    const auto& chosen = page.sets[selectedSet];
                    if (CollectionBookClient::IsRune(chosen)) {
                        if (CollectionBookClient::Unlocked(chosen)) client.Notify("This rune is already unlocked.");
                        else client.RequestRegisterAll(chosen.id);
                    } else if (CollectionBookClient::Active(chosen)) {
                        client.RequestEquip(chosen.id, false);
                    } else if (CollectionBookClient::Owns(chosen)) {
                        client.RequestEquip(chosen.id, true);
                    } else if (!CollectionBookClient::Unlocked(chosen)) {
                        client.Notify("Unlock a rune of this set first (Collection tab).");
                    } else {
                        client.RequestActivate(chosen.id);
                    }
                }
            } else {
                RegisterHandler(self, nullptr, arg);
                char info[100];
                std::snprintf(info, sizeof info, "Demo mode: type .cbook first (Register clicked #%ld)",
                              static_cast<long>(registerClicks));
                SetControlText(SelInfo, info);
            }
        }
        return 1;
    }
    return originalDispatch(self, key, kind, arg);
}

// The inventory shows its item tooltip from its own per-frame draw method (virtual slot +0x48, 0x0053A620: it draws its children, then calls
// the tooltip function for the hovered slot, every frame, before the frame is rendered). Doing it after the frame was drawn (at Present) got
// the tooltip hidden again before it was ever visible for long, which is the blinking. The book window's private vtable therefore replaces
// its draw slot: original draw first, then the tooltip of the item box under the cursor (UpdateHover decides which item: tooltipItem).
volatile LONG tooltipItem = 0;
bool ShowItemTooltip(unsigned itemType, int& step, const unsigned char* raw = nullptr);
using WindowDrawFn = void(__thiscall*)(void*);
WindowDrawFn originalDraw = nullptr;
void __fastcall DetourWindowDraw(void* self, void*) {
    originalDraw(self);
    const LONG item = tooltipItem;
    if (self == prototypeWindow && item) {
        int step = 0;
        const bool ok = ShowItemTooltip(static_cast<unsigned>(item), step);
        static int logged = 0;
        if ((!ok || logged < 3) && logged < 12) {
            ++logged;
            char text[120];
            std::snprintf(text, sizeof text, "item tooltip (draw) for %ld: %s (step %d)", static_cast<long>(item), ok ? "ok" : "FAILED", step);
            ProtoLog(text);
        }
    }
}

using GameNewFn = void*(__cdecl*)(std::size_t);
using PopupCtorFn = void*(__thiscall*)(void*, int, const char*);
using PushBackFn = void(__thiscall*)(void*, void**);
using ShowWindowFn = void(__thiscall*)(void*, int, int);

// Returns 0 on success, otherwise a step number for the log.
int BuildPrototype(unsigned& windowOut) {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return 1;
        void* memory = reinterpret_cast<GameNewFn>(0x00B8BACF)(0x1BC);
        if (!memory) return 2;
        void* window = reinterpret_cast<PopupCtorFn>(0x00806790)(memory, static_cast<int>(PrototypeId),
                                                                prototypePath);
        if (!window) return 3;
        // Button handler map at window+0x168 (the stock constructor's `edi` when it calls 0x00805D80; +0x14C is a
        // different object): key {controlId, 0} -> {function, 0}; insert returns pair<iterator,bool>.
        // Own copy of the popup class's vtable with the button dispatcher (slot +0xB4, index 45) replaced. The dispatcher's
        // key is the button's position in the layout (0-based), event kind 0 is a completed click (1 = press, 6 = poll).
        static unsigned vtableStorage[49];
        const unsigned* stock = *static_cast<unsigned**>(window);
        vtableStorage[0] = stock[-1]; // RTTI complete-object locator sits just before the vtable
        std::memcpy(&vtableStorage[1], stock, 48 * sizeof(unsigned));
        vtableStorage[1 + 45] = reinterpret_cast<unsigned>(&DetourWindowDispatch);
        originalDraw = reinterpret_cast<WindowDrawFn>(stock[18]);   // draw slot (+0x48)
        vtableStorage[1 + 18] = reinterpret_cast<unsigned>(&DetourWindowDraw);
        *static_cast<unsigned**>(window) = &vtableStorage[1];
        prototypeWindow = window;
        CollectTextControls(window);
        ApplyMode(1);
        {
            char text[80];
            std::snprintf(text, sizeof text, "text controls found: %d", textCount);
            ProtoLog(text);
        }
        reinterpret_cast<PushBackFn>(0x00526B30)(manager, &window);
        windowOut = static_cast<unsigned>(reinterpret_cast<UINT_PTR>(window));
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 99; }
}

bool ShowPrototype(int show) {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return false;
        reinterpret_cast<ShowWindowFn>(0x0058C600)(manager, static_cast<int>(PrototypeId), show);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool LoaderMatches() {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(LoaderAddress), LoaderPrologue, sizeof LoaderPrologue) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

namespace {
using LookupWindowFn = void*(__thiscall*)(void*, int);

// True while the stock window manager still holds our window (it drops every window when the player goes back to the character
// list and logs in again, which leaves prototypeBuilt pointing at a window that no longer exists).
bool WindowAlive() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return false;
        return reinterpret_cast<LookupWindowFn>(0x00585580)(manager, static_cast<int>(PrototypeId)) == prototypeWindow &&
               prototypeWindow != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void OpenBook() {
    char text[120];
    if (prototypeBuilt && !WindowAlive()) {
        ProtoLog("book window is gone (new session): rebuilding");
        prototypeBuilt = false;
        prototypeWindow = nullptr;
    }
    if (!prototypeBuilt) {
        unsigned window = 0;
        const int step = BuildPrototype(window);
        std::snprintf(text, sizeof text, "build result=%d window=%08X", step, window);
        ProtoLog(text);
        prototypeBuilt = step == 0;
    }
    if (!prototypeBuilt) return;
    const bool shown = ShowPrototype(1);
    std::snprintf(text, sizeof text, "show(1) ok=%d online=%d", shown ? 1 : 0, Bridge::SessionMatches() ? 1 : 0);
    ProtoLog(text);
    ApplyMode(1);  // the book always opens on the Sets tab
    if (Bridge::SessionMatches()) {  // refresh from the server every time the book opens
        lastVersion = ~0u;
        CollectionBookClient::Shared().RequestList(1, 0);
    }
}

// Hover glow of the action buttons (layout: buttons list in make_collection_book_layout.py; rectangles are window-relative).
struct HoverRect { int key, x, y, w, h, mask; };
constexpr HoverRect HoverRects[] = {
    {10, 8, 62, 63, 24, 7}, {11, 215, 62, 63, 24, 7}, {12, 404, 484, 63, 24, 1}, {21, 442, 484, 63, 24, 2}, {13, 482, 484, 63, 24, 1},
    {14, 326, 484, 63, 24, 1}, {15, 560, 484, 63, 24, 1}, {16, 360, 440, 100, 24, 4}, {19, 470, 440, 120, 24, 4},
    {17, 296, 288, 24, 24, 1}, {18, 624, 288, 24, 24, 1}, {20, 298, 354, 63, 24, 8},
    // the six boxes of the Collection tab (key 22 + box): the items a rune needs glow and show the game's own item tooltip
    {22, 380, 130, 36, 36, 2}, {23, 444, 130, 36, 36, 2}, {24, 508, 130, 36, 36, 2},
    {25, 380, 226, 36, 36, 2}, {26, 444, 226, 36, 36, 2}, {27, 508, 226, 36, 36, 2},
};
constexpr int ItemBoxKey0 = 22;
bool itemGlow = true;   // novaclient.ini [collection_book] item_glow=0 turns the glow of the item boxes off (the tooltip stays)

using WindowShowFn = void(__thiscall*)(void*, int, int);
WindowShowFn originalWindowShow = reinterpret_cast<WindowShowFn>(0x0058C600);   // stock show/hide of a window: thiscall(window manager, id, show)
void ProtoLog(const char* text);

// Plays one of the game's own sounds by file name (the sound manager 0x00D3B088, play-by-name 0x009FAE10; the name travels as the
// client's std::string, built with its own assign 0x00402630 and freed with 0x00B8D5ED like the stock code does).
void PlayGameSound(const char* name) {
    alignas(8) unsigned char text[0x1C];
    std::memset(text, 0, sizeof text);
    *reinterpret_cast<unsigned*>(text + 0x18) = 0xF;
    __try {
        const auto sounds = *reinterpret_cast<void**>(0x00D3B088);
        if (!sounds) return;
        reinterpret_cast<void(__thiscall*)(void*, const char*, unsigned)>(0x00402630)(text, name, static_cast<unsigned>(std::strlen(name)));
        reinterpret_cast<int(__thiscall*)(void*, void*, int, int)>(0x009FAE10)(sounds, text, 0, 0);
        if (*reinterpret_cast<unsigned*>(text + 0x18) >= 0x10) reinterpret_cast<void(__cdecl*)(void*)>(0x00B8D5ED)(*reinterpret_cast<void**>(text + 4));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The game's item tooltip for an item type, the way the stock code shows one for an item it only knows by type (the shop list at
// 0x004C8086): a template record (type in the first dword) is turned into a temporary item object with 0x008A6060 and handed to
// 0x008BE040, which opens the tooltip window (id 0x57) at the cursor; the object is destroyed afterwards (0x008A5EE0).
// `step` reports how far it got (for the log): 1 no item manager, 2 item unknown, 3 object built, 4 tooltip called.
// The tooltip window keeps a pointer to the item object it was shown with (window+0x114) and reads it on later frames, so the object must
// outlive the call: it lives in this static buffer and is only rebuilt for the next tooltip (destroying it right away, as the shop list
// does for its one-off tooltips, left the window pointing at stack garbage and made it blink).
alignas(16) unsigned char heldObject[0x400];
bool heldBuilt = false;
bool ShowItemTooltip(unsigned itemType, int& step, const unsigned char* raw) {
    alignas(16) unsigned char record[0x44];
    unsigned char* const object = heldObject;
    step = 0;
    __try {
        const auto items = *reinterpret_cast<void**>(0x00D3B07C);
        step = 1;
        if (!items || !itemType) return false;
        void* definition = reinterpret_cast<void*(__thiscall*)(void*, unsigned)>(0x00B07F00)(*reinterpret_cast<void**>(static_cast<char*>(items) + 4), itemType);
        step = 2;
        if (!definition) return false;
        if (heldBuilt) { heldBuilt = false; reinterpret_cast<void(__thiscall*)(void*)>(0x008A5EE0)(object); }
        std::memset(record, 0, sizeof record);
        std::memset(object, 0, sizeof heldObject);
        *reinterpret_cast<unsigned*>(record) = itemType;
        if (raw) std::memcpy(record, raw, 61);   // the item's real native record (enhancements, options): the tooltip then shows its effects
        reinterpret_cast<void*(__thiscall*)(void*, const void*, void*)>(0x008A6060)(object, record, definition);
        heldBuilt = true;
        step = 3;
        reinterpret_cast<void(__thiscall*)(void*, void*, int, int, int, int, int, int)>(0x008BE040)(items, object, 0, 0, 0, 0, -1, 0);
        step = 5;   // the object stays alive for the tooltip window
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void HideItemTooltip() {
    __try {
        const auto context = *reinterpret_cast<char**>(0x00D44A2C);
        if (!context) return;
        void* windows = *reinterpret_cast<void**>(*reinterpret_cast<char**>(context + 0x1C) + 0x14);
        if (windows) originalWindowShow(windows, 0x57, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void UpdateHover() {
    LONG found = -1;
    __try {
        HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &pid);
        POINT pt{};
        const auto* win = reinterpret_cast<const int*>(prototypeWindow);
        if (pid == GetCurrentProcessId() && WindowAlive() && win[0x38 / 4] && GetCursorPos(&pt) &&
            ScreenToClient(foreground, &pt)) {
            const int mx = pt.x - win[0x3C / 4], my = pt.y - win[0x40 / 4];
            for (const HoverRect& r : HoverRects) {
                if (r.key >= ItemBoxKey0) {   // only boxes that hold an item
                    const unsigned item = iconItem[30 + r.key - ItemBoxKey0];
                    if (!item || (item & 0x80000000u)) continue;
                }
                if ((r.mask & ActiveBits()) && mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) found = r.key;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = -1; }
    // The tooltip belongs to the box the cursor is on. The hover test can drop out for a few frames while the tooltip window opens (the log
    // showed enter/leave pairs 47 ms apart, each hiding and rebuilding the tooltip), so leaving a box only counts after 150 ms.
    static LONG tooltipKey = -1;
    static ULONGLONG lostSince = 0;
    const ULONGLONG now = GetTickCount64();
    if (found >= ItemBoxKey0) {
        lostSince = 0;
        if (found != tooltipKey) {
            tooltipKey = found;
            // The book window's draw shows the tooltip of this item every frame (see DetourWindowDraw), like the inventory does.
            InterlockedExchange(&tooltipItem, static_cast<LONG>(iconItem[30 + found - ItemBoxKey0]));
        }
    } else if (tooltipKey >= ItemBoxKey0) {
        if (!lostSince) lostSince = now;
        if (now - lostSince > 150) {
            InterlockedExchange(&tooltipItem, 0);
            HideItemTooltip();
            tooltipKey = -1;
            lostSince = 0;
        }
    }
    hoverKey = (found < 0 && tooltipKey >= ItemBoxKey0) ? tooltipKey : found;
    if (!itemGlow && hoverKey >= ItemBoxKey0) hoverKey = -1;   // the glow stays during a short drop-out too
}

// Re-applies the window texts whenever the client received something new.
void Refresh() {
    if (!prototypeBuilt) return;
    auto& client = CollectionBookClient::Shared();
    std::vector<CollectionBookClient::Reward> rewards;
    if (client.TakeRewards(rewards)) OpenResults(rewards);
    if (const int refine = client.TakeRefineEvent()) PlayGameSound(refine == 1 ? "suces.wav" : "fail.wav");   // the game's own refine result sounds
    // The first `.cbook` of a session opens the book and then the bridge resets the client data (it just learned the session): show
    // the catalog again at once instead of leaving the window empty until it is reopened.
    if (client.Local() && activeMode <= 2 && !client.CurrentPage().loaded && Bridge::SessionMatches() && WindowAlive() &&
        reinterpret_cast<const int*>(prototypeWindow)[0x38 / 4]) {
        lastVersion = ~0u;
        client.RequestList(currentCategory, 0);
    }
    if (client.Version() == lastVersion) return;
    lastVersion = client.Version();
    if (activeMode == 3) {
        ApplyRows();
        ApplySelection(static_cast<int>(selectedSet));
        return;
    }
    if (!Live()) {
        if (Bridge::SessionMatches() && !client.Status().empty()) SetControlText(SelInfo, client.Status().c_str());
        return;
    }
    ApplyRows();
    ApplySelection(static_cast<int>(selectedSet));
}
}

// Item icon slots 36..47 belong to the inspect window (tools/make_inspect_layout.py in the server repo, image ids 0x7024..0x702F).
void SetIconItem(int slot, unsigned itemType) {
    if (slot >= 36 && slot < IconIdCount) iconItem[slot] = itemType;
}
bool InspectLayoutInstalled() { return inspectLayout; }
// The game's own item tooltip for an item type (the Collection Book's mechanism), for other windows. Call it every frame while hovering, from
// the window's draw (the tooltip window is shown at the cursor); HideTooltip() closes it.
bool ShowTooltipFor(unsigned itemType) { int step = 0; return ShowItemTooltip(itemType, step); }
// The same, built from the item's native 61-byte record (shared/InspectProtocol.h `rec`); the first dword is the item type.
bool ShowTooltipRecord(const unsigned char* record61) { int step = 0; unsigned type = 0; std::memcpy(&type, record61, 4); return ShowItemTooltip(type, step, record61); }
void HideTooltip() { HideItemTooltip(); }
void SetInspectPage(int page) { inspectPage = page == 2 ? 2 : 1; }

void Frame() {
    if (!prototypeEnabled) return;
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !wasDown) OpenBook();
    wasDown = down;
    if (Bridge::TakeBookRequest()) OpenBook();   // the player typed .cbook
    if (prototypeBuilt) UpdateHover();
    Refresh();
}

// The Collection Book window is part of the product, not a diagnostic: it is on by default when its layout file is installed
// (Bitmaps\Interface4\UI\collection_book.ui) and can be switched with novaclient.ini [collection_book] enabled=0|1. The tracing
// (file-load log, window dumps, registration watch, novaclient_proto.log) stays opt-in through the marker file uitrace.on; the old
// uitrace_proto.on marker still selects another layout for experiments.
bool BookEnabledByConfig() {
    const int setting = GetPrivateProfileIntW(L"collection_book", L"enabled", -1, L".\\novaclient.ini");
    if (setting == 0) return false;
    if (setting == 1) return true;
    return GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\collection_book.ui") != INVALID_FILE_ATTRIBUTES;
}

LONG Attach() {
    diagnostics = GetFileAttributesW(L"uitrace.on") != INVALID_FILE_ATTRIBUTES;
    const bool legacy = GetFileAttributesW(L"uitrace_proto.on") != INVALID_FILE_ATTRIBUTES;
    prototypeEnabled = legacy || BookEnabledByConfig();
    inspectLayout = GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\inspect_character.ui") != INVALID_FILE_ATTRIBUTES;
    if (!diagnostics && !prototypeEnabled && !inspectLayout) return NO_ERROR;
    if (prototypeEnabled) {
        if (legacy) ReadPrototypeName();
        else std::snprintf(prototypePath, sizeof prototypePath, "\\Bitmaps\\Interface4\\UI\\collection_book.ui");
        if (diagnostics) {
            CollectionBookClient::SetLogger([](const char* text) { ProtoLog(text); });
            char text[300];
            std::snprintf(text, sizeof text, "book armed; press F9 or type .cbook in game; layout=%s", prototypePath);
            ProtoLog(text);
        }
    }
    itemGlow = GetPrivateProfileIntW(L"collection_book", L"item_glow", 1, L".\\novaclient.ini") != 0;
    if (prototypeEnabled) {
        // The catalog (names, costs, texts) is read from the client data files; without them the server keeps sending everything.
        const char* narrow = ".\\data";
        std::string error;
        const bool localOk = CollectionBookClient::Shared().LoadLocalCatalog(narrow, error);
        if (diagnostics) {
            char text[400];
            std::snprintf(text, sizeof text, "collection files %s: %s", localOk ? "loaded from" : "not used", localOk ? narrow : error.c_str());
            CollectionBookClient::SetLogger([](const char* t) { ProtoLog(t); });
            ProtoLog(text);
        }
    }
    InitializeCriticalSection(&lock);
    mainThreadId = GetCurrentThreadId();
    LONG result = NO_ERROR;
    if (diagnostics) {
        logFile = CreateFileW(L"novaclient_uitrace.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile != INVALID_HANDLE_VALUE) {
            result = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateW), reinterpret_cast<PVOID>(HookCreateW));
            if (result == NO_ERROR)
                result = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateA), reinterpret_cast<PVOID>(HookCreateA));
            if (result == NO_ERROR && LoaderMatches())
                result = DetourAttach(&originalLoader, reinterpret_cast<PVOID>(LoaderStub));
        }
    }
    if (result == NO_ERROR && prototypeEnabled && DispatchMatches())
        result = DetourAttach(reinterpret_cast<PVOID*>(&originalDispatch), reinterpret_cast<PVOID>(DetourDispatch));
    if (result == NO_ERROR && (prototypeEnabled || inspectLayout) && ResolveMatches())
        result = DetourAttach(reinterpret_cast<PVOID*>(&originalResolve), reinterpret_cast<PVOID>(DetourImageResolve));
    if (result == NO_ERROR && diagnostics && logFile != INVALID_HANDLE_VALUE) {
        HANDLE thread = CreateThread(nullptr, 0, WindowDumpThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        thread = CreateThread(nullptr, 0, RegistrationWatchThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return result;
}
} // namespace UiTrace
