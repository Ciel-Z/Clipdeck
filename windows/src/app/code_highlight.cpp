#include "code_highlight.h"

#include "history_repository.h"

#include <QFile>
#include <QHash>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextDocument>

namespace CodeHighlight {
namespace {

const char *const kBridgeScript = R"JS(
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
        result = hljs.highlight(code, { language: requestedLanguage, ignoreIllegals: true });
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
)JS";

struct Engine {
    QJSEngine engine;
    QJSValue function;
    bool ready = false;
    QString error;
    QHash<QString, Result> cache;

    Engine() {
        QFile file(QStringLiteral(":/highlight/highlight.min.js"));
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("highlight.js resource is missing");
            return;
        }
        // highlight.js deep-freezes its grammars and relies on ES2015 treating
        // primitives (null included) as frozen; Qt's V4 engine recurses into
        // null and throws. JavaScriptCore on mac needs no shim.
        const QJSValue shim = engine.evaluate(QStringLiteral(
            "(function () {"
            "  const isObject = o => o !== null && (typeof o === 'object' || typeof o === 'function');"
            "  const isFrozen = Object.isFrozen, freeze = Object.freeze, names = Object.getOwnPropertyNames;"
            "  Object.isFrozen = o => isObject(o) ? isFrozen(o) : true;"
            "  Object.freeze = o => isObject(o) ? freeze(o) : o;"
            "  Object.getOwnPropertyNames = o => isObject(o) ? names(o) : [];"
            // V4's RegExp source puts a backslash before every '/', even one
            // already escaped (\/ becomes \\/). highlight.js rebuilds its rules
            // from .source, so regex literals, comments and closing tags never
            // matched. Dropping that one backslash restores the pattern.
            "  const source = Object.getOwnPropertyDescriptor(RegExp.prototype, 'source').get;"
            "  Object.defineProperty(RegExp.prototype, 'source', {"
            "    get() { return source.call(this).replace(/\\\\\\//g, '/'); }, configurable: true });"
            "})();"
            // highlight.js warns through console; a bare QJSEngine has none.
            "var console = { log() {}, info() {}, warn() {}, error() {}, debug() {} };"));
        if (shim.isError()) {
            error = QStringLiteral("highlight shim failed: ") + shim.toString();
            return;
        }
        // V4 also keeps `__proto__:null` in an object literal as an own key, so
        // the grammar table would list a null "language". The bundle only uses
        // it to make lookup tables prototype-free, which nothing depends on.
        QString source = QString::fromUtf8(file.readAll());
        source.replace(QRegularExpression(QStringLiteral("\\{\\s*__proto__\\s*:\\s*null\\s*,")), QStringLiteral("{"));
        // Nor does its regex engine know Unicode property escapes (Python and
        // XML identifiers); letters beyond Latin-1 stand in for them.
        source.replace(QStringLiteral("\\p{XID_Continue}"), QStringLiteral("[\\w\\u00AA-\\uFFFF]"));
        source.replace(QStringLiteral("\\p{XID_Start}"), QStringLiteral("A-Za-z\\u00AA-\\uFFFF"));
        source.replace(QStringLiteral("\\p{L}"), QStringLiteral("A-Za-z\\u00AA-\\uFFFF"));
        const QJSValue loaded = engine.evaluate(source);
        if (loaded.isError() || engine.globalObject().property(QStringLiteral("hljs")).isUndefined()) {
            error = QStringLiteral("highlight.js failed to load: ") + loaded.toString();
            return;
        }
        const QJSValue bridge = engine.evaluate(QString::fromUtf8(kBridgeScript));
        if (bridge.isError()) {
            error = QStringLiteral("highlight bridge failed: ") + bridge.toString();
            return;
        }
        function = engine.globalObject().property(QStringLiteral("clipDeckHighlight"));
        ready = function.isCallable();
    }

    static Engine &shared() {
        static Engine instance;
        return instance;
    }
};

QString decode(QString text) {
    text.replace(QLatin1String("&#x27;"), QLatin1String("'"));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return text;
}

