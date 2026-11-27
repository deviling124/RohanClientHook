#include "GateWall.h"
#include "GateWallStore.h"
#include "detours.h"
#include <cstring>

namespace EventGate {
namespace {
// bool __thiscall(world, int bit, float x, float height, float z), RET 0x10: tests one bit of the loaded
// .atr cell at a world position. The walking code stops when bit 0 (blocked) is set. Other bits mean
// other things and are left alone. Found in rohanclient_new.exe: it calls the grid reader 0x00A94B70
// on the grid loaded by 0x00A94BE0 ("ROHANATTR").
constexpr unsigned QueryAddress = 0x00A14900;
constexpr int BlockedBit = 0;
using QueryFn = bool(__thiscall*)(void*, int, float, float, float);
QueryFn originalQuery = reinterpret_cast<QueryFn>(QueryAddress);

bool __fastcall Query(void* self, void*, int bit, float x, float height, float z) {
    if (bit == BlockedBit && Walls.Blocked(x, z, GetTickCount64())) return true;
    return originalQuery(self, bit, x, height, z);
}
}

bool MatchesReference(const unsigned char* base) {
    constexpr unsigned char query[] = {0x8B,0x54,0x24,0x10,0x83,0xEC,0x08,0x56,0x57,0x8B,0xF1,0x8D,0x44,0x24,0x08,0x50};
    __try {
        return base && !std::memcmp(base + QueryAddress - 0x00400000, query, sizeof query);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG Attach() {
    if (!MatchesReference(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr))))
        return NO_ERROR; // The server still moves players back.
    return DetourAttach(&reinterpret_cast<PVOID&>(originalQuery), reinterpret_cast<PVOID>(Query));
}
}
