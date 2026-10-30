#pragma once

#include "history_repository.h"
#include "win_launch_at_login.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QWindow>

// Settings window backend: port of CategoryManager plus the @AppStorage
// preferences bound in CategoryManagementView. QML sees it as `settings`.
class SettingsController final : public QObject {
    Q_OBJECT
    // Pane: general | images | keyboard | categories | data
    Q_PROPERTY(QString pane READ pane WRITE setPane NOTIFY paneChanged)

    // 通用
    Q_PROPERTY(bool preserveFormatting READ preserveFormatting WRITE setPreserveFormatting NOTIFY preferencesChanged)
    Q_PROPERTY(bool launchAtLogin READ launchAtLogin NOTIFY launchAtLoginChanged)
    Q_PROPERTY(QString launchStatus READ launchStatus NOTIFY launchAtLoginChanged)
    Q_PROPERTY(bool launchStatusIsError READ launchStatusIsError NOTIFY launchAtLoginChanged)
    Q_PROPERTY(bool launchNeedsSystemSettings READ launchNeedsSystemSettings NOTIFY launchAtLoginChanged)
    Q_PROPERTY(int retentionDays READ retentionDays NOTIFY preferencesChanged)

    // 图片
    Q_PROPERTY(bool recordsImages READ recordsImages WRITE setRecordsImages NOTIFY preferencesChanged)
    Q_PROPERTY(int maximumImageMegabytes READ maximumImageMegabytes WRITE setMaximumImageMegabytes NOTIFY preferencesChanged)

    // 键盘快捷键
    Q_PROPERTY(QString showDeckShortcut READ showDeckShortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString nextCategoryShortcut READ nextCategoryShortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString previousCategoryShortcut READ previousCategoryShortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString quickPasteModifier READ quickPasteModifier WRITE setQuickPasteModifier NOTIFY shortcutsChanged)
    Q_PROPERTY(QString plainTextModifier READ plainTextModifier WRITE setPlainTextModifier NOTIFY shortcutsChanged)
    Q_PROPERTY(QString shortcutError READ shortcutError NOTIFY shortcutsChanged)

    // 分类
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(qint64 selectedCategoryId READ selectedCategoryId NOTIFY categoryEditorChanged)
    Q_PROPERTY(QString categoryName READ categoryName WRITE setCategoryName NOTIFY categoryEditorChanged)
    Q_PROPERTY(QString categoryPreviewLanguage READ categoryPreviewLanguage WRITE setCategoryPreviewLanguage NOTIFY categoryEditorChanged)
    Q_PROPERTY(QString categoryScript READ categoryScript WRITE setCategoryScript NOTIFY categoryEditorChanged)
    Q_PROPERTY(QString testText READ testText WRITE setTestText NOTIFY categoryEditorChanged)
    Q_PROPERTY(QString testResult READ testResult NOTIFY testChanged)
    Q_PROPERTY(QVariantList testConsole READ testConsole NOTIFY testChanged)
    Q_PROPERTY(QString categoryError READ categoryError NOTIFY categoryEditorChanged)

    // 数据
    Q_PROPERTY(QString databaseSize READ databaseSize NOTIFY databaseChanged)
    Q_PROPERTY(QString maintenanceStatus READ maintenanceStatus NOTIFY databaseChanged)
    Q_PROPERTY(bool maintaining READ maintaining NOTIFY databaseChanged)

public:
    explicit SettingsController(HistoryRepository *repository, QObject *parent = nullptr);

    void attachWindow(QWindow *window);
    Q_INVOKABLE void open(const QString &pane = QString());

    QString pane() const { return pane_; }
    void setPane(const QString &pane);

