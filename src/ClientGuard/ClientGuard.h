#pragma once
#include <windows.h>

// Detour layer for the encrypted channel (client side). Addresses were mapped in
// docs/TRANSPORT_MAPPING.md and are for the reference build only.
namespace ClientGuard {
// True when the build's bytes at the four hook sites match the reference client. Inspect only.
bool MatchesReference(const unsigned char* base) noexcept;
// Called inside the caller's Detours transaction. Installs nothing (and returns NO_ERROR)
// unless novaclient.ini has [client_guard] mode=audit|enforce and a server key is pinned.
LONG Attach();
// One short line describing what Attach decided, for the DLL log.
const char* StatusText();
}
