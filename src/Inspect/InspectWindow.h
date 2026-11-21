#pragma once

// The inspect window: a native client window (stock popup class, layout Bitmaps\Interface4\UI\inspect_character.ui made by
// tools/make_inspect_layout.py in the server repo) that shows another player's Character Information. Opened by `.inspect <name>`.
// Frame() runs once per rendered frame on the render thread; it does nothing unless the layout file is installed.
namespace InspectWindow {
void Frame();
}
