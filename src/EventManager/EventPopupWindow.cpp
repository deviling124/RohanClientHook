#include "EventPopupWindow.h"
#include "EventInviteStore.h"
#include "../Bridge.h"
#include "../CustomEvents/EventNotice.h"
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace EventPopupWindow {
namespace {
// Native window system, the same calls as the Boss Check window; window ids 0x300..0x304 are taken.
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0;
constexpr unsigned WindowId = 0x305;
constexpr UINT_PTR TextVtable = 0x00C2EB54;
constexpr const char* LayoutPath = "\\Bitmaps\\Interface4\\UI\\event_popup.ui";
using GameNewFn = void*(__cdecl*)(std::size_t);
using PopupCtorFn = void*(__thiscall*)(void*, int, const char*);
using PushBackFn = void(__thiscall*)(void*, void**);
using OpenWindowFn = void(__thiscall*)(void*, int, int);
using CloseWindowFn = void(__thiscall*)(void*, int);
using LookupWindowFn = void*(__thiscall*)(void*, int);
using SetTextFn = void(__thiscall*)(void*, const char*);
using DispatchFn = unsigned char(__thiscall*)(void*, int, int, int);

// Dynamic text ids (DYN in tools/make_eventpopup_layout.py) and button keys (KEYS there).
constexpr int NameText = 1001, TimeText = 1002, LineText = 1003;
constexpr int KeyJoin = 1;
constexpr int TextCount = 3;
constexpr ULONGLONG RetryCreateMs = 2000;

struct TextRef { int id; void* control; };
TextRef texts[16];
int textCount = 0;
std::string written[TextCount];     // what each text shows now, so a control is only set when it changes
void* window = nullptr;
DispatchFn originalDispatch = nullptr;
std::uint32_t shownId = 0;          // the invite the window was last shown for
std::uint32_t failedId = 0;         // the player was already told this invite cannot be shown
ULONGLONG triedAt = 0;
int layoutState = -1;               // -1 not checked, 0 missing, 1 installed

bool LayoutInstalled() {
    if (layoutState < 0)
        layoutState = GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\event_popup.ui") != INVALID_FILE_ATTRIBUTES ? 1 : 0;
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
        for (UINT_PTR off = 0x30; off < 0x1C0 && textCount < 16; off += 4) {
            const UINT_PTR first = *reinterpret_cast<const UINT_PTR*>(base + off);
            const UINT_PTR last = *reinterpret_cast<const UINT_PTR*>(base + off + 4);
            if (first < 0x10000 || last < first || ((last - first) & 3) || last - first > 4 * 512) continue;
            for (UINT_PTR e = first; e < last && textCount < 16; e += 4) {
                const UINT_PTR p = *reinterpret_cast<const UINT_PTR*>(e);
                if (!IsTextControl(p)) continue;
                const int id = *reinterpret_cast<const int*>(p + 8);
                if (id < NameText || id > LineText) continue;
                bool known = false;
                for (int i = 0; i < textCount; ++i) known = known || texts[i].id == id;
                if (!known) texts[textCount++] = {id, reinterpret_cast<void*>(p)};
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Write(void* control, const char* value) {
    __try { reinterpret_cast<SetTextFn>(0x004F9610)(control, value); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void SetText(int id, const std::string& value) {
    if (written[id - NameText] == value) return;
    for (int i = 0; i < textCount; ++i) {
        if (texts[i].id != id) continue;
        Write(texts[i].control, value.c_str());
        written[id - NameText] = value;
        return;
    }
}

void Fill(const EventManager::InviteStore::View& view) {
    char time[48];
    std::snprintf(time, sizeof time, "Starts in %u:%02u", view.seconds / 60, view.seconds % 60);
    const char* line = "Do you want to join?";
    if (view.state == EventManagerWire::State::WrongPlace) line = "Go to the main world first.";
    else if (view.state == EventManagerWire::State::Failed) line = "Move failed. Try again.";
    SetText(NameText, view.name);
    SetText(TimeText, time);
    SetText(LineText, line);
}

// The window's own button dispatcher: Join; everything else (dragging) stays with the stock handler.
unsigned char __fastcall Dispatch(void* self, void*, int key, int kind, int arg) {
    if (self == window && key == KeyJoin && kind == 0) {
        const ULONGLONG now = GetTickCount64();
        const auto view = EventManager::Invites().Current(now);
        if (view.id && EventManager::Invites().Join(view.id, now))
            Bridge::LearnSayHeader(); // lets the command be sent before the player has chatted; Frame closes the popup
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

bool Create() {
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
        CollectTextControls(created);
        reinterpret_cast<PushBackFn>(0x00526B30)(manager, &created);
        window = created;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The stock manager drops every window when the player returns to the character list, so a stale pointer is rebuilt.
bool Ensure() {
    if (window && !Alive()) { window = nullptr; textCount = 0; }
    if (window) return true;
    for (auto& text : written) text.clear();
    return Create();
}

// 0x0058C600 only opens: for a window that is already open it moves it to the front, whatever the second argument is.
void Open() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (manager) reinterpret_cast<OpenWindowFn>(0x0058C600)(manager, static_cast<int>(WindowId), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The stock close: takes the window out of the open list. The window itself stays and can be opened again.
void Close() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (manager) reinterpret_cast<CloseWindowFn>(0x00585AD0)(manager, static_cast<int>(WindowId));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Looked up the way both calls above do it: the manager's list of open windows (head at +0x10; a node holds the next
// node at +0 and the window at +8; the window's id is at +0x34).
bool IsOpen() {
    __try {
        const auto* manager = *reinterpret_cast<const unsigned char* const*>(ManagerGlobal);
        if (!manager) return false;
        const auto* head = *reinterpret_cast<const unsigned char* const*>(manager + 0x10);
        const auto* node = *reinterpret_cast<const unsigned char* const*>(head);
        for (int i = 0; node != head && i < 1024; ++i) {
            const auto* open = *reinterpret_cast<const unsigned char* const*>(node + 8);
            if (*reinterpret_cast<const int*>(open + 0x34) == static_cast<int>(WindowId)) return true;
            node = *reinterpret_cast<const unsigned char* const*>(node);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Without the window the player still learns that the event is coming.
void Fallback(const EventManager::InviteStore::View& view, const char* why) {
    if (failedId == view.id) return;
    failedId = view.id;
    char text[160];
    std::snprintf(text, sizeof text, "[Event]: %s starts in %u seconds. (%s)", view.name.c_str(), view.seconds, why);
    EventNotice::ShowLocal(text);
}
}  // namespace

void Frame() {
    const ULONGLONG now = GetTickCount64();
    const auto view = EventManager::Invites().Current(now);
    if (!view.id) {
        if (shownId) {
            if (Alive()) Close(); // Join was clicked, or the event started or was cancelled
            shownId = 0;
        }
        return;
    }
    if (!LayoutInstalled()) { Fallback(view, "event_popup.ui is not installed"); return; }
    if (!window || !Alive()) {
        if (triedAt && now - triedAt < RetryCreateMs) return;
        triedAt = now;
        if (!Ensure() || textCount != TextCount) { Fallback(view, "the event window could not be opened"); return; }
        triedAt = 0;
        shownId = 0;
    }
    if (textCount != TextCount) return;
    Fill(view);
    // It cannot be dismissed: if the game closed it (Escape, a teleport), it is back on the next frame.
    if (!IsOpen()) Open();
    shownId = view.id;
}
}  // namespace EventPopupWindow
