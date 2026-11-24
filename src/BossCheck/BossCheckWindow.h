#pragma once

// The Boss Check window: a native client window (stock popup class, layout Bitmaps\Interface4\UI\boss_check.ui made by
// tools/make_bosscheck_layout.py in the server repo) listing the server's bosses with an ALIVE / DEAD mark and a Move button.
// Opened by `.bosscheck`. Frame() runs once per rendered frame on the render thread; it does nothing unless the layout is installed.
namespace BossCheckWindow {
void Frame();
}
