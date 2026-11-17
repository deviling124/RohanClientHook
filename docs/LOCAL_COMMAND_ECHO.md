# Local `.ahk` echo handling

The existing static `client-send.json` export of reference function
`0x005EC040` establishes a conditional local chat-display path after the native
send call. It calls the same thirteen-argument ingress `0x005EA790` at
`0x005ECE19`, returning to `0x005ECE1E`.

Relevant instructions:

```text
005ECDF4  MOV EDX,dword ptr [0x00D3FFC0]
005ECDFF  MOV ECX,dword ptr [EBP+0x10]
005ECE0B  MOV EAX,dword ptr [EDX+0x8]
005ECE0E  MOV EDX,dword ptr [EBP+0x8]
005ECE11  PUSH EAX   ; argument 4: local sender character ID
005ECE12  PUSH ECX   ; argument 3: sender-name string
005ECE17  PUSH ESI   ; argument 2: input text string
005ECE18  PUSH EDX   ; argument 1: submission mode, zero for Say
005ECE19  CALL 0x005EA790
005ECE1E  ...
```

Earlier `0x005EC38F` sets ESI from `[EBP+0x0C]`, the submitted text object.
The native send return value is not a condition for bypassing this local echo.
The branch is additionally controlled by an existing client configuration
lookup at `0x005ECD8C..0x005ECDAB`; its live value was not observed.

The bridge therefore suppresses a local echo only when the original ingress
return address is `0x005ECE1E`, mode is exactly zero, sender equals the
captured local character ID, that captured connection/character still matches
the current session, and bounded native text is exactly the same `.ahk`
open command accepted by the send hook. Other players, other modes/callers,
other command text and unsynchronized/stale sessions pass through unchanged.

This is static evidence and an isolated fake-original regression test. It is
not a claim that a live game UI session was executed.
