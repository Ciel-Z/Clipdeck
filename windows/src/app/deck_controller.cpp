#include "deck_controller.h"

#include "app_preferences.h"
#include "deck_image_provider.h"
#include "deck_presentation.h"
#include "history/file_paste_controller.h"
#include "win_process_util.h"
#include "win_window_effects.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace {

const QString preferredHeightKey = QStringLiteral("deck/preferredHeight");
constexpr quint32 kAcrylicTint = 0x99161618; // AABBGGRR; Phase 0 default

struct HeightLimits {
    qreal minimum;
    qreal maximum;
    qreal preferred;
};

// PanelController.updateFrame / DeckResizeHandle.
HeightLimits heightLimits(const QRect &workArea) {
    const qreal maximum = std::max(250.0, std::min(420.0, std::floor(workArea.height() * 0.42)));
    const qreal minimum = std::min(250.0, maximum);
    const qreal preferred = std::min(maximum, std::max(340.0, std::floor(workArea.height() * 0.385)));
    return {minimum, maximum, preferred};
}

// mac category dot colours (dark appearance), cycled by position.
const char *const kCategoryColors[] = {"#BF5AF2", "#64D2FF", "#30D158", "#FF375F", "#5E5CE6", "#66D4CF"};

QVector<ClipboardItem> mergeSearchResults(const QVector<ClipboardItem> &local, const QVector<ClipboardItem> &database) {
    QSet<qint64> seenDatabaseIds;
    QSet<QUuid> seenItemIds;
    QVector<ClipboardItem> merged;
    for (const QVector<ClipboardItem> *source : {&local, &database}) {
        for (const ClipboardItem &item : *source) {
            const bool fresh = item.databaseID >= 0 ? !seenDatabaseIds.contains(item.databaseID)
                                                    : !seenItemIds.contains(item.id);
            if (!fresh) continue;
            if (item.databaseID >= 0) seenDatabaseIds.insert(item.databaseID);
            else seenItemIds.insert(item.id);
            merged.append(item);
        }
    }
    return merged;
}

} // namespace

