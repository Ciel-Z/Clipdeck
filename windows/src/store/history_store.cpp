#include "history_store.h"

#include <QHash>
#include <QTextBoundaryFinder>

#include <algorithm>

namespace {

bool contains(const QVector<ClipboardItem> &items, const QUuid &id) {
    if (id.isNull()) return false;
    return std::any_of(items.cbegin(), items.cend(), [&](const ClipboardItem &item) { return item.id == id; });
}

std::optional<ClipboardItem> find(const QVector<ClipboardItem> &items, const QUuid &id) {
    if (id.isNull()) return std::nullopt;
    for (const ClipboardItem &item : items) {
        if (item.id == id) return item;
    }
    return std::nullopt;
}

bool sameFeedback(const FilePasteFeedback &lhs, const FilePasteFeedback &rhs) {
    return lhs.phase == rhs.phase && lhs.itemID == rhs.itemID && lhs.message == rhs.message
        && FileReferences::identity(lhs.available) == FileReferences::identity(rhs.available)
        && FileReferences::identity(lhs.unavailable) == FileReferences::identity(rhs.unavailable);
}

// Swift String.removeLast() drops one Character (grapheme cluster), so an
// emoji or a base letter with combining marks goes away in one Backspace.
void removeLastCharacter(QString &text) {
    if (text.isEmpty()) return;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.toEnd();
    const qsizetype previous = finder.toPreviousBoundary();
    text.truncate(previous < 0 ? 0 : previous);
}

} // namespace

ClipboardPayload ClipboardPayload::of(const ClipboardItem &item) {
    ClipboardPayload payload;
    payload.type = item.type;
    payload.text = item.text;
    payload.richTextHtml = item.richTextHtml;
    payload.imageData = item.imageData;
    payload.image = item.image;
    payload.imagePixelSize = item.imagePixelSize;
    payload.files = item.files;
    payload.complete = item.payloadComplete;
    return payload;
}

void ClipboardPayload::applyTo(ClipboardItem &item) const {
    item.type = type;
    item.text = text;
    item.richTextHtml = richTextHtml;
    item.imageData = imageData;
    item.image = image;
    item.imagePixelSize = imagePixelSize;
    item.files = files;
    item.payloadComplete = complete;
    // A summary keeps the stored full length; an edited text has its own.
    if (complete) item.textLength = text.size();
}

bool hasSameContent(const ClipboardItem &lhs, const ClipboardItem &rhs) {
    if (lhs.type != rhs.type) return false;
    switch (lhs.type) {
    case ClipboardPayloadType::Files:
        return FileReferences::identity(lhs.files) == FileReferences::identity(rhs.files);
    case ClipboardPayloadType::Image:
        if (!lhs.imageData.isEmpty() && !rhs.imageData.isEmpty()) {
            return lhs.imageData == rhs.imageData && lhs.imagePixelSize == rhs.imagePixelSize;
        }
        return lhs.databaseID >= 0 && lhs.databaseID == rhs.databaseID;
    case ClipboardPayloadType::Text:
        return lhs.text == rhs.text && lhs.richTextHtml == rhs.richTextHtml;
    }
    return false;
}

QString defaultTitle(const ClipboardItem &item) {
    switch (item.type) {
    case ClipboardPayloadType::Text: return QStringLiteral("文本");
    case ClipboardPayloadType::Image: return QStringLiteral("图片");
    case ClipboardPayloadType::Files:
        return item.files.size() == 1 ? item.files.first().name
                                      : QStringLiteral("文件 · %1 项").arg(item.files.size());
    }
    return QString();
}

HistoryStore::HistoryStore(int capacity, QObject *parent) : QObject(parent), capacity_(std::max(1, capacity)) {}

// MARK: - Setters carrying the Swift didSet behaviour

void HistoryStore::setSelectedItemId(const QUuid &id) {
    if (id == selectedItemId_) return;
    selectedItemId_ = id;
    setPreviewedItemId(QUuid());
    setFilePasteFeedback(FilePasteFeedback());
    Q_EMIT selectedItemIdChanged();
}

void HistoryStore::setSearchQuery(const QString &query) {
    if (query == searchQuery_) return;
    const QString old = searchQuery_;
    searchQuery_ = query;
    Q_EMIT searchQueryChanged();
    if (old.trimmed().isEmpty() && !query.trimmed().isEmpty()) setSelectedCategory(HistoryCategory::all());
}

