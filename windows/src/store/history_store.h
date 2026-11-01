#pragma once

#include "history/file_paste_controller.h"
#include "history_repository.h"

#include <QObject>
#include <QUuid>
#include <QVector>

#include <functional>
#include <optional>

// mac HistoryCategory. Automatic categories are keyed by name, which is how
// ClipboardItem.category references them in the shared repository.
struct HistoryCategory {
    enum Kind { All, Text, Images, Files, Automatic };

    Kind kind = All;
    QString name;

    static HistoryCategory all() { return {All, {}}; }
    static HistoryCategory text() { return {Text, {}}; }
    static HistoryCategory images() { return {Images, {}}; }
    static HistoryCategory files() { return {Files, {}}; }
    static HistoryCategory automatic(const QString &name) { return {Automatic, name}; }

    bool operator==(const HistoryCategory &other) const {
        return kind == other.kind && (kind != Automatic || name == other.name);
    }
    bool operator!=(const HistoryCategory &other) const { return !(*this == other); }
};

enum class CaptureReconciliation { Reconciled, Superseded, Removed };

// The payload half of a ClipboardItem: what an edit replaces.
struct ClipboardPayload {
    ClipboardPayloadType type = ClipboardPayloadType::Text;
    QString text;
    QString richTextHtml;
    QByteArray imageData;
    QImage image;
    QSize imagePixelSize;
    QVector<ClipboardFileReference> files;
    // false for a card summary (thumbnail / first 3000 characters).
    bool complete = true;

    static ClipboardPayload of(const ClipboardItem &item);
    void applyTo(ClipboardItem &item) const;
};

// mac ClipboardPayload.hasSameContent: files compare by sorted path list,
// everything else by full payload. Summarised images (no PNG bytes) only
// match when they share a database row.
bool hasSameContent(const ClipboardItem &lhs, const ClipboardItem &rhs);
// "文本" / "图片" / file name / "文件 · N 项"
QString defaultTitle(const ClipboardItem &item);

// 1:1 port of mac InMemoryHistoryStore. Every @Published property has a
// getter and a change signal; Swift didSet side effects live in the setters.
class HistoryStore final : public QObject {
    Q_OBJECT

public:
    explicit HistoryStore(int capacity = 100, QObject *parent = nullptr);

    // State
    const QVector<ClipboardItem> &items() const { return items_; }
    QUuid selectedItemId() const { return selectedItemId_; }
    void setSelectedItemId(const QUuid &id);
    HistoryCategory selectedCategory() const { return selectedCategory_; }
    int selectionRevealToken() const { return selectionRevealToken_; }
    const QVector<ClipboardCategory> &automaticCategories() const { return automaticCategories_; }
    QString searchQuery() const { return searchQuery_; }
    void setSearchQuery(const QString &query);
    const QVector<ClipboardItem> &searchResults() const { return searchResults_; }
    bool isSearchActive() const { return searchActive_; }
    bool isSearchEditing() const { return searchEditing_; }
    bool isSearchFieldFocused() const { return searchFieldFocused_; }
    QUuid previewedItemId() const { return previewedItemId_; }
    QUuid editingItemId() const { return editingItemId_; }
    QUuid renamingItemId() const { return renamingItemId_; }
    bool isQuickPasteOverlayVisible() const { return quickPasteOverlayVisible_; }
    // phase == Idle stands for mac's nil.
    const FilePasteFeedback &filePasteFeedback() const { return filePasteFeedback_; }
    void setFilePasteFeedback(const FilePasteFeedback &feedback);

    std::function<void(const ClipboardItem &)> onDelete;
    std::function<void(const ClipboardItem &)> onUpdate;

    // Derived
    std::optional<ClipboardItem> selectedItem() const;
    std::optional<ClipboardItem> editingItem() const;
    std::optional<ClipboardItem> renamingItem() const;
    QVector<ClipboardItem> displayedItems() const;

