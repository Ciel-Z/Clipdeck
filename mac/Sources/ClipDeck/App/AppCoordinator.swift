import AppKit

@MainActor
final class AppCoordinator: NSObject, NSApplicationDelegate {
    private let store = InMemoryHistoryStore()
    private let repository: HistoryRepository?
    private let classifier: ClassificationService?
    private let feedbackSoundPlayer = FeedbackSoundPlayer()
    private lazy var monitor = ClipboardMonitor(
        store: store,
        repository: repository,
        classifier: classifier,
        onCapture: { [weak self] in self?.feedbackSoundPlayer.playCopy() }
    )
    private lazy var pasteService = PasteService(
        onPasteKeystroke: { [weak self] in self?.feedbackSoundPlayer.playPaste() }
    )
    private var panelController: PanelController!
    private var hotKeyController: GlobalHotKeyController!
    private var statusItem: NSStatusItem!
    private var categorySettingsController: CategorySettingsController?
    private var lastExternalApplication: NSRunningApplication?
    private var searchTask: Task<Void, Never>?
    private var historyLoadTask: Task<Void, Never>?
    private var hotKeyRegistration: GlobalHotKeyController.RegistrationResult = .registered
    private var shortcutObserver: NSObjectProtocol?
    private var localKeyMonitor: Any?
    private var statusItemMouseMonitor: Any?
    private lazy var filePasteController = FilePasteController(store: store) { [weak self] in
        self?.panelController?.isVisible == true
    }

    override init() {
        AppPreferences.registerDefaults()
        repository = try? HistoryRepository(database: ClipDeckDatabase.applicationDatabase())
        if let repository {
            try? repository.ensureDefaultCategories()
            classifier = ClassificationService(repository: repository)
        } else {
            classifier = nil
        }
        super.init()
        shortcutObserver = NotificationCenter.default.addObserver(
            forName: .clipDeckShortcutChanged,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.reloadGlobalShortcut()
            }
        }
        let historyRepository = repository
        store.onDelete = { [historyRepository] item in
            DispatchQueue.global(qos: .utility).async {
                do {
                    try historyRepository?.delete(item)
                } catch {
                    NSLog("ClipDeck failed to delete persisted item: %@", String(describing: error))
                }
            }
        }
        store.onUpdate = { [historyRepository] item in
            DispatchQueue.global(qos: .utility).async {
                do {
                    try historyRepository?.update(item)
                } catch {
                    NSLog("ClipDeck failed to update persisted item: %@", String(describing: error))
                }
            }
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let isInterfaceCheck = ProcessInfo.processInfo.arguments.contains("--show-on-launch")
        NSApp.setActivationPolicy(.accessory)
        NSLog("ClipDeck application started")

        NSLog("ClipDeck creating panel controller")
        panelController = PanelController(
            store: store,
            onPaste: { [weak self] item, forcePlainText in
                self?.pasteSelection(item, forcePlainText: forcePlainText)
            },
            onManageCategories: { [weak self] in self?.showCategorySettings(selectCategories: true) },
            onSearch: { [weak self] query in self?.performSearch(query) }
        )
        NSLog("ClipDeck panel controller created")
        hotKeyController = GlobalHotKeyController { [weak self] in
            self?.toggleDeck()
        }
        NSLog("ClipDeck registering global hot key")
        hotKeyRegistration = hotKeyController.register()
        NSLog("ClipDeck global hot key registration finished")
        monitor.start()
        NSLog("ClipDeck clipboard monitor started")
        loadPersistedHistory()
        panelController.prepareForFirstPresentation()
        configureApplicationMenu()
        configureStatusItem()
        configureApplicationShortcuts()
        NSLog("ClipDeck status item configured")

        // Start WebKit after the critical Deck path is ready, so the first visit
        // to the category script editor does not pay the WebContent startup cost.
        ScriptEditorRuntime.shared.schedulePrewarm()

        if isInterfaceCheck {
            panelController.show()
        }
    }

