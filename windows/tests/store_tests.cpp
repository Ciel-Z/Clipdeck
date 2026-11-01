// Port of mac/Tests/ClipDeckTests/InMemoryHistoryStoreTests.swift (store cases).
// Reader, preview-layout and JSON formatter cases live with their own modules.
#include "history_store.h"

#include <QCoreApplication>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

ClipboardItem item(const QString &text, QDateTime copiedAt = QDateTime::currentDateTime()) {
    ClipboardItem result;
    result.type = ClipboardPayloadType::Text;
    result.text = text;
    result.title = QStringLiteral("文本");
    result.sourceID = QStringLiteral("C:/Program Files/Editor/editor.exe");
    result.sourceName = QStringLiteral("Editor");
    result.copiedAt = copiedAt;
    return result;
}

ClipboardItem image(const QByteArray &data = QByteArray(1, '\0')) {
    ClipboardItem result;
    result.type = ClipboardPayloadType::Image;
    result.imageData = data;
    result.imagePixelSize = QSize(1, 1);
    result.title = QStringLiteral("图片");
    result.copiedAt = QDateTime::currentDateTime();
    return result;
}

QVector<QUuid> ids(const QVector<ClipboardItem> &items) {
    QVector<QUuid> result;
    for (const auto &entry : items) result.append(entry.id);
    return result;
}

void testNewestItemIsSelectedAndCapacityIsEnforced() {
    HistoryStore store(2);
    const auto first = item("first"), second = item("second"), third = item("third");
    store.insert(first);
    store.insert(second);
    store.insert(third);
    check(ids(store.items()) == QVector<QUuid>{third.id, second.id}, "capacity-keeps-newest");
    check(store.selectedItemId() == third.id, "newest-item-is-selected");
}

void testExpandingPersistedHistoryPreservesPriorityItemIdentityAndSelection() {
    HistoryStore store;
    ClipboardItem priority = item("priority");
    priority.databaseID = 1;
    store.replaceItems({priority});
    ClipboardItem reloadedPriority = item("priority", priority.copiedAt);
    reloadedPriority.databaseID = 1; // new in-memory identity, same row
    ClipboardItem older = item("older", priority.copiedAt.addSecs(-60));
    older.databaseID = 2;

    store.expandPersistedItems({reloadedPriority, older});

    check(store.items().size() == 2 && store.items().at(0).databaseID == 1 && store.items().at(1).databaseID == 2,
          "expand-orders-persisted-rows");
    check(store.items().first().id == priority.id, "expand-keeps-priority-identity");
    check(store.selectedItemId() == priority.id, "expand-keeps-selection");
}

void testDuplicatePayloadMovesNewestCopyToFront() {
    HistoryStore store;
    const auto original = item("same"), other = item("other"), duplicate = item("same");
    store.insert(original);
    store.insert(other);
    store.insert(duplicate);
    check(store.items().size() == 2, "duplicate-replaces-older-copy");
    check(store.items().first().id == duplicate.id, "duplicate-moves-to-front");
}

void testReconcilingCapturedItemPreservesIdentityPositionAndSelection() {
    HistoryStore store;
    const auto captured = item("captured");
    store.insert(captured);
    ClipboardItem persisted = captured;
    persisted.databaseID = 42;

    check(store.reconcileCapturedItem(persisted) == CaptureReconciliation::Reconciled, "reconcile-returns-reconciled");
    check(store.items().first().databaseID == 42, "reconcile-stores-database-id");
    check(store.items().first().id == captured.id, "reconcile-keeps-identity");
    check(store.selectedItemId() == captured.id, "reconcile-keeps-selection");
}

void testReconcilingDeletedTransientCaptureDoesNotRestoreIt() {
    HistoryStore store;
    const auto captured = item("captured");
    store.insert(captured);
    store.deleteSelection();
    check(store.reconcileCapturedItem(captured) == CaptureReconciliation::Removed, "reconcile-deleted-returns-removed");
    check(store.items().isEmpty(), "reconcile-deleted-is-not-restored");
}

void testOlderCaptureIsSupersededByNewerDuplicate() {
    HistoryStore store;
    const auto older = item("duplicate"), newer = item("duplicate");
    store.insert(older);
    store.insert(newer);
    check(store.reconcileCapturedItem(older) == CaptureReconciliation::Superseded, "older-capture-is-superseded");
    check(ids(store.items()) == QVector<QUuid>{newer.id}, "superseded-capture-keeps-newer");
}

