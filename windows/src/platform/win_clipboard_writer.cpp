#include "win_clipboard_writer.h"

#include "win_clipboard_format.h"

#include <QBuffer>
#include <QDir>

#include <shlobj.h>

#include <cstring>

namespace WinClipboard {

namespace {

bool setData(UINT format, const void *data, size_t size) {
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!block) return false;
    void *target = GlobalLock(block);
    if (!target) {
        GlobalFree(block);
        return false;
    }
    std::memcpy(target, data, size);
    GlobalUnlock(block);
    if (!SetClipboardData(format, block)) {
        GlobalFree(block);
        return false;
    }
    return true; // the clipboard owns the block now
}

bool setBytes(UINT format, const QByteArray &bytes) { return setData(format, bytes.constData(), size_t(bytes.size())); }

bool setUnicodeText(const QString &text) {
    QString crlf = text;
    crlf.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    crlf.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    const std::wstring wide = crlf.toStdWString();
    return setData(CF_UNICODETEXT, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
}

bool setFiles(const QVector<ClipboardFileReference> &files) {
    std::wstring paths;
    for (const auto &file : files) {
        paths += QDir::toNativeSeparators(file.path).toStdWString();
        paths.push_back(L'\0');
    }
    paths.push_back(L'\0');
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    QByteArray block(int(sizeof(DROPFILES) + paths.size() * sizeof(wchar_t)), '\0');
    std::memcpy(block.data(), &header, sizeof(header));
    std::memcpy(block.data() + sizeof(header), paths.data(), paths.size() * sizeof(wchar_t));
    if (!setBytes(CF_HDROP, block)) return false;
    const DWORD copyEffect = 1; // DROPEFFECT_COPY
    return setData(formatPreferredDropEffect(), &copyEffect, sizeof(copyEffect));
}

} // namespace

WriteResult write(const ClipboardItem &item, bool plainText, HWND owner) {
    WriteResult result;
    if (!open(owner)) {
        result.error = QStringLiteral("剪贴板正被其他程序占用");
        return result;
    }
    EmptyClipboard();

    bool ok = true;
    if (item.isFiles()) {
        ok = !item.files.isEmpty() && setFiles(item.files);
    } else if (item.isImage()) {
        QImage image = item.image;
        QByteArray png = item.imageData;
        if (image.isNull() && !png.isEmpty()) image.loadFromData(png, "PNG");
        if (png.isEmpty() && !image.isNull()) {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
        }
        ok = !png.isEmpty() && setBytes(formatPng(), png);
        if (ok && !image.isNull()) ok = setBytes(CF_DIB, dibFromImage(image));
    } else {
        ok = setUnicodeText(item.text);
        if (ok && !plainText && !item.richTextHtml.isEmpty()) {
            ok = setBytes(formatHtml(), cfHtmlFromHtml(item.richTextHtml.toUtf8()));
        }
    }
    if (ok) {
        const DWORD marker = 1;
        ok = setData(formatInternalMarker(), &marker, sizeof(marker));
    }
    const DWORD error = GetLastError();
    CloseClipboard();
    if (!ok) {
        result.error = QStringLiteral("写入剪贴板失败 (%1)").arg(error);
        return result;
    }
    // Closing the clipboard advances the sequence number, so read it afterwards.
    result.ok = true;
    result.sequence = GetClipboardSequenceNumber();
    return result;
}

} // namespace WinClipboard
