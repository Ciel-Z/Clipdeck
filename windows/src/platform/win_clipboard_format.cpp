#include "win_clipboard_format.h"

#include <QBuffer>
#include <QRegularExpression>

#include <cstdio>
#include <cstring>

namespace WinClipboard {

namespace {
UINT registered(const wchar_t *name) { return RegisterClipboardFormatW(name); }

int headerValue(const QByteArray &data, const char *key) {
    const int at = data.indexOf(key);
    if (at < 0) return -1;
    int end = at + int(std::strlen(key));
    int value = 0;
    bool any = false;
    while (end < data.size() && data.at(end) >= '0' && data.at(end) <= '9') {
        value = value * 10 + (data.at(end) - '0');
        any = true;
        ++end;
    }
    return any ? value : -1;
}
} // namespace

UINT formatHtml() { static const UINT id = registered(L"HTML Format"); return id; }
UINT formatPng() { static const UINT id = registered(L"PNG"); return id; }
UINT formatInternalMarker() { static const UINT id = registered(L"ClipDeck.InternalWrite"); return id; }
UINT formatPreferredDropEffect() { static const UINT id = registered(L"Preferred DropEffect"); return id; }

bool open(HWND owner, int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

QByteArray htmlFromCfHtml(const QByteArray &cfHtml) {
    QByteArray data = cfHtml;
    while (data.endsWith('\0')) data.chop(1);
    const int startHtml = headerValue(data, "StartHTML:");
    const int endHtml = headerValue(data, "EndHTML:");
    if (startHtml >= 0 && endHtml > startHtml && endHtml <= data.size()) return data.mid(startHtml, endHtml - startHtml);
    const int startFragment = headerValue(data, "StartFragment:");
    const int endFragment = headerValue(data, "EndFragment:");
    if (startFragment >= 0 && endFragment > startFragment && endFragment <= data.size()) {
        return "<html><body>" + data.mid(startFragment, endFragment - startFragment) + "</body></html>";
    }
    return QByteArray();
}

QByteArray cfHtmlFromHtml(const QByteArray &html) {
    static const char headerTemplate[] =
        "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n";
    const QByteArray prefix = "<html><body>\r\n<!--StartFragment-->";
    const QByteArray suffix = "<!--EndFragment-->\r\n</body></html>";
    char probe[160];
    const int headerLength = std::snprintf(probe, sizeof(probe), headerTemplate, 0, 0, 0, 0);
    const int startHtml = headerLength;
    const int startFragment = startHtml + int(prefix.size());
    const int endFragment = startFragment + int(html.size());
    const int endHtml = endFragment + int(suffix.size());
    char header[160];
    std::snprintf(header, sizeof(header), headerTemplate, startHtml, endHtml, startFragment, endFragment);
    return QByteArray(header, headerLength) + prefix + html + suffix;
}

bool isSafeImageSize(qint64 width, qint64 height, qint64 maximumPixels) {
    // Divide instead of multiplying so huge dimensions cannot overflow.
    if (width <= 0 || height <= 0 || width > maximumPixels) return false;
    return height <= maximumPixels / width;
}

QByteArray pngFromDib(const QByteArray &dib, qint64 maximumPixels) {
    if (dib.size() < int(sizeof(BITMAPINFOHEADER))) return QByteArray();
    BITMAPINFOHEADER header;
    std::memcpy(&header, dib.constData(), sizeof(header));
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > quint32(dib.size())) return QByteArray();
    const qint64 width = header.biWidth;
    const qint64 height = header.biHeight < 0 ? -qint64(header.biHeight) : header.biHeight;
    if (!isSafeImageSize(width, height, maximumPixels)) return QByteArray();

    quint32 colors = header.biClrUsed;
    if (colors == 0 && header.biBitCount <= 8) colors = 1u << header.biBitCount;
    const quint32 masks = (header.biCompression == BI_BITFIELDS && header.biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
    const quint32 pixelOffset = 14 + header.biSize + masks + colors * 4;

    QByteArray bmp;
    bmp.reserve(14 + dib.size());
    bmp.append("BM", 2);
    const quint32 fileSize = quint32(14 + dib.size());
    const quint32 reserved = 0;
    bmp.append(reinterpret_cast<const char *>(&fileSize), 4);
    bmp.append(reinterpret_cast<const char *>(&reserved), 4);
    bmp.append(reinterpret_cast<const char *>(&pixelOffset), 4);
    bmp.append(dib);

    QImage image;
    if (!image.loadFromData(bmp, "BMP") || image.isNull()) return QByteArray();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return QByteArray();
    return png;
}

QByteArray dibFromImage(const QImage &source) {
    if (source.isNull()) return QByteArray();
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = image.width();
    header.biHeight = image.height(); // positive: bottom-up rows
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = DWORD(image.sizeInBytes());
    QByteArray dib(int(sizeof(header) + image.sizeInBytes()), Qt::Uninitialized);
    std::memcpy(dib.data(), &header, sizeof(header));
    char *out = dib.data() + sizeof(header);
    const qsizetype stride = image.width() * 4;
    for (int row = image.height() - 1; row >= 0; --row) {
        std::memcpy(out, image.constScanLine(row), size_t(stride));
        out += stride;
    }
    return dib;
}

} // namespace WinClipboard
