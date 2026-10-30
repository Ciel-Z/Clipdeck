#include "win_window_effects.h"

#include <dwmapi.h>

namespace WinEffects {
namespace {

struct AccentPolicy {
    int accentState;
    int accentFlags;
    DWORD gradientColor;
    int animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    void *data;
    SIZE_T size;
};

constexpr int WcaAccentPolicy = 19;

using SetWindowCompositionAttributeFn = BOOL(WINAPI *)(HWND, WindowCompositionAttributeData *);

SetWindowCompositionAttributeFn resolveSetWindowCompositionAttribute() {
    static const auto function = reinterpret_cast<SetWindowCompositionAttributeFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute")));
    return function;
}

} // namespace

bool isAccentAvailable() { return resolveSetWindowCompositionAttribute() != nullptr; }

bool setAccent(HWND window, Accent accent, quint32 tint) {
    const auto setAttribute = resolveSetWindowCompositionAttribute();
    if (!window || !setAttribute) return false;
    AccentPolicy policy{int(accent), 0, DWORD(tint), 0};
    WindowCompositionAttributeData data{WcaAccentPolicy, &policy, sizeof(policy)};
    return setAttribute(window, &data) != FALSE;
}

bool setRoundedRegion(HWND window, int radiusPixels) {
    if (!window) return false;
    if (radiusPixels <= 0) return SetWindowRgn(window, nullptr, TRUE) != 0;
    RECT client{};
    GetClientRect(window, &client);
    // CreateRoundRectRgn excludes the right/bottom edge, hence +1.
    HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, radiusPixels * 2, radiusPixels * 2);
    // The system owns the region after a successful call.
    if (SetWindowRgn(window, region, TRUE) != 0) return true;
    DeleteObject(region);
    return false;
}

bool setRoundedRegion(HWND window, const QRect &rect, int radiusPixels) {
    if (!window || rect.isEmpty()) return false;
    HRGN region = CreateRoundRectRgn(rect.left(), rect.top(), rect.left() + rect.width() + 1,
                                     rect.top() + rect.height() + 1, radiusPixels * 2, radiusPixels * 2);
    if (SetWindowRgn(window, region, FALSE) != 0) return true; // no repaint: the scene draws the shape
    DeleteObject(region);
    return false;
}

void setDarkTitleBar(HWND window) {
    if (!window) return;
    const BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(window, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
}

void setToolWindow(HWND window) {
    if (!window) return;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, (style | WS_EX_TOOLWINDOW) & ~LONG_PTR(WS_EX_APPWINDOW));
}

} // namespace WinEffects
