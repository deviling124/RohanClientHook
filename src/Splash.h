#pragma once

#include <windows.h>

namespace Splash {

// Shows the embedded game artwork while the original client finishes starting.
void Start(HINSTANCE module);
void Stop();

}