    // Loading
    void replaceItems(const QVector<ClipboardItem> &items, bool preservingSelection = false);
    void expandPersistedItems(const QVector<ClipboardItem> &persistedItems);
    void replaceCategories(const QVector<ClipboardCategory> &categories);

    // Capture
    void insert(const ClipboardItem &item);
    // A paste or copy from the Deck counts as copying the item again: it moves
    // to the front with a fresh copy time. Selection and category stay.
    void markCopied(const QUuid &itemId);
    CaptureReconciliation reconcileCapturedItem(const ClipboardItem &item);

    // Selection and categories
    void selectPrevious() { moveSelection(-1); }
    void selectNext() { moveSelection(1); }
    std::optional<ClipboardItem> deleteSelection();
    void selectCategory(const HistoryCategory &category);
    void selectPreviousCategory() { moveCategory(-1); }
    void selectNextCategory() { moveCategory(1); }
    void setQuickPasteOverlayVisible(bool visible);
    void revealSelection();

    // Search
    void beginSearch();
    void setSearchEditing(bool editing);
    void setSearchFieldFocused(bool focused);
    void finishSearchEditing();
    void resumeSearchEditingAndDeleteBackward();
    void collapseSearchIfQueryWasDeleted();
    void replaceSearchResults(const QVector<ClipboardItem> &results);
    QVector<ClipboardItem> applyLocalSearch(const QString &query);
    void endSearch();

    // Preview, editing, renaming
    void togglePreviewForSelection();
    void dismissPreview();
    void beginEditing(const ClipboardItem &item);
    void beginEditingSelection();
    void cancelEditing();
    void beginRenaming(const ClipboardItem &item);
    void beginRenamingSelection();
    void cancelRenaming();
    void updateRenamingItem(const QString &title);
    // category: the result of classifying edited text (Linux re-classifies
    // on save; categories are automatic, so the old one may no longer apply).
    void updateEditingItem(const ClipboardPayload &payload, std::optional<int> imageAngle = std::nullopt,
                           std::optional<QString> category = std::nullopt);
    void updateLinkMetadata(const QUuid &itemId, std::optional<QString> title,
                            std::optional<QByteArray> iconData, std::optional<QByteArray> previewData);

Q_SIGNALS:
    void itemsChanged();
    void selectedItemIdChanged();
    void selectedCategoryChanged();
    void selectionRevealTokenChanged();
    void automaticCategoriesChanged();
    void searchQueryChanged();
    void searchResultsChanged();
    void searchStateChanged(); // active / editing / field focused
    void previewedItemIdChanged();
    void editingItemIdChanged();
    void renamingItemIdChanged();
    void quickPasteOverlayVisibleChanged();
    void filePasteFeedbackChanged();

private:
    void setItems(QVector<ClipboardItem> items);
    void setSearchResults(QVector<ClipboardItem> results);
    void setSelectedCategory(const HistoryCategory &category);
    void setSearchFlags(bool active, bool editing, bool focused);
    void setPreviewedItemId(const QUuid &id);
    void setEditingItemId(const QUuid &id);
    void setRenamingItemId(const QUuid &id);
    QUuid firstDisplayedId() const;
    bool displayedContains(const QUuid &id) const;
    void moveSelection(int offset);
    void moveCategory(int offset);
    static void replace(const ClipboardItem &current, const ClipboardItem &updated, QVector<ClipboardItem> &collection);

    int capacity_;
    QVector<ClipboardItem> items_;
    QUuid selectedItemId_;
    HistoryCategory selectedCategory_;
    int selectionRevealToken_ = 0;
    QVector<ClipboardCategory> automaticCategories_;
    QString searchQuery_;
    QVector<ClipboardItem> searchResults_;
    bool searchActive_ = false;
    bool searchEditing_ = false;
    bool searchFieldFocused_ = false;
    QUuid previewedItemId_;
    QUuid editingItemId_;
    QUuid renamingItemId_;
    bool quickPasteOverlayVisible_ = false;
    FilePasteFeedback filePasteFeedback_;
};
