import AppKit
import ApplicationServices

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
    private var activationObserver: NSObjectProtocol?
    private var activationTimeoutTask: Task<Void, Never>?
    private var pendingApplication: NSRunningApplication?

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
        }
    }

    private func pasteAsSoonAsActive(_ application: NSRunningApplication) {
        clearPendingPaste()
        pendingApplication = application

        let center = NSWorkspace.shared.notificationCenter
        let targetProcessIdentifier = application.processIdentifier
        activationObserver = center.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            guard let activated = notification.userInfo?[NSWorkspace.applicationUserInfoKey]
                    as? NSRunningApplication,
                  activated.processIdentifier == targetProcessIdentifier else { return }
            MainActor.assumeIsolated {
                self?.sendPendingPasteIfPossible()
            }
        }

        application.activate()
        if application.isActive {
            sendPendingPasteIfPossible()
            return
        }

        activationTimeoutTask = Task { @MainActor [weak self, weak application] in
            try? await Task.sleep(for: .milliseconds(750))
            guard !Task.isCancelled else { return }
            if application?.isActive == true {
                self?.sendPendingPasteIfPossible()
            } else {
                NSLog(
                    "ClipDeck target application did not become active in time: %@",
                    application?.localizedName ?? "unknown"
                )
                self?.clearPendingPaste()
            }
        }
    }

    private func sendPendingPasteIfPossible() {
        guard let application = pendingApplication,
              application.isActive,
              !application.isTerminated else { return }
        if Self.sendPasteKeystroke(to: application.processIdentifier) {
            onPasteKeystroke()
        }
        clearPendingPaste()
    }

    private func clearPendingPaste() {
        activationTimeoutTask?.cancel()
        activationTimeoutTask = nil
        if let activationObserver {
            NSWorkspace.shared.notificationCenter.removeObserver(activationObserver)
            self.activationObserver = nil
        }
        pendingApplication = nil
    }

    @discardableResult
    private static func sendPasteKeystroke(to processIdentifier: pid_t) -> Bool {
        guard let source = CGEventSource(stateID: .hidSystemState),
              let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: true),
              let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: false) else {
            return false
        }
        keyDown.flags = .maskCommand
        keyUp.flags = .maskCommand
        keyDown.postToPid(processIdentifier)
        keyUp.postToPid(processIdentifier)
        return true
    }
}
