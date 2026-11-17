# Nova Client — Advanced Hunter Kit overlay

**Event Point Shop:** type `.epshop` to open the categorized shop with native item
icons, account balance, and purchase confirmation. The server database controls
the items and EP prices. Equipment has Split Sword with Melee Attack +10%, and
Materials has a stack of 250 Guild Stone. Each offer costs 10 EP.
Requires the paired server and SQL setup. See [Event Point Shop](docs/EVENT_POINT_SHOP.md).

**F-key kiosk search supports remote shopping across maps within the same
server instance/channel.** Use the result's existing locate action to open
the seller's native shop and buy. Requires the paired server update; see
[setup and verification](docs/KIOSK_REMOTE_SHOP.md).

Win32 client hook for the supplied unpacked Rohan client. The output DLL is
**`novaclient.dll`**. Typing **`.ahk` in normal Say chat** opens a new custom
Direct3D 9 overlay. It does not use the game's settings window or `.ui` assets.

The DLL also includes the native edit-text termination fix for kiosk prices
such as `1` appearing as `14`. It no longer depends on using the earlier separate
price-patched executable. See [the price fix](docs/PRICE_INPUT_FIX.md).

The overlay draws once at the primary device's final Present, with its own
backbuffer scene and explicit graphics-state restoration. It uses the Windows
hardware cursor while open. Device creation removes only `D3DCREATE_PUREDEVICE`
so the transform/state queries needed for preservation are supported. See
[the rendering regression fix](docs/RENDERING_FIX.md).

The overlay has eight item categories, thirteen optional stat minimums,
filter activation, Apply, Defaults, Refresh and Cancel. Any one selected stat
meeting its minimum qualifies an item. No selected stats skips that category
while filtering is active. Defaults restores the standard reference thresholds;
the server administrator's initial XML rules may differ.

## Ready-to-test files

Use these two files together in a **test copy of your normal game client
directory**, with its usual assets, dependencies and launcher arguments:

```text
bin/Autoload/rohanclient_unpacked.exe
bin/Autoload/novaclient.dll
```

Opening this prepared executable automatically loads its adjacent
`novaclient.dll` and initializes the hooks before the original client entry.
**No injector is needed.** The supplied `00_reference/rohanclient_unpacked.exe`
is preserved. If your normal launcher expects a different executable name,
give the prepared copy that filename; keep the DLL named `novaclient.dll`.

The DLL includes the NovaGuard / Rohan Online splash artwork. The 400 × 250
pixel splash shows “Initializing Game...” during startup and closes
automatically after the game presents its first frame.

The corresponding server change must also be installed. Its rebuilt DLL is in
the sibling `RohanMapServ_Hook/bin/CustomEvents/`.
Deploy it through the server's existing initialized/autoload setup with its
`mapconfig.ini` and `advancedhk.xml`, then restart the server. The overlay
requires the updated server protocol; it will report a timeout rather than
claiming settings are saved when that support is missing.

Settings are validated and saved per account on the **server**:

```text
AdvancedHunterKit/users/<account-id>.ini
```

No database is used for these preferences. The existing VIP entitlement source
is unchanged. Settings survive server restarts. Pickup uses one cached snapshot
of the account's activation and filters, and performs no settings-file I/O.
The server atomically replaces the account file before acknowledging Apply.
Manual pickup remains available.

## Build

World and Trade messages from the paired server hook appear in All Chat.
Their colors come from the Melee Attack and Ranged Attack Status labels.
See [World and Trade chat](docs/WORLD_TRADE_CHAT.md).

The client also supports dedicated server event announcements in the native
chat window, for example `[Event]: Belzac was summoned in front of Ehres Harbor`.
Their color is **`#42C762`**, including boss kill announcements. The matching
server and client DLLs are required; ordinary GM notices keep their existing
format. See [event notices and native mapping](docs/EVENT_NOTICES.md).

Open `RohanClientHook.sln` with the installed Visual Studio v145/Win32 toolchain,
or run:

```powershell
./tools/build.cmd
./tools/build-autoload.cmd
```

Build output: `bin/Release/novaclient.dll` and `bin/Release/AhkPreview.exe`.
The second command prepares the autoload executable and copies the DLL beside it.
It requires Node.js and the installed x86 Microsoft assembler. It refuses
in-place changes, existing output files and unrecognized client hashes.
For another fresh output directory:

```powershell
./tools/build-autoload.cmd "C:/path/to/supported/client.exe" "C:/path/to/fresh-output/rohanclient_unpacked.exe"
```

