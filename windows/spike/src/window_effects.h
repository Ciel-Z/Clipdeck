#pragma once

#include <QtGlobal>

#include <windows.h>

namespace spike {

// Undocumented user32 accent states used by Windows 10 for blur materials.
enum class Accent : int {
    Disabled = 0,
    BlurBehind = 3,
    AcrylicBlurBehind = 4,
};

// Phase 0 candidates for the Deck container. See docs/windows-port-plan.md §3.
enum class EffectMode : int {
    AcrylicRegion = 1,     // acrylic + SetWindowRgn rounded corners, no shadow
    BlurRegion = 2,        // classic blur + SetWindowRgn rounded corners
    AcrylicDwmShadow = 3,  // acrylic, square corners, DWM frame shadow
    SolidQmlShadow = 4,    // no OS blur; AA rounded corners and shadow drawn by QML
};

class WindowEffects {
public:
    // tint is AABBGGRR, as SetWindowCompositionAttribute expects.
    static bool setAccent(HWND window, Accent accent, quint32 tint);
    static bool isAccentAvailable();
    // radius <= 0 removes the region.
    static bool setRoundedRegion(HWND window, int radiusPixels);
    static bool setDwmShadow(HWND window, bool enabled);
};

} // namespace spike
