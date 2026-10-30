#include "deck_presentation.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

#include <cmath>

namespace DeckPresentation {

namespace {

bool isHexDigits(const QString &value) {
    for (const QChar c : value) {
        if (!c.isDigit() && !(c >= QLatin1Char('a') && c <= QLatin1Char('f'))) return false;
    }
    return true;
}

std::optional<QColor> hexColor(const QString &value) {
    if (!(value.size() == 3 || value.size() == 4 || value.size() == 6 || value.size() == 8) || !isHexDigits(value)) {
        return std::nullopt;
    }
    QString expanded;
    if (value.size() <= 4) {
        for (const QChar c : value) expanded += QString(2, c);
    } else {
        expanded = value;
    }
    bool ok = false;
    const quint64 number = expanded.toULongLong(&ok, 16);
    if (!ok) return std::nullopt;
    const bool hasAlpha = expanded.size() == 8;
    const int red = int((number >> (hasAlpha ? 24 : 16)) & 0xff);
    const int green = int((number >> (hasAlpha ? 16 : 8)) & 0xff);
    const int blue = int((number >> (hasAlpha ? 8 : 0)) & 0xff);
    const int alpha = hasAlpha ? int(number & 0xff) : 255;
    return QColor(red, green, blue, alpha);
}

std::optional<double> percentOr(const QString &value, double maximum) {
    bool ok = false;
    if (value.endsWith(QLatin1Char('%'))) {
        const double percent = value.chopped(1).toDouble(&ok);
        if (ok && percent >= 0 && percent <= 100) return percent / 100;
        return std::nullopt;
    }
    const double number = value.toDouble(&ok);
    if (ok && number >= 0 && number <= maximum) return number / maximum;
    return std::nullopt;
}

} // namespace

QString relativeTime(const QDateTime &copiedAt, const QDateTime &now) {
    const qint64 seconds = std::max<qint64>(0, copiedAt.secsTo(now));
    if (seconds < 60) return QStringLiteral("现在");
    if (seconds < 3600) return QStringLiteral("%1 分钟").arg(seconds / 60);
    if (seconds < 86400) return QStringLiteral("%1 小时").arg(seconds / 3600);
    if (seconds < 604800) return QStringLiteral("%1 天").arg(seconds / 86400);
    return QStringLiteral("%1月%2日").arg(copiedAt.date().month()).arg(copiedAt.date().day());
}

QUrl webUrl(const ClipboardItem &item) {
    if (!item.isText()) return QUrl();
    const QString candidate = item.text.trimmed();
    if (candidate.isEmpty()) return QUrl();
    for (const QChar c : candidate) {
        if (c.isSpace()) return QUrl();
    }
    const QUrl url(candidate, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")) || url.host().isEmpty()) {
        return QUrl();
    }
    return url;
}

QString linkHost(const ClipboardItem &item) {
    const QString host = webUrl(item).host();
    return host.startsWith(QLatin1String("www.")) ? host.mid(4) : host;
}

QString linkDisplayTitle(const ClipboardItem &item) {
    if (item.title != QStringLiteral("文本") && !item.title.trimmed().isEmpty()) return item.title;
    const QString host = linkHost(item);
    if (!host.isEmpty()) return host;
    const QUrl url = webUrl(item);
    return url.isValid() ? url.toString() : QStringLiteral("链接");
}

QString cardHeaderTitle(const ClipboardItem &item) {
    if (item.isFiles()) return QStringLiteral("文件 · %1 项").arg(item.files.size());
    return webUrl(item).isValid() ? QStringLiteral("链接") : item.title;
}

QString displayCodeLanguage(const QString &language) {
    static const QMap<QString, QString> names = {
        {QStringLiteral("bash"), QStringLiteral("Shell")},
        {QStringLiteral("cpp"), QStringLiteral("C++")},
        {QStringLiteral("csharp"), QStringLiteral("C#")},
        {QStringLiteral("javascript"), QStringLiteral("JavaScript")},
        {QStringLiteral("objectivec"), QStringLiteral("Objective-C")},
        {QStringLiteral("typescript"), QStringLiteral("TypeScript")},
        {QStringLiteral("xml"), QStringLiteral("HTML/XML")},
    };
    return names.value(language, language.isEmpty() ? QString() : language.left(1).toUpper() + language.mid(1));
}

QString categoryLabel(const ClipboardItem &item) {
    if (item.category.isEmpty()) return QString();
    if (item.previewLanguage.compare(QStringLiteral("code"), Qt::CaseInsensitive) != 0 || !item.isText()) {
        return item.category;
    }
    const QString detected = HistoryRepository::detectedCodeLanguage(item.text);
    return detected.isEmpty() ? item.category : displayCodeLanguage(detected);
}

std::optional<QColor> parseColor(const QString &source) {
    const QString value = source.trimmed().toLower();
    if (value.startsWith(QLatin1Char('#'))) return hexColor(value.mid(1));
    if ((value.size() == 6 || value.size() == 8) && isHexDigits(value)) {
        bool hasLetter = false;
        for (const QChar c : value) hasLetter = hasLetter || c.isLetter();
        if (hasLetter) return hexColor(value);
    }
    if (!value.startsWith(QLatin1String("rgb"))) return std::nullopt;
    const qsizetype opening = value.indexOf(QLatin1Char('('));
    if (opening < 0 || !value.endsWith(QLatin1Char(')'))) return std::nullopt;
    QStringList components = value.mid(opening + 1, value.size() - opening - 2).split(QLatin1Char(','));
    for (QString &component : components) component = component.trimmed();
    if (components.size() != 3 && components.size() != 4) return std::nullopt;
    const auto red = percentOr(components[0], 255);
    const auto green = percentOr(components[1], 255);
    const auto blue = percentOr(components[2], 255);
    if (!red || !green || !blue) return std::nullopt;
    double alpha = 1;
    if (components.size() == 4) {
        const auto parsed = percentOr(components[3], 1);
        if (!parsed) return std::nullopt;
        alpha = *parsed;
    }
    return QColor::fromRgbF(float(*red), float(*green), float(*blue), float(alpha));
}

QSizeF previewTextSize() { return QSizeF(560, 360); }

QSizeF previewImageSize(int pixelWidth, int pixelHeight, int angle) {
    if (pixelWidth <= 0 || pixelHeight <= 0) return previewTextSize();
    const bool quarterTurn = (angle % 180) != 0;
    const qreal width = quarterTurn ? pixelHeight : pixelWidth;
    const qreal height = quarterTurn ? pixelWidth : pixelHeight;
    const qreal scale = std::min(512.0 / width, 270.0 / height);
    return QSizeF(std::max(360.0, width * scale + 48), std::max(300.0, height * scale + 135));
}

namespace {

QString jsonString(const QString &value) {
    // QJsonDocument's own escaping, minus the escaped slash JSONSerialization omits.
    QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
    encoded = encoded.mid(1, encoded.size() - 2);
    return encoded.replace(QStringLiteral("\\/"), QStringLiteral("/"));
}

QString jsonNumber(double value) {
    if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 9007199254740992.0) {
        return QString::number(qint64(value));
    }
    return QString::number(value, 'g', 15);
}

void appendJson(QString &out, const QJsonValue &value, int depth) {
    const QString indent(depth * 2, QLatin1Char(' '));
    const QString inner((depth + 1) * 2, QLatin1Char(' '));
    switch (value.type()) {
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) { out += QStringLiteral("{\n\n") + indent + QLatin1Char('}'); return; }
        QStringList keys = object.keys();
        keys.sort();
        out += QStringLiteral("{\n");
        for (int index = 0; index < keys.size(); ++index) {
            out += inner + jsonString(keys.at(index)) + QStringLiteral(" : ");
            appendJson(out, object.value(keys.at(index)), depth + 1);
            out += index + 1 < keys.size() ? QStringLiteral(",\n") : QStringLiteral("\n");
        }
        out += indent + QLatin1Char('}');
        return;
    }
    case QJsonValue::Array: {
        const QJsonArray array = value.toArray();
        if (array.isEmpty()) { out += QStringLiteral("[\n\n") + indent + QLatin1Char(']'); return; }
        out += QStringLiteral("[\n");
        for (int index = 0; index < array.size(); ++index) {
            out += inner;
            appendJson(out, array.at(index), depth + 1);
            out += index + 1 < array.size() ? QStringLiteral(",\n") : QStringLiteral("\n");
        }
        out += indent + QLatin1Char(']');
        return;
    }
    case QJsonValue::String: out += jsonString(value.toString()); return;
    case QJsonValue::Double: out += jsonNumber(value.toDouble()); return;
    case QJsonValue::Bool: out += value.toBool() ? QStringLiteral("true") : QStringLiteral("false"); return;
    default: out += QStringLiteral("null"); return;
    }
}

} // namespace

