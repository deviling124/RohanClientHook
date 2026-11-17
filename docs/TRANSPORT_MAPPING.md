# Transport mapping (phase 1, in progress)

Status: **Partial, read-only.** No code, packet handling or configuration changed.
Supports `RohanMapServ_Hook/docs/CLIENT_HANDSHAKE_PLAN.md` (phase 1) and the
encrypted-channel / integrity ideas built on it.

Evidence is from REA/Ghidra on `00_reference/rohanclient_unpacked.exe`
(sha256 `622e1068...b814c0`) and import tables of that client and
`00_reference/MapTH.exe` (sha256 `a78726cf...11e4`). Addresses are image VAs for
those exact files. Decompiler output is analysis, not source.

## Confirmed

| Fact | Evidence |
| --- | --- |
| Server uses IOCP networking | MapTH imports `CreateIoCompletionPort`, `GetQueuedCompletionStatus`, `PostQueuedCompletionStatus`, WS2_32 `WSASend`/`WSARecv` (most WS2_32 imports are by ordinal). |
| Client uses overlapped and plain sockets | Imports `WSASend`, `WSARecv`, `send`, `recv`, `connect`, `WSASocketA`, `ioctlsocket`, `WSAWaitForMultipleEvents`. |
| Both binaries link OpenSSL `libeay32` and use the same four calls | Client: `RC4`, `RC4_set_key`, `EVP_Digest`, `EVP_md5`, `MD5_*`. Server: ordinals 323/3165/479/477 resolve (against `00_reference/Logserver/libeay32.dll`, a 2011 build) to `EVP_md5`, `EVP_Digest`, `RC4_set_key`, `RC4`. Same primitive set, so the server very likely implements the mirror of the client's RC4 scheme. The key is probably derived with MD5. The DLL match to MapTH's own build is assumed, not proven. |
| **The client already applies RC4 to some outbound packets** | `FUN_00407810` is called only from `FUN_00407f50` (the client's send path, the function `Bridge.cpp` hooks as `sendOriginal`). It serializes the packet, and if bit 15 of the opcode is set and a connection field at `+0x1c` is not 8, it calls `RC4(&DAT_00d3be9c, len, buf, buf)` in place (RC4 call at `0x40789a`), then hands the buffer to the connection's virtual send. The only reference to the RC4 state is at `0x407895`. *Correction:* an earlier version of these notes said "inbound"; the caller shows it is outbound. |
| Client has a GameGuard component | Imports `npkcrypt.dll` (`NPKCloseDriver`). Out of scope for the patcher, per project rules. |
| Server dispatch entry points | `DetourProcessPacket` hooks `0x00419230` (`__thiscall(this, packet)`), `SendPacketEx` is `0x00661850`. High-bit opcodes go through `0x00418db0` (`HighBitOpcodeDispatch`). Per `Targets.h`. |
| Client send/ingress hooks already exist | `RohanClient_Hook/src/Bridge.cpp` hooks the client's send at `0x407f50` (`__thiscall(conn, opcode, body, a, b)`) and chat ingress at `0x5ea790`. Both sit **above** the socket layer, so they see plaintext opcodes and bodies. |
| Current connection globals (client) | Active connection `*(0xd3ab54)`, player `*(0xd3ffc0)`. Per `Bridge.cpp`. |

## Server side of the existing scheme (MapTH.exe, confirmed by decompile)

| Function (VA) | Role |
| --- | --- |
| `0x00457750` | Per-connection key setup. Builds a fresh string with `CoCreateGuid` (`{GUID}` text; falls back to `timeGetTime` if that fails), copies 0x28 bytes, **sends it to the client in clear as opcode `0x2001` (0xAC byte body)**, then calls `0x0066a1b0` with it. |
| `0x0066a1b0` | `key = MD5(string)` via `EVP_Digest`/`EVP_md5`, then `RC4_set_key(conn+0xDC, 16, key)` and sets `conn+0x4E4 = 1`. |
| `0x00418db0` | High-bit opcode dispatcher (already in `Targets.h`). **First statement decrypts the body in place:** `RC4(conn+0xDC, hdr.len, body, body)`. Applies to every packet that reaches it. |
| `0x0066a250` | Encrypted send helper `(opcode, body, len)`: RC4 into a stack buffer (max 0x2000), then `SendPacketEx` (`0x00661850`). Used for high-bit replies. |

Frame layout seen in the dispatcher: `u16 opcode`, `u16 length`, then body
(`param_1[0]`, `param_1[1]`, body at `+4`). The RC4 state is per connection.

## Server choke points (confirmed by decompile and vtable read)

Connection class vtable (data at `0x79cfbc`, slot n at `0x79cfbc + 4n`):

| Slot / offset | Address | Role |
| --- | --- | --- |
| 10 / `+0x28` | `0x00418ce0` | **Single receive entry for every framed client packet.** `__thiscall(conn, packet)`. Copies `len+4` bytes (header included) into a stack buffer, logs if `len > 0x400`, then, unless `conn+0x570 != 0` (drops), routes: opcode bit 15 clear to `0x00419230`, set to `0x00418db0`. Only one reference: this vtable slot. The anti-crash guard (`Protection/AntiCrash.cpp`) already detours this address. |
| 11 / `+0x2c` | `0x00661850` | **`SendPacketEx(conn, opcode, data, size)`**, the common send. About 500 direct call sites plus this vtable slot. The stock `0x8001`-family replies go through it (via `(*vtable+0x2c)`), **unencrypted**. |

`0x0066a250` (RC4 encrypt then `SendPacketEx`) has a single caller (`0x66a2ef`),
so the stock RC4 on the send side covers very little. The stock RC4 is mostly
the inbound decrypt at the top of `0x00418db0` (`RC4(conn+0xDC, ...)` through
the thunk `0x00632690`).

Consequences for the new channel:

- **Receive hook:** detour `0x00418ce0`. It sees every packet before either
  dispatcher, carries the framed header, and already has a working detour in this
  project (extend it, do not add a second one). Decrypt into a local buffer,
  rewrite `len` to the plaintext length, then call the original.
- **Send hook:** detour `0x00661850`. Encrypt the body, then call the original
  with the ciphertext and `size + 16`. It covers direct calls and the vtable
  slot.
- **Stock RC4:** `0x00418db0` still RC4-decrypts high-bit packets. For
  channel connections, make the thunk `0x00632690` a copy (`in` to `out`) instead
  of RC4 when its state pointer is `conn+0xDC` of a channel connection, so it is
  not decrypted twice. Do the same on the client with its RC4 thunk.
- **To verify before coding:** that no other code path sends to the socket
  without `SendPacketEx` (check `WSASend` call sites), and what `conn+0x570`
  means (probably a "drop" or closing flag).

## Client inbound path (partially mapped)

Evidence: static code bytes and a second runtime trace (same test client).

| Fact | Evidence |
| --- | --- |
| The inbound RC4 call at `0x406bf7` is inside a large function that starts at **`0x406940`**. | Code bytes at `0x406940` show an SEH prologue (`push -1; push handler; mov eax,fs:[0]; ...`) and a `0x2160`-byte stack frame; no earlier prologue fits. Ghidra did not create a function there. |
| Inbound decrypt condition is opcode bit **`0x4000`** (not `0x8000`). | Bytes at `0x406bd7`: `test esi,0x4000; jz; cmp [edi+0x1c],8; jz;` then `RC4(0xd3be9c, len, buf, buf)`. The outbound path at `0x407810` tests bit 15 instead. The two directions use different flag bits. A later test of `0x3300` also exists. |
| It runs on the **main thread**. | The same thread id for all traced inbound RC4 calls, which sits among d3d9/MSHTML frames. |
| No static callers. | `xrefs(0x406940)` is empty. It is probably reached indirectly (vtable or message dispatch), like the server's `0x418ce0`. |
| The runtime backtraces were **not usable** past the first frame. | Frida's fuzzy backtrace on this 32-bit code without frame pointers returned unrelated frames. |

**Runtime argument trace of `0x406940` (result: not a good hook).**
Called every tick from two sites (`0x407085`, `0x407092`) for two connection
objects, with arguments that are not packet buffers (first arg `3`, then
unrelated pointers). It is a per-tick message routine; the RC4 call is on a
**local stack buffer** (`[esp+0x16c]`, length at `[esp+0x1e]`) that is not
visible at function entry. Do **not** use `0x406940` for the channel.

**Client socket layer (runtime, ws2_32 trace while idle in the world):**

| Finding | Evidence |
| --- | --- |
| Game traffic uses **overlapped `WSASend`/`WSARecv`**, not `send`/`recv`. | Only `WSASend` and `WSARecv` calls were seen; plain `send`/`recv` never fired. |
| **One call site each**: `WSASend` from `0x00aa8ae4`, `WSARecv` from `0x00aa8979`. | Every call came from these two return addresses. They sit in the socket class next to the serializer helpers (`0xaa6070`, `0xaa6090`, `0xaa6110`). |
| One game frame per `WSASend`. | Each send was 12 bytes: `04 80 08 00` + 8 bytes (opcode `0x8004`, length 8, body). Confirms the wire frame `u16 opcode, u16 len, body`. |
| Posted receives complete elsewhere. | `WSARecv` returns pending; completion is handled via events (`WSAWaitForMultipleEvents`/`WSAGetOverlappedResult` are imported). |

**Recommended client hook points (to verify before coding):**

- **Send:** keep the existing `0x00407f50` send wrapper (body plus opcode),
  or the single `WSASend` site at `0x00aa8ae4` if the channel is implemented as a
  transport layer.
- **Receive:** hook the **overlapped-receive completion** (where the received
  bytes are handed to the framer), reached from the `WSARecv` setup around
  `0x00aa8979`. Not yet located. Decrypting there, before the game's framer,
  sidesteps the stack-local buffer problem in `0x406940`. The channel must
  re-frame: the server's frames are 16 bytes longer, so the client hook has to
  decrypt each frame and hand the game the original-length frame.

## Client receive chain (confirmed by disassembly; Ghidra has no functions here)

Class: `gmTCPClient` (strings `mf__IOCPRecv`, `mf__IOCPSend`, `IOCPWorkerThread`).
Everything below runs on the **IOCP worker thread**, serially per connection.
Addresses are file VAs for `rohanclient_unpacked.exe` (sha256 `622e1068...b814c0`).
The functions are not recognized by Ghidra, so use `read_bytes` plus a
disassembler (capstone) to re-check them. Caller scans confirm the call graph.

| Address | Role |
| --- | --- |
| `0x00aa75c0` | Connect: `WSASocketA`, `connect`, posts the first receive, creates the worker thread (`0xaa75b0` then `0xaa7340`). |
| `0x00aa7340` | Worker loop: `WSAWaitForMultipleEvents`, `WSAGetOverlappedResult`; on a finished receive calls `0xaa8020(conn, bytes)`; on a finished send calls `0xaa7a60`. Closing handling at `0xaa7d20`. |
| `0x00aa8020` | **Receive completion** `this=gmTCPClient`, args `(conn, bytes)`, `ret 8`. Takes a critical section, calls `0xaa8d70`, then re-posts the next `WSARecv` via `0x00aa88f0`. |
| `0x00aa88f0` | `IOCPRecv`: posts the overlapped `WSARecv` (the single call site `0xaa8979`). |
| `0x00aa8d70` | **Bytes arrived**: advances the receive ring buffer's write position and count, then loops `ReadFrame` + `EnqueueFrame` until no complete frame remains. Sets the event for the main thread when any frame was queued. |
| **`0x00aa9b20`** | **`ReadFrame(ring, out)`** `__thiscall`, `ret 4`. Ring fields: `+8` mask, `+0xc` read pos, `+0x14` capacity, `+0x18` bytes available, `+0x1c` data. Returns `-1` fewer than 4 bytes, `-2` frame incomplete, `-3` length over the maximum in `out+2`, `0` success. On success it writes the header dword (`u16 opcode`, `u16 length`) to `out[0]`, copies exactly `length` body bytes to the destination pointer `out[1]`, and advances the ring by `4 + length`. Only called from `0xaa8d70` (two sites). |
| `0x00aa9c50` | `EnqueueFrame(queue, ...)`: hands the decoded frame to the main-thread queue. |
| `0x00406940` | Main-thread side: takes a queued message into a stack copy and RC4-decrypts it if `opcode & 0x4000` (see above). |

**Best client receive hook: detour `0x00aa9b20` (`ReadFrame`).** After it
returns `0`, the framed packet is at `out[0]` (header) and `out[1]` (body),
already delimited, on the worker thread, before the stock RC4 on the main
thread and before the frame is queued. The first 5 bytes (`sub esp,8` +
`mov eax,ecx`) are whole instructions, so a standard detour works.

Hook plan: call the original; on success and an active channel, AEAD-decrypt
the body in place (ciphertext is 16 bytes longer than plaintext), then rewrite
the header length to the plaintext length so everything downstream (`EnqueueFrame`,
`0x406940`) sees a normal frame. Edge case: the oversize check inside `ReadFrame`
uses the ciphertext length, so a frame whose plaintext is within 16 bytes of the
maximum would be rejected; keep encrypted frames at least 16 bytes under the cap
or raise the cap field.

**Runtime confirmation of `ReadFrame` (test client, idle in the world, read-only
Frida trace).** Every call came from `0xaa8db9` (the first `ReadFrame` call in
`0xaa8d70`) on one worker thread. Results alternated `0` (frame) and `-1` (no
more complete frames), exactly as the loop in `0xaa8d70` predicts. For each
success: `out[0]` held the opcode and the **plaintext** length, `out+2` held `8192`
before the call (the per-frame maximum, `0x2000`) and was overwritten with the
length, and the body was in a single fixed destination buffer (`out[1]`, same
address every call). Frames seen were opcode `0x1a44`, length 2, plain bytes
(not encrypted), repeated about once a second, which confirms normal gameplay
traffic is not RC4'd. So the maximum plaintext body for the new channel is
`8192 - 16 = 8176` bytes.

Still to verify: that the same ring holds data for the login/Gate connection and
the map connection (only one ring was seen in this trace, idle in the world) (two `gmTCPClient` slots appeared in the earlier trace), the
client's send-side counterpart (the single `WSASend` site `0xaa8ae4` and its frame
writer), and the effect of the odd debug checks in `0xaa8020` (it inspects ring
bytes for values `3` and `8`).

