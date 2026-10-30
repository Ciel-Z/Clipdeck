import AppKit
import ApplicationServices

struct FilePasteFeedback: Equatable, Sendable {
    enum Phase: Equatable, Sendable {
        case checking
        case unavailable(String)
        case partial(available: [ClipboardFileReference], unavailable: [ClipboardFileReference])
    }
    let itemID: UUID
    let requestID: UUID
    let phase: Phase
}

enum FileReferenceService {
    struct Availability: Sendable {
        let available: [ClipboardFileReference]
        let unavailable: [ClipboardFileReference]
    }

    // Called on a worker queue only. Mounted volumes can block even metadata I/O.
    static func check(_ files: [ClipboardFileReference]) -> Availability {
        var available: [ClipboardFileReference] = []
        var unavailable: [ClipboardFileReference] = []
        for file in files {
            if FileManager.default.isReadableFile(atPath: file.path) {
                available.append(file)
            } else {
                unavailable.append(file)
            }
        }
        return Availability(available: available, unavailable: unavailable)
    }
}

@MainActor
final class FilePasteController {
    private let store: InMemoryHistoryStore
    private let pasteboard: NSPasteboard
    private let isPresented: () -> Bool
    private let checker: @Sendable ([ClipboardFileReference]) -> FileReferenceService.Availability
    private let timeout: TimeInterval
    private let queue = DispatchQueue(label: "app.clipdeck.file-preflight", qos: .userInitiated)

    init(
        store: InMemoryHistoryStore,
        pasteboard: NSPasteboard = .general,
        timeout: TimeInterval = 2,
        checker: @escaping @Sendable ([ClipboardFileReference]) -> FileReferenceService.Availability = FileReferenceService.check,
        isPresented: @escaping () -> Bool
    ) {
        self.store = store
        self.pasteboard = pasteboard
        self.timeout = timeout
        self.checker = checker
        self.isPresented = isPresented
    }

    func prepare(_ item: ClipboardItem, onReady: @escaping @MainActor (ClipboardItem) -> Void) {
        guard case let .files(originalFiles) = item.payload else { return }
        if store.filePasteFeedback?.itemID == item.id,
           store.filePasteFeedback?.phase == .checking { return }
        var files = originalFiles
        if let feedback = store.filePasteFeedback, feedback.itemID == item.id,
           case let .partial(available, _) = feedback.phase {
            files = available
        }
        let requestID = UUID()
        let changeCount = pasteboard.changeCount
        store.filePasteFeedback = FilePasteFeedback(itemID: item.id, requestID: requestID, phase: .checking)
        let requestedFiles = files
        let check = checker
        queue.async {
            let result = check(requestedFiles)
            Task { @MainActor [weak self] in
                guard let self,
                      self.store.filePasteFeedback?.requestID == requestID,
                      self.store.filePasteFeedback?.phase == .checking,
                      self.isPresented(), self.store.selectedItemID == item.id else { return }
                guard self.pasteboard.changeCount == changeCount else {
                    self.store.filePasteFeedback = FilePasteFeedback(
                        itemID: item.id, requestID: requestID,
                        phase: .unavailable("剪贴板内容已更新，请重新尝试粘贴")
                    )
                    return
                }
                if result.available.isEmpty {
                    self.store.filePasteFeedback = FilePasteFeedback(
                        itemID: item.id, requestID: requestID,
                        phase: .unavailable("无法粘贴，源文件不可访问")
                    )
                } else if !result.unavailable.isEmpty {
                    self.store.filePasteFeedback = FilePasteFeedback(
                        itemID: item.id, requestID: requestID,
                        phase: .partial(available: result.available, unavailable: result.unavailable)
                    )
                } else {
                    self.store.filePasteFeedback = nil
                    onReady(ClipboardItem(
                        id: item.id, databaseID: item.databaseID, title: item.title,
                        payload: .files(result.available), sourceApplication: item.sourceApplication,
                        copiedAt: item.copiedAt
                    ))
                }
            }
        }
        // Ignore a late result after timeout; it must never overwrite a new copy.
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { [weak self] in
            guard let self, self.store.filePasteFeedback?.requestID == requestID,
                  self.store.filePasteFeedback?.phase == .checking else { return }
            self.store.filePasteFeedback = FilePasteFeedback(
                itemID: item.id, requestID: requestID,
                phase: .unavailable("检查超时，请确认磁盘或网络挂载后重试")
            )
        }
    }
}

@MainActor
final class FeedbackSoundPlayer {
    private let copySound = NSSound(named: NSSound.Name("Tink"))
    private let pasteSound = NSSound(named: NSSound.Name("Pop"))

    func playCopy() {
        copySound?.stop()
        copySound?.play()
    }

    func playPaste() {
        pasteSound?.stop()
        pasteSound?.play()
    }
}

@MainActor
final class PasteService {
    static let preserveFormattingKey = "paste.preserveFormatting"
    enum CopiedOnlyReason {
        case accessibilityPermission
        case targetUnavailable
    }

    enum Result {
        case pasted
        case copiedOnly(CopiedOnlyReason)
        case failed
    }

    private let onPasteKeystroke: () -> Void
    private var activationTimeoutTask: Task<Void, Never>?

