#pragma once
#include <windows.h>

namespace GlobalChat {
bool MatchesReference(const unsigned char* base);
LONG Attach();
}
