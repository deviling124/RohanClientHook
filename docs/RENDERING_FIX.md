# Overlay rendering and cursor regression — 2026-11-13

Reported live symptoms: world rendering becomes corrupted after opening `.ahk`,
and the mouse pointer feels laggy. The prior standalone preview did not exercise
the game's nondefault graphics state.

## Changes

The overlay now hooks device Present instead of EndScene. It draws once before
the final present, with its own BeginScene/EndScene pair and the actual primary
backbuffer explicitly bound. It preserves all original render targets, depth
surface, viewport/scissor, transforms, vertex/pixel shaders, vertex declaration,
FVF, stream/index bindings and stream frequency, plus a complete state block.
Capture failures skip drawing; cleanup also runs on partial setup failure or
C++ exceptions. It never ends a host scene when its own BeginScene fails.

The renderer establishes the FFP defaults that the stock backend assumes,
including texture coordinate/transform settings, color write mask, sRGB mode,
clip planes and vertex blending. Those temporary defaults are also restored.
FVF is restored before the exact original vertex declaration: GetFVF may retain
an earlier value after a custom declaration, and SetFVF changes the declaration.

The client has creation paths requesting PUREDEVICE | HARDWARE_VERTEXPROCESSING
(0x50). The previous backend used unchecked GetTransform results, then restored
the corresponding uninitialized matrices if those queries failed. The current
creation hook removes only PUREDEVICE, retaining all other flags. The new guard
also requires every capture query to succeed. The user's actual prior device
flags have not been captured, so this is a confirmed compatibility risk rather
than a claim that pure-device mode was observed in that live session. New logs
record requested and effective creation flags.

The UI uses the Windows cursor rather than a software cursor rendered at game
frame rate. It saves/restores cursor shape and visibility-counter adjustments;
game hide/show calls are isolated while the editor owns the cursor. Key, mouse
and cursor-warp suppression still prevent the editor from controlling the game.
Floating-point environment changes during overlay work are also restored before
calling the game's original Present.

## Verification

`tests/test-render.cmd` passed 1,623 checks using an actual hidden HAL DX9 device
and the real ImGui renderer. It checks fixed-function/FVF and shader/explicit
declaration hosts, exact state restoration, untouched offscreen/MRT pixels,
actual backbuffer output, empty/no-Begin/destructor paths and host-scene
ownership. One capability case is unavailable: this test driver permits
GetTransform even on a pure device, so the corresponding getter-failure path
could not be observed on this driver.

The graphics tests exposed and verified corrections for inherited FFP defaults
and FVF/declaration restoration order. Input tests cover native cursor ownership,
balanced visibility counters, hide/show loops and unchanged keyboard/mouse
isolation. Neither suite launches the game client. Full verification of the
reported in-game scene and pointer responsiveness still requires a client retest.

The corrected DLL is `bin/Autoload/novaclient.dll` (also in `bin/Release`). Close
the game completely, replace the deployed DLL, then reopen the prepared client.
The existing autoload executable and server hook do not require replacement.
The prior DLL is retained locally under `bin/Previous/BeforeRenderFix` for
comparison. Do not hot-unload/reload a pinned DLL in the running game.

Native findings and official API links are in
[evidence/rendering/REVIEW.md](evidence/rendering/REVIEW.md).
