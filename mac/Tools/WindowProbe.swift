import CoreGraphics
import Foundation

let windows = CGWindowListCopyWindowInfo(
    [.optionOnScreenOnly, .excludeDesktopElements],
    kCGNullWindowID
) as? [[String: Any]] ?? []

let clipDeckWindows = windows.filter {
    ($0[kCGWindowOwnerName as String] as? String) == "ClipDeck"
}

for window in clipDeckWindows {
    let bounds = window[kCGWindowBounds as String] ?? [:]
    let layer = window[kCGWindowLayer as String] ?? -1
    let alpha = window[kCGWindowAlpha as String] ?? 0
    print("ClipDeck window layer=\(layer) alpha=\(alpha) bounds=\(bounds)")
}
print("visibleWindowCount=\(clipDeckWindows.count)")