void parse(const QString &html, Result &result) {
    static const QRegularExpression tag(QStringLiteral(R"re(<span class="hljs-([^"]+)">|</span>)re"));
    QStringList scopes;
    qsizetype cursor = 0;
    auto append = [&](const QString &encoded) {
        const QString decoded = decode(encoded);
        if (decoded.isEmpty()) return;
        const int start = int(result.text.size());
        result.text += decoded;
        if (!scopes.isEmpty()) result.spans.append({start, int(decoded.size()), scopes.last()});
    };
    auto matches = tag.globalMatch(html);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.capturedStart() > cursor) append(html.mid(cursor, match.capturedStart() - cursor));
        if (match.capturedStart(1) >= 0) {
            scopes.append(match.captured(1).section(QLatin1Char(' '), 0, 0));
        } else if (!scopes.isEmpty()) {
            scopes.removeLast();
        }
        cursor = match.capturedEnd();
    }
    if (cursor < html.size()) append(html.mid(cursor));
}

// Dark appearance values of the AppKit colours the mac highlighter uses.
const QColor kPurple(191, 90, 242);
const QColor kOrange(255, 159, 10);
const QColor kTeal(64, 200, 224);
const QColor kBlue(10, 132, 255);
const QColor kMint(102, 212, 207);
const QColor kCyan(100, 210, 255);
const QColor kPink(255, 55, 95);
const QColor kSecondary(235, 235, 245, 153);

QString keywordPattern(const QString &language) {
    QStringList words;
    if (language == QLatin1String("sql")) {
        words = {"select", "from", "where", "join", "insert", "update", "delete", "create", "alter", "drop", "group",
                 "order", "by", "limit", "as", "and", "or", "not", "into", "values", "set"};
    } else if (language == QLatin1String("shell")) {
        words = {"if", "then", "else", "fi", "for", "while", "do", "done", "case", "esac", "function", "export",
                 "local", "return"};
    } else if (language == QLatin1String("javascript") || language == QLatin1String("js")
               || language == QLatin1String("json")) {
        words = {"function", "return", "const", "let", "var", "if", "else", "for", "while", "class", "new", "try",
                 "catch", "throw", "import", "export", "async", "await"};
    } else if (language == QLatin1String("java")) {
        words = {"class", "interface", "public", "private", "protected", "static", "final", "void", "return", "new",
                 "if", "else", "for", "while", "try", "catch", "throw", "package", "import"};
    }
    if (words.isEmpty()) return QString();
    return QStringLiteral("(?i)\\b(?:%1)\\b").arg(words.join(QLatin1Char('|')));
}

QVector<Run> compress(const QVector<QColor> &colours) {
    QVector<Run> runs;
    for (int i = 0; i < colours.size(); ++i) {
        if (!colours[i].isValid()) continue;
        if (!runs.isEmpty() && runs.last().start + runs.last().length == i && runs.last().color == colours[i]) {
            ++runs.last().length;
        } else {
            runs.append({i, 1, colours[i]});
        }
    }
    return runs;
}

} // namespace

Result highlight(const QString &text, const QString &language) {
    Result plain;
    plain.text = text;
    if (text.isEmpty()) return plain;
    Engine &engine = Engine::shared();
    const QString normalized = language.toLower();
    const QString key = normalized + QChar(0) + text;
    const auto cached = engine.cache.constFind(key);
    if (cached != engine.cache.constEnd()) return cached.value();
    if (!engine.ready) return plain;

    const QString requested = normalized == QLatin1String("code") ? HistoryRepository::detectedCodeLanguage(text) : normalized;
    const QJSValue output = engine.function.call({QJSValue(text), QJSValue(requested)});
    if (output.isError()) {
        engine.error = output.toString();
        return plain;
    }
    const QJsonObject payload = QJsonDocument::fromJson(output.toString().toUtf8()).object();
    if (!payload.contains(QStringLiteral("html"))) return plain;
    Result result;
    result.language = payload.value(QStringLiteral("language")).toString();
    result.relevance = payload.value(QStringLiteral("relevance")).toDouble();
    parse(payload.value(QStringLiteral("html")).toString(), result);
    if (engine.cache.size() >= 192) engine.cache.clear();
    engine.cache.insert(key, result);
    return result;
}

QString engineError() { return Engine::shared().error; }

bool isCodeLanguage(const QString &language) {
    return !language.isEmpty() && language.compare(QLatin1String("text"), Qt::CaseInsensitive) != 0;
}