    private func loadPersistedHistory() {
        guard let repository else { return }
        historyLoadTask?.cancel()
        historyLoadTask = Task.detached(priority: .userInitiated) { [weak self, weak store] in
            do {
                let categories = try repository.fetchCategories()
                guard !Task.isCancelled else { return }
                await MainActor.run { store?.replaceCategories(categories) }

                let priorityItems = try repository.fetchRecent(limit: 20)
                guard !Task.isCancelled else { return }
                await MainActor.run {
                    store?.replaceItems(priorityItems)
                    self?.refreshLinkMetadata(for: priorityItems)
                }

                Task.detached(priority: .background) { [weak store] in
                    do {
                        _ = try repository.deleteExpired(retentionDays: AppPreferences.retentionDays)
                        let remainingItems = try repository.fetchRecent()
                        await MainActor.run {
                            store?.expandPersistedItems(remainingItems)
                        }
                    } catch {
                        NSLog("ClipDeck failed to finish background history loading: %@", String(describing: error))
                    }
                }
            } catch {
                NSLog("ClipDeck failed to load persisted history: %@", String(describing: error))
            }
        }
    }

    private func refreshLinkMetadata(for items: [ClipboardItem]) {
        let historyStore = store
        for item in items where item.webURL != nil
            && (!item.hasFetchedLinkPreview || item.linkIconData == nil || item.title == "文本") {
            guard let url = item.webURL else { continue }
            let itemID = item.id
            Task.detached(priority: .utility) { [weak historyStore] in
                let metadata = await LinkMetadataService.fetch(for: url)
                await MainActor.run {
                    historyStore?.updateLinkMetadata(
                        itemID: itemID,
                        title: metadata.title,
                        iconData: metadata.iconData,
                        previewData: metadata.previewData
                    )
                }
            }
        }
    }

    private func showCategorySettings(selectCategories: Bool = false) {
        guard let repository else { return }
        rememberFrontmostExternalApplication()
        if let target = panelController.targetApplication,
           target.processIdentifier != ProcessInfo.processInfo.processIdentifier,
           !target.isTerminated {
            lastExternalApplication = target
        }
        if categorySettingsController == nil {
            categorySettingsController = CategorySettingsController(repository: repository) { [weak self] in
                self?.loadPersistedHistory()
            }
        }
        panelController.hide()
        if selectCategories {
            categorySettingsController?.showCategories()
        } else {
            categorySettingsController?.showGeneral()
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        searchTask?.cancel()
        historyLoadTask?.cancel()
        monitor.stop()
        hotKeyController.unregister()
        if let localKeyMonitor {
            NSEvent.removeMonitor(localKeyMonitor)
        }
        if let statusItemMouseMonitor {
            NSEvent.removeMonitor(statusItemMouseMonitor)
        }
    }

    private func pasteSelection(_ item: ClipboardItem, forcePlainText: Bool = false) {
        guard case .files = item.payload else {
            pasteValidatedSelection(item, forcePlainText: forcePlainText)
            return
        }
        filePasteController.prepare(item) { [weak self] validated in
            self?.pasteValidatedSelection(validated, forcePlainText: forcePlainText)
        }
    }

    private func pasteValidatedSelection(_ item: ClipboardItem, forcePlainText: Bool = false) {
        if panelController.targetsClipDeck,
           categorySettingsController?.isVisible == true {
            let didWrite = pasteService.write(
                item,
                to: .general,
                preserveFormatting: forcePlainText
                    ? false
                    : UserDefaults.standard.bool(forKey: PasteService.preserveFormattingKey)
            )
            guard didWrite else { return }
            monitor.suppressCurrentPasteboardChange()
            panelController.hide()
            categorySettingsController?.restoreFocusAndPaste()
            feedbackSoundPlayer.playPaste()
            return
        }

        let target = panelController.targetApplication ?? lastExternalApplication
        let result = pasteService.paste(item, into: target, forcePlainText: forcePlainText) { [weak self] in
            self?.categorySettingsController?.hide()
            self?.panelController.hide()
        }
        monitor.suppressCurrentPasteboardChange()

        if case let .copiedOnly(reason) = result {
            let alert = NSAlert()
            alert.messageText = "内容已复制到剪贴板"
            let canOpenSettings: Bool
            switch reason {
            case .accessibilityPermission:
                alert.informativeText = "当前运行的 ClipDeck 未被 macOS 识别为已授权。若列表中的开关已经打开，请关闭后重新打开；仍无效时请移除旧条目，再添加当前版本。"
                alert.addButton(withTitle: "打开辅助功能设置")
                alert.addButton(withTitle: "稍后")
                canOpenSettings = true
            case .targetUnavailable:
                alert.informativeText = "未找到呼出 Deck 前使用的应用，请手动按 Command + V。"
                alert.addButton(withTitle: "知道了")
                canOpenSettings = false
            }
            if alert.runModal() == .alertFirstButtonReturn,
               canOpenSettings,
               let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") {
                NSWorkspace.shared.open(url)
            }
        }
    }

