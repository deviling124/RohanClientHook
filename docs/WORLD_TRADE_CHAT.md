# World and Trade chat

In normal Say, enter `.world <message>` for World chat or `.trade <message>`
for trading. Each message reaches online players connected to the same MapTH
process, including the sender. Both channels appear in All Chat on updated
clients. The line uses the channel label, the player's full name, and the
message. World gets the Melee Attack label color from Status. Trade gets the
Ranged Attack label color.

Deploy the server `bin/CustomEvents/Hero4Hook.dll` and client
`bin/Autoload/novaclient.dll` through the existing autoload setup. Restart
MapTH and reopen updated clients. Older clients display the compatible packet
as ordinary Shout. This protocol covers players on one MapTH process.

## Wire and routing

The existing 0x1F04 envelope carries the server-owned character ID at body +0,
the full 21-byte native name field at +4, the text length at +36 and text at
+43. The marker `RGCH` is at +26, version 1 at +30 and channel at +34
(World 1, Trade 2). Flags and link fields are zero. The decoder accepts the
marked message only with exact size and validated plain text. It forwards
unmarked Shout to the original decoder. Server log entries identify the
channel and honor the existing chat-log setting.

The original client may echo a Say command locally. The existing chat bridge
suppresses that echo at the verified local submission return address
`0x005ECE1E` only for the current player's World and Trade commands.

## Native mapping

The REA bundle records the reference client SHA-256
`622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0`.
Default analysis left some virtual functions undefined. Their fixed addresses,
fields and stack usage were checked against instruction bytes and tested
where practical with offline x86 emulation.

| Location | Behavior |
| --- | --- |
| `0x00585580` | Finds a window by its native ID in the UI manager at `0x00D4C5E0`. The chat manager has ID 2 and Status has ID 7. |
| `0x005F5820`, `0x005F4670` | The manager constructs its primary chat window with ID 0. The native lookup searches the list at manager +0xF0 for window +0x144 equal to zero. |
| `0x005EF010` | The chat constructor wraps the listbox. Window +0x104 holds the wrapper and wrapper +4 holds the actual listbox. The hook validates native types before insertion. |
| `0x004F6620` | Native listbox insertion takes seven stack arguments and returns with RET 0x1C. Calling it on the primary listbox keeps the row in All Chat. |
| `0x005261D0`, `0x005262AC`, `0x0056CB80` | Status loads `CharacterStatus.ui`. Label controls use indices 0x3E (adjacent to `CO_MELEEATTACK1/2`) and 0x41 (adjacent to `CO_LONGATTACK1/2`). |
| `0x0056AE30`, `0x004F8770` | Child lookup locates the loaded label. The text box renderer forwards +0x78 as its foreground ARGB. This is the label color and is independent of the attack value. |
| `0x004F4F30`, `0x004F3320` | Native insertion copies the style marker to +0xAC on each row, including split rows. The listbox renderer forwards that field and the native palette color to its font calls. |
| `0x00516D30`, `0x00516E50` | Verified chat font call sites substitute the loaded Status ARGB on marked rows and restore the normal drawing argument. The game palette stays in place. |

The fallback colors are native pale yellow `#FFFF99` and light blue
`#82C0FB`. They apply if the Status label is unavailable, transparent or black.
The actual Status RGB comes from the installed game's resource at runtime.
The reference workspace has the executable and does not establish the
resource's exact RGB values. See [REA evidence](evidence/world-trade-rea.json).

## Validation

Run `tests/test-global-chat.cmd` after building the client. The test links the
real server command handler and real client decoder with fake native callbacks.
It checks full names, sender validation, mute checks, per-channel formatting,
private usage, All-window selection, unchanged ordinary Shout forwarding,
and Status ARGB substitution on both font paths.

`tests/event-colors.py` uses the installed Unicorn environment to run the
native All-window lookup with other tabs present and exercise the native chat
renderer up to both font calls. Live verification should compare short and
wrapped messages with the two Status labels and check all tabs from two
online players in different maps. The offline checks do not render game pixels.