void HistoryStore::setFilePasteFeedback(const FilePasteFeedback &feedback) {
    if (sameFeedback(feedback, filePasteFeedback_)) return;
    filePasteFeedback_ = feedback;
    Q_EMIT filePasteFeedbackChanged();
}

void HistoryStore::setItems(QVector<ClipboardItem> items) {
    items_ = std::move(items);
    Q_EMIT itemsChanged();
}

void HistoryStore::setSearchResults(QVector<ClipboardItem> results) {
    searchResults_ = std::move(results);
    Q_EMIT searchResultsChanged();
}

void HistoryStore::setSelectedCategory(const HistoryCategory &category) {
    if (category == selectedCategory_) return;
    selectedCategory_ = category;
    Q_EMIT selectedCategoryChanged();
}

void HistoryStore::setSearchFlags(bool active, bool editing, bool focused) {
    if (active == searchActive_ && editing == searchEditing_ && focused == searchFieldFocused_) return;
    searchActive_ = active;
    searchEditing_ = editing;
    searchFieldFocused_ = focused;
    Q_EMIT searchStateChanged();
}

void HistoryStore::setPreviewedItemId(const QUuid &id) {
    if (id == previewedItemId_) return;
    previewedItemId_ = id;
    Q_EMIT previewedItemIdChanged();
}

void HistoryStore::setEditingItemId(const QUuid &id) {
    if (id == editingItemId_) return;
    editingItemId_ = id;
    Q_EMIT editingItemIdChanged();
}

void HistoryStore::setRenamingItemId(const QUuid &id) {
    if (id == renamingItemId_) return;
    renamingItemId_ = id;
    Q_EMIT renamingItemIdChanged();
}

// MARK: - Derived state

std::optional<ClipboardItem> HistoryStore::selectedItem() const {
    const QVector<ClipboardItem> displayed = displayedItems();
    if (selectedItemId_.isNull()) {
        if (displayed.isEmpty()) return std::nullopt;
        return displayed.first();
    }
    return find(displayed, selectedItemId_);
}

std::optional<ClipboardItem> HistoryStore::editingItem() const {
    if (auto item = find(items_, editingItemId_)) return item;
    return find(searchResults_, editingItemId_);
}

std::optional<ClipboardItem> HistoryStore::renamingItem() const {
    if (auto item = find(items_, renamingItemId_)) return item;
    return find(searchResults_, renamingItemId_);
}

QVector<ClipboardItem> HistoryStore::displayedItems() const {
    const QVector<ClipboardItem> &source =
        searchActive_ && !searchQuery_.trimmed().isEmpty() ? searchResults_ : items_;
    QVector<ClipboardItem> result;
    result.reserve(source.size());
    for (const ClipboardItem &item : source) {
        bool visible = false;
        switch (selectedCategory_.kind) {
        case HistoryCategory::All: visible = true; break;
        case HistoryCategory::Text: visible = item.isText(); break;
        case HistoryCategory::Images: visible = item.isImage(); break;
        case HistoryCategory::Files: visible = item.isFiles(); break;
        case HistoryCategory::Automatic: visible = item.category == selectedCategory_.name; break;
        }
        if (visible) result.append(item);
    }
    return result;
}

QUuid HistoryStore::firstDisplayedId() const {
    const QVector<ClipboardItem> displayed = displayedItems();
    return displayed.isEmpty() ? QUuid() : displayed.first().id;
}

bool HistoryStore::displayedContains(const QUuid &id) const { return contains(displayedItems(), id); }

// MARK: - Loading

void HistoryStore::replaceItems(const QVector<ClipboardItem> &items, bool preservingSelection) {
    const QUuid previousSelection = selectedItemId_;
    setItems(items.mid(0, capacity_));
    setSelectedItemId(preservingSelection && contains(items_, previousSelection)
                          ? previousSelection
                          : (items_.isEmpty() ? QUuid() : items_.first().id));
}

