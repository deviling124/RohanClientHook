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
// True once after the player typed the local `.cbook` command (the Collection Book window should open).
bool TakeBookRequest();
// `.inspect <name>` as a click (the player menu): starts the request and teaches the Say header if the player has not chatted yet.
void RequestInspect(const char* name);
void Disconnect(AhkWindow& window,EventShopWindow* shop=nullptr,DropBuffHud* hud=nullptr);
}
