import AppKit
import Carbon

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
    /// Status menu "背景模糊" (as on Linux and Windows): the frosted material
    /// behind the Deck, or the live desktop under a plain black tint.
    static let deckBackgroundBlurKey = "deck.backgroundBlur"

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
            deckBackgroundBlurKey: true,
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
        let days = UserDefaults.standard.integer(forKey: retentionDaysKey)
        return days == 0 ? 0 : max(1, days)
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

    static func matches(
        keyCode: UInt16,
        flags: NSEvent.ModifierFlags,
        configuredKeyCode: UInt32,
        configuredModifiers: UInt32
    ) -> Bool {
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
    static let clipDeckPreviewScroll = Notification.Name("ClipDeck.previewScroll")
}
