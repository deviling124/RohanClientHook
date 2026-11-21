#include "InspectWindow.h"
#include "InspectClient.h"
#include "../UiTrace.h"
#include "../CustomEvents/EventNotice.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace InspectWindow {
namespace {
// Native window system (reference client 622e1068...; see docs/COLLECTION_BOOK_RE.md in the server repo).
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0;   // pointer to the UI window manager
constexpr unsigned WindowId = 0x301;             // the Collection Book uses 0x300
constexpr UINT_PTR TextVtable = 0x00C2EB54;      // text control objects (type 10)
constexpr const char* LayoutPath = "\\Bitmaps\\Interface4\\UI\\inspect_character.ui";
using GameNewFn = void*(__cdecl*)(std::size_t);
using PopupCtorFn = void*(__thiscall*)(void*, int, const char*);
using PushBackFn = void(__thiscall*)(void*, void**);
using ShowWindowFn = void(__thiscall*)(void*, int, int);
using LookupWindowFn = void*(__thiscall*)(void*, int);
using SetTextFn = void(__thiscall*)(void*, const char*);
using DispatchFn = unsigned char(__thiscall*)(void*, int, int, int);
using DrawFn = void(__thiscall*)(void*);

// Text control ids of the layout (DYN in tools/make_inspect_layout.py).
enum TextId {
    Title = 1000, LevelName = 1008, Race = 1009, Rank = 1010, Guild = 1011, Class = 1012, TitleText = 1013,
    Str = 1014, Vit = 1015, Int = 1016, Dex = 1017, Psy = 1018, Agi = 1019, Point = 1020,
    Melee = 1021, Ranged = 1023, Magic = 1025, PDef = 1027, MDef = 1029, Health = 1031, Mana = 1033,
    Resist = 1035, Accuracy = 1037, Evasion = 1039, Mobility = 1041, Speed = 1043, HpRec = 1045, MpRec = 1047,
    Weight = 1049, Fire = 1050, Ice = 1051, Dark = 1052, Light = 1053,
};
// Icon slot (36 + index) shows the server equipment slot below. Battle page: tools/make_inspect_layout.py SLOTS.
constexpr int IconSlotBase = 36;
constexpr unsigned BattleSlot[12] = {0, 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13};
// Costume page (COSTUME_SLOTS in the layout script): head, glasses, earring, outfit. Glasses and earring are the server slots 7 and 8; the
// costume head and outfit slots are NOT confirmed: 14 and 28 are a guess. `.inspect <name> slots` lists the slots that hold something.
constexpr unsigned CostumeSlot[4] = {14, 7, 8, 28};
constexpr int KeyBattle = 2, KeyCostume = 3;   // button keys = positions in the layout's second group

struct TextRef { int id; void* control; };
TextRef texts[96];
int textCount = 0;
void* window = nullptr;
int page = 1;                       // 1 Battle, 2 Costume
InspectWire::Profile shown;         // what the window currently shows
DispatchFn originalDispatch = nullptr;
DrawFn originalDraw = nullptr;
volatile LONG tooltipItem = 0;      // the item type under the cursor; the draw below shows its tooltip every frame
volatile LONG tooltipSlot = -1;     // its server equipment slot (the tooltip is built from that slot's native record when the server sent one)

// Item boxes (window-relative top-left corners, 32x32): the icon positions of the layout (tools/make_inspect_layout.py).
struct Box { int x, y; };
constexpr Box BattleBox[12] = {{59, 102}, {59, 140}, {59, 178}, {22, 140}, {59, 216}, {22, 254}, {59, 254}, {22, 178}, {96, 178}, {22, 216}, {96, 216}, {96, 254}};
constexpr Box CostumeBox[4] = {{62, 102}, {16, 147}, {101, 147}, {16, 206}};
constexpr int BoxSize = 32;

bool IsTextControl(UINT_PTR p) {
    __try { return p > 0x10000 && *reinterpret_cast<const UINT_PTR*>(p) == TextVtable; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Controls sit in std::vector-like triples among the window's first members; the text ones are found by their vtable.
void CollectTextControls(void* object) {
    textCount = 0;
    __try {
        const auto base = reinterpret_cast<UINT_PTR>(object);
        for (UINT_PTR off = 0x30; off < 0x1C0 && textCount < 96; off += 4) {
            const UINT_PTR first = *reinterpret_cast<const UINT_PTR*>(base + off);
            const UINT_PTR last = *reinterpret_cast<const UINT_PTR*>(base + off + 4);
            if (first < 0x10000 || last < first || ((last - first) & 3) || last - first > 4 * 512) continue;
            for (UINT_PTR e = first; e < last && textCount < 96; e += 4) {
                const UINT_PTR p = *reinterpret_cast<const UINT_PTR*>(e);
                if (!IsTextControl(p)) continue;
                const int id = *reinterpret_cast<const int*>(p + 8);
                if (id < 1000) continue;   // the dynamic texts only
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

std::string Num(int value) { return std::to_string(value); }

// The parts of the window that depend on the page: the item icons and the resistance counters (which only exist on the Battle page).
void ApplyPage() {
    UiTrace::SetInspectPage(page);
    if (page == 1) {
        for (int i = 0; i < 12; ++i) UiTrace::SetIconItem(IconSlotBase + i, shown.eq[BattleSlot[i]]);
        SetText(Fire, Num(shown.res[0])); SetText(Ice, Num(shown.res[1])); SetText(Dark, Num(shown.res[2])); SetText(Light, Num(shown.res[3]));
    } else {
        for (int i = 0; i < 12; ++i) UiTrace::SetIconItem(IconSlotBase + i, i < 4 ? shown.eq[CostumeSlot[i]] : 0u);
        for (int id : {Fire, Ice, Dark, Light}) SetText(id, "");
    }
}

// The window's own button dispatcher: the Battle and Costume tabs; everything else (drag, close) stays with the stock handler.
unsigned char __fastcall DetourDispatch(void* self, void*, int key, int kind, int arg) {
    if (self == window && kind == 0 && (key == KeyBattle || key == KeyCostume)) {
        page = key == KeyBattle ? 1 : 2;
        ApplyPage();
        return 1;
    }
    return originalDispatch(self, key, kind, arg);
}

// The stock inventory draws its item tooltip from its own draw method, every frame; the window's draw slot (+0x48) does the same here.
void __fastcall DetourDraw(void* self, void*) {
    originalDraw(self);
    const LONG item = tooltipItem;
    if (self == window && item) {
        const LONG slot = tooltipSlot;
        if (slot >= 0 && slot < static_cast<LONG>(InspectWire::SlotCount) && shown.rec[slot].size() == InspectWire::RecordSize)
            UiTrace::ShowTooltipRecord(reinterpret_cast<const unsigned char*>(shown.rec[slot].data()));
        else
            UiTrace::ShowTooltipFor(static_cast<unsigned>(item));
    }
}

bool Alive();

// Which item box is under the cursor: sets tooltipItem, and closes the tooltip a moment after the cursor left (the hover test can drop out
// for a few frames while the tooltip window opens).
void UpdateHover() {
    LONG item = 0, slotHit = -1;
    __try {
        HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &pid);
        POINT pt{};
        const auto* win = reinterpret_cast<const int*>(window);
        if (window && pid == GetCurrentProcessId() && Alive() && win[0x38 / 4] && GetCursorPos(&pt) && ScreenToClient(foreground, &pt)) {
            const int mx = pt.x - win[0x3C / 4], my = pt.y - win[0x40 / 4];
            const int count = page == 1 ? 12 : 4;
            for (int i = 0; i < count; ++i) {
                const Box& box = page == 1 ? BattleBox[i] : CostumeBox[i];
                if (mx < box.x || mx >= box.x + BoxSize || my < box.y || my >= box.y + BoxSize) continue;
                slotHit = static_cast<LONG>(page == 1 ? BattleSlot[i] : CostumeSlot[i]);
                item = static_cast<LONG>(shown.eq[slotHit]);
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { item = 0; }
    static ULONGLONG lostSince = 0;
    const ULONGLONG now = GetTickCount64();
    if (item) {
        lostSince = 0;
        if (item != tooltipItem || slotHit != tooltipSlot) { InterlockedExchange(&tooltipSlot, slotHit); InterlockedExchange(&tooltipItem, item); }
    } else if (tooltipItem) {
        if (!lostSince) lostSince = now;
        if (now - lostSince > 150) {
            InterlockedExchange(&tooltipItem, 0);
            UiTrace::HideTooltip();
            lostSince = 0;
        }
    }
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
        // Own copy of the popup class's vtable with the button dispatcher (slot +0xB4, index 45) replaced, as the Collection Book does.
        static unsigned vtableStorage[49];
        const unsigned* stock = *static_cast<unsigned**>(created);
        vtableStorage[0] = stock[-1];   // the RTTI locator sits just before the vtable
        std::memcpy(&vtableStorage[1], stock, 48 * sizeof(unsigned));
        originalDispatch = reinterpret_cast<DispatchFn>(stock[45]);
        vtableStorage[1 + 45] = reinterpret_cast<unsigned>(&DetourDispatch);
        originalDraw = reinterpret_cast<DrawFn>(stock[18]);   // the draw slot (+0x48)
        vtableStorage[1 + 18] = reinterpret_cast<unsigned>(&DetourDraw);
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

void Fill(const InspectWire::Profile& p) {
    shown = p;
    page = 1;   // every inspect opens on the Battle tab
    SetText(Title, p.name + "'s Character Information");
    SetText(LevelName, "Lv." + Num(p.level) + " " + p.name);
    // Names come from inspect_names.txt (see InspectClient.h); numbers without a line show "-". The guild name is the real one.
    {
        InspectClient::NameTable names;
        std::string text;
        if (FILE* file = std::fopen("inspect_names.txt", "rb")) {
            char buffer[4096];
            std::size_t n;
            while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0 && text.size() < 65536) text.append(buffer, n);
            std::fclose(file);
        }
        names.Parse(text);
        auto orDash = [](const std::string& value) { return value.empty() ? std::string("-") : value; };
        SetText(Race, orDash(names.Race(p.type, p.dbType)));
        SetText(Class, orDash(names.Class(p.type, p.dbType)));
        // The window's "Title" is the guild position (grade). "Rank" is shown from a title id when the character has one; the source of the
        // game's own Rank (e.g. "Soldier") is not known yet.
        SetText(TitleText, orDash(p.guildGrade >= 0 ? names.Rank(p.guildGrade) : std::string()));
        SetText(Guild, orDash(p.guild));
        SetText(Rank, orDash(p.title && p.title != 0xFFFFFFFFu ? names.Title(p.title) : std::string()));
    }
    SetText(Str, Num(p.attr[0])); SetText(Vit, Num(p.attr[1])); SetText(Int, Num(p.attr[2]));
    SetText(Dex, Num(p.attr[3])); SetText(Psy, Num(p.attr[4])); SetText(Agi, Num(p.attr[5]));
    SetText(Point, "");
    SetText(Melee, Num(p.stat[0])); SetText(Ranged, Num(p.stat[1])); SetText(Magic, Num(p.stat[2]));
    SetText(PDef, Num(p.stat[3])); SetText(MDef, Num(p.stat[4])); SetText(Accuracy, Num(p.stat[5])); SetText(Evasion, Num(p.stat[6]));
    SetText(Health, Num(p.vit[0])); SetText(HpRec, Num(p.vit[1])); SetText(Mana, Num(p.vit[2])); SetText(MpRec, Num(p.vit[3]));
    SetText(Resist, Num(p.vit[4])); SetText(Mobility, Num(p.vit[5])); SetText(Speed, Num(p.vit[6]));
    SetText(Weight, Num(p.vit[7]));
    ApplyPage();
}

// `.inspect <name> slots`: the equipment slots that hold something, a few per chat line.
void ReportSlots(const InspectWire::Profile& p) {
    char numbers[160];
    std::snprintf(numbers, sizeof numbers, "[Inspect] type=0x%X dbtype=%d guild grade=%d title=%u guild=%s", p.type, p.dbType, p.guildGrade, p.title,
                  p.guild.empty() ? "-" : p.guild.c_str());
    EventNotice::ShowLocal(numbers);
    std::string line = "[Inspect] slots:";
    int onLine = 0;
    for (unsigned i = 0; i < InspectWire::SlotCount; ++i) {
        if (!p.eq[i]) continue;
        line += " " + std::to_string(i) + "=" + std::to_string(p.eq[i]);
        if (++onLine == 4) { EventNotice::ShowLocal(line.c_str()); line = "[Inspect] slots:"; onLine = 0; }
    }
    if (onLine) EventNotice::ShowLocal(line.c_str());
}
}  // namespace

void Frame() {
    if (!UiTrace::InspectLayoutInstalled()) return;
    if (window) UpdateHover();
    auto& client = InspectClient::Shared();
    if (!client.TakeUpdate()) return;
    const bool debug = client.TakeDebug();
    if (client.state() == InspectClient::State::Ready) {
        if (!Ensure() || !textCount) {
            EventNotice::ShowLocal("[Inspect] The inspect window could not be opened.");
            return;
        }
        Fill(client.profile());
        Show();
        if (debug) ReportSlots(client.profile());
    } else {
        EventNotice::ShowLocal((std::string("[Inspect] ") + InspectClient::ErrorText(client.error())).c_str());
    }
}
}  // namespace InspectWindow