## Client send chain (confirmed by disassembly and a caller scan)

All addresses file VAs for `rohanclient_unpacked.exe`; no Ghidra functions, so
re-check with `read_bytes` plus capstone.

| Address | Role |
| --- | --- |
| `0x00407810` | Outbound serializer (known): builds the frame, RC4-encrypts the body if opcode bit 15 is set, then calls `SendLocked`. Call site `0x4078ce`, the **only** caller of `SendLocked`. |
| `0x00aa8bc0` | `SendLocked(conn, in)` `ret 4`: takes the connection critical section (`owner+0x30`), calls `PushSend`, releases. |
| `0x00aa8b50` | `PushSend(conn, in)` `ret 4`: if connected (`[conn+0x30]==2`) calls `WriteFrame`; returns `1` if the ring is full; if no send is in flight (`[conn+0x20]==0`) calls `IOCPSend`. |
| **`0x00aa9a20`** | **`WriteFrame(ring, in)`** `__thiscall`, `ret 4`. `in` = `{u16 opcode; u16 length; const void* body}` (the mirror of `ReadFrame`'s `out`). Copies the 4 header bytes, then `length & 0xfff` body bytes from `in+4`, into the send ring (`+0x10` write pos, `+0x14` capacity, `+0x18` byte count, `+8` mask, `+0x1c` data). Returns `-3` if the frame does not fit, else `0`. Only called from `PushSend`. |
| `0x00aa8a70` | `IOCPSend`: `BuildWSABUF` (`0xaa89e0`) over the ring contents, then the single `WSASend` (`0xaa8ae4`). |

Observations: the length is masked with `0xfff` when copying, so **client to
server frames carry at most 4095 body bytes**; with a 16-byte tag, 4079 plaintext.
The critical section in `SendLocked` serializes every `WriteFrame`, so a per-direction
send counter incremented inside a `WriteFrame` detour stays in ring (wire) order.
The first 5 bytes of `WriteFrame` (`push ecx; mov eax,ecx; xor ecx,ecx`) are whole
instructions, so a standard detour works.

**Best client send hook: detour `0x00aa9a20` (`WriteFrame`).** Build a second
`in` struct with the encrypted body (length plus 16) in a local buffer, call the
original with it. Not yet runtime-verified (do a short trace like the `ReadFrame`
one before coding).

**Layering consequence (simplifies the design):** AEAD wraps *outside* the stock
RC4 on both ends. Send path: plaintext, then stock RC4 if flagged (unchanged),
then AEAD at `WriteFrame`/`SendPacketEx`. Receive path: AEAD decrypt at
`ReadFrame`/`0x418ce0`, then the game's own stock RC4 decrypt runs as before. So
the stock RC4 does **not** have to be bypassed, and nothing is decrypted twice.

## What this means for security

- The "dynamic key" already exists in a weak form: a new random per-connection
  string. But the string is sent to the client **in plaintext** (opcode
  `0x2001`), and the key is just `MD5(string)`. A passive sniffer who sees the
  start of the connection can derive the RC4 key. So this layer hides nothing
  from a network observer. It only obscures traffic from someone who captured
  mid-stream.
- RC4 here has no integrity check and no replay protection.
- The key is not bound to anything about the client (no HWID, no hash).
- Normal gameplay opcodes (0x1xxx-0x2xxx) go through the *other* dispatcher
  (`0x00419230`) and are, as far as this mapping shows, **not** encrypted.

## Runtime confirmation (client, lab server)

Observed by attaching a read-only Frida trace to `rohanclient_unpacked.exe`
(test copy, lab server) from the login screen through entering the world. The
trace only logged calls to `libeay32` exports; nothing was modified.
Values below are for one session; the session string and key are not recorded
here.

| Finding | Evidence |
| --- | --- |
| **Client key setup confirmed.** | `EVP_Digest` is called from `0x40853a` with a 36-character GUID string (no braces). `RC4_set_key` follows from `0x40854b`: 16-byte key, into the RC4 state at `0xd3be9c`. The key **equals `MD5(string)`** (checked offline). So the client mirrors the server's `MD5` then `RC4_set_key`. |
| **Same RC4 state for both directions.** | Outbound RC4 caller `0x40789f` (inside `FUN_00407810`) and inbound RC4 caller `0x406bf7` (the candidate site at `0x406bee`) both use state `0xd3be9c`, in place. The server also uses a single state per connection (`conn+0xDC`) for both decrypt (`0x00418db0`) and send (`0x0066a250`). This only stays in sync while the encrypt/decrypt operations happen in the same order on both ends; it is a fragile property of the stock scheme. |
| **Inbound decrypt is selective.** | Over the whole login-to-world window the inbound RC4 count stopped at 22 while outbound kept rising. Normal in-world traffic is therefore **not** RC4'd; only the flagged (high-bit) traffic is. Lengths seen inbound include 1352 and 1043 bytes. |
| **Periodic 8-byte keepalive outbound.** | Repeated 8-byte RC4'd body (`84 32 00 00...`), roughly once a second in the world. Matches the server's `0x8004 MAP_ALIVE_CALL` (exact size 8). |
| **Dispatch path** | The 0x2001 key packet itself is not RC4'd (plain), as expected. |

Not observed: the raw bytes of the `0x2001` packet, the Gate/login path
separately from the map connection, and any traffic after a long idle period.

## Interpretation (not yet proven)

- RC4 on high-bit opcodes only matches the server's split between normal
  opcodes and high-bit opcodes (Gate/login/RC traffic). So ordinary gameplay
  packets (0x1xxx-0x2xxx) are probably **not** encrypted by this scheme at all.
  Not yet verified for the inbound direction or on the server.
- RC4 (broken as a modern cipher, no authentication, no replay protection) with
  a state at a fixed address suggests a session-wide or build-wide key set up
  elsewhere. Where the key comes from is unknown.

## Not yet established

1. Where `RC4_set_key` is called and what the key material is (static, per
   login, per session). The state at `0xd3be9c` has only one direct reference,
   so key setup probably goes through a different base or a pointer.
2. Inbound (server to client) direction, and the server-side call sites of
   `RC4`/`RC4_set_key`/`EVP_Digest` in MapTH.exe (ordinals now resolved, xrefs
   not done yet).
3. The raw frame format: length field width, opcode position, max size
   (`PacketGuard`'s `0x4038` limit is a hint), and where the frame is read from
   `WSARecv` buffers.
4. Whether the client talks to MapTH directly or only through the Gate, and
   whether encryption terminates at the Gate.
5. Which receive/send function is the best single hook point on each side
   (below the framing, so the encryption layer can wrap every packet,
   including high-bit ones).
6. Production `rohanclient.exe`: packed or not, and its code-section layout for
   a runtime integrity check.

## Consequences for the design

- An existing RC4 layer means any new "dynamic key" scheme must **replace or
  wrap** it deliberately, not sit beside it by accident. Do not apply a second
  cipher to packets the stock code already decrypts.
- Hook below the framing layer on both ends (or at the exact RC4 call site, once
  its mirror on the server is found) so one change covers all opcodes.
- Do not claim security from RC4 here. New work should use an AEAD (AES-GCM or
  ChaCha20-Poly1305) with an ephemeral key exchange, per the earlier design.
- A client-side check remains a signal, not proof. Server-side validation is
  still required.

## Next steps

0. *(Resolved by the runtime check above; kept for the evidence trail.)* Client static search: the unpacked image keeps packer
   sections (`.aspack`, `.adata`, `.mackt`) and its libeay32 call thunks sit at
   `0xa97dxx`-`0xa97e2a` in `SECTION.0`; only the `RC4` thunk (`0xa97e06`) is a
   recognized procedure, so name search and xrefs on the other OpenSSL imports
   find nothing. A byte search for the RC4 state address `0xd3be9c` finds four
   sites: `0x407896` (the outbound RC4, known) and three in code Ghidra did not
   turn into functions: `0x406bee`, `0x408542`, `0x409201`. These are the best
   candidates for the `RC4_set_key` call and the inbound decrypt. They need
   function creation (or a disassembly read) before the `0x2001` handler can be
   confirmed. The client key setup is therefore **still unconfirmed**.
1. (Done on the server.) Confirm on the client: find the handler of opcode
   `0x2001`, which should receive the string and do `MD5` then `RC4_set_key`
   into the state near `0xd3be9c`. The earlier name/xref searches missed it
   because the state is likely set through a pointer.
2. Check whether the inbound (server to client) high-bit path also uses RC4.
3. Check the Gate/login path: does the Gate use the same `0x2001` exchange?
3. Decompile the client's frame-read path that calls `FUN_00407810` and the
   server's `WSARecv` completion handler to document the frame layout.
4. Update `CLIENT_HANDSHAKE_PLAN.md` with the verified mapping.
