# Collection Book from an NPC dialog

Talking to **Obivos** (npc id 328057) shows an extra line, "I wish to open the Collection Book.", next to his normal lines.
Clicking it opens the Collection Book window. This is the only way to open it: the `.cbook` command and F9 were removed.

## How the client builds the menu (rohanclient_unpacked.exe)
- The dialog lines are not in any data file. `0x5b1290` builds them in code: a run of `cmp [esi+0xd8], <npc job>` blocks, each
  calling `AddMenuItem(menu, commandId, text)` (`0x5b1160`, thiscall). Job 0x35 (53, `<Nova Points Merchant>`) adds
  "I wish to use the Nova Points Mall." (text.bin id 11787, command 0x91).
- A click runs `0x5acff0` (called from the dialog window's click handler, vtable slot 3 `0x5b1020`): it reads the id stored for the
  selected line (vector at window+0xf4) at `0x5ad093` and jumps through a table for ids 2..0x10A. Any other id ends in `0x5b0b2f`.
  (`0x8d7e80` / the `cmp ebx, 0x91` chain is the quick-slot handler, not the dialog.)

## What `NpcMenu.cpp` does
Three Detours hooks inside those functions: remember the NPC id (`0x5b12f2`), add our line when the id matches (`0x5b2c19`), and
swallow our command id 0x1C5A on click and open the book (`0x5ad093`).

## Config (novaclient.ini, `[collection_book]`)
- `npc=328057` NPC id (0 turns the feature off). `npc_text=` changes the line. `npc_log=1` writes the id of every NPC dialog that
  is opened to `novaclient_npc.log`, to check the id.

## Through npc.bin
Any NPC whose `npc.bin` job is `npc_job` (default 80, unused by the stock npc.xls) gets the line, like the Nova Mall does for job 53.
`npc=` (an explicit id, default Obivos) keeps working next to it; set `npc=0` to rely on the job only. The job also goes to the
server through its own npc.bin, so use a value the server does not treat specially for that NPC.

## Session
The book talks to the server with `.cbk1` Say commands, which need the 21-byte Say header the client puts in front of every chat
message. `Bridge::RequestBook` builds it itself on the click (character id, the two pure lookups `0x4be240` / `0x4be320` of the chat
type word at `0xd419f8`, zeros), so nothing has to be typed first. A real Say later replaces it with the genuine header.
