#pragma once

// The Boss Check button in the World Map (M) window toolbar (layout Bitmaps\Interface4\UI\worldmap.ui with one extra stock button, made by
// tools/make_bosscheck_layout.py in the server repo). Frame() runs once per rendered frame: it finds the game's world map window by its RTTI
// class name and wraps that class's button dispatcher so a click on the new button opens the Boss Check window.
namespace WorldMapButton {
void Frame();
}
