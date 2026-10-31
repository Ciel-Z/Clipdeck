#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QString>
#include <QSyntaxHighlighter>
#include <QVector>

// Port of mac CodeHighlightService + SyntaxHighlighter: highlight.js runs in a
// QJSEngine (mac: JavaScriptCore) and its HTML is parsed back into spans.
namespace CodeHighlight {

struct Span {
    int start = 0;
    int length = 0;
    QString scope;
};

struct Result {
    QString language;
    double relevance = 0;
    QString text;
    QVector<Span> spans;
};

// language "code" auto-detects (CodeDetectionEngine first, then hljs hints).
// Main thread only: the engine is not thread safe.
Result highlight(const QString &text, const QString &language = QString());

// Why highlight.js could not load or run; empty when it works.
QString engineError();

struct Run {
    int start = 0;
    int length = 0;
    QColor color;
};

enum class Palette {
    Deck,   // mac SyntaxHighlighter on the dark Deck (system colours)
    Editor, // script-editor theme.ts (One Dark)
};

// Colour runs for a category preview language (mac makeHighlighted): "code"
// goes through highlight.js, the other languages use the regex rules.
QVector<Run> colourRuns(const QString &text, const QString &language, Palette palette = Palette::Deck);

// Whether a preview language is drawn monospaced and coloured.
bool isCodeLanguage(const QString &language);

// Rich text for QML Text.RichText: escaped, colour spans, whitespace kept.
QString highlightedHtml(const QString &text, const QString &language, const QColor &base);

QColor tokenColor(const QString &scope, Palette palette = Palette::Deck);

} // namespace CodeHighlight

// Colours a QTextDocument (the script editor) with highlight.js spans.
class CodeSyntaxHighlighter : public QSyntaxHighlighter {
public:
    CodeSyntaxHighlighter(QTextDocument *document, const QString &language, CodeHighlight::Palette palette);
    void setLanguage(const QString &language);

protected:
    void highlightBlock(const QString &text) override;

private:
    void refreshRuns();

    QString language_;
    CodeHighlight::Palette palette_;
    QVector<CodeHighlight::Run> runs_;
    bool runsValid_ = false;
    int runsRevision_ = -1;
    QString runsText_;
};

// QML: SyntaxHighlight { document: editor.textDocument; language: "javascript" }
class SyntaxHighlightAttachment : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    // "editor" (script-editor theme, always highlight.js) or "deck".
    Q_PROPERTY(QString palette READ palette WRITE setPalette NOTIFY paletteChanged)
public:
    using QObject::QObject;
    QQuickTextDocument *document() const { return document_; }
    void setDocument(QQuickTextDocument *document);
    QString language() const { return language_; }
    void setLanguage(const QString &language);
    QString palette() const { return palette_; }
    void setPalette(const QString &palette);

Q_SIGNALS:
    void documentChanged();
    void languageChanged();
    void paletteChanged();

private:
    void rebuild();

    QPointer<QQuickTextDocument> document_;
    QString language_ = QStringLiteral("javascript");
    QString palette_ = QStringLiteral("editor");
    QPointer<CodeSyntaxHighlighter> highlighter_;
};
