#pragma once

// The Event Manager join popup: a native client window (layout Bitmaps\Interface4\UI\event_popup.ui made by
// tools/make_eventpopup_layout.py in the server repo). It opens by itself while the server says an event is
// about to start, comes back at once if the game hides it (Escape, teleport), and closes on Join or at the start.
// It does not block game input.
// Frame() runs once per rendered frame on the render thread.
namespace EventPopupWindow {
void Frame();
}
