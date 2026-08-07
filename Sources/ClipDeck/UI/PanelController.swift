import AppKit
import SwiftUI

private final class ClipDeckPanel: NSPanel {
    var onEscape: (() -> Void)?
    var onMoveSelection: ((Int) -> Void)?
    var onMoveCategory: ((Int) -> Void)?
    var onSearch: (() -> Void)?
    var onFinishSearch: (() -> Void)?
    var onTypeToSearch: ((String) -> Void)?
    var onResumeSearchAndDeleteBackward: (() -> Void)?
    var onDeleteSelection: (() -> Void)?
    var onPasteSelection: (() -> Void)?
    var onTogglePreview: (() -> Void)?
    var onEditSelection: (() -> Void)?
    var onRenameSelection: (() -> Void)?
    var isSearchActive: (() -> Bool)?
    var isSearchEditing: (() -> Bool)?
    var isSearchFieldFocused: (() -> Bool)?
    var isEditingContent: (() -> Bool)?

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }

    override func sendEvent(_ event: NSEvent) {
        guard event.type == .keyDown else {
            super.sendEvent(event)
            return
        }

        if isEditingContent?() == true {
            if event.keyCode == 53 {
                onEscape?()
            } else {
                super.sendEvent(event)
            }
            return
        }

        if event.keyCode == 53 {
            onEscape?()
            return
        }

        let commandModifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        if event.keyCode == 14, commandModifiers.contains(.command) {
            onEditSelection?()
            return
        }
        if event.keyCode == 15, commandModifiers.contains(.command) {
            onRenameSelection?()
            return
        }

        if event.keyCode == 36 || event.keyCode == 76 {
            if isSearchEditing?() == true {
                onFinishSearch?()
            } else {
                onPasteSelection?()
            }
            return
        }

        if event.keyCode == 51 {
            if isSearchEditing?() == true, isSearchFieldFocused?() == true {
                super.sendEvent(event)
            } else if isSearchActive?() == true {
                onResumeSearchAndDeleteBackward?()
            } else {
                onDeleteSelection?()
            }
            return
        }

        if event.keyCode == 117 {
            if isSearchEditing?() == true {
                super.sendEvent(event)
            } else {
                onDeleteSelection?()
            }
            return
        }

        let modifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
        if event.keyCode == 49, modifiers.isEmpty, isSearchEditing?() != true {
            onTogglePreview?()
            return
        }

        if event.keyCode == 123 || event.keyCode == 124,
           isSearchEditing?() != true {
            let direction = event.keyCode == 123 ? -1 : 1
            if event.modifierFlags.intersection(.deviceIndependentFlagsMask).contains(.command) {
                onMoveCategory?(direction)
            } else {
                onMoveSelection?(direction)
            }
            return
        }

        if isSearchFieldFocused?() != true,
           let characters = typeToSearchCharacters(from: event) {
            onTypeToSearch?(characters)
            return
        }

        super.sendEvent(event)
    }

    override func keyDown(with event: NSEvent) {
        if isEditingContent?() == true {
            super.keyDown(with: event)
            return
        }
        if event.keyCode == 53 {
            NSLog("ClipDeck Escape key pressed")
            onEscape?()
            return
        }
        if event.keyCode == 123 || event.keyCode == 124 {
            if isSearchEditing?() == true {
                super.keyDown(with: event)
                return
            }
            let direction = event.keyCode == 123 ? -1 : 1
            if event.modifierFlags.intersection(.deviceIndependentFlagsMask).contains(.command) {
                onMoveCategory?(direction)
            } else {
                onMoveSelection?(direction)
            }
            return
        }
        let modifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
        if event.keyCode == 3, modifiers.contains(.command) {
            onSearch?()
            return
        }
        super.keyDown(with: event)
    }

    private func typeToSearchCharacters(from event: NSEvent) -> String? {
        let modifiers = event.modifierFlags.intersection([.command, .control, .option])
        guard modifiers.isEmpty,
              event.keyCode != 49,
              let characters = event.characters,
              !characters.isEmpty,
              characters.unicodeScalars.allSatisfy({ scalar in
                  !CharacterSet.controlCharacters.contains(scalar)
                      && !(0xF700 ... 0xF8FF).contains(scalar.value)
              }) else { return nil }
        return characters
    }

    override func cancelOperation(_ sender: Any?) {
        NSLog("ClipDeck cancel operation received")
        onEscape?()
    }
}

@MainActor
final class PanelController: NSWindowController, NSWindowDelegate {
    private static let savedHeightKey = "deck.preferredHeight"
    private let store: InMemoryHistoryStore
    private let onPaste: (ClipboardItem) -> Void
    private(set) var targetApplication: NSRunningApplication?
    var isVisible: Bool { window?.isVisible == true }

