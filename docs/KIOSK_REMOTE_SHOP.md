# Remote kiosk shopping from F search

Press **F**, search for a seller and use the result's existing locate action.
That action now opens the seller's normal shop window. Browse and buy from
any map served by the same MapTH process/channel. No character movement is
required. Online and managed offline kiosks use their existing purchase paths.
Separate channels/processes are outside this feature.

Install the paired server and client hooks together, then restart both.
The server setting is `[kiosk_search] remote_open=1` in `mapconfig.ini`.
The server can reject remote shopping with `remote_open=0` and the client
displays that status. Existing offline vending setup is still required for
offline shops. No new SQL migration is needed for this feature.

The client DLL is `bin/Autoload/novaclient.dll` (also `bin/Release`). Keep the
existing prepared autoload executable; the build refreshes its paired DLL and
manifest. The server output is the sibling project's
`bin/Release/Hero4Hook.dll` and `bin/Release/mapconfig.ini`.

An opened shop grants ten minutes of remote buying. Reopen it from F search
to refresh the session. The server still validates current price, listing,
item identity, inventory capacity, money, seller state and settlement. Closed
or sold listings can be rejected normally. A new selection replaces the
previous pending lookup; character changes invalidate client state.
Lookup failures and a 15-second lookup timeout appear in native chat.

## Native integration

- `005CC390` handles the five visible search-row actions. The selected seller
  comes from the native row string at `this+188+28*index`, using scroll control
  `24`'s `+78` first row and `this+130` result count. Native search/paging stays
  intact; only the locator action changes.
- The marked `1217` request uses the native 25-byte serializer `004207F0`.
  Both projects contain identical `shared/KioskSearchProtocol.h`. The server
  resolves the seller name to a character ID. This does not grant buying.
- The `004260A0` decoder hook only consumes replies marked `RKS1`; ordinary
  location replies keep the stock decoder and original reader position.
  Marked replies must be exactly 42 bytes, match the pending name and current
  connection/character, and arrive before timeout.
- The client sets the native shop's owner at `+1E4` and sends the standard
  `1D0C` request. Its existing response decoder opens the native window and
  builds its listings. The existing `1D09` buy UI, inventory/weight checks,
  replies and item rendering remain in use.
- Native `005B3390` contains only the seller-presence/distance auto-close.
  The hook skips it for the current remotely opened shop, bound to window,
  owner, connection and character, for ten minutes. Ordinary shops still use
  the original function. Native shop labels show the resolved seller and
  "Remote Kiosk" even when the seller has no nearby client entity.
- Lookup timeout notices are posted through the owning game window thread,
  rather than mutating native chat from the rendering callback.

Startup checks the native function fingerprints in addition to the existing
PE identity checks. Evidence is in
`docs/evidence/kiosk-search/native-client.json`: focused Ghidra exports for
reference client SHA-256
`622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0`.
These are direct static Ghidra observations. REA's initial default function
inventory did not define these routines; explicitly seeded functions in the
existing disposable analysis project supplied the positive evidence.

## Verification

Run `tests/test-kiosk-search.cmd` after building the Release client. It tests
the actual hook with mocked native dependencies: scrolled selection, wire
format, stale/malformed/unrelated replies, account/character changes, timeout,
scoped auto-close behavior, labels and reference fingerprints. Server tests
are `tests/run-kiosk-remote-tests.cmd` in the sibling project.

Live game verification remains required. With two characters on different
maps in one channel, check F search, open, buy and balances for online and
offline sellers. Also check a closed shop, a listing sold to another buyer,
insufficient money, full inventory, rapid seller changes, reopening after
ten minutes and normal nearby shopping. Build and mocked tests do not observe
an actual in-game purchase.
