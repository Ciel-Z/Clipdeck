#include "app_preferences.h"

#include "win_paste_service.h"

#include <QSettings>

#include <windows.h>

namespace AppPreferences {

namespace {

const QString retentionKey = QStringLiteral("history/retentionDays");
const QString recordsImagesKey = QStringLiteral("clipboard/recordsImages");
const QString maximumImageKey = QStringLiteral("clipboard/maximumImageMegabytes");
const QString quickPasteKey = QStringLiteral("shortcuts/quickPasteModifier");
const QString plainTextKey = QStringLiteral("shortcuts/plainTextModifier");

QString validModifier(const QString &value, const QString &fallback) {
    return value == QLatin1String("ctrl") || value == QLatin1String("shift") || value == QLatin1String("alt") ? value
                                                                                                             : fallback;
}

} // namespace

int retentionDays() { return std::max(0, QSettings().value(retentionKey, 30).toInt()); }
void setRetentionDays(int days) { QSettings().setValue(retentionKey, std::max(0, days)); }

bool preserveFormatting() { return QSettings().value(QLatin1String(WinPasteService::preserveFormattingKey), false).toBool(); }
void setPreserveFormatting(bool preserve) {
    QSettings().setValue(QLatin1String(WinPasteService::preserveFormattingKey), preserve);
}

bool recordsImages() { return QSettings().value(recordsImagesKey, true).toBool(); }
void setRecordsImages(bool records) { QSettings().setValue(recordsImagesKey, records); }
int maximumImageMegabytes() { return QSettings().value(maximumImageKey, 10).toInt(); }
void setMaximumImageMegabytes(int megabytes) { QSettings().setValue(maximumImageKey, megabytes); }

HotkeySpec defaultNextCategoryShortcut() { return {HotkeySpec::Control, VK_RIGHT}; }
HotkeySpec defaultPreviousCategoryShortcut() { return {HotkeySpec::Control, VK_LEFT}; }
HotkeySpec nextCategoryShortcut() { return HotkeySpec::load(QStringLiteral("nextCategory"), defaultNextCategoryShortcut()); }
HotkeySpec previousCategoryShortcut() {
    return HotkeySpec::load(QStringLiteral("previousCategory"), defaultPreviousCategoryShortcut());
}

QString quickPasteModifier() {
    return validModifier(QSettings().value(quickPasteKey).toString(), QStringLiteral("ctrl"));
}
void setQuickPasteModifier(const QString &modifier) { QSettings().setValue(quickPasteKey, validModifier(modifier, QStringLiteral("ctrl"))); }
QString plainTextModifier() {
    return validModifier(QSettings().value(plainTextKey).toString(), QStringLiteral("shift"));
}
void setPlainTextModifier(const QString &modifier) { QSettings().setValue(plainTextKey, validModifier(modifier, QStringLiteral("shift"))); }

int qtModifier(const QString &modifier) {
    if (modifier == QLatin1String("shift")) return Qt::ShiftModifier;
    if (modifier == QLatin1String("alt")) return Qt::AltModifier;
    return Qt::ControlModifier;
}

int qtModifierKey(const QString &modifier) {
    if (modifier == QLatin1String("shift")) return Qt::Key_Shift;
    if (modifier == QLatin1String("alt")) return Qt::Key_Alt;
    return Qt::Key_Control;
}

void resetShortcutDefaults() {
    HotkeySpec().save();
    defaultNextCategoryShortcut().save(QStringLiteral("nextCategory"));
    defaultPreviousCategoryShortcut().save(QStringLiteral("previousCategory"));
    setQuickPasteModifier(QStringLiteral("ctrl"));
    setPlainTextModifier(QStringLiteral("shift"));
}

} // namespace AppPreferences