QString formatJson(const QString &source) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(source.toUtf8(), &error);
    // JSONSerialization.isValidJSONObject: the top level must be an object or array.
    if (error.error != QJsonParseError::NoError || (!document.isObject() && !document.isArray())) return QString();
    QString out;
    appendJson(out, document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array()), 0);
    return out;
}

QString editableLanguage(const ClipboardItem &item) {
    const QString language = item.previewLanguage.toLower();
    static const QStringList editable{QStringLiteral("code"), QStringLiteral("json"), QStringLiteral("sql"),
                                      QStringLiteral("java"), QStringLiteral("javascript"), QStringLiteral("js")};
    return editable.contains(language) ? language : QString();
}

QString previewDateTime(const QDateTime &copiedAt) {
    return QStringLiteral("%1年%2月%3日 %4")
        .arg(copiedAt.date().year())
        .arg(copiedAt.date().month())
        .arg(copiedAt.date().day())
        .arg(copiedAt.time().toString(QStringLiteral("HH:mm")));
}

QString previewCategoryName(const ClipboardItem &item) {
    if (item.isFiles()) return QStringLiteral("文件 · %1 项").arg(item.files.size());
    if (!item.category.isEmpty()) return item.category;
    return item.isText() ? QStringLiteral("文本") : QStringLiteral("图片");
}

QString cardKind(const ClipboardItem &item) {
    if (item.isFiles()) return QStringLiteral("files");
    if (item.isImage()) return QStringLiteral("image");
    if (item.previewLanguage.compare(QStringLiteral("color"), Qt::CaseInsensitive) == 0 && parseColor(item.text)) {
        return QStringLiteral("color");
    }
    if (webUrl(item).isValid()) return QStringLiteral("link");
    return QStringLiteral("text");
}

bool usesImmersiveHeader(const ClipboardItem &item) {
    return item.isImage() || item.previewLanguage.compare(QStringLiteral("color"), Qt::CaseInsensitive) == 0;
}

} // namespace DeckPresentation
