#pragma once

#include "history/clipboard_snapshot.h"

#include <windows.h>

namespace WinClipboard {

enum class ReadStatus {
    Ok,
    Busy,     // another process kept the clipboard open
    Excluded, // password managers / ClipDeck's own writes asked not to be recorded
    Empty,    // nothing ClipDeck understands
};

struct ReadResult {
    ReadStatus status = ReadStatus::Empty;
    ClipboardSnapshot snapshot;
};

// Priority matches mac ClipboardReader: file list, then image, then text.
// A file copy also publishes icon bitmaps, so images are not read when
// CF_HDROP is present. Text is normalised to "\n" line endings.
ReadResult read(HWND opener);

} // namespace WinClipboard
