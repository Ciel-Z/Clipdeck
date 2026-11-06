import AppKit
import XCTest
@testable import ClipDeck

@MainActor
final class PanelControllerTests: XCTestCase {
    func testDeckIsAKeyCapableNonactivatingPanel() throws {
        let controller = PanelController(
            store: InMemoryHistoryStore(),
            onPaste: { _, _ in },
            onManageCategories: {},
            onSearch: { _ in }
        )
        let panel = try XCTUnwrap(controller.window as? NSPanel)

        XCTAssertTrue(panel.styleMask.contains(.nonactivatingPanel))
        XCTAssertTrue(panel.canBecomeKey)
        XCTAssertFalse(panel.becomesKeyOnlyIfNeeded)
    }

    func testTypingIntoTheDeckKeepsTheFirstLetterOnceTheSearchFieldTakesFocus() throws {
        let store = InMemoryHistoryStore()
        store.insert(ClipboardItem(
            payload: .text(plainText: "index.html", rtfData: nil),
            sourceApplication: SourceApplication(bundleIdentifier: nil, name: "Editor", iconData: nil)
        ))
        let controller = PanelController(store: store, onPaste: { _, _ in }, onManageCategories: {}, onSearch: { _ in })
        controller.show(targetApplication: nil, targetsClipDeck: true)
        defer { controller.hide() }
        let panel = try XCTUnwrap(controller.window)
        panel.makeKey()

        func type(_ characters: String, keyCode: UInt16) throws {
            let event = try XCTUnwrap(NSEvent.keyEvent(
                with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                windowNumber: panel.windowNumber, context: nil, characters: characters,
                charactersIgnoringModifiers: characters, isARepeat: false, keyCode: keyCode
            ))
            panel.sendEvent(event)
        }
        // The first letter starts the search; then the field takes focus, and
        // AppKit selects its text, before the next letter arrives. SwiftUI does
        // not move focus inside a test run, so the test focuses the field itself.
        try type("i", keyCode: 34)
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        func textFields(in view: NSView) -> [NSTextField] {
            view.subviews.flatMap { ($0 as? NSTextField).map { [$0] } ?? textFields(in: $0) }
        }
        let field = try XCTUnwrap(textFields(in: try XCTUnwrap(panel.contentView)).first { $0.isEditable })
        XCTAssertTrue(panel.makeFirstResponder(field))
        store.setSearchFieldFocused(true)
        let editor = try XCTUnwrap(panel.firstResponder as? NSTextView)
        XCTAssertEqual(editor.string, "i")
        for (character, keyCode) in [("n", UInt16(45)), ("d", 2), ("e", 14), ("x", 7)] {
            try type(character, keyCode: keyCode)
        }
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        XCTAssertEqual(store.searchQuery, "index")
    }

    func testDeckCanTargetClipDeckWithoutReusingAnExternalApplication() {
        let controller = PanelController(
            store: InMemoryHistoryStore(),
            onPaste: { _, _ in },
            onManageCategories: {},
            onSearch: { _ in }
        )

        controller.show(
            targetApplication: NSWorkspace.shared.frontmostApplication,
            targetsClipDeck: true
        )
        defer { controller.hide() }

        XCTAssertTrue(controller.targetsClipDeck)
        XCTAssertNil(controller.targetApplication)
        XCTAssertTrue(controller.isVisible)
    }

    func testResizeHandleCoversTopEdgeAndTracksWithoutKeyWindow() throws {
        let controller = PanelController(
            store: InMemoryHistoryStore(), onPaste: { _, _ in },
            onManageCategories: {}, onSearch: { _ in }
        )
        controller.show(targetsClipDeck: true)
        defer { controller.hide() }
        let window = try XCTUnwrap(controller.window)
        let content = try XCTUnwrap(window.contentView)
        content.layoutSubtreeIfNeeded()
        func findHandle(_ view: NSView) -> NSView? {
            if view.identifier?.rawValue == "deck-resize-handle" { return view }
            return view.subviews.lazy.compactMap(findHandle).first
        }
        let handle = try XCTUnwrap(findHandle(content))
        handle.updateTrackingAreas()
        XCTAssertEqual(handle.bounds.width, content.bounds.width, accuracy: 1)
        XCTAssertEqual(handle.bounds.height, 18, accuracy: 1)
        XCTAssertTrue(handle.trackingAreas.contains {
            $0.options.contains([.activeAlways, .inVisibleRect, .cursorUpdate, .mouseMoved])
        })
        // The left, middle and right portions of the edge must all reach the
        // native resize view instead of toolbar controls in the hosting view.
        for fraction: CGFloat in [0.05, 0.5, 0.95] {
            // AppKit hitTest takes coordinates in the receiver's superview.
            let point = handle.convert(NSPoint(x: handle.bounds.width * fraction, y: 9), to: content.superview)
            let hit = content.hitTest(point)
            XCTAssertTrue(hit === handle, "point=\(point), handle=\(handle.frame), hit=\(String(describing: hit))")
        }
        window.setContentSize(NSSize(width: window.frame.width - 100, height: 300))
        content.layoutSubtreeIfNeeded()
        handle.updateTrackingAreas()
        XCTAssertEqual(handle.bounds.width, content.bounds.width, accuracy: 1)
    }
}
