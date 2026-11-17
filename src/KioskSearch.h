#pragma once
#include <windows.h>

namespace KioskSearch {
bool MatchesReference(const unsigned char* base) noexcept;
LONG Attach(); // Uses the caller's startup Detours transaction.
void Frame(); // Reports lookup timeouts and drops sessions after character changes.
}
