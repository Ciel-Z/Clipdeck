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
}
