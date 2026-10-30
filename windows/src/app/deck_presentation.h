#pragma once

#include "history_repository.h"

#include <QColor>
#include <QDateTime>
#include <QSizeF>
#include <QUrl>

#include <optional>

// Ports of the ClipboardCardView computed properties (HistoryPanelView.swift)
// and ClipboardColorParser. Pure functions so they can be unit tested.
namespace DeckPresentation {

// "现在" / "N 分钟" / "N 小时" / "N 天" / "M月d日" (relativeTimeText)
QString relativeTime(const QDateTime &copiedAt, const QDateTime &now);
// ClipboardItem.webURL: a single http(s) URL with a host, no whitespace.
QUrl webUrl(const ClipboardItem &item);
// ClipboardItem.linkHost: host without a leading "www."
QString linkHost(const ClipboardItem &item);
// linkDisplayTitle
QString linkDisplayTitle(const ClipboardItem &item);
// cardHeaderTitle: "文件 · N 项" / "链接" / title
QString cardHeaderTitle(const ClipboardItem &item);
// categoryLabel: category name, or the detected language for code.
QString categoryLabel(const ClipboardItem &item);
QString displayCodeLanguage(const QString &language);
// ClipboardColorParser.color(from:)
std::optional<QColor> parseColor(const QString &source);

// ContentPreviewLayout: popover sizes for text/files and for images.
QSizeF previewTextSize();
QSizeF previewImageSize(int pixelWidth, int pixelHeight, int angle);
// JSONTextFormatter.format: pretty JSON with sorted keys, 2-space indent and
// " : " separators like JSONSerialization; empty when the input is invalid.
QString formatJson(const QString &source);
// ContentEditorView.editableLanguage
QString editableLanguage(const ClipboardItem &item);
// copiedAt.formatted(date: .abbreviated, time: .shortened) in zh-CN
QString previewDateTime(const QDateTime &copiedAt);
// ContentPreviewView.categoryName
QString previewCategoryName(const ClipboardItem &item);

// Card body kind used by Card.qml: text, link, color, image, files.
QString cardKind(const ClipboardItem &item);
// usesImmersiveHeader
bool usesImmersiveHeader(const ClipboardItem &item);

} // namespace DeckPresentation
