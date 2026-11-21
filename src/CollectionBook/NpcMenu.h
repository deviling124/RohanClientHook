#pragma once
#include <windows.h>
// Adds an "I wish to open the Collection Book." line to one NPC's dialog and opens the book window when it is clicked.
// novaclient.ini [collection_book]: npc=<npc id> (default 328057 = Obivos, 0 turns it off), npc_text=<menu line>, npc_log=1 (log ids).
namespace NpcMenu {
// Called inside the caller's Detours transaction, before client game code runs.
LONG Attach();
}
