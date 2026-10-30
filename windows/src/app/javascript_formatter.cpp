#include "javascript_formatter.h"

#include <QRegularExpression>
#include <QStringList>

namespace JavaScriptFormatter {

namespace {

enum class Mode { Code, SingleQuote, DoubleQuote, Template, LineComment, BlockComment, RegularExpression };

bool startsRegularExpression(const QString &output) {
    const QString trimmed = output.trimmed();
    if (trimmed.isEmpty()) return true;
    if (QStringLiteral("=([{!?:;,").contains(trimmed.back())) return true;
    for (const char *keyword : {"return", "case", "throw", "typeof", "instanceof"}) {
        if (trimmed.endsWith(QLatin1String(keyword))) return true;
    }
    return false;
}

} // namespace

QString format(const QString &source) {
    const QString characters = source;
    QString output;
    Mode mode = Mode::Code;
    int indentation = 0;
    int parenthesisDepth = 0;
    bool escaped = false;
    bool characterClass = false;
    static const QRegularExpression trailingBlanks(QStringLiteral("[ \\t]+$"));

    const auto trimOutput = [&] { output.remove(trailingBlanks); };
    const auto appendIndentIfNeeded = [&] {
        if (output.isEmpty() || output.endsWith(QLatin1Char('\n'))) output += QString(std::max(0, indentation) * 4, QLatin1Char(' '));
    };
    const auto appendNewline = [&] {
        trimOutput();
        if (!output.isEmpty() && !output.endsWith(QLatin1Char('\n'))) output += QLatin1Char('\n');
    };
    const auto appendCollapsedSpace = [&] {
        if (output.isEmpty() || output.endsWith(QLatin1Char(' ')) || output.endsWith(QLatin1Char('\n'))) return;
        output += QLatin1Char(' ');
    };

    for (qsizetype index = 0; index < characters.size(); ++index) {
        const QChar character = characters.at(index);
        const QChar next = index + 1 < characters.size() ? characters.at(index + 1) : QChar();

        switch (mode) {
        case Mode::SingleQuote:
        case Mode::DoubleQuote:
        case Mode::Template:
            output += character;
            if (escaped) {
                escaped = false;
            } else if (character == QLatin1Char('\\')) {
                escaped = true;
            } else if ((mode == Mode::SingleQuote && character == QLatin1Char('\''))
                       || (mode == Mode::DoubleQuote && character == QLatin1Char('"'))
                       || (mode == Mode::Template && character == QLatin1Char('`'))) {
                mode = Mode::Code;
            }
            break;
        case Mode::LineComment:
            if (character == QLatin1Char('\n')) {
                appendNewline();
                mode = Mode::Code;
            } else {
                output += character;
            }
            break;
        case Mode::BlockComment:
            output += character;
            if (character == QLatin1Char('*') && next == QLatin1Char('/')) {
                output += QLatin1Char('/');
                ++index;
                appendNewline();
                mode = Mode::Code;
            }
            break;
        case Mode::RegularExpression:
            output += character;
            if (escaped) escaped = false;
            else if (character == QLatin1Char('\\')) escaped = true;
            else if (character == QLatin1Char('[')) characterClass = true;
            else if (character == QLatin1Char(']')) characterClass = false;
            else if (character == QLatin1Char('/') && !characterClass) mode = Mode::Code;
            break;
        case Mode::Code:
            if (character.isSpace()) {
                appendCollapsedSpace();
            } else if (character == QLatin1Char('\'') || character == QLatin1Char('"') || character == QLatin1Char('`')) {
                appendIndentIfNeeded();
                output += character;
                mode = character == QLatin1Char('\'') ? Mode::SingleQuote
                     : character == QLatin1Char('"')  ? Mode::DoubleQuote
                                                      : Mode::Template;
            } else if (character == QLatin1Char('/') && next == QLatin1Char('/')) {
                appendIndentIfNeeded();
                output += QStringLiteral("//");
                ++index;
                mode = Mode::LineComment;
            } else if (character == QLatin1Char('/') && next == QLatin1Char('*')) {
                appendIndentIfNeeded();
                output += QStringLiteral("/*");
                ++index;
                mode = Mode::BlockComment;
            } else if (character == QLatin1Char('/') && startsRegularExpression(output)) {
                appendIndentIfNeeded();
                output += character;
                characterClass = false;
                mode = Mode::RegularExpression;
            } else if (character == QLatin1Char('{')) {
                trimOutput();
                if (!output.isEmpty()) {
                    const QChar last = output.back();
                    if (last != QLatin1Char(' ') && last != QLatin1Char('\n') && last != QLatin1Char('(')
                        && last != QLatin1Char('[')) {
                        output += QLatin1Char(' ');
                    }
                }
                appendIndentIfNeeded();
                output += QLatin1Char('{');
                ++indentation;
                appendNewline();
            } else if (character == QLatin1Char('}')) {
                trimOutput();
                if (!output.isEmpty() && !output.endsWith(QLatin1Char('\n'))) appendNewline();
                indentation = std::max(0, indentation - 1);
                appendIndentIfNeeded();
                output += QLatin1Char('}');
                appendNewline();
            } else if (character == QLatin1Char(';')) {
                trimOutput();
                appendIndentIfNeeded();
                output += QLatin1Char(';');
                if (parenthesisDepth == 0) appendNewline();
            } else if (character == QLatin1Char('(')) {
                appendIndentIfNeeded();
                output += character;
                ++parenthesisDepth;
            } else if (character == QLatin1Char(')')) {
                trimOutput();
                output += character;
                parenthesisDepth = std::max(0, parenthesisDepth - 1);
            } else if (character == QLatin1Char(',')) {
                trimOutput();
                output += QStringLiteral(", ");
            } else {
                appendIndentIfNeeded();
                output += character;
            }
            break;
        }
    }
    return output.trimmed();
}

} // namespace JavaScriptFormatter