void HistoryStore::expandPersistedItems(const QVector<ClipboardItem> &persistedItems) {
    const QUuid previousSelection = selectedItemId_;
    QHash<qint64, ClipboardItem> currentByDatabaseId;
    QVector<ClipboardItem> merged;
    for (const ClipboardItem &item : items_) {
        if (item.databaseID >= 0) currentByDatabaseId.insert(item.databaseID, item);
        else merged.append(item); // transient captures stay
    }
    for (const ClipboardItem &item : persistedItems) {
        // Keep the in-memory instance (and its identity) when the row is already shown.
        merged.append(item.databaseID >= 0 ? currentByDatabaseId.value(item.databaseID, item) : item);
    }
    std::stable_sort(merged.begin(), merged.end(),
                     [](const ClipboardItem &lhs, const ClipboardItem &rhs) { return lhs.copiedAt > rhs.copiedAt; });
    setItems(merged.mid(0, capacity_));
    setSelectedItemId(contains(items_, previousSelection) ? previousSelection
                                                          : (items_.isEmpty() ? QUuid() : items_.first().id));
}

void HistoryStore::replaceCategories(const QVector<ClipboardCategory> &categories) {
    automaticCategories_ = categories;
    Q_EMIT automaticCategoriesChanged();
}

// MARK: - Capture

void HistoryStore::insert(const ClipboardItem &item) {
    QVector<ClipboardItem> next = items_;
    next.erase(std::remove_if(next.begin(), next.end(),
                              [&](const ClipboardItem &existing) { return hasSameContent(existing, item); }),
               next.end());
    next.prepend(item);
    if (next.size() > capacity_) next.resize(capacity_);
    setItems(std::move(next));
    setSelectedCategory(HistoryCategory::all());
    setSelectedItemId(item.id);
}

void HistoryStore::markCopied(const QUuid &itemId) {
    const std::optional<ClipboardItem> current = find(items_, itemId);
    if (!current) return;
    ClipboardItem updated = *current;
    updated.copiedAt = QDateTime::currentDateTime();
    updated.copyCount += 1;
    QVector<ClipboardItem> next = items_;
    next.erase(std::remove_if(next.begin(), next.end(),
                              [&](const ClipboardItem &existing) { return existing.id == itemId; }),
               next.end());
    next.prepend(updated);
    setItems(std::move(next));
    if (find(searchResults_, itemId)) {
        replace(*current, updated, searchResults_);
        Q_EMIT searchResultsChanged();
    }
}

CaptureReconciliation HistoryStore::reconcileCapturedItem(const ClipboardItem &item) {
    const auto index = std::find_if(items_.begin(), items_.end(),
                                    [&](const ClipboardItem &existing) { return existing.id == item.id; });
    if (index == items_.end()) {
        const bool duplicate = std::any_of(items_.cbegin(), items_.cend(),
                                           [&](const ClipboardItem &existing) { return hasSameContent(existing, item); });
        return duplicate ? CaptureReconciliation::Superseded : CaptureReconciliation::Removed;
    }
    *index = item;
    Q_EMIT itemsChanged();
    for (ClipboardItem &result : searchResults_) {
        if (result.id == item.id) {
            result = item;
            Q_EMIT searchResultsChanged();
            break;
        }
    }
    return CaptureReconciliation::Reconciled;
}

// MARK: - Selection and categories

std::optional<ClipboardItem> HistoryStore::deleteSelection() {
    const std::optional<ClipboardItem> item = selectedItem();
    if (!item) return std::nullopt;
    const auto matches = [&](const ClipboardItem &candidate) {
        return item->databaseID >= 0 ? candidate.databaseID == item->databaseID : candidate.id == item->id;
    };
    QVector<ClipboardItem> nextItems = items_;
    nextItems.erase(std::remove_if(nextItems.begin(), nextItems.end(), matches), nextItems.end());
    setItems(std::move(nextItems));
    QVector<ClipboardItem> nextResults = searchResults_;
    nextResults.erase(std::remove_if(nextResults.begin(), nextResults.end(), matches), nextResults.end());
    setSearchResults(std::move(nextResults));
    setSelectedItemId(firstDisplayedId());
    if (onDelete) onDelete(*item);
    return item;
}

void HistoryStore::selectCategory(const HistoryCategory &category) {
    if (category == selectedCategory_) return;
    setSelectedCategory(category);
    if (!displayedContains(selectedItemId_)) setSelectedItemId(firstDisplayedId());
    revealSelection();
}

