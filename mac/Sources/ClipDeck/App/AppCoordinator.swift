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
        configureStatusItem()
        configureApplicationShortcuts()
        NSLog("ClipDeck status item configured")

        if isInterfaceCheck {
            panelController.show()
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        panelController?.focusVisiblePanel()
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
            DispatchQueue.main.async { [weak self] in
                self?.toggleDeckFromStatusItem()
            }
            return nil
        }
    }

    private func makeStatusBarIcon() -> NSImage? {
        let logicalSize = NSSize(width: 18, height: 18)
        if let url = Bundle.main.url(forResource: "clipdeck", withExtension: "svg"),
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

    private func toggleDeckFromStatusItem() {
        NSLog("ClipDeck panel toggled from status item left click")
        toggleDeck()
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

    private func toggleDeck() {
        statusItem?.menu?.cancelTracking()

        if panelController.isVisible {
            panelController.hide()
            return
        }

        rememberFrontmostExternalApplication()
        categorySettingsController?.hide()
        panelController.show(targetApplication: lastExternalApplication)
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