DeckController::DeckController(const QString &databasePath, DeckImageProvider *images, QObject *parent)
    : QObject(parent),
      store_(100),
      repository_(databasePath),
      settings_(&repository_),
      capture_(databasePath),
      filePaste_(new FilePasteController),
      images_(images),
      model_(images) {
    store_.onDelete = [this](const ClipboardItem &item) {
        if (item.databaseID >= 0 && !repository_.remove(item.databaseID)) {
            qWarning() << "[ClipDeck][Deck] delete failed:" << repository_.lastError();
        }
    };
    store_.onUpdate = [this](const ClipboardItem &item) {
        if (item.databaseID < 0) return;
        repository_.updateTitle(item.databaseID, item.title);
        if (item.isText() && item.payloadComplete) repository_.updateText(item.databaseID, item.text, item.category);
        if (item.isImage()) repository_.updateImageAngle(item.databaseID, item.imageAngle);
        if (item.hasFetchedLinkPreview) {
            repository_.updateLinkMetadata(item.databaseID, item.title, item.linkIconData, item.linkPreviewData);
        }
    };

    connect(&store_, &HistoryStore::itemsChanged, this, &DeckController::refreshModel);
    connect(&store_, &HistoryStore::searchResultsChanged, this, &DeckController::refreshModel);
    connect(&store_, &HistoryStore::selectedCategoryChanged, this, [this] {
        refreshModel();
        Q_EMIT selectedCategoryChanged();
    });
    connect(&store_, &HistoryStore::selectedItemIdChanged, this, &DeckController::selectionChanged);
    connect(&store_, &HistoryStore::selectionRevealTokenChanged, this, &DeckController::revealRequested);
    connect(&store_, &HistoryStore::automaticCategoriesChanged, this, &DeckController::categoriesChanged);
    connect(&store_, &HistoryStore::searchStateChanged, this, [this] {
        refreshModel();
        Q_EMIT searchChanged();
    });
    connect(&store_, &HistoryStore::searchQueryChanged, this, [this] {
        // AppCoordinator.performSearch: local filter now, FTS after 120 ms.
        store_.applyLocalSearch(store_.searchQuery().trimmed());
        refreshModel();
        if (store_.isSearchActive() && !store_.searchQuery().trimmed().isEmpty()) searchDebounce_.start();
        Q_EMIT searchChanged();
    });
    connect(&store_, &HistoryStore::quickPasteOverlayVisibleChanged, this, &DeckController::quickPasteVisibleChanged);
    connect(&store_, &HistoryStore::filePasteFeedbackChanged, this, &DeckController::feedbackChanged);
    connect(&store_, &HistoryStore::previewedItemIdChanged, this, &DeckController::refreshPopover);
    connect(&store_, &HistoryStore::editingItemIdChanged, this, &DeckController::refreshPopover);
    connect(&store_, &HistoryStore::renamingItemIdChanged, this, &DeckController::refreshPopover);
    // The popover follows its card when the strip is rebuilt (search, category).
    connect(&store_, &HistoryStore::itemsChanged, this, &DeckController::publishPopover);
    connect(&store_, &HistoryStore::searchResultsChanged, this, &DeckController::publishPopover);
    connect(&store_, &HistoryStore::selectedCategoryChanged, this, &DeckController::publishPopover);

    connect(capture_.pipeline(), &ClipboardPipeline::recentChanged, this, &DeckController::onCaptured);
    connect(capture_.pipeline(), &ClipboardPipeline::processingError, this,
            [](const QString &message) { qWarning() << "[ClipDeck][Capture]" << message; });
    paste_.setWriteObserver([this](quint32 sequence) { capture_.monitor()->suppressSequence(sequence); });
    connect(&hotkey_, &WinGlobalHotkey::triggered, this, [this] { toggle(QStringLiteral("hotkey")); });
    filePaste_->changed = [this] { store_.setFilePasteFeedback(filePaste_->feedback()); };

    // Settings → Deck
    connect(&settings_, &SettingsController::historyChanged, this, [this] {
        store_.replaceCategories(repository_.categories());
        loadPersistedHistory();
    });
    connect(&settings_, &SettingsController::categoriesChanged, this,
            [this] { store_.replaceCategories(repository_.categories()); });
    connect(&settings_, &SettingsController::shortcutsChanged, this, [this] {
        if (!(hotkey_.current() == HotkeySpec::load()) || !hotkey_.isRegistered()) registerHotkey();
        Q_EMIT shortcutsChanged();
    });
    connect(&settings_, &SettingsController::recordingChanged, this, [this](bool recording) {
        if (recording) hotkey_.unregisterHotkey();
        else registerHotkey();
    });

    searchDebounce_.setSingleShot(true);
    searchDebounce_.setInterval(120);
    connect(&searchDebounce_, &QTimer::timeout, this, [this] { performSearch(store_.searchQuery().trimmed()); });
    clock_.setInterval(30000);
    connect(&clock_, &QTimer::timeout, this, [this] {
        if (deckVisible()) model_.refreshTimes();
    });
}

DeckController::~DeckController() { capture_.stop(); }

bool DeckController::start(QQuickWindow *window, const QIcon &trayIcon) {
    window_ = window;
    WinEffects::setToolWindow(deckWindow());
    connect(window, &QWindow::activeChanged, this, &DeckController::onActiveChanged);
    connect(window, &QWindow::widthChanged, this, &DeckController::applyEffects);
    connect(window, &QWindow::heightChanged, this, &DeckController::applyEffects);
    connect(window, &QWindow::visibleChanged, this, &DeckController::deckVisibleChanged);

    tray_ = std::make_unique<WinTray>(trayIcon);
    connect(tray_.get(), &WinTray::toggleRequested, this, [this] { toggle(QStringLiteral("tray")); });
    connect(tray_.get(), &WinTray::settingsRequested, this, [this] { openSettings(QStringLiteral("general")); });
    connect(tray_.get(), &WinTray::quitRequested, this, &DeckController::quit);
    tray_->show();

    registerHotkey();
    tray_->showFirstRunHintIfNeeded(HotkeySpec::load().toString());

    // loadPersistedHistory on mac deletes expired rows before showing history.
    repository_.clearExpired(AppPreferences::retentionDays());
    store_.replaceCategories(repository_.categories());
    const bool started = capture_.start();
    if (!started) qWarning() << "[ClipDeck][Capture] clipboard monitor failed to start";
    loadPersistedHistory();
    clock_.start();
    return started;
}

