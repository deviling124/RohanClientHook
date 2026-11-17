#pragma once
// Server identity public keys the client trusts (ECDSA P-256, X || Y, big endian). These are
// PUBLIC values and are meant to be embedded at build time. Never put a private key here.
//
// PLACEHOLDER: no key is configured here, so the channel stays disabled whatever the ini says.
// Generate a server identity with tools\build-nova-keygen.cmd (RohanMapServ_Hook), keep the private
// file on the server only, and either paste the public key below or, for a test build, drop a
// generated `PinnedKeys.generated.h` next to this file (git-ignored; see tools\New-LiveTestStage.ps1
// in the docs). A generated header replaces the placeholder entirely.
#include "../../shared/NovaChannel.h"

#if __has_include("PinnedKeys.generated.h")
#include "PinnedKeys.generated.h"
#else
namespace ClientGuard {
inline constexpr std::size_t PinnedKeyCount = 0;
inline const NovaChannel::PublicKey PinnedKeys[1] = {};
} // namespace ClientGuard
#endif
