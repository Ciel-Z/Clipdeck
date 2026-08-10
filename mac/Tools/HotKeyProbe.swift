import AppKit
import Carbon

guard let source = CGEventSource(stateID: .hidSystemState),
      let keyDown = CGEvent(
        keyboardEventSource: source,
        virtualKey: CGKeyCode(kVK_ANSI_V),
        keyDown: true
      ),
      let keyUp = CGEvent(
        keyboardEventSource: source,
        virtualKey: CGKeyCode(kVK_ANSI_V),
        keyDown: false
      ) else {
    fatalError("Unable to create keyboard event")
}

keyDown.flags = [.maskCommand, .maskShift]
keyUp.flags = [.maskCommand, .maskShift]
keyDown.post(tap: .cghidEventTap)
keyUp.post(tap: .cghidEventTap)
