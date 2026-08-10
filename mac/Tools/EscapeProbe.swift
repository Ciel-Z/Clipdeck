import AppKit

NSRunningApplication
    .runningApplications(withBundleIdentifier: "app.clipdeck.local")
    .first?
    .activate()
Thread.sleep(forTimeInterval: 0.15)

guard let source = CGEventSource(stateID: .hidSystemState),
      let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 53, keyDown: true),
      let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 53, keyDown: false) else {
    fatalError("Unable to create Escape key event")
}

keyDown.post(tap: .cghidEventTap)
keyUp.post(tap: .cghidEventTap)