void testSelectionNavigationAndDeletion() {
    HistoryStore store;
    const auto older = item("older"), newer = item("newer");
    store.insert(older);
    store.insert(newer);
    store.selectNext();
    check(store.selectedItemId() == older.id, "select-next-moves-to-older");
    store.deleteSelection();
    check(ids(store.items()) == QVector<QUuid>{newer.id}, "delete-removes-selection");
    check(store.selectedItemId() == newer.id, "delete-selects-remaining");
}

void testCategorySwitchingFiltersItemsAndKeepsAVisibleSelection() {
    HistoryStore store;
    const auto text = item("text");
    const auto picture = image();
    store.insert(text);
    store.insert(picture);

    store.selectNextCategory();
    check(store.selectedCategory() == HistoryCategory::text(), "category-next-is-text");
    check(ids(store.displayedItems()) == QVector<QUuid>{text.id}, "text-category-filters");
    check(store.selectedItemId() == text.id, "text-category-selects-visible-item");

    store.selectNextCategory();
    check(store.selectedCategory() == HistoryCategory::images(), "category-next-is-images");
    check(ids(store.displayedItems()) == QVector<QUuid>{picture.id}, "images-category-filters");
    check(store.selectedItemId() == picture.id, "images-category-selects-visible-item");

    store.selectNextCategory();
    check(store.selectedCategory() == HistoryCategory::files(), "category-next-is-files");
    check(store.displayedItems().isEmpty(), "files-category-is-empty");

    store.selectNextCategory();
    check(store.selectedCategory() == HistoryCategory::all(), "category-wraps-to-all");
}

void testRevealSelectionDoesNotChangeTheRememberedItem() {
    HistoryStore store;
    store.insert(item("older"));
    store.insert(item("newer"));
    store.selectNext();
    const QUuid selected = store.selectedItemId();
    const int token = store.selectionRevealToken();
    store.revealSelection();
    check(store.selectedItemId() == selected, "reveal-keeps-selection");
    check(store.selectionRevealToken() == token + 1, "reveal-bumps-token");
}

void testSearchResultsDriveSelectionAndRestoreHistoryWhenClosed() {
    HistoryStore store;
    const auto first = item("first"), result = item("matching");
    store.insert(first);
    store.beginSearch();
    store.setSearchQuery(QStringLiteral("match"));
    store.replaceSearchResults({result});
    check(ids(store.displayedItems()) == QVector<QUuid>{result.id}, "search-results-are-displayed");
    check(store.selectedItem() && store.selectedItem()->id == result.id, "search-result-is-selected");
    store.endSearch();
    check(ids(store.displayedItems()) == QVector<QUuid>{first.id}, "end-search-restores-history");
    check(store.selectedItem() && store.selectedItem()->id == first.id, "end-search-restores-selection");
}

void testLocalSearchUpdatesImmediatelyAndExcludesImages() {
    HistoryStore store;
    const auto matching = item("Hello Search World"), other = item("unrelated");
    store.insert(other);
    store.insert(image());
    store.insert(matching);
    store.beginSearch();
    store.setSearchQuery(QStringLiteral("search"));
    const auto results = store.applyLocalSearch(store.searchQuery());
    check(ids(results) == QVector<QUuid>{matching.id}, "local-search-is-case-insensitive");
    check(ids(store.displayedItems()) == QVector<QUuid>{matching.id}, "local-search-excludes-images");
    check(store.selectedItemId() == matching.id, "local-search-selects-match");
}

void testLocalSearchPreservesAVisibleSelectionWhileQueryChanges() {
    HistoryStore store;
    const auto first = item("search first"), second = item("search second");
    store.insert(first);
    store.insert(second);
    store.beginSearch();
    store.setSearchQuery(QStringLiteral("search"));
    store.applyLocalSearch(store.searchQuery());
    store.selectNext();
    check(store.selectedItemId() == first.id, "search-select-next");
    store.setSearchQuery(QStringLiteral("search f"));
    store.applyLocalSearch(store.searchQuery());
    check(store.selectedItemId() == first.id, "search-refinement-keeps-visible-selection");
}

void testFirstSearchCharacterSelectsHistoryButLaterTypingPreservesCategory() {
    HistoryStore store;
    store.insert(item("matching text"));
    store.insert(image());
    store.selectCategory(HistoryCategory::images());
    store.beginSearch();
    check(store.selectedCategory() == HistoryCategory::images(), "begin-search-keeps-category");
    store.setSearchQuery(QStringLiteral("m"));
    check(store.selectedCategory() == HistoryCategory::all(), "first-character-selects-all");
    store.selectCategory(HistoryCategory::text());
    store.setSearchQuery(QStringLiteral("ma"));
    check(store.selectedCategory() == HistoryCategory::text(), "later-typing-keeps-category");
}

