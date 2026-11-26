#include "CostumeGuard.h"
#include "CostumeClient.h"
#include "detours.h"
#include <cstddef>
#include <cstring>

namespace CostumeGuard {
namespace {
// Reference client.
constexpr unsigned UseCall = 0x00683CDF;       // in the bag grid's double-click (0x00683C70): CALL 0x008D7E80(inventory, item)
constexpr unsigned UseAddress = 0x008D7E80;
constexpr unsigned DropAddress = 0x0068B470;   // every item drop on the ground goes through this: (item, x, y, count)
constexpr unsigned SplitAddress = 0x0068D950;  // the "how many to drop" box of a stack
constexpr UINT_PTR CursorGlobal = 0x00D3D9D0, ChatGlobal = 0x00D4DD4C;
constexpr int SystemLine = 12;                 // the line type 0x0053AF70 prints its system messages with

using UseFn = void(__fastcall*)(void*, void*, void*);
using DropFn = void(__fastcall*)(void*, void*, void*, int, int, int);
using SplitFn = void(__fastcall*)(void*, void*);
const UseFn useItem = reinterpret_cast<UseFn>(UseAddress);
DropFn originalDrop = reinterpret_cast<DropFn>(DropAddress);
SplitFn originalSplit = reinterpret_cast<SplitFn>(SplitAddress);

bool WindowOpen() { return CostumeClient::Shared().Visible(); }

// A yellow system line, the way 0x0053AF70 shows a text-table message: the text travels as the client's std::string
// (0x1C bytes, built with its own assign 0x00402630) to 0x005E9F60.
void SystemMessage(const char* message) {
    alignas(8) unsigned char text[0x1C];
    std::memset(text, 0, sizeof text);
    *reinterpret_cast<unsigned*>(text + 0x18) = 0xF;
    __try {
        void* chat = *reinterpret_cast<void**>(ChatGlobal);
        if (!chat) return;
        reinterpret_cast<void(__thiscall*)(void*, const char*, unsigned)>(0x00402630)(text, message,
                                                                                    static_cast<unsigned>(std::strlen(message)));
        reinterpret_cast<void(__thiscall*)(void*, void*, int)>(0x005E9F60)(chat, text, SystemLine);
        if (*reinterpret_cast<unsigned*>(text + 0x18) >= 0x10)
            reinterpret_cast<void(__cdecl*)(void*)>(0x00B8D5ED)(*reinterpret_cast<void**>(text + 4));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Reads the client's bag item: type +8, unique id +0xC, bag byte +0xAC, slot byte +0xB0, in-use byte +0xB6.
bool ReadItem(const void* object, CostumeClient::Item& item, bool& inUse) {
    __try {
        const auto* bytes = static_cast<const unsigned char*>(object);
        if (!bytes) return false;
        item.type = *reinterpret_cast<const unsigned*>(bytes + 8);
        item.itemId = *reinterpret_cast<const int*>(bytes + 0xC);
        item.bag = bytes[0xAC];
        item.slot = bytes[0xB0];
        inUse = bytes[0xB6] != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __fastcall DoubleClickUse(void* inventory, void* edx, void* object) {
    CostumeClient::Item item;
    bool inUse = false;
    if (ReadItem(object, item, inUse)) {
        const DoubleClick what = Decide(WindowOpen(), item.type, item.bag, inUse);
        if (what == DoubleClick::Fill) CostumeClient::Shared().Drop(item, GetTickCount64());
        if (what != DoubleClick::Use) return;       // never equip it while the window is open
    }
    useItem(inventory, edx, object);
}

void __fastcall DropItem(void* self, void* edx, void* item, int x, int y, int count) {
    if (WindowOpen()) {
        SystemMessage(BlockedMessage);             // the caller then ends the drag: the item stays in the bag
        return;
    }
    originalDrop(self, edx, item, x, y, count);
}

void __fastcall SplitDrop(void* self, void* edx) {
    if (WindowOpen()) {
        SystemMessage(BlockedMessage);
        __try {                                    // this caller does not end the drag itself
            void* cursor = *reinterpret_cast<void**>(CursorGlobal);
            if (cursor) reinterpret_cast<void(__thiscall*)(void*)>(0x004BA870)(cursor);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    originalSplit(self, edx);
}

bool Redirect(unsigned site, const void* target) {
    DWORD protection = 0;
    auto* bytes = reinterpret_cast<unsigned char*>(static_cast<UINT_PTR>(site));
    if (!VirtualProtect(bytes, 5, PAGE_EXECUTE_READWRITE, &protection)) return false;
    const int relative = static_cast<int>(reinterpret_cast<UINT_PTR>(target) - (site + 5));
    std::memcpy(bytes + 1, &relative, 4);
    VirtualProtect(bytes, 5, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), bytes, 5);
    return true;
}
}  // namespace

bool MatchesReference(const unsigned char* base) {
    __try {
        if (!base) return false;
        constexpr unsigned char call[] = {0xE8,0x9C,0x41,0x25,0x00};                         // CALL 0x008D7E80
        constexpr unsigned char use[] = {0x6A,0xFF,0x68,0xF8,0xCB,0xC0,0x00,0x64,0xA1};
        constexpr unsigned char drop[] = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x56,0x8B,0xF1,0x74,0x32};
        constexpr unsigned char split[] = {0x6A,0xFF,0x68,0x3E,0x2D,0xBF,0x00,0x64,0xA1};
        constexpr unsigned char message[] = {0x53,0x55,0x8B,0x6C,0x24,0x10,0x0F,0xB7,0xC5};
        return !std::memcmp(base + UseCall - 0x400000, call, sizeof call) &&
               !std::memcmp(base + UseAddress - 0x400000, use, sizeof use) &&
               !std::memcmp(base + DropAddress - 0x400000, drop, sizeof drop) &&
               !std::memcmp(base + SplitAddress - 0x400000, split, sizeof split) &&
               !std::memcmp(base + 0x005E9F60 - 0x400000, message, sizeof message);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG Attach() {
    if (!MatchesReference(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr)))) return NO_ERROR;
    LONG result = DetourAttach(&reinterpret_cast<PVOID&>(originalDrop), reinterpret_cast<PVOID>(DropItem));
    if (result == NO_ERROR) result = DetourAttach(&reinterpret_cast<PVOID&>(originalSplit), reinterpret_cast<PVOID>(SplitDrop));
    if (result == NO_ERROR) Redirect(UseCall, reinterpret_cast<const void*>(&DoubleClickUse));   // harmless while closed
    return result;
}
}  // namespace CostumeGuard
