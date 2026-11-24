#include "BossCheckWindow.h"
#include "BossCheckClient.h"
#include "../Bridge.h"
#include "../CustomEvents/EventNotice.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace BossCheckWindow {
namespace {
// Native window system, the same calls as the inspect window (InspectWindow.cpp); window ids 0x300 and 0x301 are taken.
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0;
constexpr unsigned WindowId = 0x302;
constexpr UINT_PTR TextVtable = 0x00C2EB54;
constexpr const char* LayoutPath = "\\Bitmaps\\Interface4\\UI\\boss_check.ui";
using GameNewFn = void*(__cdecl*)(std::size_t);
using PopupCtorFn = void*(__thiscall*)(void*, int, const char*);
using PushBackFn = void(__thiscall*)(void*, void**);
using ShowWindowFn = void(__thiscall*)(void*, int, int);
using LookupWindowFn = void*(__thiscall*)(void*, int);
using SetTextFn = void(__thiscall*)(void*, const char*);
using DispatchFn = unsigned char(__thiscall*)(void*, int, int, int);

// Dynamic text ids (DYN in tools/make_bosscheck_layout.py) and button keys (KEYS there).
constexpr int PageText = 1001, NameBase = 1010, AliveBase = 1020, DeadBase = 1030;
constexpr int KeyMoveFirst = 2, KeyFirst = 8, KeyPrev = 9, KeyNext = 10, KeyLast = 11;

struct TextRef { int id; void* control; };
TextRef texts[64];
int textCount = 0;
void* window = nullptr;
DispatchFn originalDispatch = nullptr;
int layoutState = -1;   // -1 not checked, 0 missing, 1 installed

bool LayoutInstalled() {
    if (layoutState < 0) layoutState = GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\boss_check.ui") != INVALID_FILE_ATTRIBUTES ? 1 : 0;
    return layoutState == 1;
}

bool IsTextControl(UINT_PTR p) {
    __try { return p > 0x10000 && *reinterpret_cast<const UINT_PTR*>(p) == TextVtable; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Controls sit in std::vector-like triples among the window's first members; the text ones are found by their vtable.
void CollectTextControls(void* object) {
    textCount = 0;
    __try {
        const auto base = reinterpret_cast<UINT_PTR>(object);
        for (UINT_PTR off = 0x30; off < 0x1C0 && textCount < 64; off += 4) {
            const UINT_PTR first = *reinterpret_cast<const UINT_PTR*>(base + off);
            const UINT_PTR last = *reinterpret_cast<const UINT_PTR*>(base + off + 4);
            if (first < 0x10000 || last < first || ((last - first) & 3) || last - first > 4 * 512) continue;
            for (UINT_PTR e = first; e < last && textCount < 64; e += 4) {
                const UINT_PTR p = *reinterpret_cast<const UINT_PTR*>(e);
                if (!IsTextControl(p)) continue;
                const int id = *reinterpret_cast<const int*>(p + 8);
                if (id < 1000) continue;
                bool known = false;
                for (int i = 0; i < textCount; ++i) known = known || texts[i].id == id;
                if (!known) texts[textCount++] = {id, reinterpret_cast<void*>(p)};
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetText(int id, const std::string& value) {
    for (int i = 0; i < textCount; ++i) {
        if (texts[i].id != id) continue;
        __try { reinterpret_cast<SetTextFn>(0x004F9610)(texts[i].control, value.c_str()); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
}

// Writes the current page into the six rows.
void Fill() {
    auto& client = BossCheckClient::Shared();
    for (int slot = 0; slot < BossCheckClient::RowsPerPage; ++slot) {
        const auto* row = client.RowAt(slot);
        SetText(NameBase + slot, row ? row->name : std::string());
        SetText(AliveBase + slot, row && row->alive ? "ALIVE" : "");
        SetText(DeadBase + slot, row && !row->alive ? "DEAD" : "");
    }
    SetText(PageText, "Page " + std::to_string(client.page() + 1) + " / " + std::to_string(client.Pages()));
}

// The window's own button dispatcher: Move and the pager; everything else (drag, close) stays with the stock handler.
unsigned char __fastcall DetourDispatch(void* self, void*, int key, int kind, int arg) {
    if (self == window && kind == 0 && key >= KeyMoveFirst && key <= KeyLast) {
        auto& client = BossCheckClient::Shared();
        if (key < KeyMoveFirst + BossCheckClient::RowsPerPage) {
            const int slot = key - KeyMoveFirst;
            if (client.RowAt(slot)) {
                if (client.Move(client.RowIndexAt(slot), GetTickCount64())) Bridge::LearnSayHeader();
                else EventNotice::ShowLocal((std::string("[Boss] ") + BossCheckClient::MoveText(client.rejected())).c_str());
            }
        } else {
            if (key == KeyFirst) client.First();
            else if (key == KeyPrev) client.Prev();
            else if (key == KeyNext) client.Next();
            else if (key == KeyLast) client.Last();
            Fill();
        }
        return 1;
    }
    return originalDispatch(self, key, kind, arg);
}

bool Alive() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        return manager && window && reinterpret_cast<LookupWindowFn>(0x00585580)(manager, static_cast<int>(WindowId)) == window;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// True when the window exists (built now or earlier). The stock manager drops every window when the player returns to the character
// list, so a stale pointer is detected and the window is rebuilt.
bool Ensure() {
    if (window && !Alive()) { window = nullptr; textCount = 0; }
    if (window) return true;
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return false;
        void* memory = reinterpret_cast<GameNewFn>(0x00B8BACF)(0x1BC);
        if (!memory) return false;
        void* created = reinterpret_cast<PopupCtorFn>(0x00806790)(memory, static_cast<int>(WindowId), LayoutPath);
        if (!created) return false;
        // Own copy of the popup class's vtable with the button dispatcher (slot +0xB4, index 45) replaced, as the inspect window does.
        static unsigned vtableStorage[49];
        const unsigned* stock = *static_cast<unsigned**>(created);
        vtableStorage[0] = stock[-1];   // the RTTI locator sits just before the vtable
        std::memcpy(&vtableStorage[1], stock, 48 * sizeof(unsigned));
        originalDispatch = reinterpret_cast<DispatchFn>(stock[45]);
        vtableStorage[1 + 45] = reinterpret_cast<unsigned>(&DetourDispatch);
        *static_cast<unsigned**>(created) = &vtableStorage[1];
        CollectTextControls(created);
        reinterpret_cast<PushBackFn>(0x00526B30)(manager, &created);
        window = created;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Show() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return false;
        reinterpret_cast<ShowWindowFn>(0x0058C600)(manager, static_cast<int>(WindowId), 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Notice(const char* text) { EventNotice::ShowLocal((std::string("[Boss] ") + text).c_str()); }
}  // namespace

void Frame() {
    if (!LayoutInstalled()) return;
    auto& client = BossCheckClient::Shared();
    if (Bridge::TakeBossRequest()) {
        if (!Ensure() || !textCount) {
            Notice("The Boss Check window could not be opened.");
            return;
        }
        Fill();
        Show();
        if (!client.Refresh(GetTickCount64())) Notice(BossCheckClient::ListErrorText(client.rejected()));
    }
    if (client.TakeUpdate()) {
        if (client.error()) Notice(BossCheckClient::ListErrorText(client.error()));
        else if (window && Alive()) Fill();
    }
    if (client.TakeMoveUpdate()) {
        const int code = client.error() ? client.error() : client.moved();
        Notice(BossCheckClient::MoveText(code));
        // The boss may have died since the list was read: ask again after a move that did not happen.
        if (code != 0) client.Refresh(GetTickCount64() + BossCheckClient::MinGapMs);
    }
}
}  // namespace BossCheckWindow