// MARK: - State for QML

int DeckController::selectedIndex() const {
    const auto selected = store_.selectedItem();
    return selected ? model_.indexOf(selected->id) : -1;
}

QVariantList DeckController::categories() const {
    QVariantList list;
    int position = 0;
    for (const ClipboardCategory &category : store_.automaticCategories()) {
        list.append(QVariantMap{
            {QStringLiteral("key"), QStringLiteral("auto:") + category.name},
            {QStringLiteral("name"), category.name},
            {QStringLiteral("color"), QString::fromLatin1(kCategoryColors[position++ % 6])},
        });
    }
    return list;
}

QString DeckController::selectedCategoryKey() const {
    const HistoryCategory category = store_.selectedCategory();
    switch (category.kind) {
    case HistoryCategory::All: return QStringLiteral("all");
    case HistoryCategory::Text: return QStringLiteral("text");
    case HistoryCategory::Images: return QStringLiteral("images");
    case HistoryCategory::Files: return QStringLiteral("files");
    case HistoryCategory::Automatic: return QStringLiteral("auto:") + category.name;
    }
    return QStringLiteral("all");
}

void DeckController::selectCategoryKey(const QString &key) {
    if (key == QLatin1String("all")) store_.selectCategory(HistoryCategory::all());
    else if (key == QLatin1String("text")) store_.selectCategory(HistoryCategory::text());
    else if (key == QLatin1String("images")) store_.selectCategory(HistoryCategory::images());
    else if (key == QLatin1String("files")) store_.selectCategory(HistoryCategory::files());
    else if (key.startsWith(QLatin1String("auto:"))) store_.selectCategory(HistoryCategory::automatic(key.mid(5)));
}

void DeckController::setSearchQuery(const QString &query) { store_.setSearchQuery(query); }

void DeckController::refreshModel() {
    model_.setItems(store_.displayedItems());
    Q_EMIT selectionChanged();
}

// MARK: - History loading and capture

void DeckController::loadPersistedHistory() {
    // AppCoordinator.loadPersistedHistory: 20 rows first, then the rest.
    capture_.pipeline()->requestHistory(QString(), QString(), 20, 0, [this](QVector<ClipboardItem> items) {
        store_.replaceItems(items);
        capture_.pipeline()->requestHistory(QString(), QString(), 100, 0, [this](QVector<ClipboardItem> all) {
            store_.expandPersistedItems(all);
        });
    });
}

void DeckController::onCaptured(quint64 sequence) {
    if (sequence == 0) return; // initial publish, not a new copy
    const QVector<ClipboardItem> &recent = capture_.pipeline()->recentItems();
    if (recent.isEmpty()) return;
    store_.insert(recent.first());
    if (deckVisible()) store_.revealSelection();
}

void DeckController::performSearch(const QString &query) {
    if (query.isEmpty()) return;
    const quint64 generation = ++searchGeneration_;
    capture_.pipeline()->requestHistory(QString(), query, 100, 0, [this, query, generation](QVector<ClipboardItem> results) {
        if (generation != searchGeneration_ || !store_.isSearchActive()
            || store_.searchQuery().trimmed() != query) {
            return;
        }
        const QVector<ClipboardItem> local = store_.applyLocalSearch(query);
        store_.replaceSearchResults(mergeSearchResults(local, results));
    });
}

// MARK: - Paste

void DeckController::paste(int index, bool plainText) {
    const QVector<ClipboardItem> &items = model_.items();
    if (index < 0 || index >= items.size()) return;
    const ClipboardItem item = items.at(index);
    store_.setSelectedItemId(item.id);
    store_.dismissPreview();
    pasteItem(item, plainText);
}

