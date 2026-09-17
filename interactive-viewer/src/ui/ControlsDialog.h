#pragma once

#include <windows.h>

// Shows the keyboard and mouse reference as an owned modal window. The
// implementation is native Win32 so it inherits the viewer's deployment,
// DPI, high-contrast, and offline behavior without a web runtime.
void ShowControlsDialog(HWND owner);

