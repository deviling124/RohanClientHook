#pragma once
#include <windows.h>

// The Costume Effect preview: the costume in the window's slot, and the chosen effect's aura, on the player's own character,
// on this screen only. Nothing is equipped: the character is only drawn that way.
// The aura functions are the client's own; both are __thiscall(character, key, subtype) and a key is the low 16 bits of a
// costume type. Everything here runs on the game's window thread.
namespace CostumePreview {
bool MatchesReference(const unsigned char* base);
// Hooks the two aura functions, so a real costume change ends the preview first. Joins the startup Detours transaction.
LONG Attach();
// The result costume type to show on the local player, or 0 for the player's own aura. Safe to call every frame.
void Set(unsigned costumeType);
// The bag item to wear, as it was when it was dropped into the slot. itemId 0 = none.
struct Worn {
    int itemId = 0;
    unsigned type = 0;
    int bag = 0, slot = 0;
};
// Wears the item (or puts the equipped costume back when there is none) and then shows `effectType`'s aura, 0 = the aura of
// what is worn. Safe to call every frame.
void Update(const Worn& item, unsigned effectType);
// Marks the bag item in the slot as in use, the way the item recycle window does: its icon is darkened and it cannot be used
// or moved. Pass an empty item to give it back. Safe to call every frame.
void Hold(const Worn& item);
// The local player's bag item object, if it is still where it was; null otherwise. Do not keep the pointer.
void* ItemObject(const Worn& item);
// True while another aura than the game's own is on the player.
bool Active();

struct Game {
    void(__fastcall* attach)(void* character, void*, unsigned key, int subtype);
    void(__fastcall* remove)(void* character, void*, unsigned key, int subtype);
    void* (*localPlayer)();
    unsigned (*applied)(void* character);          // character+0x540: the attached aura's key, 0 = none
    bool (*wear)(void* character, const Worn& item);   // false: the item is no longer there, nothing changed
    void (*takeOff)(void* character, const Worn& item);   // takes `item` off and shows what is really equipped again
    void (*mark)(void* character, const Worn& item, bool inUse);
    void* (*itemObject)(void* character, const Worn& item);
};
#ifdef COSTUME_PREVIEW_TESTING
void TestSetup(const Game& game);
void TestGameAttach(void* character, unsigned key, int subtype);
void TestGameRemove(void* character, unsigned key, int subtype);
#endif
}