void testFinishingSearchEditingKeepsResultsActive() {
    HistoryStore store;
    const auto result = item("matching");
    store.insert(result);
    store.beginSearch();
    store.setSearchQuery(QStringLiteral("match"));
    store.applyLocalSearch(store.searchQuery());
    store.finishSearchEditing();
    check(store.isSearchActive(), "finish-editing-keeps-search-active");
    check(!store.isSearchEditing(), "finish-editing-stops-editing");
    check(ids(store.displayedItems()) == QVector<QUuid>{result.id}, "finish-editing-keeps-results");
    check(store.selectedItemId() == result.id, "finish-editing-keeps-selection");
}

void testBackspaceResumesSearchEditingAndRemovesTheLastCharacter() {
    HistoryStore store;
    store.insert(item("matching"));
    store.beginSearch();
    store.setSearchQuery(QStringLiteral("match"));
    store.finishSearchEditing();
    store.resumeSearchEditingAndDeleteBackward();
    check(store.isSearchEditing(), "backspace-resumes-editing");
    check(store.searchQuery() == QStringLiteral("matc"), "backspace-removes-last-character");
}

void testBackspaceRemovesAWholeGraphemeCluster() {
    HistoryStore store;
    store.insert(item("matching"));
    store.beginSearch();
    // "e" + combining acute, then a family emoji built from a ZWJ sequence.
    store.setSearchQuery(QStringLiteral("ae\u0301"));
    store.resumeSearchEditingAndDeleteBackward();
    check(store.searchQuery() == QStringLiteral("a"), "backspace-removes-combining-sequence");
    store.setSearchQuery(QString::fromUtf8("a\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"));
    store.resumeSearchEditingAndDeleteBackward();
    check(store.searchQuery() == QStringLiteral("a"), "backspace-removes-zwj-emoji");
}

void testBackspaceCollapsesSearchAfterDeletingTheLastCharacter() {
    HistoryStore store;
    const auto original = item("matching");
    store.insert(original);
    store.beginSearch();
    store.setSearchQuery(QStringLiteral("m"));
    store.finishSearchEditing();
    store.resumeSearchEditingAndDeleteBackward();
    check(!store.isSearchActive(), "backspace-empty-ends-search");
    check(!store.isSearchEditing(), "backspace-empty-stops-editing");
    check(store.searchQuery().isEmpty(), "backspace-empty-clears-query");
    check(ids(store.displayedItems()) == QVector<QUuid>{original.id}, "backspace-empty-restores-history");
}

void testPreviewClosesWhenSelectionChanges() {
    HistoryStore store;
    const auto older = item("older"), newer = item("newer");
    store.insert(older);
    store.insert(newer);
    store.togglePreviewForSelection();
    check(store.previewedItemId() == newer.id, "preview-opens-for-selection");
    store.selectNext();
    check(store.selectedItemId() == older.id, "preview-select-next");
    check(store.previewedItemId().isNull(), "preview-closes-on-selection-change");
}

void testFilePasteFeedbackClearsWhenSelectionChanges() {
    HistoryStore store;
    store.insert(item("older"));
    store.insert(item("newer"));
    FilePasteFeedback feedback;
    feedback.phase = FilePasteFeedback::Unavailable;
    feedback.message = QStringLiteral("无法粘贴，源文件不可访问");
    store.setFilePasteFeedback(feedback);
    store.selectNext();
    check(store.filePasteFeedback().phase == FilePasteFeedback::Idle, "file-feedback-clears-on-selection-change");
}

void testEditingReplacesItemAndNotifiesPersistence() {
    HistoryStore store;
    const auto original = item("before");
    std::optional<ClipboardItem> persisted;
    store.onUpdate = [&](const ClipboardItem &updated) { persisted = updated; };
    store.insert(original);
    store.beginEditing(original);
    ClipboardPayload payload;
    payload.text = QStringLiteral("after");
    store.updateEditingItem(payload);
    check(store.items().size() == 1, "edit-keeps-one-item");
    check(store.items().first().text == QStringLiteral("after"), "edit-replaces-payload");
    check(persisted && persisted->text == QStringLiteral("after"), "edit-notifies-persistence");
    check(store.editingItemId().isNull(), "edit-ends-editing");
    check(store.selectedItemId() == store.items().first().id, "edit-keeps-selection");
}

