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

    func testRotatedImageIsRenderedOnlyWhenWrittenToPasteboard() throws {
        let bitmap = try XCTUnwrap(NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: 2,
            pixelsHigh: 1,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0
        ))
        let originalData = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
        let item = ClipboardItem(
            imageAngle: 90,
            payload: .image(data: originalData, width: 2, height: 1, format: .png),
            sourceApplication: source
        )
        let pasteboard = NSPasteboard(name: NSPasteboard.Name("ClipDeckTests.rotated.\(UUID())"))

        XCTAssertTrue(PasteService().write(item, to: pasteboard, preserveFormatting: false))
        let renderedData = try XCTUnwrap(pasteboard.data(forType: .png))
        let rendered = try XCTUnwrap(NSBitmapImageRep(data: renderedData))
        XCTAssertEqual(rendered.pixelsWide, 1)
        XCTAssertEqual(rendered.pixelsHigh, 2)
        guard case let .image(storedData, width, height, _) = item.payload else {
            return XCTFail("Expected image payload")
        }
        XCTAssertEqual(storedData, originalData)
        XCTAssertEqual(width, 2)
        XCTAssertEqual(height, 1)
    }
}