void HistoryStore::setQuickPasteOverlayVisible(bool visible) {
    if (visible == quickPasteOverlayVisible_) return;
    quickPasteOverlayVisible_ = visible;
    Q_EMIT quickPasteOverlayVisibleChanged();
}

void HistoryStore::revealSelection() {
    ++selectionRevealToken_; // Swift &+= : wrapping is irrelevant at int range
    Q_EMIT selectionRevealTokenChanged();
}

void HistoryStore::moveSelection(int offset) {
    const QVector<ClipboardItem> visible = displayedItems();
    if (visible.isEmpty()) return;
    int currentIndex = 0;
    for (int index = 0; index < visible.size(); ++index) {
        if (!selectedItemId_.isNull() && visible.at(index).id == selectedItemId_) {
            currentIndex = index;
            break;
        }
    }
    const int nextIndex = std::clamp(currentIndex + offset, 0, int(visible.size()) - 1);
    setSelectedItemId(visible.at(nextIndex).id);
}

void HistoryStore::moveCategory(int offset) {
    QVector<HistoryCategory> categories{HistoryCategory::all(), HistoryCategory::text(), HistoryCategory::images(),
                                        HistoryCategory::files()};
    for (const ClipboardCategory &category : automaticCategories_) {
        categories.append(HistoryCategory::automatic(category.name));
    }
    const int currentIndex = int(categories.indexOf(selectedCategory_));
    if (currentIndex < 0) return;
    const int count = int(categories.size());
    selectCategory(categories.at((currentIndex + offset % count + count) % count));
}

// MARK: - Search

void HistoryStore::beginSearch() {
    const bool focused = searchEditing_ ? searchFieldFocused_ : false;
    setSearchFlags(true, true, focused);
    setSelectedItemId(firstDisplayedId());
}

void HistoryStore::setSearchEditing(bool editing) {
    if (!searchActive_ && editing) return;
    setSearchFlags(searchActive_, editing, searchFieldFocused_);
}

void HistoryStore::setSearchFieldFocused(bool focused) { setSearchFlags(searchActive_, searchEditing_, focused); }

void HistoryStore::finishSearchEditing() { setSearchFlags(searchActive_, false, false); }

void HistoryStore::resumeSearchEditingAndDeleteBackward() {
    if (!searchActive_) return;
    setSearchFlags(true, true, false);
    if (!searchQuery_.isEmpty()) {
        QString shorter = searchQuery_;
        removeLastCharacter(shorter);
        setSearchQuery(shorter);
    }
    collapseSearchIfQueryWasDeleted();
}

void HistoryStore::collapseSearchIfQueryWasDeleted() {
    if (!searchActive_ || !searchQuery_.isEmpty()) return;
    endSearch();
}

void HistoryStore::replaceSearchResults(const QVector<ClipboardItem> &results) {
    setSearchResults(results);
    if (!displayedContains(selectedItemId_)) setSelectedItemId(firstDisplayedId());
}

QVector<ClipboardItem> HistoryStore::applyLocalSearch(const QString &query) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        replaceSearchResults({});
        return {};
    }
    QVector<ClipboardItem> matches;
    for (const ClipboardItem &item : items_) {
        bool match = item.title.contains(trimmed, Qt::CaseInsensitive);
        if (!match) {
            switch (item.type) {
            case ClipboardPayloadType::Text: match = item.text.contains(trimmed, Qt::CaseInsensitive); break;
            case ClipboardPayloadType::Files:
                match = std::any_of(item.files.cbegin(), item.files.cend(), [&](const ClipboardFileReference &file) {
                    return file.name.contains(trimmed, Qt::CaseInsensitive)
                        || file.path.contains(trimmed, Qt::CaseInsensitive);
                });
                break;
            case ClipboardPayloadType::Image: break;
            }
        }
        if (match) matches.append(item);
    }
    replaceSearchResults(matches);
    return matches;
}

void HistoryStore::endSearch() {
    setSearchFlags(false, false, false);
    setSearchQuery(QString());
    setSearchResults({});
    setSelectedItemId(items_.isEmpty() ? QUuid() : items_.first().id);
}

// MARK: - Preview, editing, renaming

void HistoryStore::togglePreviewForSelection() {
    if (selectedItemId_.isNull()) return;
    setPreviewedItemId(previewedItemId_ == selectedItemId_ ? QUuid() : selectedItemId_);
}

