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
