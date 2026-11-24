#pragma once

// The Boss Check button on the minimap (layout Bitmaps\Interface4\UI\minimap.ui with one extra button, made by
// tools/make_bosscheck_layout.py in the server repo). Frame() runs once per rendered frame: it finds the game's minimap window
// (class CUIMiniMap, by its RTTI name) and wraps that class's button dispatcher so a click on the new button opens the Boss Check window.
namespace MinimapButton {
void Frame();
}
