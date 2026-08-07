import AppKit
import XCTest
@testable import ClipDeck

@MainActor
final class PasteServiceTests: XCTestCase {
    private let source = SourceApplication(bundleIdentifier: "test", name: "Test", iconData: nil)

    func testPlainTextModeDoesNotWriteRTF() {
        let pasteboard = NSPasteboard(name: NSPasteboard.Name("ClipDeckTests.plain.\(UUID())"))
        let item = ClipboardItem(
            payload: .text(plainText: "hello", rtfData: Data("rtf".utf8)),
            sourceApplication: source
        )

        XCTAssertTrue(PasteService().write(item, to: pasteboard, preserveFormatting: false))
        XCTAssertEqual(pasteboard.string(forType: .string), "hello")
        XCTAssertNil(pasteboard.data(forType: .rtf))
    }

    func testPreserveFormattingModeWritesPlainTextAndRTF() {
        let pasteboard = NSPasteboard(name: NSPasteboard.Name("ClipDeckTests.rich.\(UUID())"))
        let rtf = Data("{\\rtf1 hello}".utf8)
        let item = ClipboardItem(
            payload: .text(plainText: "hello", rtfData: rtf),
            sourceApplication: source
        )

        XCTAssertTrue(PasteService().write(item, to: pasteboard, preserveFormatting: true))
        XCTAssertEqual(pasteboard.string(forType: .string), "hello")
        XCTAssertEqual(pasteboard.data(forType: .rtf), rtf)
    }
}
