# Custom event notices

Event summons and kills appear in the game's existing chat history using
**native green `#42C762`** (opaque ARGB `FF42C762`), with their own prefix:

```text
[Event]: Belzac was summoned in front of Ehres Harbor
[Event]: [Seijuro] killed Belzac. Congratulations.
```

The server uses the existing global notice broadcaster. The client recognizes
only the dedicated event marker, bypasses the localized `[Notice]:` formatter,
and dispatches the complete event text using native notice category 0x0D.
Ordinary GM notices and ordinary chat retain their original paths and colors.
No overlay window, new inbound GM command or client broadcast permission is added.

## Deployment and color changes

Deploy the server's `bin/CustomEvents/Hero4Hook.dll` through its existing startup
workflow, preserving `mapconfig.ini` and the runtime daily claims. Deploy this
client's `bin/Autoload/novaclient.dll` beside the existing prepared client EXE
while the client is closed. `bin/Release/novaclient.dll` contains the same DLL.
Restart MapTH and the clients. The autoload EXEs do not need rebuilding.

For this client-only color correction, replace/restart only the client DLL;
the previously deployed event server DLL remains compatible. Successful startup
adds `Event notices: native green #42C762 (stock palette 0x19).` to
`novaclient.log`, which can confirm the corrected DLL is loaded.

An older client still displays the marked packet as an ordinary blue notice
with its stock `[Notice]:` prefix. Both updated DLLs are needed for this style.

The Evasion-status screenshot supplied as a reference samples at approximately
`#6BAA7B`. The closest existing green in the chat palette is `#42C762` at index
0x19. Normal `[Notice]` uses blue `#4080FF` at index 0x22. This native green has
higher relative luminance than Notice blue. The exact appearance still depends
on the client's font/render settings and background.

`EventColorIndex` in `src/CustomEvents/EventNotice.cpp` now selects the existing
native green. `EventColor` validates its expected ARGB value; it does not write
a new RGB color into the game. A later change must select and validate another
existing palette entry, then rebuild/redeploy the client DLL. The server does
not need rebuilding for a client-only palette selection change.

## Wire contract

`shared/EventNoticeProtocol.h` mirrors the server's
`src/CustomEvents/NoticeProtocol.h`. A client-ready 0x1F0A body is:

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 1 | Mode 0, compatible with native system notices |
| 1 | 2 | Unsigned text byte length, 1..160 with a nonempty event payload |
| 3 | 6 | ASCII `ROHEVT` |
| 9 | 4 | Version/reserved bytes `01 00 00 00` |
| 13 | length | Complete text beginning `[Event]: ` |

Length must exactly consume the remaining reader body. Control characters and
embedded NULs are rejected. A recognized but malformed or unsupported-version
event is consumed without display. An unmarked notice receives the exact
original reader/cursor and original decoder return value. Source bytes are
never modified. This extension is emitted only by the server event adapter.

## Verified native path

Static Ghidra 12.1.3 analysis of supplied client SHA-256
`622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0`.
The executable was neither modified nor run. Exports under
`docs/evidence/event-notice/` use focused, no-auto-analysis imports and explicit
instruction ranges. Pseudocode is derived analysis; ABI facts below were checked
against instructions. These exports are ordinary Ghidra reports, not REA Evidence IDs.

| Purpose | Address / ABI | Observation |
| --- | --- | --- |
| Registered notice decoder | `0x00403EB0`, one cdecl reader argument, plain `RET` | Uses reader at `[EBP+8]`, returns zero in EAX. Ordinary mode 0 builds localized string 0x200 before display. |
| Raw reader layout | `0x00AA56C0` | Data pointer +4, total bytes +0x0C, cursor +0x10. Successful copies advance the cursor. |
| Normal notice formatter | Call `0x00403FCD` to `0x005E9F60` | Passes completed text and category 0x0D. Formatter selects color index 0x22. |
| Plain dispatcher | `0x005E9DE0`, `__thiscall`, `RET 0x18` | Six DWORD arguments: category, native text object, color index, zero, link pointer, count. Calls widgets through virtual +0x98. |
| Chat widget insertion | `0x005F12E0`, `RET 0x18` | Category 0x0D reaches normal notice insertion. With no links, invokes `0x004F6620`. |
| Native text copying | `0x004F4F30`, copy at `0x004F516E` through `0x00402540` | Stores color index at node +0x20/+0x24 and copies source string into a native-owned destination. |
| String layout | Pointer/inline storage +4, size +0x14, capacity +0x18 | Capacity >=16 selects the pointer. A bounded read-only view remains alive through synchronous dispatch; the hook never frees game string storage. |
| Rendered color | `0x004F3797..0x004F37B2` | Loads node +0x20, indexes the palette, and passes the resulting ARGB to native font drawing `0x00516E50`. |
| Palette | `0x00C2E930`, 66 entries | Existing notice index 0x22 is `FF4080FF`; existing green index 0x19 is `FF42C762`. |

The prior appended palette entry did not produce the intended live color, even
after increasing RGB brightness. It has been removed. The client now passes
the existing green index through the same dispatcher/widget path as native
Notice blue. No palette allocation, palette writes or renderer displacement
patches remain. Other native colors retain their original entries.

The native-emulation regression verifies that stock index 0x42 reads adjacent
string bytes `5B5C3A44`, rather than an opaque color, when renderer rebasing is
absent. This explains why an unresolved appended index can appear dark; it does
not establish exactly which runtime condition prevented the old extension from
working on the user's deployed client. The new implementation removes that
dependency entirely.

`event-notice-client.json` covers the decoder, dispatcher and reader;
`event-notice-renderer.json` covers native widget routing;
`event-notice-color-lookup.json` covers listbox insertion;
`event-notice-display-proof.json` records the exact string-copy and render
instructions. `event-notice-listbox.json` retains the listbox call chain.
`event-color-review.json` and `event-color-widget-full.json` retain the follow-up
renderer inspection and the widget's complete instruction range.

## Validation and remaining live check

Run `tests/test-events.cmd`. It links the actual server event encoder and client
decoder, checking summon/kill text, native dispatch arguments, native color selection,
all truncated payloads, invalid markers/lengths/controls, unchanged GM forwarding,
reader cursors and unavailable UI. An inert copy of the supplied client verifies
decoder/dispatcher fingerprints and the existing green/blue palette entries.

Run `python tests/event-colors.py` with the existing Unicorn environment. It
emulates the actual reference dispatcher `0x005E9DE0` and widget `0x005F12E0`
with synthetic objects and a fake listbox allocator. It checks color forwarding,
all insertion arguments, native stack cleanup and SEH restoration, then executes
the actual indexed renderer load to verify `FF42C762` versus `FF4080FF`.
No game process, network connection or GPU renderer is launched. These tests
cover native instructions beyond the C++ fake-dispatch tests; they do not render
the final game pixels.

The existing input, chat bridge and startup checks also pass. A live acceptance
test remains: summon each boss, kill it, inspect the prefix/color in chat history,
and verify an ordinary GM notice still appears normally. Static mapping and
offline tests do not establish the deployed client's visual result.
