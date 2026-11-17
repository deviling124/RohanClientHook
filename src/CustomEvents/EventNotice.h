#pragma once
#include <windows.h>

namespace EventNotice {
bool MatchesReference(const unsigned char* base);
// Displays a bounded local notice on the game window thread.
void ShowLocal(const char* message);
// Participate in the client's existing startup Detours transaction.
LONG Attach();
}