void testImageEditingStoresAngleWithoutChangingImagePayload() {
    HistoryStore store;
    const auto original = image(QByteArray("\x01\x02\x03", 3));
    store.insert(original);
    store.beginEditing(original);
    store.updateEditingItem(ClipboardPayload::of(original), 90);
    check(store.items().first().imageData == original.imageData, "image-edit-keeps-payload");
    check(store.items().first().imageAngle == 90, "image-edit-stores-angle");
}

void testEditingReclassifiesText() {
    HistoryStore store;
    ClipboardCategory json;
    json.name = QStringLiteral("JSON");
    json.icon = QStringLiteral("{}");
    json.previewLanguage = QStringLiteral("json");
    store.replaceCategories({json});
    ClipboardItem original = item("{\"a\": 1}");
    original.category = QStringLiteral("JSON");
    original.previewLanguage = QStringLiteral("json");
    store.insert(original);

    store.beginEditing(original);
    ClipboardPayload payload;
    payload.text = QStringLiteral("now just words");
    store.updateEditingItem(payload, std::nullopt, QString());
    check(store.items().first().category.isEmpty() && store.items().first().previewLanguage.isEmpty(),
          "edit-into-plain-text-drops-category");

    store.beginEditing(store.items().first());
    payload.text = QStringLiteral("{\"b\": 2}");
    store.updateEditingItem(payload, std::nullopt, QStringLiteral("JSON"));
    check(store.items().first().category == QStringLiteral("JSON")
              && store.items().first().previewLanguage == QStringLiteral("json")
              && store.items().first().categoryIcon == QStringLiteral("{}"),
          "edit-into-json-takes-the-category-look");
}

void testEditingASummaryMarksTheTextComplete() {
    HistoryStore store;
    ClipboardItem summary = item(QString(3000, QLatin1Char('x')));
    summary.payloadComplete = false; // card summary of a longer text
    summary.textLength = 5000;
    store.insert(summary);
    store.beginEditing(summary);
    ClipboardPayload payload;
    payload.text = QStringLiteral("short");
    store.updateEditingItem(payload);
    check(store.items().first().payloadComplete, "edited-text-is-complete");
    check(store.items().first().textLength == 5, "edited-text-length");

    ClipboardItem picture = image();
    picture.payloadComplete = false;
    store.insert(picture);
    store.beginEditing(picture);
    store.updateEditingItem(ClipboardPayload::of(picture), 180);
    check(!store.items().first().payloadComplete, "rotating-a-thumbnail-keeps-it-a-summary");
}

void testFilesCannotBeEdited() {
    HistoryStore store;
    ClipboardItem files;
    files.type = ClipboardPayloadType::Files;
    files.files.append({QStringLiteral("C:/a.txt"), QStringLiteral("a.txt"), QStringLiteral("text/plain"), false});
    store.insert(files);
    store.beginEditing(files);
    check(store.editingItemId().isNull(), "files-are-not-editable");
}

void testRenamingPersistsAndTitleParticipatesInLocalSearch() {
    HistoryStore store;
    const auto original = item(QStringLiteral("正文没有关键词"));
    std::optional<ClipboardItem> persisted;
    store.onUpdate = [&](const ClipboardItem &updated) { persisted = updated; };
    store.insert(original);
    store.beginRenaming(original);
    store.updateRenamingItem(QStringLiteral("项目代号 Aurora"));
    check(store.items().first().title == QStringLiteral("项目代号 Aurora"), "rename-updates-title");
    check(persisted && persisted->title == QStringLiteral("项目代号 Aurora"), "rename-notifies-persistence");
    check(store.renamingItemId().isNull(), "rename-ends-renaming");
    const auto results = store.applyLocalSearch(QStringLiteral("Aurora"));
    check(!results.isEmpty() && results.first().title == QStringLiteral("项目代号 Aurora"), "title-is-searchable");

    store.beginRenaming(store.items().first());
    store.updateRenamingItem(QStringLiteral("   "));
    check(store.items().first().title == QStringLiteral("文本"), "blank-rename-restores-default-title");
}

void testQuickPasteOverlayTracksCommandModifierState() {
    HistoryStore store;
    store.setQuickPasteOverlayVisible(true);
    check(store.isQuickPasteOverlayVisible(), "quick-paste-overlay-shows");
    store.setQuickPasteOverlayVisible(false);
    check(!store.isQuickPasteOverlayVisible(), "quick-paste-overlay-hides");
}

