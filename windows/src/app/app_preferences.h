#pragma once

#include "win_global_hotkey.h"

#include <QString>

// Port of AppPreferences.swift: QSettings keys and defaults. Image keys are
// the ones the shared capture pipeline (capture_payload.cpp) already reads.
namespace AppPreferences {

// history/retentionDays: 30 by default, 0 keeps history forever.
int retentionDays();
void setRetentionDays(int days);

bool preserveFormatting();
void setPreserveFormatting(bool preserve);

// material/blur (the Linux key): non-zero frosts the screen behind the Deck,
// 0 shows the live desktop under a plain tint. On by default here, like mac.
bool backdropBlur();
void setBackdropBlur(bool enabled);

bool recordsImages();
void setRecordsImages(bool records);
int maximumImageMegabytes();
void setMaximumImageMegabytes(int megabytes);

// Deck shortcuts. mac ⌘ maps to Ctrl: ⌘← / ⌘→ become Ctrl+← / Ctrl+→.
HotkeySpec nextCategoryShortcut();
HotkeySpec previousCategoryShortcut();
HotkeySpec defaultNextCategoryShortcut();
HotkeySpec defaultPreviousCategoryShortcut();

// "ctrl" | "shift" | "alt": quick paste defaults to Ctrl, plain text to Shift.
QString quickPasteModifier();
void setQuickPasteModifier(const QString &modifier);
QString plainTextModifier();
void setPlainTextModifier(const QString &modifier);
// Qt::KeyboardModifier and Qt::Key for a modifier name.
int qtModifier(const QString &modifier);
int qtModifierKey(const QString &modifier);

// AppPreferences.resetShortcutDefaults()
void resetShortcutDefaults();

} // namespace AppPreferences