void HistoryStore::dismissPreview() { setPreviewedItemId(QUuid()); }

void HistoryStore::beginEditing(const ClipboardItem &item) {
    if (item.isFiles()) return;
    setPreviewedItemId(QUuid());
    setRenamingItemId(QUuid());
    setEditingItemId(item.id);
}

void HistoryStore::beginEditingSelection() {
    if (const auto item = selectedItem()) beginEditing(*item);
}

void HistoryStore::cancelEditing() { setEditingItemId(QUuid()); }

void HistoryStore::beginRenaming(const ClipboardItem &item) {
    setPreviewedItemId(QUuid());
    setEditingItemId(QUuid());
    setRenamingItemId(item.id);
}

void HistoryStore::beginRenamingSelection() {
    if (const auto item = selectedItem()) beginRenaming(*item);
}

void HistoryStore::cancelRenaming() { setRenamingItemId(QUuid()); }

void HistoryStore::replace(const ClipboardItem &current, const ClipboardItem &updated,
                           QVector<ClipboardItem> &collection) {
    for (ClipboardItem &item : collection) {
        const bool match = current.databaseID >= 0 ? item.databaseID == current.databaseID : item.id == current.id;
        if (match) {
            item = updated;
            return;
        }
    }
}

void HistoryStore::updateRenamingItem(const QString &title) {
    const std::optional<ClipboardItem> current = renamingItem();
    if (!current) return;
    ClipboardItem updated = *current;
    const QString trimmed = title.trimmed();
    updated.title = trimmed.isEmpty() ? defaultTitle(updated) : trimmed;
    replace(*current, updated, items_);
    Q_EMIT itemsChanged();
    replace(*current, updated, searchResults_);
    Q_EMIT searchResultsChanged();
    setRenamingItemId(QUuid());
    setSelectedItemId(updated.id);
    if (onUpdate) onUpdate(updated);
}

void HistoryStore::updateEditingItem(const ClipboardPayload &payload, std::optional<int> imageAngle,
                                     std::optional<QString> category) {
    const std::optional<ClipboardItem> current = editingItem();
    if (!current) return;
    ClipboardItem updated = *current;
    payload.applyTo(updated);
    if (imageAngle) updated.imageAngle = ((*imageAngle % 360) + 360) % 360;
    if (category) {
        // Re-classified text takes the category's look (preview language, icon).
        updated.category = *category;
        updated.categoryIcon.clear();
        updated.previewLanguage.clear();
        for (const ClipboardCategory &automatic : automaticCategories_) {
            if (automatic.name != *category) continue;
            updated.categoryIcon = automatic.icon;
            updated.previewLanguage = automatic.previewLanguage;
        }
    }
    replace(*current, updated, items_);
    Q_EMIT itemsChanged();
    replace(*current, updated, searchResults_);
    Q_EMIT searchResultsChanged();
    setEditingItemId(QUuid());
    setSelectedItemId(updated.id);
    if (onUpdate) onUpdate(updated);
}

void HistoryStore::updateLinkMetadata(const QUuid &itemId, std::optional<QString> title,
                                      std::optional<QByteArray> iconData, std::optional<QByteArray> previewData) {
    const std::optional<ClipboardItem> current = find(items_, itemId);
    if (!current) return;
    const QString cleanTitle = title ? title->trimmed() : QString();
    const QString resolvedTitle =
        current->title == QStringLiteral("文本") && title && !cleanTitle.isEmpty() ? cleanTitle : current->title;
    const QByteArray resolvedIcon = iconData ? *iconData : current->linkIconData;
    const QByteArray resolvedPreview = previewData ? *previewData : current->linkPreviewData;
    if (current->hasFetchedLinkPreview && resolvedTitle == current->title && resolvedIcon == current->linkIconData
        && resolvedPreview == current->linkPreviewData) {
        return;
    }
    ClipboardItem updated = *current;
    updated.title = resolvedTitle;
    updated.linkIconData = resolvedIcon;
    updated.linkPreviewData = resolvedPreview;
    updated.hasFetchedLinkPreview = true;
    replace(*current, updated, items_);
    Q_EMIT itemsChanged();
    replace(*current, updated, searchResults_);
    Q_EMIT searchResultsChanged();
    if (onUpdate) onUpdate(updated);
}