void DeckController::pasteSelection() {
    if (store_.isSearchEditing()) {
        store_.finishSearchEditing();
        return;
    }
    const auto item = store_.selectedItem();
    if (!item) return;
    store_.dismissPreview();
    pasteItem(*item, false);
}

void DeckController::quickPaste(int index) {
    const QVector<ClipboardItem> displayed = store_.displayedItems();
    if (index < 0 || index >= displayed.size()) return;
    const ClipboardItem item = displayed.at(index);
    store_.setSelectedItemId(item.id);
    store_.setQuickPasteOverlayVisible(false);
    pasteItem(item, false);
}

void DeckController::pasteItem(const ClipboardItem &item, bool plainText) {
    if (item.isFiles()) {
        const QUuid id = item.id;
        filePaste_->prepare(
            item, [this, id] { return deckVisible() && store_.selectedItemId() == id; },
            [this, plainText](ClipboardItem ready) { deliverPaste(ready, plainText); });
        return;
    }
    if (!item.payloadComplete && item.databaseID >= 0) {
        // Cards hold thumbnails / the first 3000 characters; paste the full row.
        const QUuid id = item.id;
        capture_.pipeline()->requestItem(item.databaseID, [this, id, plainText](QVector<ClipboardItem> rows) {
            if (rows.isEmpty()) return;
            ClipboardItem full = rows.first();
            full.id = id;
            deliverPaste(full, plainText);
        });
        return;
    }
    deliverPaste(item, plainText);
}

void DeckController::deliverPaste(const ClipboardItem &item, bool plainText) {
    const auto result = paste_.paste(item, target_, deckWindow(), plainText, [this] { hideDeck(QStringLiteral("paste")); });
    if (result == WinPasteService::Result::CopiedOnlyElevatedTarget && tray_) {
        tray_->showCopiedOnlyMessage(QStringLiteral("目标应用以管理员身份运行，请手动按 Ctrl + V。"));
    } else if (result == WinPasteService::Result::Failed && tray_) {
        tray_->showCopiedOnlyMessage(QStringLiteral("写入剪贴板失败，请重试。"));
    }
}

// MARK: - Panel callbacks

void DeckController::escape() {
    // Order is fixed: file feedback → rename → edit → search → hide.
    if (store_.filePasteFeedback().phase != FilePasteFeedback::Idle) {
        filePaste_->cancel();
        store_.setFilePasteFeedback(FilePasteFeedback());
    } else if (!store_.renamingItemId().isNull()) {
        store_.cancelRenaming();
    } else if (!store_.editingItemId().isNull()) {
        store_.cancelEditing();
    } else if (store_.isSearchActive()) {
        store_.endSearch();
    } else {
        hideDeck(QStringLiteral("escape"));
    }
}

void DeckController::select(int index) {
    const QVector<ClipboardItem> &items = model_.items();
    if (index < 0 || index >= items.size()) return;
    if (store_.isSearchEditing()) store_.finishSearchEditing();
    store_.setSelectedItemId(items.at(index).id);
}

void DeckController::moveSelection(int offset) {
    if (offset < 0) store_.selectPrevious();
    else store_.selectNext();
}

void DeckController::moveCategory(int offset) {
    if (offset < 0) store_.selectPreviousCategory();
    else store_.selectNextCategory();
}

void DeckController::deleteSelection() { store_.deleteSelection(); }

void DeckController::openSelection() {
    const auto item = store_.selectedItem();
    if (!item) return;
    const QUrl url = DeckPresentation::webUrl(*item);
    if (url.isValid()) QDesktopServices::openUrl(url);
}

void DeckController::beginSearch() { store_.beginSearch(); }

void DeckController::typeToSearch(const QString &characters) {
    store_.beginSearch();
    store_.setSearchQuery(store_.searchQuery() + characters);
}

void DeckController::pasteClipboardIntoSearch() {
    QString pasted = QGuiApplication::clipboard()->text();
    pasted.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    if (pasted.isEmpty()) return;
    store_.setSearchEditing(true);
    store_.setSearchQuery(store_.searchQuery() + pasted);
}

