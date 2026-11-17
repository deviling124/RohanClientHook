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
void Disconnect(AhkWindow& window,EventShopWindow* shop=nullptr,DropBuffHud* hud=nullptr);
}
