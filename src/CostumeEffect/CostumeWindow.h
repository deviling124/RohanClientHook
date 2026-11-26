#pragma once

// The Costume Effect window: a native client window (stock popup class, layout Bitmaps\Interface4\UI\costume_effect.ui made
// by tools/make_costume_effect_layout.py in the server repo). Opened by `.costume`.
namespace CostumeWindow {
// Once per rendered frame on the render thread. Without the layout file it only reports that the window is missing.
void Frame();
// On the game's window thread, every frame: what the player's own character should look like while the window is open.
void UpdatePreview();
}