void DeckController::finishSearchEditing() { store_.finishSearchEditing(); }
void DeckController::resumeSearchAndDeleteBackward() { store_.resumeSearchEditingAndDeleteBackward(); }
void DeckController::collapseEmptySearch() { store_.collapseSearchIfQueryWasDeleted(); }
void DeckController::endSearch() { store_.endSearch(); }
void DeckController::setSearchFieldFocused(bool focused) { store_.setSearchFieldFocused(focused); }
void DeckController::setQuickPasteVisible(bool visible) { store_.setQuickPasteOverlayVisible(visible); }

void DeckController::openSettings(const QString &pane) {
    // Settings takes the focus; the Deck would resign key and close on mac too.
    hideDeck(QStringLiteral("settings"));
    settings_.open(pane);
}

void DeckController::registerHotkey() {
    const HotkeySpec spec = HotkeySpec::load();
    const bool ok = hotkey_.registerHotkey(spec);
    const QString error = ok ? QString() : hotkey_.lastError();
    if (tray_) tray_->setHotkeyError(error);
    settings_.reportHotkeyError(error);
}

int DeckController::quickPasteModifier() const { return AppPreferences::qtModifier(AppPreferences::quickPasteModifier()); }
int DeckController::quickPasteModifierKey() const {
    return AppPreferences::qtModifierKey(AppPreferences::quickPasteModifier());
}
int DeckController::plainTextModifier() const { return AppPreferences::qtModifier(AppPreferences::plainTextModifier()); }

int DeckController::categoryShortcutDirection(int qtKey, int qtModifiers) const {
    if (AppPreferences::previousCategoryShortcut().matchesQtKey(qtKey, qtModifiers)) return -1;
    if (AppPreferences::nextCategoryShortcut().matchesQtKey(qtKey, qtModifiers)) return 1;
    return 0;
}

void DeckController::quit() { QCoreApplication::quit(); }

// MARK: - Popovers and context menu

bool DeckController::selectIndex(int index) {
    const QVector<ClipboardItem> &items = model_.items();
    if (index < 0 || index >= items.size()) return false;
    store_.setSelectedItemId(items.at(index).id);
    return true;
}

void DeckController::togglePreview() { store_.togglePreviewForSelection(); }
void DeckController::editSelection() { store_.beginEditingSelection(); }
void DeckController::renameSelection() { store_.beginRenamingSelection(); }

void DeckController::previewItem(int index) {
    if (selectIndex(index)) store_.togglePreviewForSelection();
}

void DeckController::editItem(int index) {
    if (!selectIndex(index)) return;
    if (const auto item = store_.selectedItem()) store_.beginEditing(*item);
}

void DeckController::renameItem(int index) {
    if (!selectIndex(index)) return;
    if (const auto item = store_.selectedItem()) store_.beginRenaming(*item);
}

void DeckController::openItem(int index) {
    if (selectIndex(index)) openSelection();
}

void DeckController::deleteItem(int index) {
    if (selectIndex(index)) store_.deleteSelection();
}

void DeckController::cancelPopover() {
    if (!store_.renamingItemId().isNull()) store_.cancelRenaming();
    else if (!store_.editingItemId().isNull()) store_.cancelEditing();
    else store_.dismissPreview();
}

void DeckController::saveRename(const QString &title) { store_.updateRenamingItem(title); }

void DeckController::saveText(const QString &text) {
    ClipboardPayload payload;
    payload.type = ClipboardPayloadType::Text;
    payload.text = text; // rich text is dropped, as on mac (rtfData: nil)
    store_.updateEditingItem(payload);
}

void DeckController::saveImageAngle(int angle) {
    const auto current = store_.editingItem();
    if (!current) return;
    store_.updateEditingItem(ClipboardPayload::of(*current), angle);
}

QString DeckController::formatJson(const QString &text) const { return DeckPresentation::formatJson(text); }

