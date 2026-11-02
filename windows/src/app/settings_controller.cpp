#include "settings_controller.h"

#include "app_preferences.h"
#include "javascript_formatter.h"
#include "win_window_effects.h"

#include <QCoreApplication>
#include <QThread>

namespace {

// Runs a maintenance job on its own database connection (SQLite WAL), so the
// settings window stays responsive during VACUUM.
template <typename Job, typename Done>
void runMaintenance(QObject *context, const QString &path, Job job, Done done) {
    QThread *thread = QThread::create([context, path, job, done] {
        HistoryRepository repository(path);
        const int deleted = repository.isOpen() ? job(repository) : -1;
        const QString error = repository.isOpen() ? QString() : repository.lastError();
        QMetaObject::invokeMethod(context, [done, deleted, error] { done(deleted, error); }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowPriority);
}

} // namespace

SettingsController::SettingsController(HistoryRepository *repository, QObject *parent)
    : QObject(parent), repository_(repository), launch_(QCoreApplication::applicationFilePath()) {
    reloadCategories();
    refreshDatabaseSize();
}

void SettingsController::attachWindow(QWindow *window) {
    window_ = window;
    if (window) WinEffects::setDarkTitleBar(reinterpret_cast<HWND>(window->winId()));
}

void SettingsController::decorateSheet(QWindow *window) {
    if (window) WinEffects::setFramelessChrome(reinterpret_cast<HWND>(window->winId()));
}

void SettingsController::open(const QString &pane) {
    if (!pane.isEmpty()) setPane(pane);
    refreshDatabaseSize();
    Q_EMIT launchAtLoginChanged(); // the user may have changed it in Windows Settings
    Q_EMIT openRequested();
    if (window_) {
        window_->show();
        window_->raise();
        window_->requestActivate();
    }
}

void SettingsController::setPane(const QString &pane) {
    if (pane == pane_) return;
    pane_ = pane;
    if (pane_ == QLatin1String("data")) refreshDatabaseSize();
    Q_EMIT paneChanged();
}

// MARK: - 通用

bool SettingsController::preserveFormatting() const { return AppPreferences::preserveFormatting(); }
void SettingsController::setPreserveFormatting(bool preserve) {
    AppPreferences::setPreserveFormatting(preserve);
    Q_EMIT preferencesChanged();
}

void SettingsController::setLaunchAtLogin(bool enabled) {
    launchError_ = launch_.setEnabled(enabled);
    Q_EMIT launchAtLoginChanged();
}

void SettingsController::openStartupAppsSettings() { WinLaunchAtLogin::openStartupAppsSettings(); }

int SettingsController::retentionDays() const { return AppPreferences::retentionDays(); }

void SettingsController::applyRetention(int days) {
    if (days == AppPreferences::retentionDays()) return;
    AppPreferences::setRetentionDays(days);
    Q_EMIT preferencesChanged();
    if (days <= 0) return;
    const int deleted = repository_->clearExpired(days);
    if (deleted > 0) {
        maintenanceStatus_ = QStringLiteral("已清理 %1 条过期历史").arg(deleted);
        Q_EMIT historyChanged();
    }
    refreshDatabaseSize();
}

// MARK: - 图片

bool SettingsController::recordsImages() const { return AppPreferences::recordsImages(); }
void SettingsController::setRecordsImages(bool records) {
    AppPreferences::setRecordsImages(records);
    Q_EMIT preferencesChanged();
}
int SettingsController::maximumImageMegabytes() const { return AppPreferences::maximumImageMegabytes(); }
void SettingsController::setMaximumImageMegabytes(int megabytes) {
    AppPreferences::setMaximumImageMegabytes(megabytes);
    Q_EMIT preferencesChanged();
}

// MARK: - 键盘快捷键

QString SettingsController::showDeckShortcut() const { return HotkeySpec::load().toString(); }
QString SettingsController::nextCategoryShortcut() const { return AppPreferences::nextCategoryShortcut().toString(); }
QString SettingsController::previousCategoryShortcut() const { return AppPreferences::previousCategoryShortcut().toString(); }
QString SettingsController::quickPasteModifier() const { return AppPreferences::quickPasteModifier(); }
void SettingsController::setQuickPasteModifier(const QString &modifier) {
    AppPreferences::setQuickPasteModifier(modifier);
    Q_EMIT shortcutsChanged();
}
QString SettingsController::plainTextModifier() const { return AppPreferences::plainTextModifier(); }
void SettingsController::setPlainTextModifier(const QString &modifier) {
    AppPreferences::setPlainTextModifier(modifier);
    Q_EMIT shortcutsChanged();
}

void SettingsController::setRecording(bool recording) { Q_EMIT recordingChanged(recording); }

bool SettingsController::recordShortcut(const QString &name, int qtKey, int qtModifiers) {
    const HotkeySpec spec = HotkeySpec::fromQtKey(qtKey, qtModifiers);
    if (!spec.isValid()) return false;
    spec.save(name);
    shortcutError_.clear();
    Q_EMIT shortcutsChanged();
    return true;
}

void SettingsController::resetShortcuts() {
    AppPreferences::resetShortcutDefaults();
    shortcutError_.clear();
    Q_EMIT shortcutsChanged();
}

void SettingsController::reportHotkeyError(const QString &error) {
    if (error == shortcutError_) return;
    shortcutError_ = error;
    Q_EMIT shortcutsChanged();
}

// MARK: - 分类

QVariantList SettingsController::categories() const {
    QVariantList list;
    int position = 0;
    for (const ClipboardCategory &category : categories_) {
        list.append(QVariantMap{{QStringLiteral("id"), category.databaseID},
                                {QStringLiteral("name"), category.name},
                                {QStringLiteral("color"), HistoryRepository::categoryColor(category.icon, position++)},
                                {QStringLiteral("previewLanguage"), category.previewLanguage}});
    }
    return list;
}

void SettingsController::reloadCategories(qint64 select) {
    categories_ = repository_->categories();
    Q_EMIT categoriesChanged();
    qint64 target = select >= 0 ? select : selectedId_;
    const bool exists = std::any_of(categories_.cbegin(), categories_.cend(),
                                    [&](const ClipboardCategory &category) { return category.databaseID == target; });
    if (!exists) target = categories_.isEmpty() ? -1 : categories_.first().databaseID;
    selectCategory(target);
}

void SettingsController::selectCategory(qint64 id) {
    selectedId_ = id;
    for (int position = 0; position < categories_.size(); ++position) {
        const ClipboardCategory &category = categories_.at(position);
        if (category.databaseID != id) continue;
        name_ = category.name;
        script_ = category.script;
        previewLanguage_ = category.previewLanguage.isEmpty() ? QStringLiteral("text") : category.previewLanguage;
        color_ = HistoryRepository::categoryColor(category.icon, position);
        break;
    }
    testResult_.clear();
    testConsole_.clear();
    error_.clear();
    Q_EMIT categoryEditorChanged();
    Q_EMIT testChanged();
}

void SettingsController::setCategoryName(const QString &name) {
    if (name == name_) return;
    name_ = name;
    Q_EMIT categoryEditorChanged();
}

void SettingsController::setCategoryPreviewLanguage(const QString &language) {
    if (language == previewLanguage_) return;
    previewLanguage_ = language;
    Q_EMIT categoryEditorChanged();
}

void SettingsController::setCategoryColor(const QString &color) {
    if (color == color_) return;
    color_ = color;
    Q_EMIT categoryEditorChanged();
}

void SettingsController::setCategoryScript(const QString &script) {
    if (script == script_) return;
    script_ = script;
    Q_EMIT categoryEditorChanged();
}

bool SettingsController::categoryHasUnsavedChanges() const {
    for (int position = 0; position < categories_.size(); ++position) {
        const ClipboardCategory &stored = categories_.at(position);
        if (stored.databaseID != selectedId_) continue;
        const QString storedLanguage = stored.previewLanguage.isEmpty()
            ? QStringLiteral("text") : stored.previewLanguage;
        return name_.trimmed() != stored.name
            || color_.compare(HistoryRepository::categoryColor(stored.icon, position), Qt::CaseInsensitive) != 0
            || script_ != stored.script
            || previewLanguage_ != storedLanguage;
    }
    return false;
}

void SettingsController::setTestText(const QString &text) {
    if (text == testText_) return;
    testText_ = text;
    Q_EMIT categoryEditorChanged();
}

void SettingsController::addCategory() {
    const QStringList palette = HistoryRepository::categoryColorPalette();
    if (!repository_->createCategory(QStringLiteral("新分类"), palette.at(categories_.size() % palette.size()),
                                     QStringLiteral("function classify(item) {\n    return false;\n}"), QString())) {
        error_ = repository_->lastError();
        Q_EMIT categoryEditorChanged();
        return;
    }
    qint64 newest = -1;
    for (const ClipboardCategory &category : repository_->categories()) newest = std::max(newest, category.databaseID);
    reloadCategories(newest);
    Q_EMIT categoriesChanged();
}

void SettingsController::saveCategory() {
    if (selectedId_ < 0 || name_.trimmed().isEmpty()) return;
    const QString scriptError = repository_->categoryScriptError(script_);
    if (!scriptError.isEmpty()) {
        error_ = scriptError;
        Q_EMIT categoryEditorChanged();
        return;
    }
    if (!repository_->updateCategory(selectedId_, name_.trimmed(), color_, script_,
                                     previewLanguage_ == QLatin1String("text") ? QString() : previewLanguage_)) {
        error_ = repository_->lastError();
        Q_EMIT categoryEditorChanged();
        return;
    }
    reloadCategories(selectedId_);
}

void SettingsController::deleteCategory(qint64 id) {
    if (!repository_->removeCategory(id)) {
        error_ = repository_->lastError();
        Q_EMIT categoryEditorChanged();
        return;
    }
    if (selectedId_ == id) selectedId_ = -1;
    reloadCategories();
}

void SettingsController::reorderCategories(const QVariantList &order) {
    QVector<qint64> ids;
    for (const QVariant &id : order) ids.append(id.toLongLong());
    if (!repository_->reorderCategories(ids)) {
        error_ = repository_->lastError();
        Q_EMIT categoryEditorChanged();
    }
    categories_ = repository_->categories();
    Q_EMIT categoriesChanged();
}

void SettingsController::restorePresets() {
    if (!repository_->restoreDefaultCategories()) {
        error_ = repository_->lastError();
        Q_EMIT categoryEditorChanged();
    }
    reloadCategories();
}

void SettingsController::testScript() {
    QString error;
    QStringList console;
    const bool matches = repository_->categoryScriptMatches(script_, testText_, previewLanguage_, &error, &console);
    testConsole_.clear();
    for (const QString &line : console) {
        // Lines are "LEVEL  message" (see categoryScriptMatches).
        const qsizetype split = line.indexOf(QStringLiteral("  "));
        const QString level = split < 0 ? QStringLiteral("LOG") : line.left(split);
        const QString message = split < 0 ? line : line.mid(split + 2);
        testConsole_.append(QVariantMap{{QStringLiteral("level"), level.toLower()},
                                        {QStringLiteral("message"), message}});
    }
    testResult_ = !error.isEmpty() ? QStringLiteral("错误：") + error
                                   : matches ? QStringLiteral("匹配：true") : QStringLiteral("匹配：false");
    Q_EMIT testChanged();
}

void SettingsController::formatScript() { setCategoryScript(JavaScriptFormatter::format(script_)); }

static QVariantList displayChoices(const QVector<HistoryRepository::DisplayChoice> &choices) {
    QVariantList list;
    for (const auto &choice : choices) {
        list.append(QVariantMap{{QStringLiteral("value"), choice.value},
                                {QStringLiteral("text"), choice.label},
                                {QStringLiteral("detail"), choice.detail}});
    }
    return list;
}

QVariantList SettingsController::cardTypes() const { return displayChoices(HistoryRepository::cardTypeChoices()); }

QString SettingsController::categoryCardType() const { return HistoryRepository::cardTypeOf(previewLanguage_); }

void SettingsController::setCategoryCardType(const QString &cardType) {
    setCategoryPreviewLanguage(HistoryRepository::previewLanguageForCardType(cardType, previewLanguage_));
}

// MARK: - 数据

QString SettingsController::formatByteCount(qint64 bytes) {
    if (bytes < 1000) return QStringLiteral("%1 字节").arg(bytes);
    const char *units[] = {"KB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = -1;
    while (value >= 1000 && unit < 3) {
        value /= 1000;
        ++unit;
    }
    // Like ByteCountFormatter: whole kilobytes, one decimal from megabytes up.
    const QString number = unit == 0 ? QString::number(qRound(value)) : QString::number(value, 'f', value < 100 ? 1 : 0);
    return number + QLatin1Char(' ') + QLatin1String(units[unit]);
}

QString SettingsController::databaseSize() const { return formatByteCount(databaseBytes_); }

void SettingsController::refreshDatabaseSize() {
    databaseBytes_ = repository_->databaseByteCount();
    Q_EMIT databaseChanged();
}

void SettingsController::clearCache() {
    if (maintaining_) return;
    maintaining_ = true;
    maintenanceStatus_ = QStringLiteral("正在清理过期内容并回收空间…");
    Q_EMIT databaseChanged();
    const int days = AppPreferences::retentionDays();
    runMaintenance(
        this, repository_->databasePath(),
        [days](HistoryRepository &repository) { return repository.clearExpiredAndReclaim(days); },
        [this](int deleted, const QString &error) {
            maintenanceStatus_ = deleted < 0 ? QStringLiteral("清理失败：") + error
                                             : QStringLiteral("已删除 %1 条过期历史并回收数据库空间").arg(deleted);
            maintaining_ = false;
            refreshDatabaseSize();
            Q_EMIT historyChanged();
        });
}

void SettingsController::clearAllHistory() {
    if (maintaining_) return;
    maintaining_ = true;
    maintenanceStatus_ = QStringLiteral("正在删除全部历史…");
    Q_EMIT databaseChanged();
    runMaintenance(
        this, repository_->databasePath(),
        [](HistoryRepository &repository) { return repository.clearAll(); }, // reclaims space too
        [this](int deleted, const QString &error) {
            maintenanceStatus_ = deleted < 0 ? QStringLiteral("删除失败：") + error
                                             : QStringLiteral("已删除 %1 条历史并回收数据库空间").arg(deleted);
            maintaining_ = false;
            refreshDatabaseSize();
            Q_EMIT historyChanged();
        });
}
