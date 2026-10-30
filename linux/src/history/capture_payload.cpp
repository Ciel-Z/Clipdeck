#include "capture_payload.h"
#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QSettings>

namespace {
bool loadImage(ClipboardItem &item, QImageReader &reader, int maximumBytes) {
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 60000000) return false;
    QImage image = reader.read();
    if (image.isNull()) return false;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || png.size() > maximumBytes) return false;
    item.type = ClipboardPayloadType::Image;
    item.title = QStringLiteral("图片");
    item.image = std::move(image);
    item.imagePixelSize = size;
    item.imageData = std::move(png);
    return true;
}
}

std::optional<ClipboardItem> payloadFromSnapshot(const ClipboardSnapshot &snapshot) {
    ClipboardItem item;
    item.copiedAt = snapshot.copiedAt;
    QSettings settings;
    const bool recordsImages = settings.value(QStringLiteral("clipboard/recordsImages"), true).toBool();
    const int maximumBytes = qBound(1, settings.value(QStringLiteral("clipboard/maximumImageMegabytes"), 10).toInt(), 32)
        * 1024 * 1024;
    const auto urls = FileReferences::urls(snapshot.uriList);
    if (!urls.isEmpty()) {
        QMimeDatabase mimeDatabase;
        for (const auto &url : urls) {
            const QFileInfo info(url.toLocalFile());
            const bool directory = info.isDir();
            // Content sniffing reads a bounded header, not the entire file.
            const auto mime = directory ? mimeDatabase.mimeTypeForName(QStringLiteral("inode/directory"))
                : info.isFile() ? mimeDatabase.mimeTypeForFile(info, QMimeDatabase::MatchContent)
                : mimeDatabase.mimeTypeForName(QStringLiteral("application/octet-stream"));
            if (urls.size() == 1 && !directory && mime.name().startsWith(QStringLiteral("image/"))) {
                if (!recordsImages) return std::nullopt;
                if (info.size() <= 0 || info.size() > 64LL * 1024 * 1024) return std::nullopt;
                QImageReader reader(info.absoluteFilePath());
                reader.setDecideFormatFromContent(true);
                if (loadImage(item, reader, maximumBytes)) return item;
                // Unsupported image formats remain usable native file references.
            }
            item.files.append({info.absoluteFilePath(), info.fileName(), mime.name(), directory});
        }
        item.type = ClipboardPayloadType::Files;
        item.title = item.files.size() == 1 ? item.files.first().name
            : QStringLiteral("文件 · %1 项").arg(item.files.size());
        return item;
    }
    if (!snapshot.png.isEmpty()) {
        if (!recordsImages || snapshot.png.size() > maximumBytes) return std::nullopt;
        QByteArray data = snapshot.png;
        QBuffer buffer(&data); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setDecideFormatFromContent(true);
        if (!loadImage(item, reader, maximumBytes)) return std::nullopt;
        return item;
    }
    item.text = snapshot.latin1 ? QString::fromLatin1(snapshot.text) : QString::fromUtf8(snapshot.text);
    item.richTextHtml = QString::fromUtf8(snapshot.html);
    item.title = QStringLiteral("文本");
    if (item.text.isEmpty()) return std::nullopt;
    return item;
}
