# Character Information trace (diagnostic)

Goal: find where the stock Character Information window (`CharacterStatus.ui`, UI manager id 7) reads its stats and
equipment, so another player's data can be shown in it ("inspect player").

1. Copy `bin/Release/novaclient.dll` (also refreshed in `bin/Autoload`) next to the client as usual.
2. Create an empty file `statustrace.on` in the client folder (next to the exe).
3. Start the client and log in. Press **F10** once (capture ON). The Character Information window object is dumped.
4. Open Character Information (the normal key or button), wait a second. Switch the Battle, Costume, Production and
   Gathering buttons once each, and equip or unequip one item.
5. Press **F10** again (capture OFF; the window object is dumped a second time).
6. Close the client and send `novaclient_statustrace.log` from the same folder.

What the log holds:

- `ShowWindow` lines (always, while the file exists): caller address, window id and show flag.
- `SetText` lines (only between the two F10 presses): caller address, control id and text. The caller addresses
  point at the code that fills the labels; the texts show which stat each label is.
- Two dumps of the window object: dwords, with the first dword of every pointer, so vtables and child objects show.

Delete `statustrace.on` to turn it off; without that file nothing is hooked. Status: builds cleanly; not run in game.

## Run 1 result and v2

Run 1 showed F10 works (the log has the capture markers; the game gives no on-screen feedback), but window id 7 is the
HUD status bar (name, HP/MP, exp), not Character Information, and opening Character Information did not call
`ShowWindow` (`0x58C600`) at all. v2 therefore adds:

- `VISIBILITY id=... SHOWN/hidden` lines whenever any window changes visibility (polled from the manager list).
- On each F10: a list of every window (id, vtable, visible) and, for visible windows, their text controls with the text.

New steps: with `statustrace.on` present, open Character Information, press F10 (window list is dumped while it is open),
close it, and send `novaclient_statustrace.log`. The SHOWN/hidden lines show its id.

## Run 2 result and v3

Run 2: the window list walks the manager's list in a cycle (253 unique windows, then it repeats), the visible byte at
+0x38 never changed when Character Information was opened, and the Character Information labels never go through
`SetText`. So v3 finds the player's stat block by value: **F11** scans memory for the stat values of the character
in the reference screenshot (Strength 30013 with Vitality 30008 within 0x100 bytes) and logs hex around each match.
Use `statustrace_values.txt` (three numbers: Strength Vitality Intelligence) for another character.
Steps: log in as that character, press F11 once, close the client, send the log.
