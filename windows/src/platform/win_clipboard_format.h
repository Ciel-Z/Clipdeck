#pragma once

#include <QByteArray>
#include <QImage>

#include <windows.h>

// Clipboard format helpers shared by the reader and the writer.
namespace WinClipboard {

// Registered format ids (registered once, cached).
UINT formatHtml();                // "HTML Format"
UINT formatPng();                 // "PNG"
UINT formatInternalMarker();      // ClipDeck's own writes; the monitor skips them
UINT formatPreferredDropEffect(); // "Preferred DropEffect"

// OpenClipboard fails while another process holds it; retry briefly.
bool open(HWND owner, int attempts = 10);

// CF_HTML <-> the plain HTML document the history stores (text/html on Linux).
QByteArray htmlFromCfHtml(const QByteArray &cfHtml);
QByteArray cfHtmlFromHtml(const QByteArray &html);

// Images: CF_DIB/CF_DIBV5 payload <-> PNG. pngFromDib refuses images above
// maximumPixels (mac ClipboardReader.maximumPixelCount).
bool isSafeImageSize(qint64 width, qint64 height, qint64 maximumPixels = 60000000);
QByteArray pngFromDib(const QByteArray &dib, qint64 maximumPixels = 60000000);
QByteArray dibFromImage(const QImage &image);

} // namespace WinClipboard
