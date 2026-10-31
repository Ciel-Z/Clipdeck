import Foundation
import JavaScriptCore

final class ClassificationService: @unchecked Sendable {
    struct ConsoleEntry: Equatable, Sendable {
        enum Level: String, Sendable {
            case log
            case info
            case warn
            case error
            case debug
        }

        let level: Level
        let message: String
    }

    struct EvaluationResult: Sendable {
        let matches: Bool
        let error: String?
        let console: [ConsoleEntry]
    }

    private let repository: HistoryRepository

    init(repository: HistoryRepository) {
        self.repository = repository
    }

    func classify(_ item: ClipboardItem) throws -> ClipboardItem {
        try repository.assignCategory(matchingCategory(for: item), to: item)
    }

    /// Runs the category scripts in order and returns the first match, without
    /// writing the item's category. Script errors are still recorded.
    func matchingCategory(for item: ClipboardItem) throws -> ClipboardCategory? {
        guard case let .text(text, _) = item.payload else { return nil }
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
        return matchedCategory
    }

    static func test(script: String, text: String, previewLanguage: String? = nil) -> EvaluationResult {
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

    private static func evaluate(_ script: String, input: [String: Any]) -> EvaluationResult {
        guard let context = JSContext() else {
            return EvaluationResult(
                matches: false,
                error: "无法创建 JavaScript 执行环境",
                console: []
            )
        }
        var exceptionMessage: String?
        var consoleEntries: [ConsoleEntry] = []
        context.exceptionHandler = { _, exception in
            exceptionMessage = exception?.toString() ?? "未知 JavaScript 异常"
        }
        let consoleSink: @convention(block) (String, String) -> Void = { level, message in
            guard consoleEntries.count < 200,
                  let parsedLevel = ConsoleEntry.Level(rawValue: level) else { return }
            consoleEntries.append(ConsoleEntry(
                level: parsedLevel,
                message: String(message.prefix(4_000))
            ))
        }
        context.setObject(consoleSink, forKeyedSubscript: "__clipDeckConsoleSink" as NSString)
        context.evaluateScript(Self.consoleBridgeScript)
        if let exceptionMessage {
            return EvaluationResult(matches: false, error: exceptionMessage, console: consoleEntries)
        }
        context.evaluateScript(script)
        if let exceptionMessage {
            return EvaluationResult(matches: false, error: exceptionMessage, console: consoleEntries)
        }
        guard let function = context.objectForKeyedSubscript("classify"), !function.isUndefined else {
            return EvaluationResult(
                matches: false,
                error: "脚本必须定义 classify(item) 函数",
                console: consoleEntries
            )
        }
        guard let value = function.call(withArguments: [input]) else {
            return EvaluationResult(
                matches: false,
                error: exceptionMessage ?? "分类脚本没有返回值",
                console: consoleEntries
            )
        }
        if let exceptionMessage {
            return EvaluationResult(matches: false, error: exceptionMessage, console: consoleEntries)
        }
        guard value.isBoolean else {
            return EvaluationResult(
                matches: false,
                error: "classify(item) 必须返回 Boolean",
                console: consoleEntries
            )
        }
        return EvaluationResult(matches: value.toBool(), error: nil, console: consoleEntries)
    }

    private static let consoleBridgeScript = #"""
        (() => {
            const formatValue = value => {
                if (typeof value === "string") return value;
                if (typeof value === "undefined") return "undefined";
                if (typeof value === "function" || typeof value === "symbol") return String(value);
                try {
                    const encoded = JSON.stringify(value);
                    return encoded === undefined ? String(value) : encoded;
                } catch (_) {
                    return String(value);
                }
            };
            const emit = level => (...values) => {
                __clipDeckConsoleSink(level, values.map(formatValue).join(" "));
            };
            globalThis.console = Object.freeze({
                log: emit("log"),
                info: emit("info"),
                warn: emit("warn"),
                error: emit("error"),
                debug: emit("debug")
            });
        })();
        """#
}