    private func configureStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        guard let button = statusItem.button else { return }
        button.image = makeStatusBarIcon()
        button.imageScaling = .scaleProportionallyDown
        button.imagePosition = .imageOnly
        button.target = nil
        button.action = nil
        button.toolTip = "左键显示 ClipDeck，右键打开菜单"
        if case let .failed(status) = hotKeyRegistration {
            button.toolTip = "全局快捷键注册失败：\(status)"
        }

        statusItemMouseMonitor = NSEvent.addLocalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown]
        ) { [weak self, weak button] event in
            guard let self,
                  let button,
                  event.window === button.window,
                  button.bounds.contains(button.convert(event.locationInWindow, from: nil)) else {
                return event
            }

            if event.type == .rightMouseDown {
                NSMenu.popUpContextMenu(self.makeStatusMenu(), with: event, for: button)
                return nil
            }

            // Command-drag is owned by macOS so the status item can be repositioned.
            guard !event.modifierFlags.contains(.command) else {
                return event
            }

            // The status button can reclaim key status while its mouse-down is
            // still being dispatched. Present on the next run-loop turn so the
            // Deck remains the focused window after a left click.
            let targetsClipDeck = self.categorySettingsController?.ownsFocusedWindow == true
            if targetsClipDeck {
                self.categorySettingsController?.rememberFocusForDeck()
            }
            DispatchQueue.main.async { [weak self] in
                self?.toggleDeckFromStatusItem(targetsClipDeck: targetsClipDeck)
            }
            return nil
        }
    }

    private func configureApplicationMenu() {
        let mainMenu = NSMenu()

        let applicationMenuItem = NSMenuItem()
        mainMenu.addItem(applicationMenuItem)
        let applicationMenu = NSMenu(title: "ClipDeck")
        applicationMenu.addItem(
            withTitle: "ClipDeck 设置…",
            action: #selector(openCategorySettings),
            keyEquivalent: ","
        ).target = self
        applicationMenu.addItem(.separator())
        applicationMenu.addItem(
            withTitle: "退出 ClipDeck",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q"
        ).target = NSApp
        applicationMenuItem.submenu = applicationMenu

        let editMenuItem = NSMenuItem()
        mainMenu.addItem(editMenuItem)
        let editMenu = NSMenu(title: "编辑")
        editMenu.addItem(withTitle: "撤销", action: Selector(("undo:")), keyEquivalent: "z")
        let redoItem = editMenu.addItem(
            withTitle: "重做",
            action: Selector(("redo:")),
            keyEquivalent: "z"
        )
        redoItem.keyEquivalentModifierMask = [.command, .shift]
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "剪切", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "复制", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editMenuItem.submenu = editMenu

        NSApp.mainMenu = mainMenu
    }

    private func makeStatusBarIcon() -> NSImage? {
        let logicalSize = NSSize(width: 18, height: 18)
        if let url = statusBarIconURL(),
           let source = NSImage(contentsOf: url) {
            // AppKit's SVG representation can fail to redraw after an Xcode
            // relaunch. Rasterize it once and retain the pixels on the status
            // item instead of relying on the lazy SVG representation.
            let scale = max(2, NSScreen.main?.backingScaleFactor ?? 2)
            let pixelWidth = Int(logicalSize.width * scale)
            let pixelHeight = Int(logicalSize.height * scale)
            if let representation = NSBitmapImageRep(
                bitmapDataPlanes: nil,
                pixelsWide: pixelWidth,
                pixelsHigh: pixelHeight,
                bitsPerSample: 8,
                samplesPerPixel: 4,
                hasAlpha: true,
                isPlanar: false,
                colorSpaceName: .deviceRGB,
                bytesPerRow: 0,
                bitsPerPixel: 0
            ), let context = NSGraphicsContext(bitmapImageRep: representation) {
                NSGraphicsContext.saveGraphicsState()
                NSGraphicsContext.current = context
                context.imageInterpolation = .high
                source.draw(
                    in: NSRect(x: 0, y: 0, width: pixelWidth, height: pixelHeight),
                    from: .zero,
                    operation: .sourceOver,
                    fraction: 1
                )
                NSGraphicsContext.restoreGraphicsState()

                representation.size = logicalSize
                let image = NSImage(size: logicalSize)
                image.addRepresentation(representation)
                image.isTemplate = false
                return image
            }
        }

        let fallback = NSImage(
            systemSymbolName: "chevron.left.forwardslash.chevron.right",
            accessibilityDescription: "ClipDeck"
        )
        fallback?.size = logicalSize
        fallback?.isTemplate = true
        return fallback
    }

    private func statusBarIconURL() -> URL? {
        if let bundled = Bundle.main.url(forResource: "clipdeck", withExtension: "svg") {
            return bundled
        }

        // `swift run` does not allow Package.swift to declare a resource above
        // the package root. Keep development builds pointed at the same shared
        // asset that Xcode and Linux package directly.
        let sharedAsset = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // App
            .deletingLastPathComponent() // ClipDeck
            .deletingLastPathComponent() // Sources
            .deletingLastPathComponent() // mac
            .deletingLastPathComponent() // repository root
            .appendingPathComponent("shared/assets/clipdeck.svg")
        return FileManager.default.fileExists(atPath: sharedAsset.path) ? sharedAsset : nil
    }

    private func toggleDeckFromStatusItem(targetsClipDeck: Bool) {
        NSLog("ClipDeck panel toggled from status item left click")
        toggleDeck(targetsClipDeck: targetsClipDeck)
    }

    private func configureApplicationShortcuts() {
        localKeyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            let modifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
            guard event.keyCode == 43, modifiers == .command else { return event }
            self?.showCategorySettings()
            return nil
        }
    }

    private func makeStatusMenu() -> NSMenu {
        let menu = NSMenu()
        let settingsItem = menu.addItem(withTitle: "设置…", action: #selector(openCategorySettings), keyEquivalent: ",")
        settingsItem.target = self
        settingsItem.image = NSImage(systemSymbolName: "gearshape", accessibilityDescription: "设置")
        menu.addItem(.separator())
        let quitItem = menu.addItem(withTitle: "退出 ClipDeck", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        quitItem.target = NSApp
        quitItem.image = NSImage(systemSymbolName: "power", accessibilityDescription: "退出")
        return menu
    }

    @objc private func openCategorySettings() {
        showCategorySettings()
    }

    private func reloadGlobalShortcut() {
        guard hotKeyController != nil else { return }
        hotKeyRegistration = hotKeyController.register()
        if case let .failed(status) = hotKeyRegistration {
            statusItem?.button?.toolTip = "全局快捷键注册失败：\(status)"
        } else {
            statusItem?.button?.toolTip = "左键显示 ClipDeck，右键打开菜单"
        }
    }

    private func toggleDeck(targetsClipDeck requestedTarget: Bool? = nil) {
        statusItem?.menu?.cancelTracking()

        if panelController.isVisible {
            panelController.hide()
            return
        }

        let targetsClipDeck = requestedTarget
            ?? (categorySettingsController?.ownsFocusedWindow == true)
        if targetsClipDeck {
            // Keep Settings visible and keep ClipDeck active. The nonactivating
            // Deck becomes key while the settings window remains ordered in front.
            categorySettingsController?.rememberFocusForDeck()
            NSApp.activate()
        } else {
            rememberFrontmostExternalApplication()
        }
        panelController.show(
            targetApplication: targetsClipDeck ? nil : lastExternalApplication,
            targetsClipDeck: targetsClipDeck
        )
    }

    private func rememberFrontmostExternalApplication() {
        guard let application = NSWorkspace.shared.frontmostApplication,
              application.processIdentifier != ProcessInfo.processInfo.processIdentifier,
              !application.isTerminated else { return }
        lastExternalApplication = application
    }

    private func performSearch(_ query: String) {
        searchTask?.cancel()
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let localResults = store.applyLocalSearch(trimmed)
        guard !trimmed.isEmpty, let repository else {
            return
        }

        searchTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(120))
                let results = try await repository.searchTextAsync(trimmed)
                guard !Task.isCancelled,
                      let self,
                      self.store.isSearchActive,
                      self.store.searchQuery.trimmingCharacters(in: .whitespacesAndNewlines) == trimmed else { return }
                self.store.replaceSearchResults(Self.mergeSearchResults(localResults, results))
            } catch {
                guard !Task.isCancelled else { return }
                NSLog("ClipDeck search failed: %@", String(describing: error))
            }
        }
    }

    private static func mergeSearchResults(
        _ localResults: [ClipboardItem],
        _ databaseResults: [ClipboardItem]
    ) -> [ClipboardItem] {
        var seenDatabaseIDs = Set<Int64>()
        var seenItemIDs = Set<UUID>()
        return (localResults + databaseResults).filter { item in
            if let databaseID = item.databaseID {
                return seenDatabaseIDs.insert(databaseID).inserted
            }
            return seenItemIDs.insert(item.id).inserted
        }
    }

}