    // An active process is not necessarily the keyboard owner: the nonactivating
    // Deck can still hold key focus until AppKit finishes ordering it out.
    // Trigger actions start on keyDown / gesture recognition. Delivery depends
    // only on focus handoff, never on releasing the triggering keys or mouse.
    static func canDeliverPaste(targetIsFrontmost: Bool, deckHasKeyWindow: Bool) -> Bool {
        targetIsFrontmost && !deckHasKeyWindow
    }

    init(onPasteKeystroke: @escaping () -> Void = {}) {
        self.onPasteKeystroke = onPasteKeystroke
    }

    func paste(
        _ item: ClipboardItem,
        into application: NSRunningApplication?,
        forcePlainText: Bool = false,
        dismissDeck: () -> Void
    ) -> Result {
        let pasteboard = NSPasteboard.general
        let didWrite = write(
            item,
            to: pasteboard,
            preserveFormatting: forcePlainText
                ? false
                : UserDefaults.standard.bool(forKey: Self.preserveFormattingKey)
        )
        guard didWrite else { return .failed }

        dismissDeck()
        guard let application,
              application.processIdentifier != ProcessInfo.processInfo.processIdentifier,
              !application.isTerminated else {
            NSLog("ClipDeck could not paste directly because the target application is unavailable")
            return .copiedOnly(.targetUnavailable)
        }

        guard AXIsProcessTrusted() else { return .copiedOnly(.accessibilityPermission) }

        pasteAsSoonAsActive(application)
        return .pasted
    }

    func write(
        _ item: ClipboardItem,
        to pasteboard: NSPasteboard,
        preserveFormatting: Bool
    ) -> Bool {
        if case let .files(files) = item.payload, files.isEmpty { return false }
        pasteboard.clearContents()
        switch item.payload {
        case let .text(plainText, richText):
            let didWrite = pasteboard.setString(plainText, forType: .string)
            if preserveFormatting, let richText {
                _ = pasteboard.setData(richText, forType: .rtf)
            }
            return didWrite
        case let .image(data, _, _, format):
            if item.imageAngle != 0,
               let rendered = ImageRotation.render(data, angle: item.imageAngle) {
                return pasteboard.setData(rendered.data, forType: .png)
            }
            let type: NSPasteboard.PasteboardType = format == .png ? .png : .tiff
            return pasteboard.setData(data, forType: type)
        case let .files(files):
            // Finder supplies file-reference URLs, not path URLs. Preserve the
            // CFURL string directly: bridging through Swift URL/NSURL can resolve
            // it back into a path URL and lose the original representation.
            let entries = files.map { file in
                let entry = NSPasteboardItem()
                entry.setString(Self.filePasteboardURLString(file.url), forType: .fileURL)
                return entry
            }
            return pasteboard.writeObjects(entries)
        }
    }

    static func filePasteboardURLString(_ url: URL) -> String {
        if let reference = CFURLCreateFileReferenceURL(nil, url as CFURL, nil)?.takeRetainedValue() {
            return CFURLGetString(reference) as String
        }
        // Some volumes do not support reference URLs. Preflight remains the
        // authority for accessibility; retain the native path URL as fallback.
        return url.absoluteString
    }

    private func pasteAsSoonAsActive(_ application: NSRunningApplication) {
        clearPendingPaste()
        let changeCount = NSPasteboard.general.changeCount
        let started = ProcessInfo.processInfo.systemUptime
        application.activate()
        activationTimeoutTask = Task { @MainActor [weak self, weak application] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .milliseconds(30)) } catch { return }
                guard let self, let application, !application.isTerminated else { return }
                guard NSPasteboard.general.changeCount == changeCount else {
                    NSLog("ClipDeck paste cancelled: clipboard changed before focus returned")
                    return
                }
                let elapsed = ProcessInfo.processInfo.systemUptime - started
                let frontmost = NSWorkspace.shared.frontmostApplication?.processIdentifier
                if Self.canDeliverPaste(
                    targetIsFrontmost: application.isActive && frontmost == application.processIdentifier,
                    deckHasKeyWindow: NSApp.keyWindow != nil
                ) {
                    // Use the normal session keyboard route, including the target's
                    // WebView/Electron input dispatch, not process-only injection.
                    if Self.sendPasteKeystroke() {
                        NSLog("ClipDeck paste dispatched to %@ after %.0f ms; types=%@",
                              application.localizedName ?? "unknown", elapsed * 1000,
                              NSPasteboard.general.types?.map(\.rawValue).joined(separator: ",") ?? "")
                        self.onPasteKeystroke()
                    }
                    return
                }
                if elapsed >= 2 {
                    NSLog("ClipDeck paste not sent: focus not ready; target=%@; frontmost=%d; keyWindow=%@. Clipboard retained for manual paste.",
                          application.localizedName ?? "unknown", frontmost ?? 0,
                          NSApp.keyWindow?.title ?? "none")
                    return
                }
            }
        }
    }

    private func clearPendingPaste() {
        activationTimeoutTask?.cancel()
        activationTimeoutTask = nil
    }

    @discardableResult
    private static func sendPasteKeystroke() -> Bool {
        guard let source = CGEventSource(stateID: .hidSystemState),
              let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: true),
              let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: false) else {
            return false
        }
        keyDown.flags = .maskCommand
        keyUp.flags = .maskCommand
        keyDown.post(tap: .cghidEventTap)
        keyUp.post(tap: .cghidEventTap)
        return true
    }
}