    bool preserveFormatting() const;
    void setPreserveFormatting(bool preserve);
    bool launchAtLogin() const { return launch_.isOn(); }
    QString launchStatus() const { return launchError_.isEmpty() ? launch_.statusMessage() : launchError_; }
    bool launchStatusIsError() const { return !launchError_.isEmpty(); }
    bool launchNeedsSystemSettings() const { return launch_.isRegistered() && launch_.isDisabledInStartupApps(); }
    Q_INVOKABLE void setLaunchAtLogin(bool enabled);
    Q_INVOKABLE void openStartupAppsSettings();
    int retentionDays() const;
    // Applies a retention period; shortening it deletes expired history now.
    Q_INVOKABLE void applyRetention(int days);

    bool recordsImages() const;
    void setRecordsImages(bool records);
    int maximumImageMegabytes() const;
    void setMaximumImageMegabytes(int megabytes);

    QString showDeckShortcut() const;
    QString nextCategoryShortcut() const;
    QString previousCategoryShortcut() const;
    QString quickPasteModifier() const;
    void setQuickPasteModifier(const QString &modifier);
    QString plainTextModifier() const;
    void setPlainTextModifier(const QString &modifier);
    QString shortcutError() const { return shortcutError_; }
    // Recording pauses the global hot key so the old combination can be pressed.
    Q_INVOKABLE void setRecording(bool recording);
    // name: showDeck | nextCategory | previousCategory. False when the key is not usable.
    Q_INVOKABLE bool recordShortcut(const QString &name, int qtKey, int qtModifiers);
    Q_INVOKABLE void resetShortcuts();
    // The Deck reports whether the new global hot key could be registered.
    void reportHotkeyError(const QString &error);

    QVariantList categories() const;
    qint64 selectedCategoryId() const { return selectedId_; }
    QString categoryName() const { return name_; }
    void setCategoryName(const QString &name);
    QString categoryPreviewLanguage() const { return previewLanguage_; }
    void setCategoryPreviewLanguage(const QString &language);
    QString categoryScript() const { return script_; }
    void setCategoryScript(const QString &script);
    QString testText() const { return testText_; }
    void setTestText(const QString &text);
    QString testResult() const { return testResult_; }
    QVariantList testConsole() const { return testConsole_; }
    QString categoryError() const { return error_; }
    Q_INVOKABLE void selectCategory(qint64 id);
    Q_INVOKABLE void addCategory();
    Q_INVOKABLE void saveCategory();
    Q_INVOKABLE void deleteCategory(qint64 id);
    // The list reorders itself while dragging; the final order is saved once.
    Q_INVOKABLE void reorderCategories(const QVariantList &ids);
    Q_INVOKABLE void restorePresets();
    Q_INVOKABLE void testScript();
    Q_INVOKABLE void formatScript();
    Q_INVOKABLE QStringList previewLanguages() const;

    QString databaseSize() const;
    QString maintenanceStatus() const { return maintenanceStatus_; }
    bool maintaining() const { return maintaining_; }
    Q_INVOKABLE void refreshDatabaseSize();
    Q_INVOKABLE void clearCache();
    Q_INVOKABLE void clearAllHistory();

    // ByteCountFormatter(.file)-style size text.
    static QString formatByteCount(qint64 bytes);

Q_SIGNALS:
    void paneChanged();
    void openRequested();
    void preferencesChanged();
    void launchAtLoginChanged();
    void shortcutsChanged();
    void recordingChanged(bool recording);
    void categoriesChanged();
    void categoryEditorChanged();
    void testChanged();
    void databaseChanged();
    // History rows were deleted outside the Deck: reload it.
    void historyChanged();

private:
    void reloadCategories(qint64 select = -1);

    HistoryRepository *repository_;
    WinLaunchAtLogin launch_;
    QPointer<QWindow> window_;
    QString pane_ = QStringLiteral("general");
    QString launchError_;
    QString shortcutError_;
    QVector<ClipboardCategory> categories_;
    qint64 selectedId_ = -1;
    QString name_;
    QString previewLanguage_ = QStringLiteral("text");
    QString script_;
    QString testText_;
    QString testResult_;
    QVariantList testConsole_;
    QString error_;
    qint64 databaseBytes_ = 0;
    QString maintenanceStatus_;
    bool maintaining_ = false;
};
