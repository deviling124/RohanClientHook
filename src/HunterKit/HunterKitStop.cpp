#include "HunterKitStop.h"
#include "../CustomEvents/EventNotice.h"
#include <windows.h>
#include <cstring>

namespace HunterKitStop {
namespace {
// The game's Hunter Kit is CAutoBattleMgr, window 0x150 of the native window manager. SetRunning(false) is what its
// Stop button calls (0x00764C78), and 0x00900CC5 stops it with the same lookup, flag test and call as below.
constexpr UINT_PTR ManagerGlobal = 0x00D4C5E0, LookupAddress = 0x00585580, SetRunningAddress = 0x0076C890,
                   KitVtable = 0x00C579C4;
constexpr int KitWindowId = 0x150;
constexpr std::size_t RunningFlag = 0x24C;
using LookupFn = void*(__thiscall*)(void*, int);
using SetRunningFn = void(__thiscall*)(void*, int);

bool StopIfRunning() {
    constexpr unsigned char setRunning[] = {0x6A,0xFF,0x68,0xC0,0xB1,0xBF,0x00,0x64,0xA1,0x00,0x00,0x00,0x00,0x50,0x64,0x89};
    __try {
        if (std::memcmp(reinterpret_cast<const void*>(SetRunningAddress), setRunning, sizeof setRunning)) return false;
        void* manager = *reinterpret_cast<void**>(ManagerGlobal);
        if (!manager) return false;
        auto* kit = static_cast<unsigned char*>(reinterpret_cast<LookupFn>(LookupAddress)(manager, KitWindowId));
        // Only while running: SetRunning(false) also halts the character, which must not happen to a manual player.
        if (!kit || *reinterpret_cast<UINT_PTR*>(kit) != KitVtable || !kit[RunningFlag]) return false;
        reinterpret_cast<SetRunningFn>(SetRunningAddress)(kit, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

void OnAttackedByPlayer() {
    if (StopIfRunning()) EventNotice::ShowLocal("[Hunter Kit] Stopped: another player attacked you.");
}
}
