#pragma once
#include "clipboard_snapshot.h"
#include "../history_repository.h"
#include <optional>

// Called only by the capture worker. Image references are recognized from
// their real file type/decoder, never a co-published application thumbnail.
std::optional<ClipboardItem> payloadFromSnapshot(const ClipboardSnapshot &snapshot);
