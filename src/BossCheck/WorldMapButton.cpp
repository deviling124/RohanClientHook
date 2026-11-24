#include "WorldMapButton.h"
#include "../Bridge.h"
#include "../CustomEvents/EventNotice.h"
#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace WorldMapButton {
namespace {
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0;
constexpr int WorldMapKey = 16;         // the new button's position among the layout's buttons (make_bosscheck_layout.py prints it)
constexpr unsigned DispatchSlot = 45;   // +0xB4, the button dispatcher of the window classes (see InspectWindow.cpp)
constexpr int MaxWindowId = 0x400;
using LookupWindowFn = void*(__thiscall*)(void*, int);
using DispatchFn = unsigned char(__thiscall*)(void*, int, int, int);

DispatchFn originalDispatch = nullptr;
bool patched = false;
ULONGLONG lastScan = 0;
int scans = 0, layoutState = -1;

bool LayoutInstalled() {
    if (layoutState < 0) layoutState = GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\boss_check.ui") != INVALID_FILE_ATTRIBUTES ? 1 : 0;
    return layoutState == 1;
}

// Diagnostic: novaclient_worldmap.log beside the game: the windows the scans saw and every button key the world map sent.
void Log(const char* text) {
    HANDLE file = CreateFileW(L"novaclient_worldmap.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[400];
    const int n = std::snprintf(line, sizeof line, "%s\r\n", text);
    DWORD written = 0;
    if (n > 0) WriteFile(file, line, static_cast<DWORD>(n < 399 ? n : 399), &written, nullptr);
    CloseHandle(file);
}

unsigned char __fastcall Dispatch(void* self, void*, int key, int kind, int arg) {
    static int logged = 0;
    if (logged < 60) {
        ++logged;
        char text[120];
        std::snprintf(text, sizeof text, "dispatch key=%d kind=%d arg=%d", key, kind, arg);
        Log(text);
    }
    if (key == WorldMapKey && kind == 0) {   // a completed click
        Bridge::OpenBossCheck();
        return 1;
    }
    return originalDispatch(self, key, kind, arg);
}

// The object's RTTI type name (vtable[-1] -> complete object locator -> type descriptor -> name) copied into `name`; false when unreadable.
bool RttiName(void* object, char (&name)[80], void*** vtableOut) {
    name[0] = 0;
    __try {
        auto** vtable = *reinterpret_cast<void***>(object);
        if (!vtable || reinterpret_cast<UINT_PTR>(vtable) < 0x10000) return false;
        const auto* locator = reinterpret_cast<const UINT_PTR*>(vtable[-1]);
        if (!locator) return false;
        const auto* descriptor = reinterpret_cast<const char*>(locator[3]);
        if (!descriptor) return false;
        std::strncpy(name, descriptor + 8, sizeof name - 1);
        name[sizeof name - 1] = 0;
        if (vtableOut) *vtableOut = vtable;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = 0; return false; }
}

bool ContainsNoCase(const char* text, const char* word) {
    const std::size_t n = std::strlen(word);
    for (; *text; ++text) {
        std::size_t i = 0;
        while (i < n && text[i] && std::tolower(static_cast<unsigned char>(text[i])) == std::tolower(static_cast<unsigned char>(word[i]))) ++i;
        if (i == n) return true;
    }
    return false;
}

bool Patch(void** vtable) {
    __try {
        void** slot = &vtable[DispatchSlot];
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        originalDispatch = reinterpret_cast<DispatchFn>(*slot);
        *slot = reinterpret_cast<void*>(&Dispatch);
        VirtualProtect(slot, sizeof(void*), old, &old);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Looks at every window id for a class named like the world map ("WorldMap" in its RTTI name, not a minimap or guild war map).
void* Find(void*** vtableOut, bool verbose) {
    void* found = nullptr;
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return nullptr;
        for (int id = 1; id < MaxWindowId; ++id) {
            void* window = reinterpret_cast<LookupWindowFn>(0x00585580)(manager, id);
            if (!window) continue;
            char name[80];
            RttiName(window, name, nullptr);
            if (verbose) { char text[200]; std::snprintf(text, sizeof text, "id=0x%X window=%p class=%s", id, window, name); Log(text); }
            if (!found && ContainsNoCase(name, "WorldMap")) { found = window; RttiName(window, name, vtableOut); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (verbose) Log("scan faulted"); }
    return found;
}
}  // namespace

void Frame() {
    if (patched || !LayoutInstalled()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - lastScan < 1000) return;   // the windows exist once the player is in the world; look once a second until the world map does
    lastScan = now;
    ++scans;
    void** vtable = nullptr;
    const bool verbose = scans == 8 || scans == 20;   // two full lists of the windows (a few seconds apart) for the log
    if (verbose) { char text[60]; std::snprintf(text, sizeof text, "scan %d", scans); Log(text); }
    if (!Find(&vtable, verbose)) return;
    patched = true;
    if (Patch(vtable)) {
        Log("world map dispatcher patched");
        EventNotice::ShowLocal("[Boss] World Map Boss Check button ready.");
    } else {
        Log("world map dispatcher patch FAILED");
    }
}
}  // namespace WorldMapButton
