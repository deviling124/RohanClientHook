# Direct3D 9 overlay state regression — static investigation

Inspected 2026-11-13. No game process was launched, attached to, or changed.
The investigated reference is `rohanclient_unpacked.exe`, SHA-256
`622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0`.

## Confirmed facts

The reference contains a path that requests `D3DCREATE_PUREDEVICE` together
with hardware vertex processing. This establishes a compatibility hazard in
the previous overlay; it does not establish the flags used in the user's
particular live session.

1. `Direct3DCreate9` is imported through IAT address `0x00C25720`, with thunk
   `0x00BAF9E8`. Function `0x00A82690` calls that thunk at `0x00A826B1` and
   stores the resulting `IDirect3D9` pointer through `0x00A81100`.
2. `0x00A81140` returns that stored interface pointer. Device creation in
   `0x00A83EF0` retrieves it at `0x00A84239`, loads behavior flags from its
   copied settings structure at offset +0x0C, and calls vtable slot 16
   (`[EDX+0x40]`) at `0x00A84264` with the seven COM stack arguments. This is
   `IDirect3D9::CreateDevice`, rather than the device interface's slot-16
   `Reset` method.
3. `0x00A84880` has a conditional path at `0x00A84ABD` that writes `0x50`
   into those behavior flags. That combines PUREDEVICE `0x10` and
   HARDWARE_VERTEXPROCESSING `0x40`. Alternative branches write `0x40` or
   SOFTWARE_VERTEXPROCESSING `0x20`.
4. `0x00A82250` contains another corresponding flag override: at
   `0x00A82323..0x00A82329`, it clears software vertex processing and ORs
   `0x50` into settings+0x0C. Both paths use the boolean getter
   `0x00A81E60`, which reads singleton field +0x1DA. The singleton constructor
   `0x00A821A0` initially zeroes its fields; this investigation did not find
   the caller/configuration that enables that flag in a live run.

Focused Ghidra output, including source hash, pseudocode and instructions, is
in `client-device-creation.json`. It was exported in the existing disposable
analysis project with no auto-analysis and explicit function entries. Types
inferred by the decompiler are imperfect; the COM argument order, offsets
and branch constants above were checked against instructions.

## Overlay incompatibility

The vendored `vendor/imgui/backends/imgui_impl_dx9.cpp` declares
`D3DMATRIX last_world, last_view, last_projection` without initialization,
calls three `GetTransform` operations without checking their results, then
later calls `SetTransform` using all three matrices. The backend's own
comment explains why it also saves these transforms outside its state block.

Microsoft documents that [GetTransform does not return state on a pure
device](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-gettransform).
Its [D3DCREATE documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcreate)
and [DirectX FAQ](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/directx-9-frequently-asked-questions)
also explain the pure-device state-query restriction. Therefore a pure device
cannot supply the matrices that this backend tries to restore. Passing
uninitialized matrices to the game device is a concrete possible state
corruption mechanism, consistent with a failure first occurring when the
overlay renders. It is not a runtime diagnosis of the reported world textures.

## Recommended guards

- Remove only PUREDEVICE from the flags passed to the original CreateDevice;
  retain the requested vertex-processing mode and all other behavior flags.
- Check every state query used by an outer frame guard. If complete capture
  fails, skip overlay drawing instead of restoring unknown values.
- Save before the entire overlay frame, including backend NewFrame/resource
  setup, and restore after it on all exits. Preserve render targets/depth,
  viewport/scissor, transforms, vertex declaration/FVF, shaders, streams,
  indices and texture/sampler/render state as appropriate.
- Drawing at Present can avoid drawing into an intermediate scene target,
  but changing the hook position alone does not make failed state queries safe.
- Treat state-block Apply failure as a rendering failure rather than assuming
  the game state was restored. The vendored backend currently ignores that
  return value too.

The existing backend does capture a `D3DSBT_ALL` block and calls Capture, so
the finding is not that it omits state backup entirely. On a compatible
non-pure device with successful operations, this review did not establish
another particular leaked sampler/texture state. The root changes and an
isolated fake-device regression suite should verify explicit state isolation;
full game rendering still needs a live test after rebuilding and restarting
the client.
