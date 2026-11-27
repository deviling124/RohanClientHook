#pragma once

namespace HunterKitStop {
// Stops the game's own Hunter Kit if it is running, the way its Stop button does, and says why in chat.
// Does nothing otherwise. Game thread only. See RohanMapServ_Hook/docs/HUNTER_KIT_STOP.md.
void OnAttackedByPlayer();
}
