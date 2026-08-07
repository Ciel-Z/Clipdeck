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
        let input: [String: Any] = [
            "text": text,
            "textLength": text.count,
            "sourceApplication": [
                "bundleId": item.sourceApplication.bundleIdentifier ?? "",
                "name": item.sourceApplication.name,
            ],
            "copiedAt": Int64(item.copiedAt.timeIntervalSince1970 * 1_000),
        ]

        for category in try repository.fetchCategories() {
            var categoryInput = input
            if category.previewLanguage?.lowercased() == "code" {
                let detection = CodeDetectionEngine.shared.detect(text)
                categoryInput["codeEngineMatched"] = detection != nil
                categoryInput["detectedLanguage"] = detection?.language ?? ""
                categoryInput["codeScore"] = detection?.score ?? 0
            }
            let result = Self.evaluate(category.script, input: categoryInput)
            try repository.recordCategoryError(result.error, for: category.id)
            if result.matches {
                return try repository.assignCategory(category, to: item)
            }
        }
        return try repository.assignCategory(nil, to: item)
    }

    static func test(script: String, text: String, previewLanguage: String? = nil) -> (matches: Bool, error: String?) {
        var input: [String: Any] = [
            "text": text,
            "textLength": text.count,
            "sourceApplication": ["bundleId": "app.clipdeck.script-test", "name": "脚本测试"],
            "copiedAt": Int64(Date().timeIntervalSince1970 * 1_000),
        ]
        if previewLanguage?.lowercased() == "code" {
            let detection = CodeDetectionEngine.shared.detect(text)
            input["codeEngineMatched"] = detection != nil
            input["detectedLanguage"] = detection?.language ?? ""
            input["codeScore"] = detection?.score ?? 0
        }
        return evaluate(script, input: input)
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