void DeckController::activateDeck() {
    if (!deckVisible()) return;
    SetForegroundWindow(deckWindow());
    window_->requestActivate();
}

void DeckController::refreshPopover() {
    QUuid id = store_.renamingItemId();
    if (id.isNull()) id = store_.editingItemId();
    if (id.isNull()) id = store_.previewedItemId();
    if (id.isNull()) {
        popoverId_ = QUuid();
        popoverItem_ = ClipboardItem();
        publishPopover();
        return;
    }
    if (id != popoverId_) {
        popoverId_ = id;
        std::optional<ClipboardItem> found;
        for (const ClipboardItem &item : store_.items()) {
            if (item.id == id) found = item;
        }
        for (const ClipboardItem &item : store_.searchResults()) {
            if (!found && item.id == id) found = item;
        }
        if (!found) return;
        popoverItem_ = *found;
        // Cards hold summaries; the popover shows the full text and image.
        if (!popoverItem_.payloadComplete && popoverItem_.databaseID >= 0) {
            capture_.pipeline()->requestItem(popoverItem_.databaseID, [this, id](QVector<ClipboardItem> rows) {
                if (rows.isEmpty() || popoverId_ != id) return;
                ClipboardItem full = rows.first();
                full.id = id;
                full.title = popoverItem_.title;
                full.imageAngle = popoverItem_.imageAngle;
                popoverItem_ = full;
                if (full.isImage()) {
                    QImage image = full.image;
                    if (image.isNull()) image.loadFromData(full.imageData, "PNG");
                    images_->insert(QStringLiteral("full/") + id.toString(QUuid::WithoutBraces), image);
                }
                publishPopover();
            });
        }
    }
    publishPopover();
}

void DeckController::publishPopover() {
    QVariantMap map;
    const QString kind = !store_.renamingItemId().isNull() ? QStringLiteral("rename")
        : !store_.editingItemId().isNull()                ? QStringLiteral("edit")
        : !store_.previewedItemId().isNull()              ? QStringLiteral("preview")
                                                          : QString();
    if (!kind.isEmpty() && !popoverId_.isNull()) {
        const ClipboardItem &item = popoverItem_;
        const QString id = item.id.toString(QUuid::WithoutBraces);
        const QString fullKey = QStringLiteral("full/") + id;
        const QSizeF size = item.isImage()
            ? DeckPresentation::previewImageSize(item.imagePixelSize.width(), item.imagePixelSize.height(), item.imageAngle)
            : DeckPresentation::previewTextSize();
        QVariantList files;
        for (const ClipboardFileReference &file : item.files) {
            files.append(QVariantMap{{QStringLiteral("name"), file.name},
                                     {QStringLiteral("path"), file.path},
                                     {QStringLiteral("isDirectory"), file.isDirectory}});
        }
        const QString language = DeckPresentation::editableLanguage(item);
        map = QVariantMap{
            {QStringLiteral("kind"), kind},
            {QStringLiteral("index"), model_.indexOf(item.id)},
            {QStringLiteral("itemId"), id},
            {QStringLiteral("payload"), item.isFiles() ? QStringLiteral("files") : item.isImage() ? QStringLiteral("image") : QStringLiteral("text")},
            {QStringLiteral("loading"), !item.payloadComplete && item.databaseID >= 0},
            {QStringLiteral("categoryName"), DeckPresentation::previewCategoryName(item)},
            {QStringLiteral("sourceName"), item.sourceName},
            {QStringLiteral("dateText"), DeckPresentation::previewDateTime(item.copiedAt)},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("text"), item.text.left(200000)},
            {QStringLiteral("editableLanguage"), language},
            {QStringLiteral("monospaced"), !language.isEmpty()},
            {QStringLiteral("imageUrl"), !item.isImage() ? QString()
                 : QStringLiteral("image://deck/") + (images_->contains(fullKey) ? fullKey : QStringLiteral("thumb/") + id)},
            {QStringLiteral("imageWidth"), item.imagePixelSize.width()},
            {QStringLiteral("imageHeight"), item.imagePixelSize.height()},
            {QStringLiteral("imageAngle"), item.imageAngle},
            {QStringLiteral("files"), files},
            {QStringLiteral("previewWidth"), size.width()},
            {QStringLiteral("previewHeight"), size.height()},
        };
    }
    popover_ = map;
    Q_EMIT popoverChanged();
}

