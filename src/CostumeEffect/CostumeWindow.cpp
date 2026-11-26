#include "CostumeWindow.h"
#include "CostumeClient.h"
#include "CostumePreview.h"
#include "../CustomEvents/EventNotice.h"
#include "../UiTrace.h"
#include <windows.h>
#include <cstddef>
#include <cstring>
#include <string>

namespace CostumeWindow {
namespace {
// Native window system, the same calls as the Boss Check window; window ids 0x300..0x303 are taken.
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0;
constexpr unsigned WindowId = 0x304;
constexpr UINT_PTR TextVtable = 0x00C2EB54;
constexpr const char* LayoutPath = "\\Bitmaps\\Interface4\\UI\\costume_effect.ui";
using GameNewFn = void*(__cdecl*)(std::size_t);
using PopupCtorFn = void*(__thiscall*)(void*, int, const char*);
using PushBackFn = void(__thiscall*)(void*, void**);
using ShowWindowFn = void(__thiscall*)(void*, int, int);
using LookupWindowFn = void*(__thiscall*)(void*, int);
using SetTextFn = void(__thiscall*)(void*, const char*);
using DispatchFn = unsigned char(__thiscall*)(void*, int, int, int);

// Dynamic text ids (DYN in tools/make_costume_effect_layout.py) and button keys (KEYS there).
constexpr int TicketsText = 1001, CostumeText = 1002, EffectText = 1003, Line1Text = 1004, Line2Text = 1005;
constexpr int KeyCloseX = 1, KeyApply = 2, KeyClose = 3, KeyPrev = 4, KeyNext = 5, KeyBox = 6;
constexpr int TextCount = 5;

struct TextRef { int id; void* control; };
TextRef texts[16];
int textCount = 0;
std::string written[TextCount];     // what each text shows now, so a control is only set when it changes
unsigned iconShown = 0;
void* window = nullptr;
DispatchFn originalDispatch = nullptr;
// Dropping a bag item on the slot, the way the item recycle window takes one: when the mouse is let go over the window, ask
// the client's cursor object what it is holding.
constexpr UINT_PTR CursorGlobal = 0x00D3D9D0;
constexpr unsigned StockRelease = 0x004D4810;             // the popup class's mouse release method (vtable slot 3)
constexpr int BoxX = 123, BoxY = 62, BoxSize = 78, BoxMargin = 6;   // the item box in make_costume_effect_layout.py
using ReleaseFn = unsigned char(__fastcall*)(void*, void*);
using HeldItemFn = const unsigned char*(__thiscall*)(void*);   // the bag item on the cursor, else null
using EndDragFn = void(__thiscall*)(void*);
ReleaseFn originalRelease = nullptr;
bool open = false;                  // the window was asked to show and has not been closed
bool seenVisible = false;
ULONGLONG openedAt = 0;
int layoutState = -1;

bool LayoutInstalled() {
    if (layoutState < 0)
        layoutState = GetFileAttributesW(L"Bitmaps\\Interface4\\UI\\costume_effect.ui") != INVALID_FILE_ATTRIBUTES ? 1 : 0;
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
                if (id < TicketsText || id > Line2Text) continue;
                bool known = false;
                for (int i = 0; i < textCount; ++i) known = known || texts[i].id == id;
                if (!known) texts[textCount++] = {id, reinterpret_cast<void*>(p)};
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetControlText(void* control, const char* value) {
    __try { reinterpret_cast<SetTextFn>(0x004F9610)(control, value); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetText(int id, const std::string& value) {
    std::string& old = written[id - TicketsText];
    const std::string next = value.empty() ? std::string(" ") : value;
    if (old == next) return;
    for (int i = 0; i < textCount; ++i) {
        if (texts[i].id != id) continue;
        old = next;
        SetControlText(texts[i].control, next.c_str());
        return;
    }
}

// A message is shown on the two bottom lines, split at a space when it is long.
void SetMessage(const std::string& message) {
    constexpr std::size_t Width = 46;
    std::string first = message, second;
    if (first.size() > Width) {
        std::size_t cut = first.rfind(' ', Width);
        if (cut == std::string::npos) cut = Width;
        second = first.substr(cut + (first[cut] == ' ' ? 1 : 0));
        first.resize(cut);
    }
    SetText(Line1Text, first);
    SetText(Line2Text, second);
}

void Fill() {
    const auto view = CostumeClient::Shared().Snapshot();
    unsigned icon = 0;
    if (!view.loaded) {
        SetText(TicketsText, "Costume Effect Tickets: -");
        SetText(CostumeText, view.loading ? "Loading..." : "Not available");
        if (view.placed.itemId) icon = view.placed.type;
        SetText(EffectText, "No change");
        SetMessage(view.loading ? std::string() : view.status);
    } else {
        const auto& state = view.state;
        SetText(TicketsText, "Costume Effect Tickets: " + std::to_string(state.tickets));
        if (view.placed.itemId) {
            icon = view.placed.type;
            std::string has = "plain";
            for (const auto& recipe : state.recipes)
                if (recipe.result == view.placed.type) has = recipe.name;
            SetText(CostumeText, view.copy ? "Costume: " + has + " (click box to remove)"
                                           : std::string("Click the box to remove it"));
        } else {
            SetText(CostumeText, "Drag a costume from your bag here");
        }
        std::string effect = "No change";
        for (const auto& recipe : state.recipes)
            if (recipe.result == view.effect) effect = recipe.name;
        if (view.effect) icon = view.effect;
        SetText(EffectText, effect);
        if (view.confirming) SetMessage("Your old costume will be gone. Press Apply again.");
        else if (!view.status.empty()) SetMessage(view.status);
        else if (!view.copy) SetMessage(std::string());
        else if (!view.effect) SetMessage("Choose an effect with the arrows.");
        else if (!state.tickets) SetMessage("You need a Costume Effect Ticket to apply.");
        else SetMessage("The preview is only visible to you.");
    }
    if (icon != iconShown) {
        iconShown = icon;
        UiTrace::SetCostumeIcon(icon);
    }
}

void Fill();

// The tooltip of the item in the slot. As in the inventory, the window's own draw shows it again every frame the mouse is there.
constexpr int SlotX = 145, SlotY = 83, SlotSize = 32, SlotMargin = 4;   // the inner slot in make_costume_effect_layout.py
using DrawFn = void(__fastcall*)(void*, void*);
DrawFn originalDraw = nullptr;
bool tooltipShown = false;
ULONGLONG tooltipLostSince = 0;

bool MouseOnSlot(const void* self) {
    __try {
        const auto mouse = *reinterpret_cast<const unsigned char**>(0x00D44A44);
        if (!mouse) return false;
        const auto bytes = static_cast<const unsigned char*>(self);
        const int x = *reinterpret_cast<const int*>(mouse + 0x14) - *reinterpret_cast<const int*>(bytes + 0x3C) - SlotX;
        const int y = *reinterpret_cast<const int*>(mouse + 0x18) - *reinterpret_cast<const int*>(bytes + 0x40) - SlotY;
        return x >= -SlotMargin && y >= -SlotMargin && x < SlotSize + SlotMargin && y < SlotSize + SlotMargin;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void HideSlotTooltip() {
    tooltipLostSince = 0;
    if (!tooltipShown) return;
    tooltipShown = false;
    UiTrace::HideTooltip();
}

// The bag item in the slot, when its tooltip may be shown. Not while combining: the server is about to take the item.
void* TooltipItem() {
    const auto view = CostumeClient::Shared().Snapshot();
    if (view.combining || !view.placed.itemId) return nullptr;
    return CostumePreview::ItemObject({view.placed.itemId, view.placed.type, view.placed.bag, view.placed.slot});
}

void __fastcall Draw(void* self, void* edx) {
    originalDraw(self, edx);
    if (self != window) return;
    void* item = TooltipItem();
    if (!item) {                       // emptied, used up or moved: the tooltip must not keep pointing at it
        HideSlotTooltip();
        return;
    }
    if (MouseOnSlot(self)) {
        tooltipLostSince = 0;
        tooltipShown = UiTrace::ShowTooltipObject(item) || tooltipShown;
    } else if (tooltipShown) {         // the test can drop out for a few frames while the tooltip window opens
        const ULONGLONG now = GetTickCount64();
        if (!tooltipLostSince) tooltipLostSince = now;
        if (now - tooltipLostSince > 150) HideSlotTooltip();
    }
}

bool DragSupported() {
    __try {
        constexpr unsigned char held[] = {0x83,0xB9,0x9C,0x00,0x00,0x00,0x01,0x74,0x03,0x33,0xC0,0xC3,0x8B,0x81,0xA0};
        constexpr unsigned char endDrag[] = {0x56,0x8B,0xF1,0x8B,0x86,0x9C,0x00,0x00,0x00,0x57,0x33,0xFF};
        return !std::memcmp(reinterpret_cast<const void*>(0x004BA3A0), held, sizeof held) &&
               !std::memcmp(reinterpret_cast<const void*>(0x004BA870), endDrag, sizeof endDrag);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The costume being dragged, when the mouse is over the slot. Only numbers are read from the client's item.
bool DroppedOnSlot(void* self, CostumeClient::Item& out) {
    __try {
        void* cursor = *reinterpret_cast<void**>(CursorGlobal);
        const auto mouse = *reinterpret_cast<const unsigned char**>(0x00D44A44);
        if (!cursor || !mouse) return false;
        const unsigned char* item = reinterpret_cast<HeldItemFn>(0x004BA3A0)(cursor);
        if (!item || item[0xB6]) return false;               // already in use, by this or another window
        const auto bytes = static_cast<const unsigned char*>(self);
        const int x = *reinterpret_cast<const int*>(mouse + 0x14) - *reinterpret_cast<const int*>(bytes + 0x3C) - BoxX;
        const int y = *reinterpret_cast<const int*>(mouse + 0x18) - *reinterpret_cast<const int*>(bytes + 0x40) - BoxY;
        if (x < -BoxMargin || y < -BoxMargin || x >= BoxSize + BoxMargin || y >= BoxSize + BoxMargin) return false;
        out.type = *reinterpret_cast<const unsigned*>(item + 8);
        out.itemId = *reinterpret_cast<const int*>(item + 0xC);
        out.bag = item[0xAC];
        out.slot = item[0xB0];
        reinterpret_cast<EndDragFn>(0x004BA870)(cursor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

unsigned char __fastcall Release(void* self, void* edx) {
    CostumeClient::Item item;
    if (self == window && DroppedOnSlot(self, item)) {
        // Bag 0 is what the character wears: it has to be unequipped first.
        if (item.bag == 0) EventNotice::ShowLocal("[Costume] Unequip the costume first.");
        else if (item.type >> 16 != 0x43) EventNotice::ShowLocal("[Costume] Only costumes go in that slot.");
        else CostumeClient::Shared().Drop(item, GetTickCount64());
        Fill();
        return 1;
    }
    return originalRelease(self, edx);
}

// The window's own button dispatcher; the drag bar stays with the stock handler.
unsigned char __fastcall Dispatch(void* self, void*, int key, int kind, int arg) {
    if (self == window && key >= KeyCloseX && key <= KeyBox) {
        auto& client = CostumeClient::Shared();
        if (key == KeyCloseX || key == KeyClose) {
            if (kind == 0) { client.Close(); open = false; }
            return originalDispatch(self, KeyCloseX, kind, arg);   // the stock close button's own handling hides the window
        }
        if (kind != 0) return originalDispatch(self, key, kind, arg);
        if (key == KeyApply) client.Combine(GetTickCount64());
        else if (key == KeyPrev) client.StepEffect(-1);
        else if (key == KeyNext) client.StepEffect(1);
        else { HideSlotTooltip(); client.Remove(); }
        Fill();
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
        originalDraw = reinterpret_cast<DrawFn>(stock[18]);       // the draw method (vtable slot 18, +0x48)
        vtableStorage[1 + 18] = reinterpret_cast<unsigned>(&Draw);
        if (stock[3] == StockRelease && DragSupported()) {
            originalRelease = reinterpret_cast<ReleaseFn>(stock[3]);
            vtableStorage[1 + 3] = reinterpret_cast<unsigned>(&Release);
        }
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

bool Show() {
    __try {
        const auto manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return false;
        reinterpret_cast<ShowWindowFn>(0x0058C600)(manager, static_cast<int>(WindowId), 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The visible flag is the byte at +0x38.
bool Visible() {
    __try { return window && *(static_cast<const unsigned char*>(window) + 0x38) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}  // namespace

void Frame() {
    auto& client = CostumeClient::Shared();
    if (!client.Visible()) { HideSlotTooltip(); open = false; return; }
    if (!LayoutInstalled()) {
        client.Close();
        EventNotice::ShowLocal("[Costume] The window file costume_effect.ui is not installed.");
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!open) {
        if (!Ensure() || textCount != TextCount) {
            client.Close();
            EventNotice::ShowLocal("[Costume] The Costume Effect window could not be opened.");
            return;
        }
        Fill();
        Show();
        open = true;
        seenVisible = false;
        openedAt = now;
        return;
    }
    // Closed by the game (Escape, a map change, the character list): end the session, which also ends the preview.
    const bool visible = Alive() && Visible();
    seenVisible = seenVisible || visible;
    if (!visible && (seenVisible || now - openedAt > 2000)) {
        client.Close();
        HideSlotTooltip();
        open = false;
        return;
    }
    if (!visible || !TooltipItem()) HideSlotTooltip();
    if (visible) Fill();
}

void UpdatePreview() {
    const auto view = CostumeClient::Shared().Snapshot();
    CostumePreview::Worn worn;
    // Off while combining: the server is about to take the item.
    const bool show = CostumeClient::Shared().Visible() && !view.combining;
    if (show) worn = {view.placed.itemId, view.placed.type, view.placed.bag, view.placed.slot};
    CostumePreview::Update(worn, show ? view.effect : 0);
    // The item stays marked while combining too, so it cannot be moved while the server takes it.
    CostumePreview::Worn held;
    if (CostumeClient::Shared().Visible()) held = {view.placed.itemId, view.placed.type, view.placed.bag, view.placed.slot};
    CostumePreview::Hold(held);
}
}  // namespace CostumeWindow
