#pragma once
#include <windows.h>

namespace AutotradeLogin {
constexpr int Result = -23023;
bool MatchesReference(const unsigned char* base) noexcept;
LONG Attach();
}