// MARK: - Window

void DeckController::toggle(const QString &trigger) {
    if (deckVisible()) hideDeck(trigger);
    else showDeck(trigger);
}

void DeckController::showDeck(const QString &trigger) {
    if (!window_) return;
    // rememberFrontmostExternalApplication(), before the Deck takes focus.
    HWND foreground = GetForegroundWindow();
    if (foreground && !WinProcess::belongsToCurrentProcess(foreground)) target_ = foreground;
    qInfo() << "[ClipDeck][Deck] show via" << trigger << "target" << WinProcess::describeWindow(target_);

    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    window_->setScreen(screen);
    const HeightLimits limits = heightLimits(screen->availableGeometry());
    updateGeometry(QSettings().value(preferredHeightKey, limits.preferred).toReal());

    model_.refreshTimes();
    if (!store_.selectedItem()) store_.setSelectedItemId(store_.displayedItems().isEmpty() ? QUuid()
                                                                                          : store_.displayedItems().first().id);
    window_->show();
    applyEffects();
    SetForegroundWindow(deckWindow());
    window_->requestActivate();
    store_.revealSelection();
    Q_EMIT deckShown();
}

void DeckController::hideDeck(const QString &reason) {
    if (!deckVisible()) return;
    qInfo() << "[ClipDeck][Deck] hide:" << reason;
    store_.setQuickPasteOverlayVisible(false);
    // Popovers are attached to the panel and go away with it.
    store_.cancelRenaming();
    store_.cancelEditing();
    store_.dismissPreview();
    window_->hide();
}

void DeckController::updateGeometry(qreal height) {
    QScreen *screen = window_->screen();
    const QRect area = screen->availableGeometry();
    const HeightLimits limits = heightLimits(area);
    const qreal deckHeight = std::clamp(height, limits.minimum, limits.maximum);
    // 8 DIP from both edges, flush with the work area bottom.
    window_->setGeometry(area.x() + 8, int(area.y() + area.height() - deckHeight), area.width() - 16, int(deckHeight));
}

void DeckController::applyEffects() {
    HWND window = deckWindow();
    if (!window || !deckVisible()) return;
    // Acrylic redraws slowly while resizing; the classic blur keeps up.
    WinEffects::setAccent(window, resizing_ ? WinEffects::Accent::BlurBehind : WinEffects::Accent::AcrylicBlurBehind,
                          kAcrylicTint);
    WinEffects::setRoundedRegion(window, int(std::lround(24 * window_->devicePixelRatio())));
}

void DeckController::beginResize(qreal globalY) {
    resizing_ = true;
    resizeStartY_ = globalY;
    resizeStartHeight_ = window_->height();
    applyEffects();
    Q_EMIT resizingChanged();
}

void DeckController::updateResize(qreal globalY) {
    if (!resizing_) return;
    updateGeometry(resizeStartHeight_ + (resizeStartY_ - globalY));
}

void DeckController::endResize() {
    if (!resizing_) return;
    resizing_ = false;
    QSettings().setValue(preferredHeightKey, window_->height());
    applyEffects();
    store_.revealSelection();
    Q_EMIT resizingChanged();
}

void DeckController::onActiveChanged() {
    if (!window_ || window_->isActive() || !window_->isVisible()) return;
    if (resizing_ || paste_.isPending()) return;
    // windowDidResignKey: decide on the next event-loop turn.
    QTimer::singleShot(0, this, [this] {
        if (!window_ || window_->isActive() || !window_->isVisible()) return;
        if (WinProcess::belongsToCurrentProcess(GetForegroundWindow())) return;
        hideDeck(QStringLiteral("lost activation"));
    });
}

HWND DeckController::deckWindow() const { return window_ ? reinterpret_cast<HWND>(window_->winId()) : nullptr; }
