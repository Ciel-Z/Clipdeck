import AppKit
import Carbon
import ImageIO

enum AppPreferences {
    static let recordsImagesKey = "clipboard.recordsImages"
    static let maximumImageMegabytesKey = "clipboard.maximumImageMegabytes"
    static let retentionDaysKey = "history.retentionDays"
    static let hotKeyCodeKey = "shortcuts.showDeck.keyCode"
    static let hotKeyModifiersKey = "shortcuts.showDeck.modifiers"
    static let nextCategoryKeyCodeKey = "shortcuts.nextCategory.keyCode"
    static let nextCategoryModifiersKey = "shortcuts.nextCategory.modifiers"
    static let previousCategoryKeyCodeKey = "shortcuts.previousCategory.keyCode"
    static let previousCategoryModifiersKey = "shortcuts.previousCategory.modifiers"
    static let quickPasteModifierKey = "shortcuts.quickPaste.modifier"
    static let plainTextModifierKey = "shortcuts.plainText.modifier"

    static func registerDefaults() {
        UserDefaults.standard.register(defaults: [
            recordsImagesKey: true,
            maximumImageMegabytesKey: 10,
            retentionDaysKey: 30,
            hotKeyCodeKey: Int(kVK_ANSI_V),
            hotKeyModifiersKey: Int(cmdKey | shiftKey),
            nextCategoryKeyCodeKey: Int(kVK_RightArrow),
            nextCategoryModifiersKey: Int(cmdKey),
            previousCategoryKeyCodeKey: Int(kVK_LeftArrow),
            previousCategoryModifiersKey: Int(cmdKey),
            quickPasteModifierKey: Int(cmdKey),
            plainTextModifierKey: Int(shiftKey),
        ])
    }

    static var recordsImages: Bool {
        UserDefaults.standard.bool(forKey: recordsImagesKey)
    }

    static var maximumImageByteCount: Int {
        let megabytes = max(1, UserDefaults.standard.integer(forKey: maximumImageMegabytesKey))
        return megabytes * 1_024 * 1_024
    }

    static var retentionDays: Int {
        min(90, max(1, UserDefaults.standard.integer(forKey: retentionDaysKey)))
    }

    static var hotKeyCode: UInt32 {
        UInt32(UserDefaults.standard.integer(forKey: hotKeyCodeKey))
    }

    static var hotKeyModifiers: UInt32 {
        UInt32(UserDefaults.standard.integer(forKey: hotKeyModifiersKey))
    }

    static var nextCategoryKeyCode: UInt32 { UInt32(UserDefaults.standard.integer(forKey: nextCategoryKeyCodeKey)) }
    static var nextCategoryModifiers: UInt32 { UInt32(UserDefaults.standard.integer(forKey: nextCategoryModifiersKey)) }
    static var previousCategoryKeyCode: UInt32 { UInt32(UserDefaults.standard.integer(forKey: previousCategoryKeyCodeKey)) }
    static var previousCategoryModifiers: UInt32 { UInt32(UserDefaults.standard.integer(forKey: previousCategoryModifiersKey)) }
    static var quickPasteModifier: UInt32 { UInt32(UserDefaults.standard.integer(forKey: quickPasteModifierKey)) }
    static var plainTextModifier: UInt32 { UInt32(UserDefaults.standard.integer(forKey: plainTextModifierKey)) }

    static func carbonModifiers(from flags: NSEvent.ModifierFlags) -> UInt32 {
        var value: UInt32 = 0
        if flags.contains(.control) { value |= UInt32(controlKey) }
        if flags.contains(.option) { value |= UInt32(optionKey) }
        if flags.contains(.shift) { value |= UInt32(shiftKey) }
        if flags.contains(.command) { value |= UInt32(cmdKey) }
        return value
    }

    static func matches(keyCode: UInt16, flags: NSEvent.ModifierFlags, configuredKeyCode: UInt32, configuredModifiers: UInt32) -> Bool {
        UInt32(keyCode) == configuredKeyCode && carbonModifiers(from: flags) == configuredModifiers
    }

    static func isQuickPasteModifierActive(in flags: NSEvent.ModifierFlags) -> Bool {
        carbonModifiers(from: flags) == quickPasteModifier
    }

    static func isPlainTextModifierActive(in flags: NSEvent.ModifierFlags) -> Bool {
        carbonModifiers(from: flags) == plainTextModifier
    }

    static func resetShortcutDefaults() {
        UserDefaults.standard.set(Int(kVK_ANSI_V), forKey: hotKeyCodeKey)
        UserDefaults.standard.set(Int(cmdKey | shiftKey), forKey: hotKeyModifiersKey)
        UserDefaults.standard.set(Int(kVK_RightArrow), forKey: nextCategoryKeyCodeKey)
        UserDefaults.standard.set(Int(cmdKey), forKey: nextCategoryModifiersKey)
        UserDefaults.standard.set(Int(kVK_LeftArrow), forKey: previousCategoryKeyCodeKey)
        UserDefaults.standard.set(Int(cmdKey), forKey: previousCategoryModifiersKey)
        UserDefaults.standard.set(Int(cmdKey), forKey: quickPasteModifierKey)
        UserDefaults.standard.set(Int(shiftKey), forKey: plainTextModifierKey)
    }
}

extension Notification.Name {
    static let clipDeckShortcutChanged = Notification.Name("clipDeckShortcutChanged")
}

struct ClipboardReader: Sendable {
    static let maximumPixelCount = 60_000_000

    func read(from pasteboard: NSPasteboard = .general) -> ClipboardPayload? {
        // File drags often also publish a string representation. They are outside v1 scope.
        if pasteboard.availableType(from: [.fileURL]) != nil {
            return nil
        }

        if let imagePayload = readImage(from: pasteboard) {
            return imagePayload
        }

        guard let plainText = pasteboard.string(forType: .string), !plainText.isEmpty else {
            return nil
        }
        return .text(
            plainText: plainText,
            rtfData: pasteboard.data(forType: .rtf)
        )
    }

    private func readImage(from pasteboard: NSPasteboard) -> ClipboardPayload? {
        guard AppPreferences.recordsImages else { return nil }
        let candidates: [NSPasteboard.PasteboardType] = [.png, .tiff]
        guard let type = pasteboard.availableType(from: candidates),
              let data = pasteboard.data(forType: type),
              data.count <= AppPreferences.maximumImageByteCount,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? Int,
              let height = properties[kCGImagePropertyPixelHeight] as? Int,
              Self.isSafeImageSize(width: width, height: height) else {
            return nil
        }
        let format: ClipboardImageFormat = type == .png ? .png : .tiff
        return .image(data: data, width: width, height: height, format: format)
    }

    static func isSafeImageSize(width: Int, height: Int) -> Bool {
        guard width > 0, height > 0, width <= maximumPixelCount else { return false }
        return height <= maximumPixelCount / width
    }
}
