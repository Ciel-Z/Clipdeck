import AppKit
import SwiftUI

private final class ClipDeckPanel: NSPanel {
    var onEscape: (() -> Void)?
    var onMoveSelection: ((Int) -> Void)?
    var onMoveCategory: ((Int) -> Void)?
    var onSearch: (() -> Void)?
    var onFinishSearch: (() -> Void)?
    var onTypeToSearch: ((String) -> Void)?
    var onPasteSearch: (() -> Void)?
    var onResumeSearchAndDeleteBackward: (() -> Void)?
    var onCollapseEmptySearch: (() -> Void)?
    var onDeleteSelection: (() -> Void)?
    var onPasteSelection: (() -> Void)?
    var onTogglePreview: (() -> Void)?
    var onEditSelection: (() -> Void)?
    var onRenameSelection: (() -> Void)?
    var onOpenSelection: (() -> Void)?
    var onQuickPaste: ((Int) -> Void)?
    var onQuickPasteModifierChanged: ((Bool) -> Void)?
    var isSearchActive: (() -> Bool)?
    var isSearchEditing: (() -> Bool)?
    var isSearchFieldFocused: (() -> Bool)?
    var isEditingContent: (() -> Bool)?
    var isPreviewingContent: (() -> Bool)?
    private var horizontalWheelTargetX: CGFloat?

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }

    override func sendEvent(_ event: NSEvent) {
        if event.type == .scrollWheel, routeVerticalWheelToCards(event) {
            return
        }

        if event.type == .flagsChanged {
            onQuickPasteModifierChanged?(AppPreferences.isQuickPasteModifierActive(in: event.modifierFlags))
            super.sendEvent(event)
            return
        }

        guard event.type == .keyDown else {
            super.sendEvent(event)
            return
        }

        let commandModifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let shortcutModifiers = commandModifiers.intersection([.command, .shift, .option, .control])
        if event.keyCode == 12, shortcutModifiers == .command {
            NSApp.terminate(nil)
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

        if AppPreferences.isQuickPasteModifierActive(in: event.modifierFlags),
           let quickPasteIndex = Self.quickPasteIndex(for: event.keyCode) {
            onQuickPaste?(quickPasteIndex)
            return
        }
        // Context-menu shortcuts remain available whenever the Deck panel is key,
        // even when the context menu itself is closed.
        if event.keyCode == 14, shortcutModifiers == .command {
            onEditSelection?()
            return
        }
        if event.keyCode == 15, shortcutModifiers == .command {
            onRenameSelection?()
            return
        }
        if event.keyCode == 31, shortcutModifiers == .command {
            onOpenSelection?()
            return
        }
        if event.keyCode == 9,
           shortcutModifiers == .command,
           isSearchActive?() == true {
            onPasteSearch?()
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
                DispatchQueue.main.async { [weak self] in
                    let hasMarkedText = (self?.firstResponder as? NSTextView)?.hasMarkedText() == true
                    if !hasMarkedText {
                        self?.onCollapseEmptySearch?()
                    }
                }
            } else if isSearchActive?() == true {
                if shortcutModifiers == .command {
                    onDeleteSelection?()
                } else if shortcutModifiers.isEmpty {
                    onResumeSearchAndDeleteBackward?()
                }
            } else if shortcutModifiers == .command {
                onDeleteSelection?()
            }
            return
        }

        if event.keyCode == 117 {
            if isSearchEditing?() == true {
                super.sendEvent(event)
            }
            return
        }

        if (event.keyCode == 125 || event.keyCode == 126),
           shortcutModifiers.isEmpty,
           isPreviewingContent?() == true {
            NotificationCenter.default.post(
                name: .clipDeckPreviewScroll,
                object: nil,
                userInfo: ["direction": event.keyCode == 125 ? 1 : -1]
            )
            return
        }

        let modifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
        if event.keyCode == 49, modifiers.isEmpty, isSearchEditing?() != true {
            onTogglePreview?()
            return
        }

        if AppPreferences.matches(
               keyCode: event.keyCode,
               flags: event.modifierFlags,
               configuredKeyCode: AppPreferences.previousCategoryKeyCode,
               configuredModifiers: AppPreferences.previousCategoryModifiers
           ) {
            onMoveCategory?(-1)
            return
        }
        if AppPreferences.matches(
               keyCode: event.keyCode,
               flags: event.modifierFlags,
               configuredKeyCode: AppPreferences.nextCategoryKeyCode,
               configuredModifiers: AppPreferences.nextCategoryModifiers
           ) {
            onMoveCategory?(1)
            return
        }
        if event.keyCode == 123 || event.keyCode == 124,
           commandModifiers.intersection([.command, .shift, .option, .control]).isEmpty {
            onMoveSelection?(event.keyCode == 123 ? -1 : 1)
            return
        }

        if isSearchFieldFocused?() != true,
           let characters = typeToSearchCharacters(from: event) {
            onTypeToSearch?(characters)
            return
        }

        super.sendEvent(event)
    }

    private func routeVerticalWheelToCards(_ event: NSEvent) -> Bool {
        let deltaX = event.scrollingDeltaX
        let deltaY = event.scrollingDeltaY
        guard abs(deltaY) > abs(deltaX), abs(deltaY) > 0.01,
              let contentView,
              let hitView = contentView.hitTest(
                  contentView.convert(event.locationInWindow, from: nil)
              ),
              let scrollView = Self.enclosingScrollView(from: hitView) else {
            return false
        }

        let clipView = scrollView.contentView
        guard let documentView = scrollView.documentView else { return false }
        let maximumX = max(0, documentView.frame.width - clipView.bounds.width)
        let maximumY = max(0, documentView.frame.height - clipView.bounds.height)

        // The card strip is tall and overflows only horizontally. This excludes
        // the compact category strip and vertically scrolling preview/editor.
        guard clipView.bounds.height >= 120,
              maximumX > 1,
              maximumY <= 2 else { return false }

        let current = clipView.bounds.origin
        if event.phase == .began || event.momentumPhase == .began {
            horizontalWheelTargetX = current.x
        }
        let startingX = horizontalWheelTargetX ?? current.x
        let multiplier: CGFloat = event.hasPreciseScrollingDeltas ? 1 : 44
        let targetX = min(max(0, startingX - deltaY * multiplier), maximumX)
        horizontalWheelTargetX = targetX

        if NSWorkspace.shared.accessibilityDisplayShouldReduceMotion {
            clipView.scroll(to: NSPoint(x: targetX, y: current.y))
            scrollView.reflectScrolledClipView(clipView)
            horizontalWheelTargetX = nil
            return true
        }

        NSAnimationContext.runAnimationGroup { context in
            context.duration = event.hasPreciseScrollingDeltas ? 0.08 : 0.14
            context.timingFunction = CAMediaTimingFunction(name: .easeOut)
            clipView.animator().setBoundsOrigin(NSPoint(x: targetX, y: current.y))
        } completionHandler: { [weak self, weak scrollView] in
            Task { @MainActor in
                guard let self else { return }
                if abs((self.horizontalWheelTargetX ?? targetX) - targetX) < 0.5 {
                    self.horizontalWheelTargetX = nil
                }
                scrollView?.reflectScrolledClipView(clipView)
            }
        }
        return true
    }

    private static func enclosingScrollView(from view: NSView) -> NSScrollView? {
        var candidate: NSView? = view
        while let current = candidate {
            if let scrollView = current as? NSScrollView {
                return scrollView
            }
            candidate = current.superview
        }
        return nil
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
        if AppPreferences.matches(
               keyCode: event.keyCode,
               flags: event.modifierFlags,
               configuredKeyCode: AppPreferences.previousCategoryKeyCode,
               configuredModifiers: AppPreferences.previousCategoryModifiers
           ) {
            onMoveCategory?(-1)
            return
        }
        if AppPreferences.matches(
               keyCode: event.keyCode,
               flags: event.modifierFlags,
               configuredKeyCode: AppPreferences.nextCategoryKeyCode,
               configuredModifiers: AppPreferences.nextCategoryModifiers
           ) {
            onMoveCategory?(1)
            return
        }
        let arrowModifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
        if (event.keyCode == 123 || event.keyCode == 124),
           arrowModifiers.isEmpty {
            onMoveSelection?(event.keyCode == 123 ? -1 : 1)
            return
        }
        let modifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
        if event.keyCode == 3, modifiers == .command {
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

    private static func quickPasteIndex(for keyCode: UInt16) -> Int? {
        let numberRow: [UInt16: Int] = [
            18: 0, 19: 1, 20: 2, 21: 3, 23: 4,
            22: 5, 26: 6, 28: 7, 25: 8,
        ]
        let numberPad: [UInt16: Int] = [
            83: 0, 84: 1, 85: 2, 86: 3, 87: 4,
            88: 5, 89: 6, 91: 7, 92: 8,
        ]
        return numberRow[keyCode] ?? numberPad[keyCode]
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
    private let onPaste: (ClipboardItem, Bool) -> Void
    private(set) var targetApplication: NSRunningApplication?
    private(set) var targetsClipDeck = false
    var isVisible: Bool { window?.isVisible == true }

    init(
        store: InMemoryHistoryStore,
        onPaste: @escaping (ClipboardItem, Bool) -> Void,
        onManageCategories: @escaping () -> Void,
        onSearch: @escaping (String) -> Void
    ) {
        self.store = store
        self.onPaste = onPaste

        let panel = ClipDeckPanel(
            contentRect: .zero,
            styleMask: [.borderless, .fullSizeContentView, .nonactivatingPanel],
            backing: .buffered,
            defer: true
        )
        panel.level = .floating
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        panel.isFloatingPanel = true
        panel.becomesKeyOnlyIfNeeded = false
        // Visibility is managed by windowDidResignKey. AppKit's automatic
        // deactivation hiding races with activation when invoked from the menu bar.
        panel.hidesOnDeactivate = false
        panel.isMovable = false
        panel.hasShadow = true
        panel.backgroundColor = .clear
        panel.isOpaque = false

        super.init(window: panel)
        panel.onEscape = { [weak self, weak store] in
            if store?.filePasteFeedback != nil {
                store?.filePasteFeedback = nil
            } else if store?.renamingItemID != nil {
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
        panel.onPasteSearch = { [weak store] in
            guard let store,
                  let pasted = NSPasteboard.general.string(forType: .string) else { return }
            let normalized = pasted.replacingOccurrences(
                of: #"\s+"#,
                with: " ",
                options: .regularExpression
            )
            guard !normalized.isEmpty else { return }
            store.setSearchEditing(true)
            store.searchQuery.append(normalized)
        }
        panel.onResumeSearchAndDeleteBackward = { [weak store] in
            store?.resumeSearchEditingAndDeleteBackward()
        }
        panel.onCollapseEmptySearch = { [weak store] in
            store?.collapseSearchIfQueryWasDeleted()
        }
        panel.isSearchActive = { [weak store] in store?.isSearchActive == true }
        panel.isSearchEditing = { [weak store] in store?.isSearchEditing == true }
        panel.isSearchFieldFocused = { [weak store] in store?.isSearchFieldFocused == true }
        panel.isEditingContent = { [weak store] in
            store?.editingItemID != nil || store?.renamingItemID != nil
        }
        panel.isPreviewingContent = { [weak store] in store?.previewedItemID != nil }
        panel.onDeleteSelection = { [weak store] in store?.deleteSelection() }
        panel.onPasteSelection = { [weak store] in
            guard let store, let item = store.selectedItem else { return }
            store.dismissPreview()
            onPaste(item, false)
        }
        panel.onTogglePreview = { [weak store] in store?.togglePreviewForSelection() }
        panel.onEditSelection = { [weak store] in store?.beginEditingSelection() }
        panel.onRenameSelection = { [weak store] in store?.beginRenamingSelection() }
        panel.onOpenSelection = { [weak store] in
            guard let url = store?.selectedItem?.webURL else { return }
            NSWorkspace.shared.open(url)
        }
        panel.onQuickPaste = { [weak store] index in
            guard let store, store.displayedItems.indices.contains(index) else { return }
            let item = store.displayedItems[index]
            store.selectedItemID = item.id
            store.setQuickPasteOverlayVisible(false)
            onPaste(item, false)
        }
        panel.onQuickPasteModifierChanged = { [weak store] pressed in
            store?.setQuickPasteOverlayVisible(pressed)
        }
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

    func prepareForFirstPresentation() {
        guard let panel = window else { return }
        updateFrame(of: panel, display: false)
        panel.contentView?.layoutSubtreeIfNeeded()
    }

    func show(
        targetApplication: NSRunningApplication? = nil,
        targetsClipDeck: Bool = false
    ) {
        guard let panel = window else { return }
        self.targetsClipDeck = targetsClipDeck
        if targetsClipDeck {
            self.targetApplication = nil
        } else if let targetApplication,
           targetApplication.processIdentifier != ProcessInfo.processInfo.processIdentifier,
           !targetApplication.isTerminated {
            self.targetApplication = targetApplication
        } else if let frontmost = NSWorkspace.shared.frontmostApplication,
                  frontmost.processIdentifier != ProcessInfo.processInfo.processIdentifier,
                  !frontmost.isTerminated {
            self.targetApplication = frontmost
        }
        updateFrame(of: panel, display: true)
        store.selectedItemID = store.selectedItemID ?? store.displayedItems.first?.id
        store.revealSelection()

        // LSUIElement/accessory apps are not guaranteed to receive an application
        // activation callback after a status-item click in a packaged build. A
        // nonactivating NSPanel can become key and receive keyboard events while
        // deliberately leaving the previously frontmost application active.
        panel.orderFrontRegardless()
        focusVisiblePanel()
        NSLog(
            "ClipDeck panel shown at %@; visible=%@; key=%@; active=%@; target=%@",
            NSStringFromRect(panel.frame),
            panel.isVisible ? "true" : "false",
            panel.isKeyWindow ? "true" : "false",
            NSApp.isActive ? "true" : "false",
            targetsClipDeck ? "ClipDeck" : (self.targetApplication?.localizedName ?? "none")
        )
    }

    func focusVisiblePanel() {
        guard let panel = window, panel.isVisible else { return }
        panel.makeKeyAndOrderFront(nil)
        NSLog(
            "ClipDeck panel focus completed; key=%@; active=%@; firstResponder=%@",
            panel.isKeyWindow ? "true" : "false",
            NSApp.isActive ? "true" : "false",
            panel.firstResponder.map { String(describing: type(of: $0)) } ?? "none"
        )
    }

    private func updateFrame(of panel: NSWindow, display: Bool) {
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
            display: display
        )
    }

    func hide() {
        store.filePasteFeedback = nil
        if window?.isVisible == true {
            NSLog("ClipDeck panel hidden")
        }
        store.setQuickPasteOverlayVisible(false)
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
