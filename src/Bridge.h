#pragma once
#include <windows.h>
class AhkWindow;
class EventShopWindow;
class DropBuffHud;
namespace Bridge {
// Called inside the caller's Detours transaction, before client game code runs.
LONG Attach();
// Called on the game thread each time the server says another player hit this one (it only stops the Hunter Kit).
void SetAttackedByPlayerHandler(void (*handler)());
void Frame(AhkWindow& window,HWND gameWindow,EventShopWindow* shop=nullptr,DropBuffHud* hud=nullptr);
void Pump();
bool SessionMatches();
// True once after the Collection Book window was requested (NPC dialog line).
bool TakeBookRequest();
// Asks for the Collection Book window and teaches the Say header if the player has not chatted yet (NPC dialog line).
void RequestBook();
// `.inspect <name>` as a click (the player menu): starts the request and teaches the Say header if the player has not chatted yet.
void RequestInspect(const char* name);
// True once after the Boss Check window was requested (a click on the minimap skull button).
bool TakeBossRequest();
// Opens the Boss Check window as a click (the minimap button) and teaches the Say header if needed.
void OpenBossCheck();
// Teaches the Say header if the player has not chatted yet, so a queued command (a click in the Boss Check window) can be sent.
void LearnSayHeader();
void Disconnect(AhkWindow& window,EventShopWindow* shop=nullptr,DropBuffHud* hud=nullptr);
}
