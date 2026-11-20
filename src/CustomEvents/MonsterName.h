#pragma once
#include <windows.h>
#include <cstdint>
#include <string_view>

// Disguise Event: shows a scrambled name for one monster type while the server asks for it.
// The client's own monster table keeps one script object per monster ID; its name strings are
// overwritten in place (same length) and restored when the server says "=" or goes quiet.
namespace MonsterName {
bool MatchesReference(const unsigned char* base);
// Records every monster script as the client loads monster.bin. Joins the startup Detours transaction.
LONG Attach();
// name "=" restores the real name. Returns true when at least one name string changed (or was restored).
bool Handle(std::uint32_t monsterId, std::string_view name);
#ifdef EVENT_NOTICE_TESTING
void TestRegister(std::uint32_t monsterId, void* script);
void TestExpire();
#endif
}
