import AppKit

@MainActor
final class ClipboardMonitor {
    private let store: InMemoryHistoryStore
    private let repository: HistoryRepository?
    private let classifier: ClassificationService?
    private let reader = ClipboardReader()
    private let processingQueue = DispatchQueue(label: "app.clipdeck.clipboard-reader", qos: .utility)
    private var timer: Timer?
    private var lastChangeCount = NSPasteboard.general.changeCount
    private var ignoredChangeCount: Int?

    init(store: InMemoryHistoryStore, repository: HistoryRepository? = nil, classifier: ClassificationService? = nil) {
        self.store = store
        self.repository = repository
        self.classifier = classifier
    }

    func start() {
        guard timer == nil else { return }
        let timer = Timer(timeInterval: 0.4, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.poll()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    func suppressCurrentPasteboardChange() {
        let count = NSPasteboard.general.changeCount
        ignoredChangeCount = count
        lastChangeCount = count
    }

    private func poll() {
        let pasteboard = NSPasteboard.general
        let changeCount = pasteboard.changeCount
        guard changeCount != lastChangeCount else { return }
        lastChangeCount = changeCount
        if ignoredChangeCount == changeCount {
            ignoredChangeCount = nil
            return
        }

        let sourceApplication = Self.frontmostApplication()
        processingQueue.async { [reader, repository, classifier] in
            guard let payload = reader.read() else { return }
            let capturedItem = ClipboardItem(payload: payload, sourceApplication: sourceApplication)
            let item: ClipboardItem
            do {
                let savedItem = try repository?.save(capturedItem) ?? capturedItem
                item = try classifier?.classify(savedItem) ?? savedItem
            } catch {
                NSLog("ClipDeck failed to persist clipboard item: %@", String(describing: error))
                item = capturedItem
            }
            Task { @MainActor [weak self] in
                self?.store.insert(item)
            }
        }
    }

    private static func frontmostApplication() -> SourceApplication {
        guard let application = NSWorkspace.shared.frontmostApplication else {
            return SourceApplication(bundleIdentifier: nil, name: "Unknown", iconData: nil)
        }
        let iconData = application.bundleURL
            .map { NSWorkspace.shared.icon(forFile: $0.path) }
            .flatMap { $0.tiffRepresentation }
        return SourceApplication(
            bundleIdentifier: application.bundleIdentifier,
            name: application.localizedName ?? "Unknown",
            iconData: iconData
        )
    }
}
