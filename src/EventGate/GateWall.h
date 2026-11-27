#pragma once
#include <windows.h>

// Event gates: answers "blocked" to the client's walk check inside a closed gate, so the character stops
// at the wall. The server (RohanMapServ_Hook, docs/EVENT_GATE.md) says which gates are closed and still
// moves back anyone who gets through. Nothing is drawn and the loaded map is not modified.
namespace EventGate {
bool MatchesReference(const unsigned char* base);
// Joins the startup Detours transaction. Does nothing on another client build.
LONG Attach();
}
