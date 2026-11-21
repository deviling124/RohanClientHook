#include "PlayerMenu.h"
#include "../Bridge.h"
#include "detours.h"
#include <cstring>

// Client internals (rohanclient_unpacked.exe 622e1068...):
//  - The player menu window class (vtable 0xC33EEC, constructor 0x55EFD0, window id 0xC1) keeps the clicked player's 8 byte object id at
//    window+0xE8 and its buttons in a vector at window+0xD4.
//  - Its click handler (vtable slot +0x0C, 0x55F0E0) finds the clicked button's position `di` in that vector and at 0x55F18B switches on it:
//    `movzx eax,di / cmp eax,9 / ja 0x55F646 / jmp [eax*4+0x55F670]`; 0x55F646 closes the menu. Position 0 is the first line of the layout
//    (control id 1, "Use Shop"), which the layout made by tools/make_playermenu_layout.py in the server repo relabels "Inspect".
//  - 0x50B330 (thiscall, manager 0xD44A34, const id*) returns the player object of an id; word [object+6] == 3 is a player.
//    0x477DE0 (thiscall, local player 0xD3FFC0, (object, 1)) returns the character name as a C string.
namespace {
constexpr DWORD kSwitchSite = 0x55F18B;
constexpr DWORD kMenuClose = 0x55F646;
constexpr UINT_PTR kManager = 0xD44A34, kLocalPlayer = 0xD3FFC0;
PVOID switchTrampoline = reinterpret_cast<PVOID>(kSwitchSite);

bool SiteMatches() {
    static const unsigned char expected[] = {0x0F, 0xB7, 0xC7, 0x83, 0xF8, 0x09, 0x0F, 0x87};   // movzx eax,di; cmp eax,9; ja
    __try { return std::memcmp(reinterpret_cast<const void*>(kSwitchSite), expected, sizeof expected) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __cdecl OnInspectClick(void* window) {
    char name[24]{};
    __try {
        unsigned id[2];
        std::memcpy(id, static_cast<const char*>(window) + 0xE8, sizeof id);
        const auto manager = *reinterpret_cast<void**>(kManager);
        const auto local = *reinterpret_cast<void**>(kLocalPlayer);
        if (!manager || !local) return;
        void* object = reinterpret_cast<void*(__thiscall*)(void*, const void*)>(0x50B330)(manager, id);
        if (!object || *reinterpret_cast<const unsigned short*>(static_cast<char*>(object) + 6) != 3) return;
        const char* text = reinterpret_cast<const char*(__thiscall*)(void*, void*, int)>(0x477DE0)(local, object, 1);
        if (!text) return;
        std::size_t length = 0;
        while (length < sizeof name - 1 && text[length]) ++length;
        if (!length || text[length]) return;   // empty or too long for a character name
        std::memcpy(name, text, length);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    Bridge::RequestInspect(name);
}

__declspec(naked) void SwitchStub() {
    __asm {
        test di, di              // position 0: the first line ("Inspect")
        jne original
        pushad
        push ebp                 // the menu window
        call OnInspectClick
        add esp, 4
        popad
        mov eax, kMenuClose      // the stock code then closes the menu
        jmp eax
    original:
        jmp dword ptr [switchTrampoline]
    }
}
}

namespace PlayerMenu {
LONG Attach() {
    if (GetPrivateProfileIntW(L"inspect", L"menu", 1, L".\\novaclient.ini") == 0) return NO_ERROR;
    if (!SiteMatches()) return NO_ERROR;   // another client build: leave the menu alone
    return DetourAttach(&switchTrampoline, reinterpret_cast<PVOID>(SwitchStub));
}
}
