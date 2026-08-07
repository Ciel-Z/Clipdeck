import Carbon
import OSLog

private let hotKeyEventHandler: EventHandlerUPP = { _, event, userData in
    guard let event,
          let userData,
          GetEventClass(event) == OSType(kEventClassKeyboard),
          GetEventKind(event) == UInt32(kEventHotKeyPressed) else {
        return OSStatus(eventNotHandledErr)
    }

    let controller = Unmanaged<GlobalHotKeyController>
        .fromOpaque(userData)
        .takeUnretainedValue()
    DispatchQueue.main.async {
        MainActor.assumeIsolated {
            controller.performAction()
        }
    }
    return noErr
}

@MainActor
final class GlobalHotKeyController {
    enum RegistrationResult: Equatable {
        case registered
        case failed(status: OSStatus)
    }

    private var hotKey: EventHotKeyRef?
    private var eventHandler: EventHandlerRef?
    private let action: () -> Void
    private let logger = Logger(subsystem: "app.clipdeck", category: "GlobalHotKey")

    init(action: @escaping () -> Void) {
        self.action = action
    }

    @discardableResult
    func register() -> RegistrationResult {
        unregister()

        var eventType = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed)
        )
        let handlerStatus = InstallEventHandler(
            GetApplicationEventTarget(),
            hotKeyEventHandler,
            1,
            &eventType,
            Unmanaged.passUnretained(self).toOpaque(),
            &eventHandler
        )
        guard handlerStatus == noErr else {
            logger.error("Unable to install hot key event handler: \(handlerStatus)")
            NSLog("ClipDeck hot key event handler failed: %d", handlerStatus)
            return .failed(status: handlerStatus)
        }

        let identifier = EventHotKeyID(signature: Self.signature, id: 1)
        let registrationStatus = RegisterEventHotKey(
            UInt32(kVK_ANSI_V),
            UInt32(cmdKey | shiftKey),
            identifier,
            GetApplicationEventTarget(),
            0,
            &hotKey
        )
        guard registrationStatus == noErr else {
            logger.error("Unable to register Command-Shift-V: \(registrationStatus)")
            NSLog("ClipDeck hot key registration failed: %d", registrationStatus)
            if let eventHandler { RemoveEventHandler(eventHandler) }
            eventHandler = nil
            return .failed(status: registrationStatus)
        }
        logger.info("Registered global hot key Command-Shift-V")
        NSLog("ClipDeck registered global hot key Command-Shift-V")
        return .registered
    }

    func unregister() {
        if let hotKey { UnregisterEventHotKey(hotKey) }
        if let eventHandler { RemoveEventHandler(eventHandler) }
        hotKey = nil
        eventHandler = nil
    }

    fileprivate func performAction() {
        logger.debug("Global hot key pressed")
        NSLog("ClipDeck global hot key pressed")
        action()
    }

    private static let signature: OSType = {
        let bytes = Array("CLPD".utf8)
        return bytes.reduce(0) { ($0 << 8) | OSType($1) }
    }()
}