void testLinkMetadataReplacesDefaultTitleOnce() {
    HistoryStore store;
    const auto link = item("https://example.com");
    int updates = 0;
    store.onUpdate = [&](const ClipboardItem &) { ++updates; };
    store.insert(link);
    store.updateLinkMetadata(link.id, QStringLiteral("  Example Domain "), QByteArray("icon"), std::nullopt);
    check(store.items().first().title == QStringLiteral("Example Domain"), "link-title-replaces-default");
    check(store.items().first().hasFetchedLinkPreview, "link-marked-fetched");
    store.updateLinkMetadata(link.id, QStringLiteral("Example Domain"), QByteArray("icon"), std::nullopt);
    check(updates == 1, "unchanged-link-metadata-is-not-persisted-again");
}

void testAutomaticCategoryFilters() {
    HistoryStore store;
    ClipboardItem link = item("https://example.com");
    link.category = QStringLiteral("链接");
    const auto plain = item("plain");
    store.insert(plain);
    store.insert(link);
    ClipboardCategory category;
    category.name = QStringLiteral("链接");
    store.replaceCategories({category});
    for (int step = 0; step < 4; ++step) store.selectNextCategory();
    check(store.selectedCategory() == HistoryCategory::automatic(QStringLiteral("链接")), "category-cycle-reaches-automatic");
    check(ids(store.displayedItems()) == QVector<QUuid>{link.id}, "automatic-category-filters-by-name");
    store.selectPreviousCategory();
    check(store.selectedCategory() == HistoryCategory::files(), "category-previous-steps-back");
}

void testSignalsFireOnlyOnChange() {
    HistoryStore store;
    int selectionSignals = 0;
    QObject::connect(&store, &HistoryStore::selectedItemIdChanged, [&] { ++selectionSignals; });
    const auto entry = item("one");
    store.insert(entry);
    store.setSelectedItemId(entry.id);
    check(selectionSignals == 1, "selection-signal-only-on-change");
}

} // namespace

void testMarkCopiedMovesTheItemToTheFront() {
    HistoryStore store;
    ClipboardItem older = item("older");
    older.copiedAt = QDateTime::currentDateTime().addSecs(-3600);
    const ClipboardItem newer = item("newer");
    store.insert(older);
    store.insert(newer);
    store.setSelectedItemId(newer.id);

    store.markCopied(older.id);
    check(store.items().first().id == older.id && store.items().size() == 2, "copied-item-moves-to-front");
    check(store.items().first().copiedAt > older.copiedAt
              && store.items().first().copyCount == older.copyCount + 1,
          "copied-item-gets-a-fresh-copy-time");
    check(store.selectedItemId() == newer.id, "marking-copied-keeps-the-selection");
    store.markCopied(QUuid::createUuid());
    check(store.items().size() == 2, "marking-an-unknown-item-does-nothing");
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    testNewestItemIsSelectedAndCapacityIsEnforced();
    testExpandingPersistedHistoryPreservesPriorityItemIdentityAndSelection();
    testDuplicatePayloadMovesNewestCopyToFront();
    testReconcilingCapturedItemPreservesIdentityPositionAndSelection();
    testReconcilingDeletedTransientCaptureDoesNotRestoreIt();
    testOlderCaptureIsSupersededByNewerDuplicate();
    testSelectionNavigationAndDeletion();
    testCategorySwitchingFiltersItemsAndKeepsAVisibleSelection();
    testRevealSelectionDoesNotChangeTheRememberedItem();
    testSearchResultsDriveSelectionAndRestoreHistoryWhenClosed();
    testLocalSearchUpdatesImmediatelyAndExcludesImages();
    testLocalSearchPreservesAVisibleSelectionWhileQueryChanges();
    testFirstSearchCharacterSelectsHistoryButLaterTypingPreservesCategory();
    testFinishingSearchEditingKeepsResultsActive();
    testBackspaceResumesSearchEditingAndRemovesTheLastCharacter();
    testBackspaceRemovesAWholeGraphemeCluster();
    testBackspaceCollapsesSearchAfterDeletingTheLastCharacter();
    testPreviewClosesWhenSelectionChanges();
    testFilePasteFeedbackClearsWhenSelectionChanges();
    testEditingReplacesItemAndNotifiesPersistence();
    testImageEditingStoresAngleWithoutChangingImagePayload();
    testFilesCannotBeEdited();
    testEditingASummaryMarksTheTextComplete();
    testEditingReclassifiesText();
    testMarkCopiedMovesTheItemToTheFront();
    testRenamingPersistsAndTitleParticipatesInLocalSearch();
    testQuickPasteOverlayTracksCommandModifierState();
    testLinkMetadataReplacesDefaultTitleOnce();
    testAutomaticCategoryFilters();
    testSignalsFireOnlyOnChange();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
