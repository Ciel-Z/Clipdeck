#pragma once

#include "history_repository.h"

#include <windows.h>

namespace WinClipboard {

struct WriteResult {
    bool ok = false;
    quint32 sequence = 0; // GetClipboardSequenceNumber() right after the write
    QString error;
};

// Puts a history item back on the system clipboard (mac PasteService
// writeToPasteboard). Text carries CF_UNICODETEXT plus CF_HTML unless
// plainText; images carry "PNG" and CF_DIB; files carry CF_HDROP with a copy
// drop effect. Every write is tagged so ClipDeck's own monitor skips it.
WriteResult write(const ClipboardItem &item, bool plainText, HWND owner);

} // namespace WinClipboard
