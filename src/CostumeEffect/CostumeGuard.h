#pragma once
#include <windows.h>

// What the open Costume Effect window changes in the rest of the client:
//  - double-clicking a costume in the bag puts it into the window's slot instead of equipping it;
//  - nothing can be dropped on the ground, with the same kind of system line the refine window gives.
namespace CostumeGuard {
enum class DoubleClick { Use, Fill, Ignore };
// What a double-click on a bag item does. `inUse` is the item's in-use byte (it is already in a slot somewhere).
inline DoubleClick Decide(bool windowOpen, unsigned type, int bag, bool inUse) {
    if (!windowOpen || type >> 16 != 0x43 || bag == 0) return DoubleClick::Use;
    return inUse ? DoubleClick::Ignore : DoubleClick::Fill;
}
inline constexpr const char* BlockedMessage = "You cannot engage in another activity during Costume Effect.";
bool MatchesReference(const unsigned char* base);
// Joins the startup Detours transaction; does nothing if the client does not match.
LONG Attach();
}
