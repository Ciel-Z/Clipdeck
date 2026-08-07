import AppKit
import ApplicationServices

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

        application.activate()
        guard AXIsProcessTrusted() else { return .copiedOnly(.accessibilityPermission) }

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.04) {
            Self.sendPasteWhenActive(application, remainingAttempts: 15)
        }
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
            let type: NSPasteboard.PasteboardType = format == .png ? .png : .tiff
            return pasteboard.setData(data, forType: type)
        }
    }

    private static func sendPasteWhenActive(
        _ application: NSRunningApplication,
        remainingAttempts: Int
    ) {
        guard !application.isTerminated else { return }
        guard application.isActive else {
            guard remainingAttempts > 0 else {
                NSLog("ClipDeck target application did not become active in time: %@", application.localizedName ?? "unknown")
                return
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.04) {
                sendPasteWhenActive(application, remainingAttempts: remainingAttempts - 1)
            }
            return
        }

        let processIdentifier = application.processIdentifier
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.06) {
            guard application.isActive, !application.isTerminated else { return }
            sendPasteKeystroke(to: processIdentifier)
        }
    }

    private static func sendPasteKeystroke(to processIdentifier: pid_t) {
        guard let source = CGEventSource(stateID: .hidSystemState),
              let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: true),
              let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: false) else {
            return
        }
        keyDown.flags = .maskCommand
        keyUp.flags = .maskCommand
        keyDown.postToPid(processIdentifier)
        keyUp.postToPid(processIdentifier)
    }
}