QColor tokenColor(const QString &scope, Palette palette) {
    if (palette == Palette::Editor) {
        if (scope == QLatin1String("keyword") || scope == QLatin1String("meta-keyword")) return QColor(0xc6, 0x78, 0xdd);
        if (scope == QLatin1String("string") || scope == QLatin1String("regexp") || scope == QLatin1String("template-tag")
            || scope == QLatin1String("template-variable")) return QColor(0x98, 0xc3, 0x79);
        if (scope == QLatin1String("number") || scope == QLatin1String("literal")) return QColor(0xd1, 0x9a, 0x66);
        if (scope == QLatin1String("comment") || scope == QLatin1String("quote")) return QColor(0x7f, 0x84, 0x8e);
        if (scope == QLatin1String("title") || scope == QLatin1String("function")) return QColor(0x61, 0xaf, 0xef);
        if (scope == QLatin1String("property") || scope == QLatin1String("attr")) return QColor(0xe5, 0xc0, 0x7b);
        if (scope == QLatin1String("built_in") || scope == QLatin1String("type") || scope == QLatin1String("class"))
            return QColor(0xe5, 0xc0, 0x7b);
        if (scope == QLatin1String("operator") || scope == QLatin1String("punctuation")) return QColor(0xab, 0xb2, 0xbf);
        return QColor();
    }
    if (scope == QLatin1String("keyword") || scope == QLatin1String("selector-tag") || scope == QLatin1String("meta-keyword"))
        return kPurple;
    if (scope == QLatin1String("string") || scope == QLatin1String("regexp") || scope == QLatin1String("template-tag")
        || scope == QLatin1String("template-variable")) return kOrange;
    if (scope == QLatin1String("comment") || scope == QLatin1String("quote")) return kSecondary;
    if (scope == QLatin1String("number") || scope == QLatin1String("literal") || scope == QLatin1String("symbol")
        || scope == QLatin1String("bullet")) return kTeal;
    if (scope == QLatin1String("title") || scope == QLatin1String("section") || scope == QLatin1String("name")
        || scope == QLatin1String("selector-id")) return kBlue;
    if (scope == QLatin1String("type") || scope == QLatin1String("built_in") || scope == QLatin1String("class")
        || scope == QLatin1String("attribute")) return kMint;
    if (scope == QLatin1String("variable") || scope == QLatin1String("params") || scope == QLatin1String("attr")
        || scope == QLatin1String("property")) return kCyan;
    if (scope == QLatin1String("meta") || scope == QLatin1String("doctag")) return kPink;
    return QColor();
}

QVector<Run> colourRuns(const QString &text, const QString &language, Palette palette) {
    const QString normalized = language.toLower();
    if (!isCodeLanguage(normalized) || text.isEmpty()) return {};
    QVector<QColor> colours(text.size());

    // The editor always edits JavaScript; the Deck auto-detects "code".
    if (normalized == QLatin1String("code") || palette == Palette::Editor) {
        const Result highlighted = highlight(text, normalized);
        if (highlighted.text != text) return {};
        for (const Span &span : highlighted.spans) {
            const QColor colour = tokenColor(span.scope, palette);
            if (!colour.isValid() || span.start + span.length > colours.size()) continue;
            for (int i = span.start; i < span.start + span.length; ++i) colours[i] = colour;
        }
        return compress(colours);
    }

    auto apply = [&](const QString &pattern, const QColor &colour) {
        if (pattern.isEmpty()) return;
        const QRegularExpression expression(pattern);
        auto matches = expression.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            for (qsizetype i = match.capturedStart(); i < match.capturedEnd(); ++i) colours[i] = colour;
        }
    };
    apply(keywordPattern(normalized), kPurple);
    apply(QStringLiteral("\\b(?:true|false|null|undefined|nil|NULL)\\b"), kPurple);
    apply(QStringLiteral("\\b\\d+(?:\\.\\d+)?\\b"), kTeal);
    apply(QStringLiteral("(['\"])(?:\\\\.|(?!\\1).)*\\1"), kOrange);
    apply(QStringLiteral("//.*|/\\*[\\s\\S]*?\\*/|--.*|#(?![# ]).*"), kSecondary);
    if (normalized == QLatin1String("json")) {
        apply(QStringLiteral("\"(?:\\\\.|[^\"])*\"(?=\\s*:)"), kBlue);
    } else if (normalized == QLatin1String("markdown")) {
        apply(QStringLiteral("(?m)^#{1,6}\\s+.*$"), kBlue);
        apply(QStringLiteral("`[^`]+`|\\*\\*[^*]+\\*\\*"), kOrange);
    }
    return compress(colours);
}

