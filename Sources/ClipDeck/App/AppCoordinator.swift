import AppKit

@MainActor
final class AppCoordinator: NSObject, NSApplicationDelegate {
    private let store = InMemoryHistoryStore()
    private let repository: HistoryRepository?
    private let classifier: ClassificationService?
    private lazy var monitor = ClipboardMonitor(store: store, repository: repository, classifier: classifier)
    private let pasteService = PasteService()
    private var panelController: PanelController!
    private var hotKeyController: GlobalHotKeyController!
    private var statusItem: NSStatusItem!
    private var categorySettingsController: CategorySettingsController?
    private var lastExternalApplication: NSRunningApplication?
    private var searchTask: Task<Void, Never>?
    private var hotKeyRegistration: GlobalHotKeyController.RegistrationResult = .registered

    override init() {
        repository = try? HistoryRepository(database: ClipDeckDatabase.applicationDatabase())
        if let repository {
            try? repository.ensureDefaultCategories()
            classifier = ClassificationService(repository: repository)
        } else {
            classifier = nil
        }
        super.init()
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
        NSApp.setActivationPolicy(isInterfaceCheck ? .regular : .accessory)
        NSLog("ClipDeck application started")

        NSLog("ClipDeck creating panel controller")
        panelController = PanelController(
            store: store,
            onPaste: { [weak self] item in self?.pasteSelection(item) },
            onManageCategories: { [weak self] in self?.showCategorySettings() },
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
        configureStatusItem()
        NSLog("ClipDeck status item configured")

        if isInterfaceCheck {
            panelController.show()
        }
    }

    private func loadPersistedHistory() {
        guard let repository else { return }
        Task.detached(priority: .utility) { [weak store] in
            do {
                _ = try repository.deleteExpired()
                let items = try repository.fetchRecent()
                let categories = try repository.fetchCategories()
                await MainActor.run {
                    store?.replaceCategories(categories)
                    store?.replaceItems(items)
                }
            } catch {
                NSLog("ClipDeck failed to load persisted history: %@", String(describing: error))
            }
        }
    }

    private func showCategorySettings() {
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
        categorySettingsController?.show()
    }

    func applicationWillTerminate(_ notification: Notification) {
        searchTask?.cancel()
        monitor.stop()
        hotKeyController.unregister()
    }

    private func pasteSelection(_ item: ClipboardItem) {
        let target = panelController.targetApplication ?? lastExternalApplication
        let result = pasteService.paste(item, into: target) { [weak self] in
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
        statusItem.button?.image = NSImage(systemSymbolName: "clipboard", accessibilityDescription: "ClipDeck")
        let menu = NSMenu()
        menu.addItem(withTitle: "显示 ClipDeck", action: #selector(togglePanel), keyEquivalent: "")
        menu.addItem(withTitle: "设置…", action: #selector(openCategorySettings), keyEquivalent: ",")
        switch hotKeyRegistration {
        case .registered:
            let shortcutItem = NSMenuItem(title: "快捷键：Command + Shift + V", action: nil, keyEquivalent: "")
            shortcutItem.isEnabled = false
            menu.addItem(shortcutItem)
        case let .failed(status):
            let errorItem = NSMenuItem(title: "快捷键注册失败（\(status)）", action: nil, keyEquivalent: "")
            errorItem.isEnabled = false
            menu.addItem(errorItem)
            statusItem.button?.toolTip = "全局快捷键注册失败：\(status)"
        }
        menu.addItem(.separator())
        menu.addItem(withTitle: "退出", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        statusItem.menu = menu
    }

    @objc private func togglePanel() {
        NSLog("ClipDeck panel toggled")
        toggleDeck()
    }

    @objc private func openCategorySettings() {
        showCategorySettings()
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
                let results = try await Task.detached { try repository.searchText(trimmed) }.value
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
