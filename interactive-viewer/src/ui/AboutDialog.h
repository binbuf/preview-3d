#pragma once
#include <windows.h>

// Shows the About window: app icon, name, compiled-in version, copyright,
// license, and links to the project's GitHub repository and license.
// Native Win32 and custom-drawn to match the viewer's dark chrome, with DPI,
// high-contrast, and keyboard (Esc / Enter) support like the controls window.
void ShowAboutDialog(HWND owner);
