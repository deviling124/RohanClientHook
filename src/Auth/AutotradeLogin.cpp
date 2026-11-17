#include "AutotradeLogin.h"
#include "detours.h"
#include <cstring>

namespace AutotradeLogin {
namespace {
using LoginResultFn = unsigned(__thiscall*)(void*, int);
using StringFn = const char*(__thiscall*)(void*, int);
constexpr unsigned LoginResultAddress = 0x00871a70;
constexpr unsigned StringAddress = 0x00aaa0a0;
auto originalLoginResult = reinterpret_cast<LoginResultFn>(LoginResultAddress);
auto originalString = reinterpret_cast<StringFn>(StringAddress);
thread_local bool buildingAutotradePrompt = false;

struct PromptScope {
    bool previous = buildingAutotradePrompt;
    explicit PromptScope(bool autotrade) noexcept { buildingAutotradePrompt = autotrade; }
    ~PromptScope() { buildingAutotradePrompt = previous; }
};

const char* __fastcall LookupString(void* self, void*, int id) {
    if (buildingAutotradePrompt) {
        switch (id) {
        case 0x39: return "Autotrade";
        case 0x3a: return "Your character is currently in autotrade.";
        case 0x3b: return "Close your offline Kiosk and log in?";
        }
    }
    return originalString(self, id);
}

unsigned __fastcall LoginResult(void* self, void*, int result) {
    PromptScope scope(result == Result);
    // Native -23 constructs the existing Yes/No dialog and keeps its
    // authenticated loginremove callback. Its text is copied during this call.
    return originalLoginResult(self, result == Result ? -23 : result);
}
}

bool MatchesReference(const unsigned char* base) noexcept {
    __try {
        if (!base) return false;
        constexpr unsigned char login[] = {0x6a,0xff,0x68,0x27,0x88,0xc0,0x00,0x64,0xa1,0x00,0x00,0x00,0x00,0x50,0x64,0x89};
        constexpr unsigned char strings[] = {0x51,0x56,0x57,0x8b,0x7c,0x24,0x10,0x85,0xff,0x8b,0xf1,0x7d,0x18,0x83,0x7e,0x1c};
        return !std::memcmp(base + LoginResultAddress - 0x400000, login, sizeof login) &&
            !std::memcmp(base + StringAddress - 0x400000, strings, sizeof strings);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LONG Attach() {
    auto result = DetourAttach(&reinterpret_cast<PVOID&>(originalLoginResult), reinterpret_cast<PVOID>(LoginResult));
    if (result == NO_ERROR)
        result = DetourAttach(&reinterpret_cast<PVOID&>(originalString), reinterpret_cast<PVOID>(LookupString));
    return result;
}
}
