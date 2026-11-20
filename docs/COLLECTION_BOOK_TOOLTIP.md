# Item tooltip on the Collection tab

Hovering one of the item boxes of the Collection tab (the items a rune needs) glows the box and shows the game's own item tooltip,
the one the inventory shows. Verified in game on the test client.

## How the game does it (reference client `rohanclient_unpacked.exe`)

| Address | Finding |
| --- | --- |
| `0x008BE040` | Item tooltip. `thiscall(ecx = *(0xD3B07C), item object, 0, 0, 0, 0, -1, 0)`, `RET 0x1C`. Hides window `0x57`, fills it and shows it at the cursor. The window keeps the pointer to the item object (`window+0x114`) and reads it later. |
| `0x008A6060` | Builds a temporary item object: `thiscall(this = buffer of at least 0xEC bytes, 0x44-byte record (first dword = item type), item definition)`. |
| `0x00B07F00` | Item definition by type: `thiscall(ecx = *(*(0xD3B07C) + 4), type)`. |
| `0x008A5EE0` | Destroys such an object. |
| window vtable slot `+0x48` | Per-frame draw of a window (the popup class: `0x004D3250`, draws its children). The inventory's override (`0x0053A620`) draws, then calls the tooltip function for the hovered slot **on every frame**; that is why the tooltip never blinks there. |

## What the client hook does (`src/UiTrace.cpp`)

* `UpdateHover` (once per frame, at Present) finds the item box under the cursor (`HoverRects`, keys 22 to 27) and stores the item type in
  `tooltipItem`; leaving a box counts after 150 ms (the hover test can drop out for a few frames).
* The book window's private vtable replaces its draw slot (`DetourWindowDraw`): original draw, then `ShowItemTooltip(tooltipItem)` every
  frame, exactly like the inventory. The item object lives in a static buffer because the tooltip window keeps pointing at it.
* When the cursor leaves, `HideItemTooltip` hides window `0x57` (`0x0058C600(manager, 0x57, 0)`).
* The glow uses the button hover images (`0x7050 + key`, `0x7070 + key`); `novaclient.ini` `[collection_book]` `item_glow=0` switches it off.

## What did not work

Showing the tooltip at Present (after the frame was drawn), once or every frame: the game hides the tooltip window again before it is drawn,
so it blinked or stayed invisible. The tooltip has to be refreshed inside the draw of the window that hovers the item.
