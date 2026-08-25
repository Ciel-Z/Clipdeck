import Foundation
import JavaScriptCore

final class ClassificationService: @unchecked Sendable {
    private let repository: HistoryRepository

    init(repository: HistoryRepository) {
        self.repository = repository
    }

    func classify(_ item: ClipboardItem) throws -> ClipboardItem {
        guard case let .text(text, _) = item.payload else {
            return try repository.assignCategory(nil, to: item)
        }
        let input = Self.makeInput(
            text: text,
            sourceBundleIdentifier: item.sourceApplication.bundleIdentifier ?? "",
            sourceName: item.sourceApplication.name,
            copiedAt: item.copiedAt
        )

        var categoryErrors: [(categoryID: Int64, message: String?)] = []
        var matchedCategory: ClipboardCategory?
        for category in try repository.fetchCategories() {
            let categoryInput = Self.input(
                input,
                enrichedFor: category.previewLanguage,
                text: text
            )
            let result = Self.evaluate(category.script, input: categoryInput)
            categoryErrors.append((category.id, result.error))
            if result.matches {
                matchedCategory = category
                break
            }
        }
        try repository.recordCategoryErrors(categoryErrors)
        return try repository.assignCategory(matchedCategory, to: item)
    }

    static func test(script: String, text: String, previewLanguage: String? = nil) -> (matches: Bool, error: String?) {
        let baseInput = makeInput(
            text: text,
            sourceBundleIdentifier: "app.clipdeck.script-test",
            sourceName: "脚本测试",
            copiedAt: Date()
        )
        let input = input(baseInput, enrichedFor: previewLanguage, text: text)
        return evaluate(script, input: input)
    }

    private static func makeInput(
        text: String,
        sourceBundleIdentifier: String,
        sourceName: String,
        copiedAt: Date
    ) -> [String: Any] {
        [
            "text": text,
            "textLength": text.count,
            "sourceApplication": [
                "bundleId": sourceBundleIdentifier,
                "name": sourceName,
            ],
            "copiedAt": Int64(copiedAt.timeIntervalSince1970 * 1_000),
        ]
    }

    private static func input(
        _ base: [String: Any],
        enrichedFor previewLanguage: String?,
        text: String
    ) -> [String: Any] {
        guard previewLanguage?.lowercased() == "code" else { return base }
        let detection = CodeDetectionEngine.shared.detect(text)
        var result = base
        result["codeEngineMatched"] = detection != nil
        result["detectedLanguage"] = detection?.language ?? ""
        result["codeScore"] = detection?.score ?? 0
        return result
    }

    private static func evaluate(_ script: String, input: [String: Any]) -> (matches: Bool, error: String?) {
        guard let context = JSContext() else { return (false, "无法创建 JavaScript 执行环境") }
        var exceptionMessage: String?
        context.exceptionHandler = { _, exception in
            exceptionMessage = exception?.toString() ?? "未知 JavaScript 异常"
        }
        context.evaluateScript(script)
        if let exceptionMessage { return (false, exceptionMessage) }
        guard let function = context.objectForKeyedSubscript("classify"), !function.isUndefined else {
            return (false, "脚本必须定义 classify(item) 函数")
        }
        guard let value = function.call(withArguments: [input]) else {
            return (false, exceptionMessage ?? "分类脚本没有返回值")
        }
        if let exceptionMessage { return (false, exceptionMessage) }
        guard value.isBoolean else { return (false, "classify(item) 必须返回 Boolean") }
        return (value.toBool(), nil)
    }
}
