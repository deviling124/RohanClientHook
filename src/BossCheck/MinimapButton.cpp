#include "MinimapButton.h"
#include "../Bridge.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace MinimapButton {
namespace {
// Native window system, the same calls as the Boss Check and inspect windows; window ids 0x300..0x302 are taken.
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0;
constexpr unsigned WindowId = 0x303;
constexpr const char* LayoutPath = "\\Bitmaps\\Interface4\\UI\\boss_button.ui";
constexpr int MaxWindowId = 0x400;
// Where the button sits, relative to the minimap window: at the 9 o'clock position of the round map. The circle is centred about 103 pixels
// below the window top and its west compass marker is about 13 pixels inside the window's left edge, so the 32x32 button is centred
// vertically on the circle (103 - 16 = 87) and ends 8 pixels inside the window edge (x = window x - 24), clear of the marker.
// (The minimap draws over anything placed further inside it.)
constexpr int LeftOfMinimap = 24, BelowTop = 87;
using GameNewFn = void*(__cdecl*)(std::size_t);
using PopupCtorFn = void*(__thiscall*)(void*, int, const char*);
using PushBackFn = void(__thiscall*)(void*, void**);
using ShowWindowFn = void(__thiscall*)(void*, int, int);
using LookupWindowFn = void*(__thiscall*)(void*, int);
using DispatchFn = unsigned char(__thiscall*)(void*, int, int, int);

void* window = nullptr;
DispatchFn originalDispatch = nullptr;
ULONGLONG lastScan = 0;
int layoutState = -1;

bool LayoutInstalled() {
    if (layoutState < 0)
        layoutState = GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\boss_button.ui") != INVALID_FILE_ATTRIBUTES &&
                      GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\boss_check.ui") != INVALID_FILE_ATTRIBUTES ? 1 : 0;
    return layoutState == 1;
}

// The window's own button dispatcher: the only button opens Boss Check; everything else stays with the stock handler.
unsigned char __fastcall Dispatch(void* self, void*, int key, int kind, int arg) {
    if (self == window && key == 0 && kind == 0) {   // a completed click
        Bridge::OpenBossCheck();
        return 1;
    }
    return originalDispatch(self, key, kind, arg);
}

// True when the object's RTTI type name is CUIMiniMap (vtable[-1] -> complete object locator -> type descriptor -> name).
bool IsMinimap(void* object) {
    __try {
        auto** vtable = *reinterpret_cast<void***>(object);
        if (!vtable || reinterpret_cast<UINT_PTR>(vtable) < 0x10000) return false;
        const auto* locator = reinterpret_cast<const UINT_PTR*>(vtable[-1]);
        if (!locator) return false;
        const auto* descriptor = reinterpret_cast<const char*>(locator[3]);
        return descriptor && std::strcmp(descriptor + 8, ".?AVCUIMiniMap@@") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The minimap window, or null while it does not exist.
void* FindMinimap() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return nullptr;
        for (int id = 1; id < MaxWindowId; ++id) {
            void* other = reinterpret_cast<LookupWindowFn>(0x00585580)(manager, id);
            if (other && IsMinimap(other)) return other;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

bool Alive() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        return manager && window && reinterpret_cast<LookupWindowFn>(0x00585580)(manager, static_cast<int>(WindowId)) == window;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// True when the window exists (built now or earlier). The stock manager drops every window when the player returns to the character list.
bool Ensure() {
    if (window && !Alive()) window = nullptr;
    if (window) return true;
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return false;
        void* memory = reinterpret_cast<GameNewFn>(0x00B8BACF)(0x1BC);
        if (!memory) return false;
        void* created = reinterpret_cast<PopupCtorFn>(0x00806790)(memory, static_cast<int>(WindowId), LayoutPath);
        if (!created) return false;
        // Own copy of the popup class's vtable with the button dispatcher (slot +0xB4, index 45) replaced, as the other windows do.
        static unsigned vtableStorage[49];
        const unsigned* stock = *static_cast<unsigned**>(created);
        vtableStorage[0] = stock[-1];   // the RTTI locator sits just before the vtable
        std::memcpy(&vtableStorage[1], stock, 48 * sizeof(unsigned));
        originalDispatch = reinterpret_cast<DispatchFn>(stock[45]);
        vtableStorage[1 + 45] = reinterpret_cast<unsigned>(&Dispatch);
        *static_cast<unsigned**>(created) = &vtableStorage[1];
        reinterpret_cast<PushBackFn>(0x00526B30)(manager, &created);
        window = created;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Shows the window if it is not shown and puts it next to the minimap. The position comes from the minimap window's own rectangle
// (x, y, width, height at +0x3C..+0x48, in the game's own coordinates), so it follows the resolution and wherever the minimap is.
void ShowAtMinimap(void* minimap) {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager || !window || !minimap) return;
        const auto* map = reinterpret_cast<const int*>(minimap);
        const int mapX = map[0x3C / 4], mapY = map[0x40 / 4], mapW = map[0x44 / 4];
        if (mapW < 100 || mapW > 400 || mapX < 0 || mapY < -50) return;   // not laid out yet
        auto* words = reinterpret_cast<int*>(window);
        // The visible flag is the byte at +0x38 (the word around it holds other data); show the window while it is 0.
        if (!*reinterpret_cast<const unsigned char*>(reinterpret_cast<const unsigned char*>(window) + 0x38))
            reinterpret_cast<ShowWindowFn>(0x0058C600)(manager, static_cast<int>(WindowId), 1);
        words[0x3C / 4] = mapX - LeftOfMinimap;
        words[0x40 / 4] = mapY + BelowTop;
        // The controls cache their absolute positions: 0x0056B240 (thiscall, no arguments) recomputes them from the window origin.
        reinterpret_cast<void(__thiscall*)(void*)>(0x0056B240)(window);
        static int logged = 0;
        if (logged < 5) {   // diagnostic: novaclient_minimap.log
            ++logged;
            char text[160];
            std::snprintf(text, sizeof text, "window %p visible byte=%u at %d,%d, minimap rect %d,%d,%d", window, static_cast<unsigned>(*(reinterpret_cast<const unsigned char*>(window) + 0x38)), words[0x3C / 4], words[0x40 / 4], mapX, mapY, mapW);
            HANDLE file = CreateFileW(L"novaclient_minimap.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                char line[200];
                const int n = std::snprintf(line, sizeof line, "%s\r\n", text);
                DWORD written = 0;
                if (n > 0) WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
                CloseHandle(file);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
}  // namespace

void Frame() {
    if (!LayoutInstalled()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - lastScan < 1000) return;   // once a second: the minimap exists once the player is in the world
    lastScan = now;
    void* minimap = FindMinimap();
    if (minimap && Ensure()) ShowAtMinimap(minimap);
}
}  // namespace MinimapButton
