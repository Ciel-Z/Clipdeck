import Foundation
import JavaScriptCore

final class CodeDetectionEngine: @unchecked Sendable {
    struct Match: Sendable, Equatable {
        let language: String
        let score: Int
    }

    private struct Signal {
        let expression: NSRegularExpression
        let weight: Int

        init(_ pattern: String, weight: Int) {
            expression = try! NSRegularExpression(
                pattern: pattern,
                options: [.caseInsensitive, .anchorsMatchLines]
            )
            self.weight = weight
        }

        func matches(_ text: String) -> Bool {
            expression.firstMatch(
                in: text,
                range: NSRange(location: 0, length: (text as NSString).length)
            ) != nil
        }
    }

    private struct LanguageRule {
        let language: String
        let minimumScore: Int
        let signals: [Signal]
    }

    static let shared = CodeDetectionEngine()

    // Rules are deliberately ordered. More distinctive languages are checked first;
    // the first language whose combined signals reach its threshold wins.
    private let rules: [LanguageRule] = [
        LanguageRule(language: "swift", minimumScore: 5, signals: [
            Signal(#"\bimport\s+(Foundation|SwiftUI|AppKit|UIKit|Combine|PackageDescription)\b"#, weight: 7),
            Signal(#"(^|\n)\s*@(?:State|Binding|ObservedObject|StateObject|Environment|EnvironmentObject|MainActor|ViewBuilder)\b"#, weight: 6),
            Signal(#"\b(?:protocol|extension|actor|struct|enum)\s+[A-Za-z_]\w*"#, weight: 5),
            Signal(#"\bfunc\s+[A-Za-z_]\w*\s*\([^)]*:[^)]*\)"#, weight: 6),
            Signal(#"\bfunc\s+[A-Za-z_]\w*\s*\([^)]*\)\s*(?:async\s+|throws\s+)*->\s*[^\{]+\{"#, weight: 6),
            Signal(#"\b(?:public|private|internal|fileprivate|open)\s+func\s+[A-Za-z_]\w*\s*\("#, weight: 6),
            Signal(#"\bvar\s+body\s*:\s*some\s+View\b"#, weight: 7),
            Signal(#"\b(?:guard|if)\s+let\s+\w+[^\n{]*\{"#, weight: 5),
            Signal(#"(^|\n)\s*\.[A-Za-z_]\w*\s*\([^\n)]*\b[A-Za-z_]\w*\s*:"#, weight: 5),
            Signal(#"\{\s*\[(?:weak|unowned)\s+\w+\][^}]*\bin\b"#, weight: 6),
        ]),
        LanguageRule(language: "objectivec", minimumScore: 6, signals: [
            Signal(#"@(?:interface|implementation|protocol)\s+\w+"#, weight: 8),
            Signal(#"#import\s+[<\"](?:Foundation|UIKit|AppKit)/"#, weight: 8),
            Signal(#"\[[A-Za-z_]\w*\s+[A-Za-z_]\w*(?::[^\]]*)?\]"#, weight: 6),
        ]),
        // Same Java signals as HistoryRepository on Linux and Windows. Type-shaped
        // ones are case-sensitive so C++ "vector<int> v =" stays C++.
        LanguageRule(language: "java", minimumScore: 6, signals: [
            Signal(#"\b(?:public|protected|private)\s+(?:abstract\s+|final\s+|static\s+)*(?:class|interface|enum|record)\s+\w+[^\n{]*\{"#, weight: 8),
            Signal(#"\bpackage\s+[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*\s*;"#, weight: 7),
            Signal(#"\bimport\s+(?:static\s+)?[A-Za-z_]\w*(?:\.[A-Za-z_*]\w*)+\s*;"#, weight: 6),
            Signal(#"\bSystem\.(?:out|err)\.\w+\s*\("#, weight: 7),
            Signal(#"\b(?:public|protected|private|static|final)\s+[A-Za-z_<>,?\[\]]+\s+\w+\s*\([^)]*\)\s*(?:throws\s+[\w.,\s]+)?\{"#, weight: 6),
            Signal(#"(?:^|\n)\s*@(?:Override|Autowired|Test|Bean|Component|Service|Repository|Controller|RestController|Data|Getter|Setter|SuppressWarnings|FunctionalInterface|Deprecated)\b"#, weight: 6),
            Signal(#"(?-i:\b(?:String|[A-Z]\w*<[^<>\n]*(?:<[^<>\n]*>[^<>\n]*)?>)(?:\[\])*\s+[a-z_]\w*\s*(?:=(?!=)|;))"#, weight: 6),
            Signal(#"(?-i:\bnew\s+[A-Z]\w*<(?:\s*[A-Z][\w.]*(?:<[^<>\n]*>)?(?:\[\])*\s*,?)*>\s*\()"#, weight: 6),
        ]),
        LanguageRule(language: "kotlin", minimumScore: 6, signals: [
            Signal(#"\bfun\s+\w+\s*\([^)]*\)\s*(?::\s*[\w<>,?.]+)?\s*[={]"#, weight: 7),
            Signal(#"\b(?:data|sealed|enum)\s+class\s+\w+"#, weight: 7),
            Signal(#"\b(?:val|var)\s+\w+\s*:\s*[A-Z]\w*"#, weight: 6),
            Signal(#"\bcompanion\s+object\b|\bwhen\s*\([^)]*\)\s*\{"#, weight: 6),
        ]),
        LanguageRule(language: "python", minimumScore: 6, signals: [
            Signal(#"(^|\n)\s*(?:async\s+)?def\s+\w+\s*\([^)]*\)\s*(?:->[^:]+)?\s*:"#, weight: 8),
            Signal(#"(^|\n)\s*class\s+\w+(?:\([^)]*\))?\s*:"#, weight: 8),
            Signal(#"(^|\n)\s*from\s+[\w.]+\s+import\s+"#, weight: 6),
            Signal(#"(^|\n)\s*import\s+[A-Za-z_]\w*(?:\s+as\s+\w+)?\s*$"#, weight: 6),
            Signal(#"if\s+__name__\s*==\s*['\"]__main__['\"]\s*:"#, weight: 8),
        ]),
        LanguageRule(language: "typescript", minimumScore: 6, signals: [
            Signal(#"\binterface\s+\w+(?:\s+extends\s+\w+)?\s*\{"#, weight: 7),
            Signal(#"\btype\s+\w+(?:<[^>]+>)?\s*="#, weight: 7),
            Signal(#"\b(?:const|let|var)\s+\w+\s*:\s*(?:[A-Za-z_]|\{|\[)"#, weight: 6),
            Signal(#"\b(?:public|private|protected|readonly)\s+\w+\s*[?:]\s*\w+"#, weight: 6),
        ]),
        LanguageRule(language: "javascript", minimumScore: 5, signals: [
            Signal(#"\b(?:async\s+)?function\s*\w*\s*\([^)]*\)\s*\{"#, weight: 7),
            Signal(#"\b(?:const|let|var)\s+[A-Za-z_$]\w*\s*="#, weight: 5),
            Signal(#"(?:\([^)]*\)|[A-Za-z_$]\w*)\s*=>"#, weight: 6),
            Signal(#"\b(?:import|export)\s+(?:default\s+|\{)[^\n]+"#, weight: 6),
            Signal(#"(?-i:\b(?:document|window|console)\.\w+\s*\()"#, weight: 6),
        ]),
        LanguageRule(language: "cpp", minimumScore: 6, signals: [
            Signal(#"^\s*#include\s*[<\"][^>\"]+[>\"]"#, weight: 8),
            Signal(#"\bstd::\w+|\b(?:cout|cin)\s*(?:<<|>>)"#, weight: 7),
            Signal(#"\bnamespace\s+\w+\s*\{"#, weight: 6),
            Signal(#"\btemplate\s*<[^>]+>"#, weight: 7),
        ]),
        LanguageRule(language: "csharp", minimumScore: 6, signals: [
            Signal(#"\busing\s+System(?:\.\w+)*\s*;"#, weight: 8),
            Signal(#"\bnamespace\s+[\w.]+\s*[;{]"#, weight: 7),
            Signal(#"\b(?:public|private|protected|internal)\s+(?:sealed\s+|abstract\s+)?(?:class|interface|record|struct)\s+\w+"#, weight: 6),
            Signal(#"\bConsole\.Write(?:Line)?\s*\("#, weight: 7),
        ]),
        LanguageRule(language: "go", minimumScore: 6, signals: [
            Signal(#"^\s*package\s+\w+\s*$"#, weight: 7),
            Signal(#"\bfunc\s+(?:\([^)]*\)\s*)?\w+\s*\([^)]*\)"#, weight: 7),
            Signal(#"\b(?:var|type)\s+\w+\s+(?:struct|interface)\s*\{"#, weight: 7),
            Signal(#"\bgo\s+\w+\s*\([^)]*\)|\bdefer\s+\w+\s*\([^)]*\)"#, weight: 6),
        ]),
        LanguageRule(language: "rust", minimumScore: 6, signals: [
            Signal(#"\bfn\s+\w+\s*\([^)]*\)\s*(?:->[^\{]+)?\{"#, weight: 8),
            Signal(#"\bimpl(?:<[^>]+>)?\s+\w+"#, weight: 7),
            Signal(#"\blet\s+mut\s+\w+"#, weight: 6),
            Signal(#"\buse\s+(?:crate|self|super|std)::"#, weight: 7),
            Signal(#"\b(?:Some|Ok|Err)!?\s*\("#, weight: 6),
        ]),
        LanguageRule(language: "php", minimumScore: 6, signals: [
            Signal(#"<\?php\b"#, weight: 9),
            Signal(#"\b(?:public|private|protected)\s+function\s+\w+\s*\("#, weight: 7),
            Signal(#"\$[A-Za-z_]\w*\s*(?:=|->)"#, weight: 6),
        ]),
        LanguageRule(language: "ruby", minimumScore: 6, signals: [
            Signal(#"(^|\n)\s*def\s+\w+[!?=]?\s*(?:\([^)]*\))?\s*$[\s\S]*^\s*end\s*$"#, weight: 8),
            Signal(#"(^|\n)\s*class\s+\w+(?:\s*<\s*\w+)?\s*$[\s\S]*^\s*end\s*$"#, weight: 8),
            Signal(#"(^|\n)\s*require(?:_relative)?\s+['\"][^'\"]+['\"]"#, weight: 6),
        ]),
        LanguageRule(language: "lua", minimumScore: 6, signals: [
            Signal(#"\b(?:local\s+)?function\s+\w+(?::\w+)?\s*\([^)]*\)[\s\S]*\bend\b"#, weight: 8),
            Signal(#"(^|\n)\s*local\s+\w+\s*="#, weight: 6),
        ]),
        LanguageRule(language: "bash", minimumScore: 6, signals: [
            Signal(#"^\s*#!\s*/(?:usr/bin/env\s+)?(?:ba|z|k)?sh\b"#, weight: 9),
            Signal(#"(^|\n)\s*(?:sudo|git|curl|wget|ssh|scp|rsync|brew|npm|yarn|pnpm|docker|kubectl)\s+[-\w]"#, weight: 6),
            Signal(#"(^|\n)\s*(?:export\s+)?[A-Z_][A-Z0-9_]*=\S+"#, weight: 6),
            Signal(#"\$\{[A-Za-z_]\w*(?::-[^}]*)?\}"#, weight: 6),
        ]),
        LanguageRule(language: "xml", minimumScore: 6, signals: [
            Signal(#"<!DOCTYPE\s+html\b|<\?xml\s+version="#, weight: 9),
            Signal(#"<([A-Za-z][\w:-]*)(?:\s+[^<>]*)?>[\s\S]*</\1\s*>"#, weight: 7),
            Signal(#"<(?:html|head|body|script|style|div|span|template|svg)\b[^>]*>"#, weight: 6),
        ]),
        LanguageRule(language: "css", minimumScore: 6, signals: [
            Signal(#"(?:^|\n)\s*(?:[.#][\w-]+|[a-z][\w-]*(?:\s+[a-z][\w-]*)*)\s*\{[^}]*[\w-]+\s*:[^;}]+[;}]*"#, weight: 8),
            Signal(#"@(?:media|supports|keyframes|font-face)\b[^\{]*\{"#, weight: 7),
        ]),
    ]

    func detect(_ source: String) -> Match? {
        let text = source.trimmingCharacters(in: .whitespacesAndNewlines)
        guard text.count >= 4, !Self.looksLikeNaturalProse(text) else { return nil }
        for rule in rules {
            let score = rule.signals.reduce(into: 0) { total, signal in
                if signal.matches(text) { total += signal.weight }
            }
            if score >= rule.minimumScore {
                return Match(language: rule.language, score: score)
            }
        }
        return nil
    }

    private static func looksLikeNaturalProse(_ text: String) -> Bool {
        guard !text.contains("\n"), text.count >= 24 else { return false }
        let structuralCharacters = text.filter { "{}[]();=<>$#@".contains($0) }.count
        let words = text.split(whereSeparator: { $0.isWhitespace }).count
        return words >= 5 && structuralCharacters == 0
    }
}

final class CodeHighlightService: @unchecked Sendable {
    struct Span: Sendable {
        let range: NSRange
        let scope: String
    }

    struct Result: Sendable {
        let language: String?
        let relevance: Double
        let text: String
        let spans: [Span]

        static func plain(_ text: String) -> Result {
            Result(language: nil, relevance: 0, text: text, spans: [])
        }
    }

    static let shared = CodeHighlightService()

    static func displayName(for language: String) -> String {
        let names = [
            "bash": "Shell", "c": "C", "cpp": "C++", "csharp": "C#",
            "css": "CSS", "go": "Go", "java": "Java", "javascript": "JavaScript",
            "json": "JSON", "kotlin": "Kotlin", "lua": "Lua", "objectivec": "Objective-C",
            "php": "PHP", "python": "Python", "ruby": "Ruby", "rust": "Rust",
            "sql": "SQL", "swift": "Swift", "typescript": "TypeScript",
            "xml": "HTML/XML", "yaml": "YAML",
        ]
        return names[language.lowercased()] ?? language.capitalized
    }

    private let lock = NSLock()
    private var context: JSContext?
    private var function: JSValue?
    private var cache: [CacheKey: Result] = [:]

    private struct CacheKey: Hashable {
        let text: String
        let language: String?
    }

    func highlight(_ text: String, language: String? = nil) -> Result {
        guard !text.isEmpty else { return .plain(text) }
        let normalized = language?.lowercased()
        let key = CacheKey(text: text, language: normalized)
        lock.lock()
        defer { lock.unlock() }
        if let cached = cache[key] { return cached }
        guard prepareIfNeeded(), let function else { return .plain(text) }

        let requestedLanguage = normalized == "code"
            ? (CodeDetectionEngine.shared.detect(text)?.language ?? "")
            : (normalized ?? "")
        guard let json = function.call(withArguments: [text, requestedLanguage])?.toString(),
              let data = json.data(using: .utf8),
              let payload = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let html = payload["html"] as? String else { return .plain(text) }
        let parsed = Self.parse(html: html)
        let result = Result(
            language: (payload["language"] as? String).flatMap { $0.isEmpty ? nil : $0 },
            relevance: payload["relevance"] as? Double ?? 0,
            text: parsed.text,
            spans: parsed.spans
        )
        if cache.count >= 192 { cache.removeAll(keepingCapacity: true) }
        cache[key] = result
        return result
    }

    private func prepareIfNeeded() -> Bool {
        if context != nil, function != nil { return true }
        guard let scriptURL = Self.resourceURL(named: "highlight.min", extension: "js"),
              let script = try? String(contentsOf: scriptURL, encoding: .utf8),
              let context = JSContext() else { return false }
        var loadError: String?
        context.exceptionHandler = { _, exception in loadError = exception?.toString() }
        context.evaluateScript(script)
        guard loadError == nil, context.objectForKeyedSubscript("hljs")?.isUndefined == false else {
            return false
        }
        context.evaluateScript(Self.bridgeScript)
        guard loadError == nil,
              let function = context.objectForKeyedSubscript("clipDeckHighlight"),
              !function.isUndefined else { return false }
        self.context = context
        self.function = function
        return true
    }

    private static func resourceURL(named name: String, extension fileExtension: String) -> URL? {
        #if SWIFT_PACKAGE
        Bundle.module.url(forResource: name, withExtension: fileExtension)
        #else
        Bundle.main.url(forResource: name, withExtension: fileExtension)
        #endif
    }

    private static let bridgeScript = #"""
    function clipDeckHighlight(code, requestedLanguage) {
        const preferredLanguages = [
            "bash", "c", "cpp", "csharp", "css", "go", "java", "javascript",
            "json", "kotlin", "lua", "objectivec", "php", "python", "ruby",
            "rust", "sql", "swift", "typescript", "xml", "yaml"
        ].filter(language => hljs.getLanguage(language));
        function hintedLanguage(source) {
            if (/^\s*#include\s*[<"]|\bstd::|\bcout\s*<</m.test(source)) return "cpp";
            if (/\b(public|private|protected)\s+(class|interface|enum|record)\b|\bSystem\.(out|err)\.|\bimport\s+(static\s+)?\w+(\.[\w*]+)+\s*;|\bpackage\s+\w+(\.\w+)*\s*;|(^|\n)\s*@(Override|Autowired|Test|SuppressWarnings)\b|\b(String|[A-Z]\w*<[^<>\n]*>)(\[\])*\s+[a-z_]\w*\s*(=(?!=)|;)|\bnew\s+[A-Z]\w*<(\s*[A-Z][\w.]*(<[^<>\n]*>)?(\[\])*\s*,?)*>\s*\(/m.test(source)) return "java";
            if (/(^|\n)\s*(def|async\s+def)\s+\w+\s*\([^)]*\)\s*:|(^|\n)\s*from\s+\w+\s+import\s+/m.test(source)) return "python";
            if (/\bimport\s+(Foundation|SwiftUI|AppKit|UIKit|Combine|PackageDescription)\b/m.test(source)) return "swift";
            if (/(^|\n)\s*(?:@(?:State|Binding|ObservedObject|StateObject|Environment|EnvironmentObject|MainActor|ViewBuilder)\b|(?:public|private|internal|fileprivate|open)?\s*(?:func|protocol|extension|actor|struct|enum)\s+\w+)/m.test(source)) return "swift";
            if (/\b(?:var\s+body\s*:\s*some\s+View|some\s+View|guard\s+let|if\s+let|case\s+let)\b|\{\s*\[(?:weak|unowned)\s+\w+\][^}]*\bin\b/m.test(source)) return "swift";
            if (/(^|\n)\s*\.[a-zA-Z_]\w*\s*\([^)]*\b[a-zA-Z_]\w*\s*:/m.test(source)) return "swift";
            if (/(^|\n)\s*package\s+main\b|(^|\n)\s*func\s+\w+\s*\([^)]*\)/m.test(source)) return "go";
            if (/(^|\n)\s*fn\s+\w+\s*\([^)]*\)|\blet\s+mut\b|\bimpl\s+\w+/m.test(source)) return "rust";
            if (/\b(const|let|var)\s+\w+\s*=|=>|\bfunction\s+\w*\s*\(/m.test(source)) return "javascript";
            return "";
        }

        let result;
        let confidenceBoost = 0;
        if (requestedLanguage && hljs.getLanguage(requestedLanguage)) {
            result = hljs.highlight(code, {
                language: requestedLanguage,
                ignoreIllegals: true
            });
        } else {
            const hint = hintedLanguage(code);
            if (hint && hljs.getLanguage(hint)) {
                result = hljs.highlight(code, { language: hint, ignoreIllegals: true });
                confidenceBoost = 2;
            } else {
                result = hljs.highlightAuto(code, preferredLanguages);
            }
        }
        return JSON.stringify({
            language: result.language || "",
            relevance: (result.relevance || 0) + confidenceBoost,
            html: result.value
        });
    }
    """#

    private static func parse(html: String) -> (text: String, spans: [Span]) {
        guard let regex = try? NSRegularExpression(
            pattern: #"<span class="hljs-([^"]+)">|</span>"#
        ) else { return (decode(html), []) }
        let source = html as NSString
        let fullRange = NSRange(location: 0, length: source.length)
        let matches = regex.matches(in: html, range: fullRange)
        var output = ""
        var spans: [Span] = []
        var scopes: [String] = []
        var cursor = 0

        func appendText(_ encoded: String) {
            let decoded = decode(encoded)
            guard !decoded.isEmpty else { return }
            let location = (output as NSString).length
            output += decoded
            if let scope = scopes.last {
                spans.append(Span(
                    range: NSRange(location: location, length: (decoded as NSString).length),
                    scope: scope
                ))
            }
        }

        for match in matches {
            if match.range.location > cursor {
                appendText(source.substring(with: NSRange(
                    location: cursor,
                    length: match.range.location - cursor
                )))
            }
            if match.range(at: 1).location != NSNotFound {
                let className = source.substring(with: match.range(at: 1))
                scopes.append(className.split(separator: " ").first.map(String.init) ?? className)
            } else if !scopes.isEmpty {
                scopes.removeLast()
            }
            cursor = NSMaxRange(match.range)
        }
        if cursor < source.length {
            appendText(source.substring(from: cursor))
        }
        return (output, spans)
    }

    private static func decode(_ text: String) -> String {
        text
            .replacingOccurrences(of: "&#x27;", with: "'")
            .replacingOccurrences(of: "&#39;", with: "'")
            .replacingOccurrences(of: "&quot;", with: "\"")
            .replacingOccurrences(of: "&lt;", with: "<")
            .replacingOccurrences(of: "&gt;", with: ">")
            .replacingOccurrences(of: "&amp;", with: "&")
    }
}
