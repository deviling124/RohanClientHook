# Handoff: encrypted channel (client + server)

Written at the end of a long session so a new one can start cold. Read this, then
`ENCRYPTED_CHANNEL_DESIGN.md` (design, status table, live results) and `TRANSPORT_MAPPING.md` (evidence).

## Goal
Gepard-style protection for NovaROHAN: encrypted, authenticated game traffic with dynamic keys, plus a way
to require our own client (`novaclient.dll`). Not full anti-cheat: a modified or re-implemented client still
gets in; the server must keep validating gameplay.

## Repos (siblings under `C:\Users\anden\OneDrive\Documents\`)
- `RohanClient_Hook` (not git): client hook DLL `novaclient.dll`. Channel code in `src/ClientGuard/`.
- `RohanMapServ_Hook` (git, branch main, **nothing committed by me**): server hook `Hero4Hook.dll` for MapTH.exe.
  Channel code in `src/ServerGuard/`. Operator doc: `docs/ENCRYPTED_CHANNEL.md`.
- `RohanClient_Patcher`: the launcher/patcher (this session's original project; rules in its `CLAUDE.md`).
- Shared core, identical copies in both hook repos: `shared/NovaChannel.h/.cpp`, `NovaChannelCng.cpp`.

## What exists (all off by default)
- **Core:** ECDH P-256 handshake signed by the server's ECDSA identity key (pinned in the client), AES-256-GCM
  frames, strict per-direction counters, automatic per-direction ratchet every 65536 frames. Windows CNG only.
  **Protocol version 2.** AEAD sits OUTSIDE the stock RC4 (no RC4 bypass needed).
- **Negotiation:** a capable server sets a 4-byte offer (`NCH`+version) at offset 44 of the stock `0x2001` key frame.
  Client sends hello only after seeing it. Sequence: hello (plain) -> server hello (plain, last plain server frame)
  -> client sealed `ChannelReady` (opcode 0x3F04) -> everything sealed. Opcodes 0x3F01/02/04 are PLACEHOLDERS.
- **Client hooks:** `OnRecvBytes 0xaa8d70`, `ReadFrame 0xaa9b20`, `PushSend 0xaa8b50` (+ `SendLocked 0xaa8bc0` called).
  Enabled by `novaclient.ini` `[client_guard] mode=audit|enforce` AND a pinned key in `PinnedKeys.h` (empty = off).
- **Server hooks:** `SendPacketEx 0x00661850`, receive entry `0x00418CE0`. Enabled by `mapconfig.ini`
  `[client_guard] mode=audit` + `identity_file=` (private key). `enforce` is accepted but behaves as audit.
- **Tools:** `RohanMapServ_Hook/tools/build-nova-keygen.cmd` (makes the identity key, owner-only ACL),
  `RohanClient_Hook/tools/New-LiveTestStage.ps1` (stages test DLLs + throwaway key; `-ExistingPrivateKey`,
  `-TestRatchetInterval N`), `tools/Start-TestClient.cmd` (launches the TEST client copy).

## Tests (run from each repo root; need Visual Studio 18 x86)
- Client repo: `tests\test-nova-channel.cmd` (1305), `test-client-guard.cmd` (248), `test-channel-interop.cmd` (331;
  real client engine vs real server engine, uses the sibling server repo).
- Server repo: `tests\run-server-guard-tests.cmd` (247, run from repo root; reads ./mapconfig.ini).
- Three older server tests (`run-startup-tests`, `run-raw-patch-tests`, `run-bootstrap-tests`) fail identically on
  an unmodified HEAD; unrelated.

## Live results (lab server 172.24.27.44, test client copy, throwaway key)
- Works end to end; game normal; every client frame sealed (traced via Frida on `WSASend`). Fallback with the
  client `disabled` works (plaintext). Short-interval ratchet test crossed 6+ boundaries in 9 min without a drop.
- Two bugs found only live, both fixed and pinned by tests: (1) fingerprint must be taken BEFORE Protection patches
  `0x418CE0` (`ServerGuard::Inspect`); (2) guard must be installed LAST in `InstallPacketDispatchHook` because
  `KioskDbDrain` also hooks `SendPacketEx` and refuses patched bytes. `tests/ServerGuardTests.cpp` pins the order.

## Current state of the test setup (verify, do not assume)
- Test client copy: `E:\00_Adrian\ROHANPS\NovaROHAN-Launcher-Test\client` has the NORMAL v2 `novaclient.dll`,
  ini `mode=audit`; backups beside it (`*.bak-*`, `*.v1-bak-*`, `*.v2-normal-bak-*`, `*.ratchet-test-*`).
  The running client may still hold the old DLL.
- **The lab server may still run the short-interval TEST `Hero4Hook.dll`** (version byte 0xE2). The normal one is in
  `RohanClient_Hook\bin\LiveTest\20261117-084325\server\`. The user must swap it and restart MapTH.
- `bin\LiveTest\*` holds a THROWAWAY private key; delete when done. Original `NovaROHAN` folder was never modified.

## Open items / next steps
1. **Enforce mode: DONE and live-tested on the lab (2026-11-17).** Engine refuses a connection that was
   offered the channel and ignores it (first frame != hello), and one sending >8 plaintext frames after the
   hello. Audit logs `would refuse in enforce`. 265 server + 331 interop tests. Live: hook client fine
   (maps, reconnect, kiosk, chat, buffs); plain client refused at first opcode 0xE001; no other would-refuse
   lines (Gate not affected). Server repo changes are still UNCOMMITTED. Still open: the Gate's real nature
   (source suggests a separate inter-server link with CONNSTATUS_GATE; status offset unknown), a soak test.
2. **Client packaging:** production `rohanclient.exe` is ASPack-packed (3.8 MB); the hook only works on the unpacked
   build (11.5 MB). Decision pending: ship the unpacked, autoload-patched client as `rohanclient.exe` via the
   patcher (recommended) vs hooking after unpack.
3. **Real key and pinning:** generate the production identity key on the server host, pin its public half in
   `PinnedKeys.h`, never commit the private file.
4. Attestation (EXE/`.text` hashes) + signed allowlist from `tools/New-NovaCoreRelease.ps1`; placeholder opcodes
   to verify; fresh ECDH re-seed (post-compromise security); Gate/other-connection handling; map-transfer test.

## Rules to keep
Never print or commit a private key. Never modify `E:\00_Adrian\ROHANPS\NovaROHAN` (original client). Ask before
deploying anything, restarting servers, or running `New-NovaLocalTrial.ps1`. Verify, then report what is unverified.
