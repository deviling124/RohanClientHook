# Advanced Hunter Kit client chat bridge

Inspected 2026-11-13 using a fresh focused Ghidra export in the existing
disposable `KioskPrice` project, with no auto-analysis. The client executable
was neither modified nor launched. This is static evidence, not a network or
live UI observation.

Reference `rohanclient_unpacked.exe` SHA-256:
`622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0`.

## Send bridge

`0x00407F50` has ECX as its connection/transport object and four 32-bit stack
arguments: opcode, body pointer, metadata value 1, metadata value 2. Its paths
end in `RET 0x10`; a suitable C++ function-pointer type is:

```cpp
using SendFn = int (__thiscall*)(void*, int, const void*, unsigned, unsigned);
```

The existing chat submission path loads ECX from `0x00D3AB54`. At
`0x005ECD5D..0x005ECD80`, it pushes values from globals `0x00D7B8EC` and
`0x00D7B8E8`, body pointer, then opcode before the call. Names/semantics of the
two metadata values are not established; preserve their actual values.
The Say branch reaches this common call site with opcode `0x1F03`.

Say encoder `0x00401300` reads signed 16-bit text length at body+14 and signed
16-bit link length at body+16 and copies exactly `21+textLength+linkLength`
bytes through `0x00AA55C0`. Text starts at body+21. This is the pre-serializer
body, not a four-byte wire-header packet. Capture the first 21 bytes of an
authentic plain `.ahk` Say submission; use that current context/header when
building bounded ASCII `.ahkui1` commands, changing text length and clearing
link length/reserved link metadata. Keep each command at most 160 text bytes.
Do not replay stale context across player/connection changes.

First 16 bytes at `0x00407F50`:
`83 EC 0C 56 8B F1 8A 86 6D 08 00 00 84 C0 74 09`.

## Receive bridge

The registered `0x1F04` decoder is `0x00402B80`. Its private/direct Shout body
matches the server hook's existing 43-byte client layout:

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 4 | sender ID |
| 4 | 21 | sender name |
| 25 | 1 | flag |
| 26 | 4 | metadata |
| 30 | 4 | metadata |
| 34 | 2 | metadata |
| 36 | 2 | text length |
| 38 | 2 | item-link length |
| 40 | 3 | link metadata |
| 43 | text length | message text |

The decoder builds separate native string objects for text and sender name,
then calls `0x005EA790` at `0x00402F0F`, returning to `0x00402F14`. It passes
mode 3. It does **not** directly call `0x005EADD0` on this path.

`0x005EA790` uses ECX as its chat object and 13 DWORD stack arguments. Its
cleanup is `RET 0x34`. Use 32-bit argument slots even where decompilation
suggests a char. In order, those arguments are:

1. mode (3 on the Shout path)
2. text string object pointer
3. sender-name string object pointer
4. sender ID
5. metadata DWORD from wire+26
6. metadata DWORD from wire+30
7. metadata word from wire+34 (passed in a DWORD slot)
8. zero on this path
9. boolean derived from wire+25
10. zero on this path
11. link-data pointer
12. decoded link-object pointer
13. link count

The two strings have this observed old MSVC representation:

- object+4: 16-byte inline buffer, or pointer to heap text
- object+0x14: 32-bit byte length
- object+0x18: 32-bit capacity
- capacity<16 chooses the inline buffer; otherwise dereference object+4

The hook should bound both lengths, read through guarded memory access, and
copy to its own buffer immediately rather than retain game string pointers.
Do not call the modern CRT string destructor on these game objects.

First 16 bytes at `0x005EA790`:
`6A FF 68 EB D3 BE 00 64 A1 00 00 00 00 50 64 89`.

## Sender-ID caveat

`0x00402D1B..0x00402D36` compares incoming sender ID with the local character
ID at `*(0x00D3FFC0)+8`. If equal, it replaces the sender-name string with the
local character's own name. Thus machine responses using the current
character ID do not reliably retain the `[AdvancedHK]` name.

Use sender ID zero for the private machine response and keep `[AdvancedHK]`
as its sender name. The proposed ingress match is: original return address
`0x00402F14`, mode 3, sender ID zero, exact sender name `[AdvancedHK]`, protocol
prefix `AHKUI1`, and the current request nonce. Whether zero is treated as a
blocked sender by any live configuration has not been tested. Do not use
sender name/nonce as server authorization: server-side identity and VIP
validation still apply to all saves.

## Evidence files

- `chat-bridge.json`: encoder, decoder, candidate display, send wrapper;
  includes complete selected-function instructions and pseudocode.
- `chat-ingress.json`: actual Shout display ingress and explicit decoder/send
  call-site instruction ranges. An explicit range begins in a prior
  instruction at `0x00402E60`, so its first two undecoded bytes are not
  evidence of a new instruction boundary; the relevant complete call block
  begins at `0x00402EC7`.

Decompiler types/stack reconstruction are imperfect in this import; argument
counts and string offsets above were checked against the actual instruction
sequences. Dynamic thread ownership, hooks/render integration and live
delivery remain unverified.
