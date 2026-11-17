# Encrypted channel and client integrity: design

Status: **Slice 1 implemented (portable core + tests). Nothing is wired into the
game.** No hook, packet handling, config or game behavior changed. Built on
`TRANSPORT_MAPPING.md` (read it first) and the plan in
`RohanMapServ_Hook/docs/CLIENT_HANDSHAKE_PLAN.md`, which this design extends.

### Implementation status

| Piece | State |
| --- | --- |
| `shared/NovaChannel.h/.cpp`: handshake, AEAD framing, strict counters, ratchet, HKDF | Done, unit tested |
| `shared/NovaChannelCng.cpp`: Windows CNG crypto (ECDH/ECDSA P-256, AES-256-GCM, HMAC/HKDF/SHA-256) | Done, known-answer tested |
| `tests/NovaChannelTests.cpp` + `tests/test-nova-channel.cmd` | 1305 checks pass (incl. per-direction ratchet, in-flight frames, a run across the 65536-frame boundary); deliberate bugs in the AAD and the ratchet direction were caught |
| Client guard: `src/ClientGuard/` (`Engine`, detour layer, `PinnedKeys.h`), wired into `Runtime.cpp`, in the project | Done, **disabled by default**, tested with fakes (`tests/test-client-guard.cmd`, 248 checks). Not run against a real server yet. |
| Server guard in `RohanMapServ_Hook`: `src/ServerGuard/` (engine, detours on `0x418CE0` and `0x661850`), mirrored `shared/` core, `[client_guard]` config, key tool `tools/NovaChannelKeyGen.cpp`, `docs/ENCRYPTED_CHANNEL.md` | Done, **off by default**, 247 server tests + 331 interop tests (incl. a 65636-frame session across the re-key boundary) (`tests/test-channel-interop.cmd`, real client engine against real server engine). Not run against a live client. `enforce` is not available (Gate connections). |
| Live test staging: `tools/New-LiveTestStage.ps1` builds a test client DLL with a THROWAWAY public key baked in (via a temporary, git-ignored `PinnedKeys.generated.h`), plus the server DLL, a throwaway private key, ini snippets and a README, into `bin/LiveTest/<stamp>/`. Deploys nothing. | Staged once; the live run needs the server owner to deploy (the lab server is on another host/VM) |
| Real opcodes (`OpcodeClientHello` etc. are PLACEHOLDERS), attestation content, signed allowlist, `[client_guard]` config, audit/enforce modes | Not started |
| Identity key tool and key handling on the server host | Not started |

### First live run (2026-11-17, lab server, test client copy, throwaway key, both sides `audit`)

Result: **the channel works end to end; the game ran normally in the world.** Evidence:

- Client log: `client guard: mode=audit, hooks attached`; server console: guard hooks attached (after two
  fixes below).
- Read-only Frida trace of the client's `WSASend` (about 17 s in the world): **every** outbound frame is
  sealed. The 8-byte keepalive `0x8004` went from 12 bytes on the wire (4 + 8) to **28** (4 + 8 + 16 tag);
  `0x1308` (empty body) is now 20 bytes (4 + 16); `0x120f` 20 -> 24 body+tag; `0x1207` 36 -> 52, and so on
  (declared length = plaintext + 16). No plaintext-sized frame appeared. Two samples of the same frame
  type differ in every byte (different counter nonce), i.e. the bodies are not static.
- The game kept working, which also shows inbound opening works (a failed open would have dropped the
  connection).

Two defects the live run found, both fixed and pinned by tests (the unit tests could not see them):
1. `unsupported MapTH layout`: Protection (anti_crash) detours `0x418CE0` first, so a fingerprint taken later
   never matches. The fingerprint is now taken before any hook (`ServerGuard::Inspect`).
2. Startup stage FAILED: `KioskDbDrain` also hooks `SendPacketEx` (`0x661850`) and refuses to start when its
   original bytes are already patched. The guard is now the last hook of that stage.

