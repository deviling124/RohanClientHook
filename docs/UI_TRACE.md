# UI layout trace (diagnostic)

Finds which code loads each native window's `.ui` layout. For the collection book reverse engineering
(see the server repo's `docs/COLLECTION_BOOK_RE.md`).

1. Close the client. Copy `bin/Release/novaclient.dll` (also refreshed in `bin/Autoload`) beside the prepared client
   executable as usual.
2. Create an empty file named `uitrace.on` in the client folder (the working directory, next to the exe).
3. Start the client, log in, and open a few windows one at a time: inventory, bank, character info, trade,
   skills, minimap, an NPC dialog. Note the order you opened them.
4. Close the client and send `novaclient_uitrace.log` from the same folder.

Each entry is a `.ui` file path followed by the call stack as raw addresses. Delete `uitrace.on` to turn it off;
without that file nothing is hooked or logged. Only files ending in `.ui` are recorded.

Status: builds cleanly; not yet run in the game.
