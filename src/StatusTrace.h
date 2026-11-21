#pragma once
#include <windows.h>

// Diagnostic for the "inspect other players" feature: finds how the stock Character Information window
// (CharacterStatus.ui, UI manager id 7) reads its stats and equipment. Off unless a file named `statustrace.on`
// exists in the game folder. Output goes to `novaclient_statustrace.log`.
//
// Use: start the client, log in, press F10 (capture ON, the window object is dumped), open Character Information
// and wait a second, press F10 again (capture OFF). Every native SetText call between the two presses is logged with
// its caller address, and every ShowWindow call is logged always. See docs/STATUS_TRACE.md.
// Attach() must be called inside the caller's open Detours transaction, like the other modules.
namespace StatusTrace {
LONG Attach();
void Frame(); // once per rendered frame on the render thread; polls F10
}
