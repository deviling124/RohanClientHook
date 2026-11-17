# Event Point Shop item icons

Target: `00_reference/rohanclient_unpacked.exe`, SHA-256
`622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0`.
All findings below are static observations from this exact x86 image. Live game
rendering has not been observed during this implementation.

REA Ghidra 12.1.3 recovered strings, RTTI and byte references but did not define
the needed code functions in its default analysis. `rea-discovery.json` retains
that evidence and its limitations. Existing `ExportGmNotice.java` was then run
with explicit function/instruction seeds in a disposable no-auto-analysis
Ghidra project. The resulting JSON files retain both assembly and decompilation.
Calling conventions and arguments below follow assembly; several decompiler
prototypes have incomplete inferred arguments.

## Verified path

| Evidence | Observation |
| --- | --- |
| `verified-ranges.json`, `0x008b5350` | The icon-loading routine iterates the item-template map belonging to `[0x00d3b07c]+4`. |
| `verified-ranges.json`, `0x008b5814` | It uses template `+4` as the key, accesses the icon map at manager `+0x14`, and stores an eight-byte resource handle. This catalog exists independently of a character owning the item. |
| `item-handle.json`, `0x008b6640` | Native inventory icon selection uses the same template key and map. Pet variants use additional maps; the shop uses the standard template icon. |
| `lookup-functions.json`, `0x00841310` | Map head is `map+4`; root is `head+4`; nodes have left/right at `0/8`, unsigned key at `0x0c`, handle at `0x10/0x14`, and sentinel byte at `0x19`. The native accessor inserts defaults for missing keys. Our reader does not insert and bounds the search to 64 nodes. |
| `icon-functions.json`, `0x005fbaf0`; `verified-ranges.json`, `0x005fbb88` | Native icon rendering passes an eight-byte handle to resource manager `[0x00d3d3c4]` using `0x00a08390`. |
| `texture-functions.json`, `0x00a08390` | Resolver is `void* __thiscall(void* manager, const uint32_t* handle)`, returning the native resource pointer. It uses its cache and may invoke `0x00a08130` to load a resource. |
| `texture-functions.json`, `0x00a3c910`; `item-handle.json`, `0x00a46e20` | Native UI uses image width/height at resource `+0x68/+0x6c` and requires a nonnull texture at `+0x70`. |
| `verified-ranges.json`, `0x00a4827d` | Native renderer passes resource `+0x70` to `ID3DXSprite::Draw` (vtable `+0x24`, texture, RECT, center, position, color). This is the DX9 texture used by ImGui. |

## Adapter behavior

`EventShopIcons::Draw(device, itemType, size)` runs only on the existing game
render/window thread inside `Dx9Frame`'s saved graphics-state scope. The normal
client startup compatibility checks remain required. Independently, the adapter
requires main image base `0x400000` and exact complete-instruction signatures at
the resolver entry, native item lookup, and native sprite draw. The standalone
preview fails these signatures and never calls a game function.

The resolver can load/cache a missing native resource. It must not be called from
a worker thread. This code owns no native item, manager, map entry or resource.
SEH contains invalid native reads/calls during unavailable state; missing keys,
invalid handles, absent resources and textures from another DX9 device produce
an explicit placeholder. It never replaces a missing item's icon with another
item's artwork.

Each selected texture is checked with `GetDevice` against the supplied rendering
device and `GetLevelDesc`. UV limits use native image dimensions divided by the
texture dimensions, allowing padded textures. A temporary COM reference keeps
the texture alive until `ReleaseFrame()` after ImGui rendering, on aborted
frames, or before reset/shutdown. Up to 128 references are held per frame; excess
icons fail to placeholders. There is no persistent texture cache to invalidate
on character changes or device reset.

Validation: `ItemIcons.cpp` compiles with MSVC v145 x86, C++17, `/W4`, with no
warnings. The three byte signatures were independently compared against the
reference PE. Full game validation still needs actual configured item IDs,
ordinary/padded icon assets, reconnects, zone transitions and device reset. The
standalone preview only establishes placeholder rendering, not game asset loading.
