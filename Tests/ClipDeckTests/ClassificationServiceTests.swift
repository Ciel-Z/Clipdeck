import Foundation
import XCTest
@testable import ClipDeck

final class ClassificationServiceTests: XCTestCase {
    private let source = SourceApplication(bundleIdentifier: "com.example.editor", name: "Editor", iconData: nil)

    func testFirstMatchingDefaultCategoryWins() throws {
        let repository = try makeRepository()
        try repository.ensureDefaultCategories()
        let service = ClassificationService(repository: repository)

        let json = try service.classify(repository.save(ClipboardItem(
            payload: .text(plainText: "{\"value\": 1}", rtfData: nil),
            sourceApplication: source
        )))
        let url = try service.classify(repository.save(ClipboardItem(
            payload: .text(plainText: "https://example.com", rtfData: nil),
            sourceApplication: source
        )))

        XCTAssertEqual(json.category?.name, "JSON")
        XCTAssertEqual(url.category?.name, "URL")
    }

    func testDefaultJSONCategoryOnlyMatchesObjectsAndArrays() throws {
        let repository = try makeRepository()
        try repository.ensureDefaultCategories()
        let service = ClassificationService(repository: repository)

        for text in ["{\"value\": 1}", "[1, 2, 3]"] {
            let item = try service.classify(repository.save(ClipboardItem(
                payload: .text(plainText: text, rtfData: nil),
                sourceApplication: source
            )))
            XCTAssertEqual(item.category?.name, "JSON", text)
        }

        for text in ["123", "true", "null", "\"JSON string\"", "value: 1", "{broken}"] {
            let item = try service.classify(repository.save(ClipboardItem(
                payload: .text(plainText: text, rtfData: nil),
                sourceApplication: source
            )))
            XCTAssertNotEqual(item.category?.name, "JSON", text)
        }
    }

    func testScriptErrorIsRecordedAndLaterCategoryStillMatches() throws {
        let repository = try makeRepository()
        let broken = try repository.createCategory(
            name: "Broken",
            icon: nil,
            script: "function classify(item) { throw new Error('broken rule'); }",
            previewLanguage: nil,
            sortOrder: 0
        )
        _ = try repository.createCategory(
            name: "Fallback",
            icon: nil,
            script: "function classify(item) { return true; }",
            previewLanguage: nil,
            sortOrder: 1
        )
        let service = ClassificationService(repository: repository)
        let item = try service.classify(repository.save(ClipboardItem(
            payload: .text(plainText: "anything", rtfData: nil),
            sourceApplication: source
        )))

        XCTAssertEqual(item.category?.name, "Fallback")
        XCTAssertNotNil(try repository.categoryError(id: broken.id))
    }

    func testDeletingCategoryKeepsContent() throws {
        let repository = try makeRepository()
        let category = try repository.createCategory(name: "All", icon: nil, script: "function classify(item) { return true; }", previewLanguage: nil, sortOrder: 0)
        let service = ClassificationService(repository: repository)
        _ = try service.classify(repository.save(ClipboardItem(payload: .text(plainText: "kept", rtfData: nil), sourceApplication: source)))

        try repository.deleteCategory(id: category.id)
        let items = try repository.fetchRecent()
        XCTAssertEqual(items.count, 1)
        XCTAssertNil(items.first?.category)
    }

    func testCategoryUpdateAndReorder() throws {
        let repository = try makeRepository()
        let first = try repository.createCategory(name: "First", icon: nil, script: "function classify(item) { return false; }", previewLanguage: nil, sortOrder: 0)
        let second = try repository.createCategory(name: "Second", icon: nil, script: "function classify(item) { return false; }", previewLanguage: nil, sortOrder: 1)

        try repository.updateCategory(id: first.id, name: "Renamed", icon: "star", script: first.script, previewLanguage: "json")
        try repository.reorderCategories([second.id, first.id])

        let categories = try repository.fetchCategories()
        XCTAssertEqual(categories.map(\.name), ["Second", "Renamed"])
        XCTAssertEqual(categories.last?.icon, "star")
        XCTAssertEqual(categories.last?.previewLanguage, "json")
    }

    func testJavaScriptFormattingPreservesLiteralsAndAddsStructure() {
        let source = "function classify(item){try{JSON.parse(item.text);return /[{}]/.test(item.text);}catch(_){return false;}}"
        let formatted = JavaScriptFormatter.format(source)

        XCTAssertTrue(formatted.contains("function classify(item) {\n"))
        XCTAssertTrue(formatted.contains("    try {\n"))
        XCTAssertTrue(formatted.contains("/[{}]/.test(item.text);"))
        XCTAssertTrue(formatted.contains("return false;"))
    }

    private func makeRepository() throws -> HistoryRepository {
        try HistoryRepository(database: ClipDeckDatabase(path: ":memory:"))
    }
}
