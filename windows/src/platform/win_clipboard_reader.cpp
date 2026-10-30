#include "win_clipboard_reader.h"

#include "win_clipboard_format.h"

#include <QDir>
#include <QString>
#include <QUrl>

#include <shellapi.h>

namespace WinClipboard {

namespace {

constexpr SIZE_T kMaximumBlock = 256ull * 1024 * 1024;

QByteArray blockBytes(HANDLE handle) {
    if (!handle) return QByteArray();
    const SIZE_T size = GlobalSize(handle);
    if (size == 0 || size > kMaximumBlock) return QByteArray();
    const void *data = GlobalLock(handle);
    if (!data) return QByteArray();
    QByteArray bytes(static_cast<const char *>(data), qsizetype(size));
    GlobalUnlock(handle);
    return bytes;
}

bool available(UINT format) { return format != 0 && IsClipboardFormatAvailable(format) != FALSE; }

bool excluded() {
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT viewerIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    static const UINT canInclude = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (available(exclude) || available(viewerIgnore) || available(formatInternalMarker())) return true;
    if (available(canInclude)) {
        const QByteArray flag = blockBytes(GetClipboardData(canInclude));
        DWORD value = 1;
        if (flag.size() >= int(sizeof(DWORD))) memcpy(&value, flag.constData(), sizeof(DWORD));
        if (value == 0) return true;
    }
    return false;
}

QByteArray fileList() {
    if (!available(CF_HDROP)) return QByteArray();
    HANDLE handle = GetClipboardData(CF_HDROP);
    if (!handle) return QByteArray();
    HDROP drop = static_cast<HDROP>(handle);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    QByteArray list;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) continue;
        std::wstring buffer(length + 1, L'\0');
        DragQueryFileW(drop, index, buffer.data(), length + 1);
        const QString path = QString::fromWCharArray(buffer.c_str());
        list += QUrl::fromLocalFile(QDir::fromNativeSeparators(path)).toEncoded() + "\r\n";
    }
    return list;
}

QByteArray imagePng() {
    if (available(formatPng())) {
        const QByteArray png = blockBytes(GetClipboardData(formatPng()));
        if (!png.isEmpty()) return png;
    }
    for (UINT format : {UINT(CF_DIBV5), UINT(CF_DIB)}) {
        if (!available(format)) continue;
        const QByteArray png = pngFromDib(blockBytes(GetClipboardData(format)));
        if (!png.isEmpty()) return png;
    }
    return QByteArray();
}

QByteArray unicodeText() {
    if (!available(CF_UNICODETEXT)) return QByteArray();
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return QByteArray();
    const SIZE_T size = GlobalSize(handle);
    if (size < sizeof(wchar_t) || size > kMaximumBlock) return QByteArray();
    const void *data = GlobalLock(handle);
    if (!data) return QByteArray();
    QString text = QString::fromWCharArray(static_cast<const wchar_t *>(data), -1);
    // The block may be larger than the string; fromWCharArray stops at NUL.
    GlobalUnlock(handle);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.toUtf8();
}

} // namespace

ReadResult read(HWND opener) {
    ReadResult result;
    if (!open(opener)) {
        result.status = ReadStatus::Busy;
        return result;
    }
    struct Closer { ~Closer() { CloseClipboard(); } } closer;

    if (excluded()) {
        result.status = ReadStatus::Excluded;
        return result;
    }
    ClipboardSnapshot &snapshot = result.snapshot;
    snapshot.copiedAt = QDateTime::currentDateTime();
    snapshot.uriList = fileList();
    if (snapshot.uriList.isEmpty()) {
        snapshot.png = imagePng();
        snapshot.text = unicodeText();
        if (available(formatHtml())) snapshot.html = htmlFromCfHtml(blockBytes(GetClipboardData(formatHtml())));
    }
    result.status = (snapshot.uriList.isEmpty() && snapshot.png.isEmpty() && snapshot.text.isEmpty())
        ? ReadStatus::Empty : ReadStatus::Ok;
    return result;
}

} // namespace WinClipboard
