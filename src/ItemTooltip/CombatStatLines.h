#pragma once
#include <windows.h>

// Item tooltip lines for the combat stats (PvP / PvE attack and defense, skill damage and skill defense).
// The client's tooltip builders print one line per item attribute through a switch over the type; types above 175 are dropped.
// For each type below a copy of the game's own type 175 case ("PVP Defense : + n%") is made with the type and the text id replaced
// and the builders' "type too big" jump goes through a small dispatcher to those copies. See docs/COMBAT_STAT_TOOLTIP.md.
namespace CombatStatLines {
bool MatchesReference(const unsigned char* base);
// Writes the stubs and the jumps. Runs inside the startup Detours transaction (single threaded, before the game starts).
LONG Attach();
}
