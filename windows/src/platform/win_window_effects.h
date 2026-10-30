#pragma once

#include <QRect>
#include <QtGlobal>

#include <windows.h>

// Deck container material (Phase 0: acrylic + region corners). See
// docs/windows-port-plan.md §5.2 and windows/spike/TESTING.md.
namespace WinEffects {

// Undocumented user32 accent states used by Windows 10 for blur materials.
enum class Accent : int {
    Disabled = 0,
    BlurBehind = 3,
    AcrylicBlurBehind = 4,
};

// tint is AABBGGRR, as SetWindowCompositionAttribute expects.
bool setAccent(HWND window, Accent accent, quint32 tint);
bool isAccentAvailable();
// radius <= 0 removes the region.
bool setRoundedRegion(HWND window, int radiusPixels);
// A rounded rect inside the client area (physical pixels). On a
// DirectComposition window this shapes hit-testing, not what is drawn.
bool setRoundedRegion(HWND window, const QRect &rect, int radiusPixels);
// Keeps the window out of Alt+Tab and the taskbar (mac nonactivating panel).
void setToolWindow(HWND window);
// Dark caption for the settings window (DWMWA_USE_IMMERSIVE_DARK_MODE: 20,
// 19 on Win10 1809–1909).
void setDarkTitleBar(HWND window);

} // namespace WinEffects
