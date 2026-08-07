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

    func testDefaultPresetSetAndNewLanguageClassification() throws {
        let repository = try makeRepository()
        try repository.ensureDefaultCategories()
        let categories = try repository.fetchCategories()

        XCTAssertEqual(categories.map(\.name), ["JSON", "URL", "SQL", "Markdown", "代码", "色彩"])
        XCTAssertFalse(categories.contains { $0.name == "Shell" })
        XCTAssertTrue(categories.allSatisfy { $0.script.contains("\n") })

        let service = ClassificationService(repository: repository)
        let samples = [
            ("public class Example { private int value; }", "代码"),
            ("const answer = value => value + 1;", "代码"),
            ("def greet(name):\n    return f\"Hello {name}\"", "代码"),
            ("func greet(name: String) -> String { return \"Hello \\(name)\" }", "代码"),
            ("#34c759", "色彩"),
            ("rgb(52, 199, 89)", "色彩"),
        ]
        for (text, expectedCategory) in samples {
            let item = try service.classify(repository.save(ClipboardItem(
                payload: .text(plainText: text, rtfData: nil),
                sourceApplication: source
            )))
            XCTAssertEqual(item.category?.name, expectedCategory, text)
        }

        let invalidColor = try service.classify(repository.save(ClipboardItem(
            payload: .text(plainText: "rgb(999, 0, 0)", rtfData: nil),
            sourceApplication: source
        )))
        XCTAssertNotEqual(invalidColor.category?.name, "色彩")
    }

    func testUniversalCodeHighlighterDetectsCommonLanguagesAndPreservesText() {
        let samples = [
            ("public class Example { public static void main(String[] args) {} }", "java"),
            ("const answer = (value) => value + 1;", "javascript"),
            ("def greet(name):\n    return f'Hello {name}'", "python"),
            ("struct HistoryPanelView: View {\n    @State private var page = 0\n    var body: some View { Text(\"Hi\") }\n}", "swift"),
            (".contextMenu {\n    Button(\"粘贴\", action: paste)\n        .keyboardShortcut(.return, modifiers: [])\n}", "swift"),
        ]
        for (source, expectedLanguage) in samples {
            let result = CodeHighlightService.shared.highlight(source, language: "code")
            XCTAssertEqual(result.text, source)
            XCTAssertEqual(result.language, expectedLanguage)
            XCTAssertFalse(result.spans.isEmpty)
        }
    }

    func testShortSwiftFragmentsAreClassifiedAsCodeButAFrameworkNameAloneIsNot() throws {
        let repository = try makeRepository()
        try repository.ensureDefaultCategories()
        let service = ClassificationService(repository: repository)
        let swiftFragments = [
            "@State private var categoryPageStart = 0",
            "private var body: some View { Text(\"Hello\") }",
            "guard let window else { return }",
            ".frame(maxWidth: .infinity, alignment: .leading)",
        ]
        for text in swiftFragments {
            let item = try service.classify(repository.save(ClipboardItem(
                payload: .text(plainText: text, rtfData: nil),
                sourceApplication: source
            )))
            XCTAssertEqual(item.category?.name, "代码", text)
        }

        let plainWord = try service.classify(repository.save(ClipboardItem(
            payload: .text(plainText: "SwiftUI", rtfData: nil),
            sourceApplication: source
        )))
        XCTAssertNil(plainWord.category)
    }

    func testWeightedCodeDetectionEngineRecognizesLanguagesWithoutNaturalLanguageFalsePositives() throws {
        let matches: [(String, String)] = [
            ("@State private var categoryPageStart = 0", "swift"),
            ("public class Example { private int value; }", "java"),
            ("const answer = value => value + 1;", "javascript"),
            ("def greet(name):\n    return f\"Hello {name}\"", "python"),
            ("#include <iostream>\nstd::cout << value;", "cpp"),
            ("package main\nfunc main() { fmt.Println(\"Hi\") }", "go"),
            ("fn main() { let mut value = 1; }", "rust"),
            ("#!/bin/zsh\ngit status", "bash"),
            ("<div class=\"card\">Hello</div>", "xml"),
            (".card { color: red; }", "css"),
        ]
        for (source, language) in matches {
            XCTAssertEqual(CodeDetectionEngine.shared.detect(source)?.language, language, source)
        }

        let plainTexts = [
            "SwiftUI",
            "The public class starts tomorrow morning",
            "Please import the latest report and export a PDF when finished",
            "I use git every day for work",
            "function",
            "let us meet after lunch",
            "颜色是 red，后续再调整",
        ]
        for source in plainTexts {
            XCTAssertNil(CodeDetectionEngine.shared.detect(source), source)
        }
    }

    func testRestoringPresetsRecreatesAndResetsThemWithoutDeletingCustomCategories() throws {
        let repository = try makeRepository()
        try repository.ensureDefaultCategories()
        let initial = try repository.fetchCategories()
        let json = try XCTUnwrap(initial.first { $0.name == "JSON" })
        let url = try XCTUnwrap(initial.first { $0.name == "URL" })
        try repository.updateCategory(
            id: json.id,
            name: json.name,
            icon: "star",
            script: "function classify(item) { return false; }",
            previewLanguage: "text"
        )
        try repository.deleteCategory(id: url.id)
        _ = try repository.createCategory(
            name: "自定义",
            icon: "tag",
            script: "function classify(item) { return false; }",
            previewLanguage: "text",
            sortOrder: 99
        )

        try repository.restoreDefaultCategories()
        let restored = try repository.fetchCategories()
        let restoredJSON = try XCTUnwrap(restored.first { $0.name == "JSON" })
        XCTAssertEqual(restoredJSON.icon, "curlybraces")
        XCTAssertEqual(restoredJSON.script, DefaultCategoryScripts.json)
        XCTAssertNotNil(restored.first { $0.name == "URL" })
        XCTAssertNotNil(restored.first { $0.name == "自定义" })
        XCTAssertEqual(restored.last?.name, "色彩")
    }

    private func makeRepository() throws -> HistoryRepository {
        try HistoryRepository(database: ClipDeckDatabase(path: ":memory:"))
    }
}
