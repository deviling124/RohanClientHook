#include "CostumePreview.h"
#include "detours.h"
#include <cstring>

namespace CostumePreview {
namespace {
// Reference client. Attach looks up the gameeffect.bin row (0x6B, key), spawns its effect and stores key at +0x540.
// Remove looks up the same row and removes that effect, so it needs the key that was attached.
constexpr unsigned AttachAddress = 0x00476920, RemoveAddress = 0x004769B0;
constexpr int CostumeSubtype = 0x6B;
using AuraFn = void(__fastcall*)(void*, void*, unsigned, int);
AuraFn originalAttach = reinterpret_cast<AuraFn>(AttachAddress);
AuraFn originalRemove = reinterpret_cast<AuraFn>(RemoveAddress);

void* ClientLocal() {
    __try { return *reinterpret_cast<void**>(0x00D3FFC0); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
unsigned ClientApplied(void* character) {
    __try { return *reinterpret_cast<unsigned*>(static_cast<char*>(character) + 0x540); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The client's own dressing call (item\EquipDef.cpp), the one its costume preview window uses. Slot 14 is the costume.
// __thiscall(*0xD3B078, character, model, item, previous item, slot, force shown). It changes only what is drawn, and for
// slot 14 swaps the aura too. It must be told what is being replaced (`previous`): with neither a new nor a previous item it
// does nothing, so the previewed model and its aura would stay on.
constexpr unsigned DressAddress = 0x008E0080, ModelSetAddress = 0x004EF610, ItemAtAddress = 0x00AD05D0;
constexpr int CostumeSlot = 14;
using DressFn = void(__thiscall*)(void*, void*, void*, void*, void*, int, int);
using ModelSetFn = void*(__thiscall*)(void*);
using ItemAtFn = void*(__thiscall*)(void*, int, int);

void* ItemAt(void* character, int bag, int slot) {
    void* inventory = *reinterpret_cast<void**>(static_cast<char*>(character) + 0x5B4);
    return inventory ? reinterpret_cast<ItemAtFn>(ItemAtAddress)(inventory, bag, slot) : nullptr;
}
bool Dress(void* character, void* item, void* previous, int forceShown) {
    void* renderer = *reinterpret_cast<void**>(0x00D3B078);
    void* set = reinterpret_cast<ModelSetFn>(ModelSetAddress)(character);
    if (!renderer || !set) return false;
    const int current = *reinterpret_cast<int*>(static_cast<char*>(set) + 8);
    void** models = *reinterpret_cast<void***>(static_cast<char*>(set) + 0xC);
    void* model = models ? models[current] : nullptr;
    if (!model) return false;
    reinterpret_cast<DressFn>(DressAddress)(renderer, character, model, item, previous, CostumeSlot, forceShown);
    return true;
}
// The bag item, if it is still where it was when it was dropped in: type at +8, unique id at +0xC.
unsigned char* BagItem(void* character, const Worn& wanted) {
    auto* item = static_cast<unsigned char*>(ItemAt(character, wanted.bag, wanted.slot));
    if (!item || *reinterpret_cast<unsigned*>(item + 8) != wanted.type || *reinterpret_cast<int*>(item + 0xC) != wanted.itemId)
        return nullptr;
    return item;
}
bool ClientWear(void* character, const Worn& wanted) {
    __try {
        void* item = BagItem(character, wanted);
        return item && Dress(character, item, ItemAt(character, 0, CostumeSlot), 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ClientTakeOff(void* character, const Worn& worn) {
    __try {
        void* shownItem = BagItem(character, worn);
        void* equipped = ItemAt(character, 0, CostumeSlot);
        if (!shownItem) {                       // the item is gone: at least its aura comes off
            const unsigned aura = ClientApplied(character);
            if (aura) originalRemove(character, nullptr, aura, CostumeSubtype);
        }
        Dress(character, equipped, shownItem, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// +0xB6 is the "in use" byte the item recycle window sets on the bag items in its slots (0x005C36DD) and clears again.
void ClientMark(void* character, const Worn& wanted, bool inUse) {
    __try {
        if (unsigned char* item = BagItem(character, wanted)) item[0xB6] = inUse ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void* ClientItemObject(void* character, const Worn& wanted) {
    __try { return BagItem(character, wanted); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
Game game{nullptr, nullptr, ClientLocal, ClientApplied, ClientWear, ClientTakeOff, ClientMark, ClientItemObject};
bool ready = false;

void* subject = nullptr;     // the character wearing the preview
unsigned shown = 0;          // the preview key on it
unsigned saved = 0;          // the aura it had before, 0 = none
unsigned asked = 0;          // the last costume type asked for, so a refused one is not retried every frame
int requested = 0;           // the last item asked to be worn, for the same reason
void* dressed = nullptr;     // the character wearing a previewed costume
Worn worn;                   // and which costume that is
Worn held;                   // the bag item marked as in use
void* holder = nullptr;      // whose bag it is in

// Takes the preview off and puts the previous aura back.
void Restore() {
    if (!subject) return;
    void* character = subject;
    const unsigned key = shown, previous = saved;
    subject = nullptr;
    shown = saved = 0;
    if (character != game.localPlayer()) return;   // that character is gone (logout, character change): do not touch it
    originalRemove(character, nullptr, key, CostumeSubtype);
    if (previous) originalAttach(character, nullptr, previous, CostumeSubtype);
}

// The game itself is changing this character's aura: the preview comes off first and the game's own call then runs as usual.
void __fastcall AttachAura(void* character, void* edx, unsigned key, int subtype) {
    if (subtype == CostumeSubtype && character && character == subject) Restore();
    originalAttach(character, edx, key, subtype);
}
void __fastcall RemoveAura(void* character, void* edx, unsigned key, int subtype) {
    if (subtype == CostumeSubtype && character && character == subject) Restore();
    originalRemove(character, edx, key, subtype);
}
}  // namespace

void Set(unsigned costumeType) {
    if (!ready) return;
    void* local = game.localPlayer();
    if (subject && subject != local) { subject = nullptr; shown = saved = 0; asked = 0; }
    if (costumeType == asked) return;
    asked = costumeType;
    Restore();
    const unsigned key = costumeType & 0xFFFF;
    if (!key || !local) return;
    const unsigned previous = game.applied(local);
    if (previous) originalRemove(local, nullptr, previous, CostumeSubtype);
    originalAttach(local, nullptr, key, CostumeSubtype);
    if (game.applied(local) != key) {              // no aura row for that costume in this client
        if (previous) originalAttach(local, nullptr, previous, CostumeSubtype);
        return;
    }
    subject = local;
    shown = key;
    saved = previous;
}

void Update(const Worn& item, unsigned effectType) {
    if (!ready) return;
    void* local = game.localPlayer();
    if (dressed && dressed != local) { dressed = nullptr; requested = 0; }   // logout or character change: nothing to put back
    if (item.itemId != requested) {
        Set(0);                              // the aura goes back to what the model being replaced came with
        asked = 0;
        requested = item.itemId;
        if (dressed) { game.takeOff(dressed, worn); dressed = nullptr; }
        if (item.itemId && local && game.wear(local, item)) { dressed = local; worn = item; }
    }
    Set(effectType);
}

void Hold(const Worn& item) {
    if (!ready) return;
    void* local = game.localPlayer();
    if (holder && holder != local) { holder = nullptr; held = {}; }
    if (holder && item.itemId != held.itemId) {
        game.mark(holder, held, false);
        holder = nullptr;
        held = {};
    }
    if (item.itemId && local) {
        game.mark(local, item, true);            // every time: the client may have rebuilt the item
        holder = local;
        held = item;
    }
}

void* ItemObject(const Worn& item) {
    if (!ready || !item.itemId) return nullptr;
    void* local = game.localPlayer();
    return local ? game.itemObject(local, item) : nullptr;
}

bool Active() { return subject != nullptr; }

bool MatchesReference(const unsigned char* base) {
    __try {
        if (!base) return false;
        // SUB ESP,0x40 / MOV EAX,[security cookie] / first pushes
        constexpr unsigned char attach[] = {0x83,0xEC,0x40,0xA1,0x90,0x5E,0xD3,0x00,0x53,0x55,0x8B,0x6C,0x24,0x4C,0x56,0x57};
        constexpr unsigned char dress[] = {0x6A,0xFF,0x68,0xD2,0xD0,0xC0,0x00,0x64,0xA1,0x00,0x00,0x00,0x00,0x50,0x64,0x89};
        constexpr unsigned char models[] = {0x8B,0x41,0x64,0xC3};
        constexpr unsigned char itemAt[] = {0x53,0x56,0x8B,0xF1,0x57,0x8D,0x5E,0x0C,0x53,0xFF,0x15,0xD4,0x51,0xC2,0x00,0x8B};
        constexpr unsigned char remove[] = {0x83,0xEC,0x40,0xA1,0x90,0x5E,0xD3,0x00,0x8B,0x54,0x24,0x44,0x56,0x57,0x8B,0xF1};
        return !std::memcmp(base + AttachAddress - 0x400000, attach, sizeof attach) &&
               !std::memcmp(base + RemoveAddress - 0x400000, remove, sizeof remove) &&
               !std::memcmp(base + DressAddress - 0x400000, dress, sizeof dress) &&
               !std::memcmp(base + ModelSetAddress - 0x400000, models, sizeof models) &&
               !std::memcmp(base + ItemAtAddress - 0x400000, itemAt, sizeof itemAt);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG Attach() {
    if (!MatchesReference(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr)))) return NO_ERROR;   // no preview
    LONG result = DetourAttach(&reinterpret_cast<PVOID&>(originalAttach), reinterpret_cast<PVOID>(AttachAura));
    if (result == NO_ERROR)
        result = DetourAttach(&reinterpret_cast<PVOID&>(originalRemove), reinterpret_cast<PVOID>(RemoveAura));
    if (result == NO_ERROR) {
        ready = true;
    }
    return result;
}

#ifdef COSTUME_PREVIEW_TESTING
void TestSetup(const Game& replacement) {
    game = replacement;
    originalAttach = replacement.attach;
    originalRemove = replacement.remove;
    subject = nullptr;
    shown = saved = asked = 0;
    requested = 0;
    dressed = nullptr;
    holder = nullptr;
    worn = held = {};
    ready = true;
}
void TestGameAttach(void* character, unsigned key, int subtype) { AttachAura(character, nullptr, key, subtype); }
void TestGameRemove(void* character, unsigned key, int subtype) { RemoveAura(character, nullptr, key, subtype); }
#endif
}