Fallback run (same server still in `audit`, client `mode=disabled`): **passes.** Client log says
`client guard: disabled (no hooks installed)`, the game played normally, and the trace shows plaintext-sized
frames: keepalive `0x8004` is 12 bytes on the wire (4 + 8, no tag; its body bytes still differ per frame
because that opcode goes through the stock RC4). So a server in `audit` serves a non-participating client
exactly as before. (Only idle keepalives were observed in that short window.)

Second encrypted run (client `mode=audit` again, same session never reconnected, ~263 sends traced while the
tester walked, chatted, used the inventory and opened a kiosk): **every one of the 17 frame types seen was
sealed** (declared length = plaintext + 16, total = length + 4, no plaintext-sized frame): keepalive `0x8004`
(152, 28 bytes), movement-like `0x1211` (56), `0x1207`, `0x120f`, `0x150f`, chat `0x1f03/04/06/07/08`, kiosk
`0x1d01/03/05/07` (the kiosk open request `0x1d05` was 443 bytes, 427 + tag, and the server's kiosk translator
still worked on the decrypted body), `0x1e03`, `0x1414`, and the RC4-flagged `0x8002` (sealed on top of the
stock RC4, confirming the layering). The client was not disconnected.

Protocol version 2 run (both DLLs rebuilt with the per-direction ratchet, same throwaway key): the client log shows
`mode=audit, hooks attached`, the game played normally, and the trace shows the keepalive `0x8004` at 28 bytes
on the wire, i.e. the version 2 handshake works live. **The ratchet itself has not been exercised live:** at one
frame per second it first fires after about 18 hours, so a live check needs a build with a short interval.

