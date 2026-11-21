#pragma once
#include <windows.h>

// "Inspect" line in the right-click player menu. The stock menu (window id 0xC1, layout PlayerMenu.ui) has a switched-off second button; the
// layout made by tools/make_playermenu_layout.py (server repo) switches it on and calls it Inspect, and this hook runs `.inspect <name>` for the
// clicked player when that button is clicked. Does nothing when novaclient.ini [inspect] menu=0. Called inside the caller's open Detours transaction.
namespace PlayerMenu {
LONG Attach();
}
