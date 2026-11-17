# Item links in chat show the native tooltip

Status: **fixed on the server; no client change needed.** Verified live on the
test client for Say/Shout, Party, Guild and Whisper (2026-11-18). Server commit
`2782a1f` in RohanMapServ_Hook.

Item links appear in chat as bracketed text, for example `[Sprint Scroll(Superior)]`.
Clicking one now opens the game's own item tooltip (title, Required Level,
description, Sale price, Cool Down). An Alt+Click client hook was planned but
turned out to be unnecessary.

## Root cause

The client sends a chat link as text plus a link blob, with three link-entry
type bytes in the packet header. The client's receive parser (`0x00401C90`)
counts the non-zero type bytes as link entries. The server hook dropped those
bytes, so every received link decoded to zero entries and no tooltip was possible.

| Channel | Client sends types at | Client reads types at | Server fix |
| --- | --- | --- | --- |
| Say, Shout, Party, Guild | packet +22..24 | packet +40..42 | `ChatCommon.cpp` |
| Whisper | packet +43..45 | packet +65..67 | `Whisper.cpp` |

The server's native handlers never read these bytes and their relay structs have
no slot for them. The fix remembers the three bytes per sender id when the
packet arrives and writes them back on the outbound broadcast, only when the
message carries a link (link length above zero). The table is keyed by sender id
so it does not depend on which thread runs the relay.

A link with one item has a 66-byte blob: 5 header bytes plus a 0x3D-byte item
record. The parser copies each record into a 0x44-byte native item slot.

## Native pieces found (reference client SHA-256 622e1068...b814c0)

| Location | Finding |
| --- | --- |
| `0x00402B80`, `0x00401C90` | Shout/Say decoder and the link parser. Type byte 1 = item record. |
| `0x008BE040` | Native item tooltip. `__thiscall`, `ecx = *(0xD3B07C)`, seven stack args (`RET 0x1C`): item record pointer then six values, zeros for inventory hover (for example the call at `0x0053A776`). Hides window 0x57 with `0x58C600(ctx, 0x57, 0)`, shows it at the cursor, and builds the lines. |
| `0x008AD870`, `0x008B1280` | Tooltip line builders (Required Level, Cool Down). |
| `0x00AAA0A0` | Text-table lookup by id (`data/text.bin`). Ids: Required Level 0x1F45, Cool Down time 0x1F4B, Sale price 0x16E46D. |
| `0x005CBAD0`, global `0xD4DBA8` | Generic `CUIToolTipBox`. Its callers are minimap, skill and card-game text, not items. |

## How it was found

Static disassembly located the link parser. A Frida trace on the running test
client showed which code builds the tooltip text and captured a real received
Say packet, which had a link length of 0x42 and all three type bytes zero.

## If tooltips ever stop working

Check that the server DLL includes the link-type fix, then capture a received
packet and confirm bytes +40..42 (Say) or +65..67 (Whisper) are non-zero. To
restrict the tooltip to Alt+Click only, hook `WM_LBUTTONDOWN` in
`src/Runtime.cpp` and suppress the click unless Alt is held; that is not
implemented.