A short-interval **test pair** can be built with `tools\New-LiveTestStage.ps1 -ExistingPrivateKey <key> -TestRatchetInterval 256`
(compiles both DLLs with `/DNOVA_TEST_RATCHET_INTERVAL=256` through the `CL` environment variable, into
`bin\LiveTest\<stamp>-ratchet256\`; the protocol version byte becomes `0xE2` in those builds so they never
negotiate with a normal build, and they never touch `bin\Release`). All the guard and interop suites also pass
when built with an interval of 16, which crosses the boundary many times through the real engines.

**Live re-key run (short-interval test pair, interval 256, ~9 minutes, one client process, same key):**
the connection **crossed the re-key boundary at least six times and stayed up** (the client sent 1569 traced
`WSASend` calls, 356 of them keepalives alone, so its sending counter passed 256 many times). Every observed
frame stayed sealed across all of them (declared length >= 16, tag included), the game kept working (walking,
chat, a kiosk, inventory), the process never reconnected, and no `channel:` line was reported. The client's
sending direction is therefore proven live; the server-to-client direction crossing is inferred (the game would
have disconnected on a failed open) but was not counted directly.

Not yet covered by a live run: long sessions and an actual map transfer, the other direction's wire view (only the client's
send side was observed on the wire), the Gate connection, and anything under load.

### Client guard, as built (slice 2)

- **Enabling:** off unless `novaclient.ini` beside the DLL has `[client_guard]` `mode=audit` or
  `mode=enforce` **and** a server key is pinned in `src/ClientGuard/PinnedKeys.h` (empty today, so
  it stays off). Disabled means no detours are installed at all.
- **Hooks (not `WriteFrame`/`ReadFrame` alone):** `OnRecvBytes` `0xaa8d70` (remembers the
  connection for the worker thread), `ReadFrame` `0xaa9b20` (decrypt inbound), `PushSend`
  `0xaa8b50` (seal outbound; it runs under the connection critical section and gives the
  connection pointer that `WriteFrame` does not). `SendLocked` `0xaa8bc0` is called, not hooked,
  to push our own frames. The four prologues are fingerprinted in `ClientGuard::MatchesReference`,
  which `Runtime.cpp` requires, like the other modules.
- **Negotiation (backward compatible):** a channel-capable server sets a 4-byte offer
  (`NCH` + version) at offset 44 of the stock `0x2001` key frame. The client sends a hello only
  after seeing it. A stock server never gets one (an unknown opcode would drop the connection).
- **Sequence:** client hello (plaintext) -> server hello (plaintext, last plaintext server frame;
  everything after is sealed) -> client sends a **sealed `ChannelReady`** and seals everything
  after it. Frames the client pushed before the ready frame stay plaintext, so the server must
  accept plaintext until it opens `ChannelReady`. Ordering is exact because the ready frame is
  sealed inside `PushSend` under the same lock as every other send.
- **Failure:** after the server hello, any rejected frame returns `-3` from `ReadFrame`, which
  makes the game drop the connection (the server is already sealing, so nothing could be read
  anyway). `audit` and `enforce` differ in one way: `enforce` also refuses a server that does not
  offer the channel (blocks a downgrade by stripping the offer).
- **Not done / known gaps:**
  - **Wire re-keying: done** (protocol version 2; automatic per-direction ratchet every 65536 frames,
    see "3. Re-keying"). Still missing: a fresh ECDH re-seed (post-compromise security) and any
    time-based trigger.
  - Attestation is empty (no EXE or `.text` hashes yet). The server side does not exist, so the
    path has only been exercised against a simulated server built on the same core.
  - The opcode placeholders and the `ClientGuard` `PushSend` result codes (2/3/4 = queued) come
    from disassembly and still need a live run.
  - The DLL builds to `bin\Release\novaclient.dll`; nothing was copied into `bin\Autoload`.

Library choice made for slice 1: **Windows CNG** (no new dependency). It uses P-256
(ECDH, ECDSA) and AES-256-GCM instead of X25519/Ed25519/ChaCha20; the Session API
does not expose the primitives, so libsodium can replace `NovaChannelCng.cpp` later.
Run the tests with `tests\test-nova-channel.cmd` (needs Visual Studio 18, as the
other test scripts do).

## Goals

1. Encrypt and authenticate game traffic with a **per-session key that is never
   sent on the wire** and is **re-keyed during the session** (Gepard-style).
2. Make the server able to check the client's **EXE file** and **code section in
   memory**, bound to that session, so a modified client does not get a working
   session by default.
3. Roll out safely: audit first, enforce later, with a clear failure behavior.

## Non-goals and honest limits

- Not tamper-proof. The player controls their PC. A rewritten client that
  implements the protocol, or a hook placed before encryption, still works.
  The key sits in the client's memory.
- This stops: passive sniffing, packet editing and replay on the wire,
  off-the-shelf bots and trainers that patch the stock client, running without
  the client DLL.
- This does **not** replace server-side validation of movement, combat, items
  and currency. Treat all client reports as signals.
- GameGuard (`npkcrypt.dll`) is out of scope.

## What exists today (summary)

Per connection, MapTH sends a random string in clear (opcode `0x2001`) and uses
`MD5(string)` as an RC4 key. Only high-bit opcodes (Gate/login/RC) are RC4'd.
Normal gameplay opcodes are not encrypted. See `TRANSPORT_MAPPING.md`. The new
layer **replaces** this; it does not stack beside it.

## Threat model

| Threat | In scope | Mitigation |
| --- | --- | --- |
| Passive sniffer | yes | Ephemeral key exchange; key never on the wire |
| Active man in the middle | yes | Server signs its handshake; client pins the server identity key |
| Packet editing / injection | yes | AEAD tag over header and body |
| Replay / reordering | yes | Strict per-direction counter as nonce |
| Stock client patched in memory | partly | Code-section challenge, server-chosen ranges |
| EXE or DLL swapped on disk | partly | File hash attested in the handshake, checked against signed allowlist |
| Client without the DLL | yes | No handshake, no gameplay (enforce mode) |
| Re-implemented client | **no** | Documented limitation; server-side validation |
| Hook before encryption | **no** | Same |

## Cryptographic choices

Use a vetted library, not a custom cipher.

- **Recommended: libsodium, statically linked, x86** on both ends. It gives
  X25519, Ed25519, ChaCha20-Poly1305 (or AES-GCM where AES-NI exists), and
  HKDF-style KDF helpers, with no OS-version dependency. Ed25519 matches what
  the patcher already uses.
- Alternative with no new dependency: Windows CNG (ECDH P-256, ECDSA P-256,
  AES-256-GCM). Needs a check of the minimum client and server OS.
- Randomness from the library or the OS (never `rand`, `timeGetTime`).
- **Server identity key**: a new Ed25519 key pair, **separate from the release
  signing key**. The private key lives only on the server host. The public key
  is embedded in the client DLL (public, not a secret) and can be distributed
  with the launcher config. Never put the private key in the client, the
  repo or `Config`. Rotating it needs a client update, so embed two (current
  and next).

## Protocol

New messages use a new opcode range. The opcode must be chosen only after
checking both dispatchers; do not reuse `0x2001`.

### 1. Handshake (once per connection, before any gameplay)

```
C -> S  ClientHello  { version, cli_eph_pub[32], cli_nonce[32], attestation }
S -> C  ServerHello  { version, srv_eph_pub[32], srv_nonce[32],
                       sig = Ed25519(srv_identity_priv, transcript_hash) }
```

- `transcript_hash = H(version || cli_eph_pub || cli_nonce || srv_eph_pub ||
  srv_nonce || attestation || connection-id)`. The client verifies `sig` with
  the pinned public key and aborts on failure.
- `shared = X25519(eph_priv, peer_eph_pub)`.
- `okm = HKDF-SHA256(salt = transcript_hash, ikm = shared,
  info = "nova-channel-v1")` split into `k_c2s`, `k_s2c` (32 bytes each) and
  `nonce_prefix_c2s`, `nonce_prefix_s2c` (4 bytes each).
- `attestation` is part of the transcript, so it is bound to the keys (see
  Integrity below).
- Hello messages travel in clear (they contain only public values). Nothing
  secret is sent.

### 2. Packet protection (after the handshake)

Frame stays `u16 opcode, u16 length, body`, so the stock framing and size
limits keep working.

- `length` counts the ciphertext **including a 16-byte tag**. Max body shrinks
  by 16 (stock raw limit is `0x4038` per the anti-crash guard).
- AEAD: ChaCha20-Poly1305 (or AES-256-GCM). Key = `k_dir`.
- Nonce (12 bytes) = `nonce_prefix_dir (4)` || `counter (8, little endian)`.
  The counter is per direction, starts at 0, increments per packet.
- AAD = `opcode || length || counter`. The header is authenticated even though
  it is visible.
- TCP delivers in order, so the receiver expects exactly `counter`. Any tag
  failure, counter mismatch or oversize **closes the connection** (not "drop
  one packet"), with rate-limited logging. No silent resync.
- All opcodes are encrypted once the channel is up, including the high-bit ones.
  The stock RC4 for those is bypassed (see Integration).

### 3. Re-keying

**Implemented (protocol version 2): automatic per-direction ratchet, no extra frame.**
- Each direction re-keys **itself** after exactly `RatchetEveryMessages` = 65536 frames. The sender
  ratchets its sending key right after sealing frame number 65535 of the epoch, and the receiver ratchets
  its receiving key right after opening that same frame. Counters are strict and TCP is ordered, so both
  ends cross the boundary at the same frame; no `Rekey` message exists and frames in flight are never
  ambiguous (the earlier "re-key both directions at once" idea would have broken them).
- `key' || nonce_prefix' = HKDF(ikm = key || nonce_prefix, info = label || direction || epoch')`, old key
  erased, counter restarts at 0. The direction is named absolutely (client to server or server to client),
  so both ends derive the same value. A frame sealed under an old epoch fails to open once the receiver
  moved on; an interval mismatch between the ends is caught at the first boundary.
- **What it gives:** forward secrecy for earlier epochs. **What it does not give:** post-compromise security.
  A key stolen now still derives every later epoch (the chain is one-way, not re-seeded).
- **Not built (future):** a fresh signed ECDH exchange inside the channel to re-seed (needs an explicit,
  sealed rekey message plus a per-direction switch-over rule) and a time-based trigger. At typical traffic
  (about one frame a second idle, more when active) 65536 frames is hours of play, so the ratchet fires
  rarely; the exchange matters more for very long sessions than for typical ones.
- The version byte is part of the stock-key-frame offer, so a version 1 client never matches a version 2
  server's offer and keeps playing plaintext (safe fallback). Update both ends together.

### 4. Version and failure behavior

- Version field in both hellos; unsupported version gets a clean "update the
  client" message (no keys, no nonces in logs).
- Handshake deadline (default 15 s from when the hello can be delivered), taken
  from a monotonic clock, not the client's clock. Missing or failed handshake:
  audit mode logs; enforce mode disconnects through the verified native path.
- No automatic account ban from a timeout.

## Integrity: EXE file and code section

These ride on the same channel, so they are authenticated and bound to the
session.

### File integrity (EXE and DLLs)

- **Client side:** hash the on-disk `rohanclient*.exe` and `novaclient.dll`
  (SHA-256). The launcher can already do this before Play; the client DLL repeats
  it and reports it in `attestation`.
- **Server side:** compare against a **signed allowlist** (below). Unknown hash:
  audit logs, enforce rejects.
- Weakness (state it): a patched client reports the stock hash. The check mostly
  stops accidents, old versions and lazy swaps. The server-chosen challenge below
  is what raises the bar.

### Code-section integrity in memory

- **Challenge:** server sends `Challenge{nonce, [(offset, len) x K]}` with
  server-chosen, random ranges of `.text` (K small, ranges a few KB). Client
  replies `HMAC-SHA256(k_c2s, nonce || SHA256(range bytes...))`. The server
  recomputes from its own copy of the expected bytes and compares in constant
  time. Fresh nonce and new ranges each time, so a stored reply is useless.
- The server needs the expected `.text`. Source: the release tooling, from the
  same patched EXE the patcher ships.
- **Hooks and relocations:** the client DLL patches code (detours). Either the
  server's expected image includes those patches (hash the patched build), or
  the challenge skips ranges listed in the release's patch-site mask. Decide per
  release; the mask is part of the signed allowlist.
- **Packed or protected production EXE (`rohanclient.exe`):** unknown. Hashing a
  packed section means something different; settle this before implementing
  (open item).
- Cadence: one challenge at handshake, then every 20-60 s with jitter. Late or
  wrong answer: audit logs, enforce disconnects after a grace window. Do not
  disconnect on one late reply.
- Limit (state it): an emulator can keep a pristine copy of `.text` and hash
  that, while running a modified one. The dynamic ranges and the key binding
  prevent replay and simple relays; they do not prove the running code.

### Signed allowlist (links to this repo)

- Extend the patcher's release build (`tools/New-NovaCoreRelease.ps1`, using
  `RohanPatchCli`) to emit a small **signed `integrity` document** per release:
  EXE and DLL SHA-256, `.text` range layout, expected `.text` bytes or their
  block hashes, and the patch-site mask. Sign it with the existing **release
  signing key**; the **server verifies** with the release public key.
- The server reloads it when a new release is published. The private key never
  goes near the server or the client (project rule).
- Keep the **server identity key** (transport) and the **release signing key**
  (content) separate.

## Integration points (verify each before coding)

| Where | Hook | Status |
| --- | --- | --- |
| Server receive | `0x00418ce0` (vtable slot `+0x28`): the single entry for every framed packet, ahead of both dispatchers. The anti-crash guard already detours it; extend that detour. | Confirmed |
| Server send | `SendPacketEx` `0x00661850` (vtable slot `+0x2c`, ~500 direct callers). `0x0066a250` has one caller and barely matters. | Confirmed |
| Stock RC4 | **No bypass needed.** The AEAD layer sits outside it on both ends (send: stock RC4 then AEAD; receive: AEAD then stock RC4), so the stock RC4 stays in sync and nothing is decrypted twice. It can be removed later as a separate step. | Resolved by layering |
| Server key setup | Replace `0x00457750` / `0x0066a1b0` (stock `0x2001` + MD5/RC4) | Confirmed |
| Client send | `0x00407f50` / `0x00407810` (serializer + outbound RC4) | Confirmed |
| Client receive | **`0x00aa9b20` `ReadFrame(ring, out)`**: IOCP worker thread, returns a delimited frame (`out[0]` header, `out[1]` body). After it succeeds, decrypt the body in place and rewrite the length to plaintext. Only called from the receive path. | **Confirmed by disassembly and a runtime trace** (arguments and results as predicted; max frame `8192`, so plaintext max `8176`) |
| Client send | **`0x00aa9a20` `WriteFrame(ring, in)`**: serialized by `SendLocked`'s critical section, only called from `PushSend`; max body 4095 (`length & 0xfff`), so 4079 plaintext. Counter order equals wire order. | **Confirmed by disassembly; runtime trace still to do** |
| Client key handler | The `0x2001` handler and RC4 state at `0xd3be9c` | **Not confirmed** |
| Existing protections | `Protection/*`, kiosk, chat | Run on plaintext, after decrypt, unchanged |

The AEAD layer is the **outermost** layer on the wire, outside the stock RC4. The
stock RC4 therefore keeps running unchanged for flagged opcodes, and does not need
a bypass. Connections without a channel (audit mode, unsupported clients) pass
through the hooks untouched and keep the stock behavior. Handshake messages must
pass through the hooks before the channel is up.

New modules (names as in the plan): server `src/ClientGuard/`, client
`src/ClientGuard/`, plus a shared wire header mirrored in both repos. Handshake
state must not depend on the AHK/event UI. Follow the plan's lifecycle rules:
new connection means new state, no borrowed pointers across threads, no locks
held while sending or disconnecting.

## Modes

```ini
[client_guard]
mode=audit            ; disabled | audit | enforce
handshake_timeout_s=15
challenge_interval_s=40
rekey_ratchet_s=300
rekey_full_s=1800
```

Placeholders, not existing settings. `audit` runs everything, logs decisions,
and never blocks. Switch to `enforce` only after audit logs on real sessions look
clean.

## Rollout

1. Phase 1 mapping: finish the open items below.
2. Portable protocol and state machine, unit-tested with a controlled clock and
   fake native calls. Test vectors for the KDF and AEAD framing; fuzz the frame
   parser and hello messages.
3. Integrate in audit mode on both hooks (handshake, channel, challenge).
4. Live tests: login, relog, map transfer, idle, disconnect, server restart,
   latency and load. Check the channel against the stock RC4 path on high-bit
   opcodes.
5. Signed allowlist from the release pipeline; server loads and verifies it.
6. Enforce mode, announced, with the client update available first.

## Acceptance checklist

- A valid client completes the handshake and plays with no UI or commands.
- Sniffing the whole session reveals no key material; a captured session cannot
  be decrypted without the client's ephemeral private key.
- Any modified, replayed, reordered or truncated packet closes the connection.
- A wrong or missing server signature makes the client refuse to continue.
- Client without the DLL, or with an unsupported version: refused in enforce,
  logged in audit.
- Stale challenge replies and old counters never extend or revive a session.
- Re-key happens on schedule with no dropped packets; old keys are erased.
- Relog, transfer and pointer reuse never inherit state.
- Existing chat, kiosk, AHK, events and item protection still work.
- Live tests establish the real behavior; offline tests are not claimed to
  prove it. An emulated client remains a documented limitation.

## Open items (must be settled before coding)

1. ~~Client `0x2001` handler and key setup~~ **Confirmed at runtime** (see
   `TRANSPORT_MAPPING.md`): client `MD5` then `RC4_set_key` at `0x40853a` /
   `0x40854b`, inbound RC4 at `0x406bf7`, one shared RC4 state `0xd3be9c` for
   both directions, normal gameplay traffic not RC4'd. Still open: the handler
   function's own address and the point where inbound frames enter below the
   RC4 call, to place the new decrypt hook.
2. ~~Server common receive point~~ **Found:** `0x00418ce0`. Still to verify:
   that no path sends without `SendPacketEx`, and the meaning of `conn+0x570`.
3. A free opcode range, confirmed against both dispatchers.
4. Whether the client connects to MapTH directly or only through the Gate, and
   whether the Gate needs the same channel.
5. Packed or unpacked production client, and its `.text` layout.
6. Library choice (libsodium vs CNG) and the minimum Windows version of servers
   and clients.
7. Handling of the stock RC4 on connections that fall back (audit mode).
8. How the server host stores and protects the identity private key.
