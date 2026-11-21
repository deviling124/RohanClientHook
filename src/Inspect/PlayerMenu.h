#pragma once
#include <windows.h>

// "Inspect" in the right-click player menu. The stock menu (window id 0xC1, layout PlayerMenu.ui) has "Use Shop" as its first line; the layout made
// by tools/make_playermenu_layout.py (server repo) relabels it Inspect, and this hook runs `.inspect <name>` for the clicked player when that line is
// clicked. Does nothing when novaclient.ini [inspect] menu=0. Called inside the caller's open Detours transaction.
namespace PlayerMenu {
LONG Attach();
}
