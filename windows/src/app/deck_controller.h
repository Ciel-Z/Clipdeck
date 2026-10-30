#pragma once

#include "deck_item_model.h"
#include "history_store.h"
#include "settings_controller.h"
#include "win_capture_service.h"
#include "win_global_hotkey.h"
#include "win_paste_service.h"
#include "win_tray.h"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>

#include <memory>

#include <windows.h>

class DeckImageProvider;
class FilePasteController;

// Windows counterpart of AppCoordinator + PanelController: owns capture,
// HistoryStore, paste, hot key and tray, and drives the Deck window. QML sees
// it as `deck`; every key/mouse action maps onto a HistoryStore method.
class DeckController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(DeckItemModel *model READ model CONSTANT)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(int revealToken READ revealToken NOTIFY revealRequested)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(QString selectedCategoryKey READ selectedCategoryKey NOTIFY selectedCategoryChanged)
    Q_PROPERTY(bool searchActive READ searchActive NOTIFY searchChanged)
    Q_PROPERTY(bool searchEditing READ searchEditing NOTIFY searchChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchChanged)
    Q_PROPERTY(bool quickPasteVisible READ quickPasteVisible NOTIFY quickPasteVisibleChanged)
    Q_PROPERTY(QString feedbackMessage READ feedbackMessage NOTIFY feedbackChanged)
    Q_PROPERTY(bool resizing READ resizing NOTIFY resizingChanged)
    Q_PROPERTY(bool deckVisible READ deckVisible NOTIFY deckVisibleChanged)
    // The window keeps the maximum height; the Deck is the bottom deckHeight of it,
    // so resizing never resizes the native window (or its swap chain).
    Q_PROPERTY(qreal deckHeight READ deckHeight NOTIFY deckHeightChanged)
    Q_PROPERTY(QString backdropUrl READ backdropUrl NOTIFY backdropChanged)
    // Preview / edit / rename popover: {kind: "" | "preview" | "edit" | "rename", index, …}
    Q_PROPERTY(QVariantMap popover READ popover NOTIFY popoverChanged)
    // Configurable in Settings → 键盘快捷键 (Qt::KeyboardModifier / Qt::Key values).
    Q_PROPERTY(int quickPasteModifier READ quickPasteModifier NOTIFY shortcutsChanged)
    Q_PROPERTY(int quickPasteModifierKey READ quickPasteModifierKey NOTIFY shortcutsChanged)
    Q_PROPERTY(int plainTextModifier READ plainTextModifier NOTIFY shortcutsChanged)