`tools/build.cmd` also refreshes this project's generated autoload DLL and
manifest when they already exist. For a deployed game directory, copy the new
`bin/Release/novaclient.dll` beside its prepared executable, with the client
closed. The startup executable does not need rebuilding when only
overlay/server-settings code changes.

`novaclient.log` is written beside the DLL. Startup failure presents a message
and stops the prepared executable. Unsupported builds install no native hooks.
The DLL stays loaded for process lifetime; live unloading is not supported.

## Preview and verification

`bin/Release/AhkPreview.exe` runs the actual overlay view in an independent DX9
test host. Its mock backend is in memory: it never connects to the game/server
or writes account preferences. F10 reopens its window. Visual checks covered
category selection, all stat controls, editing, Apply acknowledgement, Cancel,
reopening and Refresh. Screenshot: [docs/ahk-preview.png](docs/ahk-preview.png).

```powershell
./tests/test-model.cmd
./tests/test-bridge.cmd
./tests/test-input.cmd
./tests/test-price-input.cmd
./tests/test-render.cmd
./tests/test-roundtrip.cmd
./tests/test-events.cmd
python tests/event-colors.py
node --test tests/autoload.test.mjs
& 'C:/Users/anden/AppData/Local/Programs/Python/Python310/python.exe' tests/bootstrap.py
```

The rendering test uses a real hidden DX9 test device and the actual ImGui
renderer. It checks offscreen texture pixels, backbuffer output and complete
state restoration for both fixed-function and shader/custom-declaration modes.
It also checks nested host scenes and cleanup on scope exit. This driver permits
GetTransform on a pure device, so that specific query-failure scenario is marked
as unavailable rather than claimed tested.

The round-trip test compiles the real settings backend from the sibling
`RohanMapServ_Hook` project and checks actual folder persistence/reload and
account isolation through the real client model. The Python bootstrap tests
require Unicorn. These tests cover complete settings
snapshots, signed thresholds, nonce/stale-response handling, save failures,
account changes during a pending save, native ABI forwarding, input isolation,
startup rejection, and the actual generated x86 loader with fake Win32 APIs.
The loader tests verify register/flag/stack restoration and failure paths without
launching the game. The server has separate policy, native and disk-storage tests.

**Full in-game verification remains outstanding.** Check that `.ahk` opens,
mouse/keyboard interaction does not control the character, Apply changes actual
pet pickup, another account remains independent, settings survive relog/restart,
and fullscreen/device reset still render the overlay. The preview and isolated
tests do not establish those live results.

## Source and compatibility

- `src/AhkWindow.*`: custom overlay and request/response state.
- `src/Runtime.cpp`: early initialization, DX9 rendering/reset and input hooks.
- `src/PriceInput.*`: bounded, NUL-terminated native edit text for price parsing.
- `src/Dx9Frame.*`: checked state capture, backbuffer scene and restoration.
- `src/Bridge.*`: verified native Say/Shout bridge; game-thread sends only.
- `src/CustomEvents/EventNotice.*`: marked event notices using the existing native green chat color.
- `shared/EventNoticeProtocol.h`: versioned event notice marker and bounded codec, mirrored in the server.
- `shared/AhkProtocol.h`: bounded versioned settings format, mirrored in the server.
- `tools/client-bootstrap.asm`: entry shim that loads and initializes the DLL.
- `tools/patch-client-autoload.mjs`: offline, hash-gated PE preparation.
- `docs/evidence/`: client imports, native signatures and instruction evidence.

Supported reference SHA-256:
`622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0`.
The autoload preparer also recognizes the earlier isolated kiosk-price fix
`c5e9503a426d93e5a867e6b76eff5123fa3066c5647328bfbdc52d1282f0c191`.
Other builds require analysis of their own addresses/layouts. The existing
reference is x86 with fixed base `0x400000`. Rendering/window messages must
share the game window's thread; initialization must occur before DX9 creation.

Client evidence was first collected from the identical reference in the sibling
server workspace, so some historical evidence paths name that original location.
All new client source, dependencies, tools, tests and outputs live in this project.

Dependencies are vendored with licenses: Microsoft Detours and
[Dear ImGui v1.92.5](https://github.com/ocornut/imgui/tree/v1.92.5), using its
[Win32/DX9 backends](https://github.com/ocornut/imgui/tree/v1.92.5/examples/example_win32_directx9).
`tools/fetch-imgui.ps1` reproduces the pinned official-source download.