    init(
        store: InMemoryHistoryStore,
        onPaste: @escaping (ClipboardItem) -> Void,
        onManageCategories: @escaping () -> Void,
        onSearch: @escaping (String) -> Void
    ) {
        self.store = store
        self.onPaste = onPaste

        let panel = ClipDeckPanel(
            contentRect: .zero,
            styleMask: [.borderless, .fullSizeContentView],
            backing: .buffered,
            defer: true
        )
        panel.level = .floating
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = true
        panel.isMovable = false
        panel.hasShadow = true
        panel.backgroundColor = .clear
        panel.isOpaque = false

        super.init(window: panel)
        panel.onEscape = { [weak self, weak store] in
            if store?.renamingItemID != nil {
                store?.cancelRenaming()
            } else if store?.editingItemID != nil {
                store?.cancelEditing()
            } else if store?.isSearchActive == true {
                store?.endSearch()
            } else {
                self?.hide()
            }
        }
        panel.onMoveSelection = { [weak store] direction in
            if direction < 0 {
                store?.selectPrevious()
            } else {
                store?.selectNext()
            }
        }
        panel.onMoveCategory = { [weak store] direction in
            if direction < 0 {
                store?.selectPreviousCategory()
            } else {
                store?.selectNextCategory()
            }
        }
        panel.onSearch = { [weak store] in store?.beginSearch() }
        panel.onFinishSearch = { [weak store] in store?.finishSearchEditing() }
        panel.onTypeToSearch = { [weak store] characters in
            guard let store else { return }
            store.beginSearch()
            store.searchQuery.append(characters)
        }
        panel.onResumeSearchAndDeleteBackward = { [weak store] in
            store?.resumeSearchEditingAndDeleteBackward()
        }
        panel.isSearchActive = { [weak store] in store?.isSearchActive == true }
        panel.isSearchEditing = { [weak store] in store?.isSearchEditing == true }
        panel.isSearchFieldFocused = { [weak store] in store?.isSearchFieldFocused == true }
        panel.isEditingContent = { [weak store] in
            store?.editingItemID != nil || store?.renamingItemID != nil
        }
        panel.onDeleteSelection = { [weak store] in store?.deleteSelection() }
        panel.onPasteSelection = { [weak store] in
            guard let store, let item = store.selectedItem else { return }
            store.dismissPreview()
            onPaste(item)
        }
        panel.onTogglePreview = { [weak store] in store?.togglePreviewForSelection() }
        panel.onEditSelection = { [weak store] in store?.beginEditingSelection() }
        panel.onRenameSelection = { [weak store] in store?.beginRenamingSelection() }
        panel.delegate = self
        panel.contentViewController = NSHostingController(
            rootView: HistoryPanelView(
                store: store,
                onPaste: onPaste,
                onClose: { [weak panel] in panel?.orderOut(nil) },
                onManageCategories: onManageCategories,
                onSearch: onSearch
            )
        )
        panel.contentView?.wantsLayer = true
        panel.contentView?.layer?.cornerRadius = 24
        panel.contentView?.layer?.cornerCurve = .continuous
        panel.contentView?.layer?.masksToBounds = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func toggle() {
        guard let panel = window else { return }
        if panel.isVisible {
            hide()
        } else {
            show()
        }
    }

    func show(targetApplication: NSRunningApplication? = nil) {
        guard let panel = window else { return }
        if let targetApplication,
           targetApplication.processIdentifier != ProcessInfo.processInfo.processIdentifier,
           !targetApplication.isTerminated {
            self.targetApplication = targetApplication
        } else if let frontmost = NSWorkspace.shared.frontmostApplication,
                  frontmost.processIdentifier != ProcessInfo.processInfo.processIdentifier,
                  !frontmost.isTerminated {
            self.targetApplication = frontmost
        }
        let screen = Self.preferredScreen()
        let visibleFrame = screen.visibleFrame
        let horizontalInset: CGFloat = 8
        let width = visibleFrame.width - horizontalInset * 2
        let maximumHeight = max(250, min(420, floor(visibleFrame.height * 0.42)))
        let minimumHeight = min(250, maximumHeight)
        let defaultHeight = min(maximumHeight, max(340, floor(visibleFrame.height * 0.385)))
        let savedHeight = (UserDefaults.standard.object(forKey: Self.savedHeightKey) as? NSNumber)
            .map { CGFloat(truncating: $0) }
        let requestedHeight = savedHeight ?? (panel.frame.height > 0 ? panel.frame.height : defaultHeight)
        let height = min(max(requestedHeight, minimumHeight), maximumHeight)
        panel.setFrame(
            NSRect(
                x: visibleFrame.minX + horizontalInset,
                y: visibleFrame.minY,
                width: width,
                height: height
            ),
            display: true
        )
        store.selectedItemID = store.selectedItemID ?? store.displayedItems.first?.id
        store.revealSelection()
        NSApp.activate()
        panel.makeKeyAndOrderFront(nil)
        NSLog(
            "ClipDeck panel shown at %@; visible=%@; target=%@",
            NSStringFromRect(panel.frame),
            panel.isVisible ? "true" : "false",
            self.targetApplication?.localizedName ?? "none"
        )
    }

    func hide() {
        if window?.isVisible == true {
            NSLog("ClipDeck panel hidden")
        }
        window?.orderOut(nil)
    }

    func windowDidResignKey(_ notification: Notification) {
        NSLog("ClipDeck panel resigned key")
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if NSApp.isActive,
               (self.store.isSearchEditing
                   || self.store.previewedItemID != nil
                   || self.store.editingItemID != nil
                   || self.store.renamingItemID != nil) {
                return
            }
            self.hide()
        }
    }

    private static func preferredScreen() -> NSScreen {
        let mouseLocation = NSEvent.mouseLocation
        return NSScreen.screens.first { $0.frame.contains(mouseLocation) }
            ?? NSScreen.main
            ?? NSScreen.screens[0]
    }
}