public:
    explicit DeckController(const QString &databasePath, DeckImageProvider *images, QObject *parent = nullptr);
    ~DeckController() override;

    // Wires the QML window, starts capture, loads history, registers the hot key.
    bool start(QQuickWindow *window, const QIcon &trayIcon);

    DeckItemModel *model() { return &model_; }
    HistoryStore *store() { return &store_; }
    int selectedIndex() const;
    int revealToken() const { return store_.selectionRevealToken(); }
    QVariantList categories() const;
    QString selectedCategoryKey() const;
    bool searchActive() const { return store_.isSearchActive(); }
    bool searchEditing() const { return store_.isSearchEditing(); }
    QString searchQuery() const { return store_.searchQuery(); }
    void setSearchQuery(const QString &query);
    bool quickPasteVisible() const { return store_.isQuickPasteOverlayVisible(); }
    QString feedbackMessage() const { return store_.filePasteFeedback().message; }
    bool resizing() const { return resizing_; }
    bool deckVisible() const { return window_ && window_->isVisible(); }
    qreal deckHeight() const { return deckHeight_; }
    QString backdropUrl() const;

    // Window
    Q_INVOKABLE void toggle(const QString &trigger);
    Q_INVOKABLE void showDeck(const QString &trigger);
    Q_INVOKABLE void hideDeck(const QString &reason);
    Q_INVOKABLE void beginResize(qreal globalY);
    Q_INVOKABLE void updateResize(qreal globalY);
    Q_INVOKABLE void endResize();

    // Panel callbacks (PanelController.swift)
    Q_INVOKABLE void escape();
    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void moveSelection(int offset);
    Q_INVOKABLE void moveCategory(int offset);
    Q_INVOKABLE void selectCategoryKey(const QString &key);
    Q_INVOKABLE void paste(int index, bool plainText);
    Q_INVOKABLE void pasteSelection();
    Q_INVOKABLE void quickPaste(int index);
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void openSelection();
    Q_INVOKABLE void beginSearch();
    Q_INVOKABLE void typeToSearch(const QString &characters);
    Q_INVOKABLE void pasteClipboardIntoSearch();
    Q_INVOKABLE void finishSearchEditing();
    Q_INVOKABLE void resumeSearchAndDeleteBackward();
    Q_INVOKABLE void collapseEmptySearch();
    Q_INVOKABLE void endSearch();
    Q_INVOKABLE void setSearchFieldFocused(bool focused);
    Q_INVOKABLE void setQuickPasteVisible(bool visible);
    // pane: general | images | keyboard | categories | data (empty keeps the last one)
    Q_INVOKABLE void openSettings(const QString &pane = QString());
    Q_INVOKABLE void quit();
    SettingsController *settings() { return &settings_; }
    int quickPasteModifier() const;
    int quickPasteModifierKey() const;
    int plainTextModifier() const;
    // -1 previous category, 1 next category, 0 neither (configurable shortcuts).
    Q_INVOKABLE int categoryShortcutDirection(int qtKey, int qtModifiers) const;

    // Popovers and the context menu (HistoryPanelView .popover / .contextMenu)
    QVariantMap popover() const { return popover_; }
    Q_INVOKABLE void togglePreview();
    Q_INVOKABLE void editSelection();
    Q_INVOKABLE void renameSelection();
    Q_INVOKABLE void previewItem(int index);
    Q_INVOKABLE void editItem(int index);
    Q_INVOKABLE void renameItem(int index);
    Q_INVOKABLE void openItem(int index);
    Q_INVOKABLE void deleteItem(int index);
    Q_INVOKABLE void cancelPopover();
    Q_INVOKABLE void saveRename(const QString &title);
    Q_INVOKABLE void saveText(const QString &text);
    Q_INVOKABLE void saveImageAngle(int angle);
    Q_INVOKABLE QString formatJson(const QString &text) const;
    Q_INVOKABLE void scrollPreview(int direction) { Q_EMIT previewScrollRequested(direction); }
    Q_INVOKABLE void activateDeck();

Q_SIGNALS:
    void selectionChanged();
    void revealRequested();
    void categoriesChanged();
    void selectedCategoryChanged();
    void searchChanged();
    void quickPasteVisibleChanged();
    void feedbackChanged();
    void resizingChanged();
    void deckVisibleChanged();
    void deckShown();
    void deckHeightChanged();
    void backdropChanged();
    void popoverChanged();
    void previewScrollRequested(int direction);
    void shortcutsChanged();

private:
    void registerHotkey();
    void refreshPopover();
    void publishPopover();
    bool selectIndex(int index);
    void refreshModel();
    void loadPersistedHistory();
    void onCaptured(quint64 sequence);
    void performSearch(const QString &query);
    void pasteItem(const ClipboardItem &item, bool plainText);
    void deliverPaste(const ClipboardItem &item, bool plainText);
    void captureBackdrop(QScreen *screen, const QRect &frame);
    void updateRegion();
    void applyEffects();
    void onActiveChanged();
    HWND deckWindow() const;

    HistoryStore store_;
    HistoryRepository repository_; // GUI-thread connection for edits (Linux does the same)
    SettingsController settings_;
    WinCaptureService capture_;
    WinPasteService paste_;
    WinGlobalHotkey hotkey_;
    std::unique_ptr<WinTray> tray_;
    std::unique_ptr<FilePasteController> filePaste_;
    DeckImageProvider *images_;
    DeckItemModel model_;
    QPointer<QQuickWindow> window_;
    QVariantMap popover_;
    QUuid popoverId_;
    ClipboardItem popoverItem_;
    HWND target_ = nullptr;
    QTimer searchDebounce_;
    QTimer clock_;
    quint64 searchGeneration_ = 0;
    bool resizing_ = false;
    qreal resizeStartY_ = 0;
    qreal resizeStartHeight_ = 0;
    qreal deckHeight_ = 340;
    qreal minimumHeight_ = 250;
    qreal maximumHeight_ = 420;
    int backdropRevision_ = 0;
};
