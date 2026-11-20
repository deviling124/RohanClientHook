#pragma once
#include <windows.h>

// Diagnostic for reverse engineering the native window system. Off unless a file named `uitrace.on` exists in the
// game folder (the client's working directory). When on, every CreateFile of a `.ui` layout is appended to
// `novaclient_uitrace.log` with the caller's stack, so the loader and the owning window class can be identified.
// Attach() must be called inside the caller's open Detours transaction, like the other modules.
namespace UiTrace {
LONG Attach();
// Called once per rendered frame on the render thread (only does anything when uitrace.on exists). F9 builds a
// prototype native window (id 0x300) from the stock generic popup class and shows it; see docs/COLLECTION_BOOK_RE.md.
void Frame();
}
