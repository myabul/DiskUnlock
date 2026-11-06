#pragma once
#include "../pch.h"
#include <sstream>

namespace ui {

// Registers the window class and creates the main window. Returns nullptr on failure.
HWND CreateMainWindow(HINSTANCE inst, int showCmd);
// Keyboard table owned by the main window; the message loop feeds messages to it first.
HACCEL MainWindowAccel();
inline constexpr const wchar_t* kWindowClass = L"DiskUnlockStudioWindow";

}  // namespace ui
