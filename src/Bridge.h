#pragma once
#include <windows.h>
class AhkWindow;
class EventShopWindow;
class DropBuffHud;
namespace Bridge {
// Called inside the caller's Detours transaction, before client game code runs.
LONG Attach();
void Frame(AhkWindow& window,HWND gameWindow,EventShopWindow* shop=nullptr,DropBuffHud* hud=nullptr);
void Pump();
bool SessionMatches();
// True once after the Collection Book window was requested (NPC dialog line).
bool TakeBookRequest();
// Asks for the Collection Book window and teaches the Say header if the player has not chatted yet (NPC dialog line).
void RequestBook();
// `.inspect <name>` as a click (the player menu): starts the request and teaches the Say header if the player has not chatted yet.
void RequestInspect(const char* name);
void Disconnect(AhkWindow& window,EventShopWindow* shop=nullptr,DropBuffHud* hud=nullptr);
}