QString highlightedHtml(const QString &text, const QString &language, const QColor &base) {
    auto css = [](const QColor &c) {
        return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
    };
    QString html = QStringLiteral("<div style=\"white-space:pre-wrap;color:%1\">").arg(css(base));
    const QVector<Run> runs = colourRuns(text, language);
    int cursor = 0;
    for (const Run &run : runs) {
        if (run.start > cursor) html += text.mid(cursor, run.start - cursor).toHtmlEscaped();
        html += QStringLiteral("<span style=\"color:%1\">").arg(css(run.color));
        html += text.mid(run.start, run.length).toHtmlEscaped();
        html += QStringLiteral("</span>");
        cursor = run.start + run.length;
    }
    if (cursor < text.size()) html += text.mid(cursor).toHtmlEscaped();
    html += QStringLiteral("</div>");
    return html;
}

} // namespace CodeHighlight

CodeSyntaxHighlighter::CodeSyntaxHighlighter(QTextDocument *document, const QString &language,
                                             CodeHighlight::Palette palette)
    : QSyntaxHighlighter(static_cast<QObject *>(nullptr)), language_(language), palette_(palette) {
    // Connected before QSyntaxHighlighter's own handler (setDocument below) so
    // the runs are stale by the time it re-highlights the changed blocks.
    // revision() alone is not enough: setPlainText does not advance it. Our
    // own formatting also reports a change (same length, same revision), which
    // must not throw the runs away once per block.
    connect(document, &QTextDocument::contentsChange, this, [this](int from, int removed, int added) {
        if (!runsValid_) return;
        if (removed != added || this->document()->revision() != runsRevision_) {
            runsValid_ = false;
        } else if (from == 0 && added >= this->document()->characterCount() - 1) {
            runsValid_ = this->document()->toPlainText() == runsText_; // same-length setPlainText
        }
    });
    setParent(document);
    setDocument(document);
}

void CodeSyntaxHighlighter::setLanguage(const QString &language) {
    if (language_ == language) return;
    language_ = language;
    runsValid_ = false;
    rehighlight();
}

void CodeSyntaxHighlighter::refreshRuns() {
    if (!document() || runsValid_) return;
    runsValid_ = true;
    runsRevision_ = document()->revision();
    runsText_ = document()->toPlainText();
    runs_ = CodeHighlight::colourRuns(runsText_, language_, palette_);
}

void CodeSyntaxHighlighter::highlightBlock(const QString &text) {
    refreshRuns();
    const int blockStart = currentBlock().position();
    const int blockEnd = blockStart + int(text.size());
    size_t signature = 0;
    for (const CodeHighlight::Run &run : runs_) {
        const int start = std::max(run.start, blockStart);
        const int end = std::min(run.start + run.length, blockEnd);
        if (start >= end) continue;
        QTextCharFormat format;
        format.setForeground(run.color);
        setFormat(start - blockStart, end - start, format);
        signature = qHashMulti(signature, start - blockStart, end - start, run.color.rgba());
    }
    // A changed state makes QSyntaxHighlighter continue into the next block,
    // so edits that open or close a multi-line comment recolour what follows.
    setCurrentBlockState(int(signature & 0x7fffffff));
}

void SyntaxHighlightAttachment::rebuild() {
    delete highlighter_;
    if (!document_ || !document_->textDocument()) return;
    const auto palette = palette_ == QLatin1String("deck") ? CodeHighlight::Palette::Deck : CodeHighlight::Palette::Editor;
    highlighter_ = new CodeSyntaxHighlighter(document_->textDocument(), language_, palette);
}

void SyntaxHighlightAttachment::setDocument(QQuickTextDocument *document) {
    if (document_ == document) return;
    document_ = document;
    rebuild();
    Q_EMIT documentChanged();
}

void SyntaxHighlightAttachment::setPalette(const QString &palette) {
    if (palette_ == palette) return;
    palette_ = palette;
    rebuild();
    Q_EMIT paletteChanged();
}

void SyntaxHighlightAttachment::setLanguage(const QString &language) {
    if (language_ == language) return;
    language_ = language;
    if (highlighter_) highlighter_->setLanguage(language_);
    Q_EMIT languageChanged();
}
