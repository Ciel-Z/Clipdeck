import XCTest
@testable import ClipDeck

final class CategoryAppearanceTests: XCTestCase {
    func testStoredColourWinsAndOlderIconsFallBackByPosition() {
        XCTAssertEqual(CategoryColor.hex(icon: "#30d158", position: 0), "#30D158")
        XCTAssertEqual(CategoryColor.hex(icon: "curlybraces", position: 7), CategoryColor.palette[1])
        XCTAssertEqual(CategoryColor.hex(icon: nil, position: 0), CategoryColor.palette[0])
    }

    func testCustomColourRoundTripsThroughHex() {
        XCTAssertEqual(CategoryColor.hex(CategoryColor.color("#12AB9F")), "#12AB9F")
        XCTAssertFalse(CategoryColor.isPreset("#12AB9F"))
        XCTAssertTrue(CategoryColor.isPreset("#bf5af2"))
    }

    func testCardTypesAndCodeLanguagesShareOneStoredValue() {
        XCTAssertEqual(CardDisplayChoice.cardType(of: nil), "text")
        XCTAssertEqual(CardDisplayChoice.cardType(of: "url"), "url")
        XCTAssertEqual(CardDisplayChoice.cardType(of: "json"), "code")
        XCTAssertEqual(CardDisplayChoice.previewLanguage(forCardType: "code", current: "sql"), "sql")
        XCTAssertEqual(CardDisplayChoice.previewLanguage(forCardType: "code", current: "color"), "code")
        XCTAssertEqual(CardDisplayChoice.previewLanguage(forCardType: "text", current: "json"), "text")
    }
}
