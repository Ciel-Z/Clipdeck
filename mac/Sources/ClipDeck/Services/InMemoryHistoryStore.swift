import Foundation

enum HistoryCategory: Hashable {
    case all
    case text
    case images
    case files
    case automatic(Int64)
}

enum CaptureReconciliation: Equatable {
    case reconciled
    case superseded
    case removed
}

@MainActor
final class InMemoryHistoryStore: ObservableObject {
    @Published private(set) var items: [ClipboardItem] = []
    @Published var selectedItemID: ClipboardItem.ID? {
        didSet {
            if oldValue != selectedItemID {
                previewedItemID = nil
                filePasteFeedback = nil
            }
        }
    }
    @Published private(set) var selectedCategory: HistoryCategory = .all
    @Published private(set) var selectionRevealToken = 0
    @Published private(set) var automaticCategories: [ClipboardCategory] = []
    @Published var searchQuery = "" {
        didSet {
            if oldValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
               !searchQuery.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                selectedCategory = .all
            }
        }
    }
    @Published private(set) var searchResults: [ClipboardItem] = []
    @Published private(set) var isSearchActive = false
    @Published private(set) var isSearchEditing = false
    @Published private(set) var isSearchFieldFocused = false
    @Published private(set) var previewedItemID: ClipboardItem.ID?
    @Published private(set) var editingItemID: ClipboardItem.ID?
    @Published private(set) var renamingItemID: ClipboardItem.ID?
    @Published private(set) var isQuickPasteOverlayVisible = false
    @Published var filePasteFeedback: FilePasteFeedback?
    var onDelete: ((ClipboardItem) -> Void)?
    var onUpdate: ((ClipboardItem) -> Void)?

    private let capacity: Int

    init(capacity: Int = 100) {
        self.capacity = max(1, capacity)
    }

    func replaceItems(_ items: [ClipboardItem], preservingSelection: Bool = false) {
        let previousSelection = selectedItemID
        self.items = Array(items.prefix(capacity))
        selectedItemID = preservingSelection
            && self.items.contains(where: { $0.id == previousSelection })
            ? previousSelection
            : self.items.first?.id
    }

    func expandPersistedItems(_ persistedItems: [ClipboardItem]) {
        let previousSelection = selectedItemID
        let currentByDatabaseID = Dictionary(
            uniqueKeysWithValues: items.compactMap { item in
                item.databaseID.map { ($0, item) }
            }
        )
        let transientItems = items.filter { $0.databaseID == nil }
        let resolvedPersistedItems = persistedItems.map { item in
            guard let databaseID = item.databaseID else { return item }
            return currentByDatabaseID[databaseID] ?? item
        }
        self.items = Array(
            (transientItems + resolvedPersistedItems)
                .sorted { $0.copiedAt > $1.copiedAt }
                .prefix(capacity)
        )
        selectedItemID = self.items.contains(where: { $0.id == previousSelection })
            ? previousSelection
            : self.items.first?.id
    }

    func replaceCategories(_ categories: [ClipboardCategory]) {
        automaticCategories = categories
    }

    var selectedItem: ClipboardItem? {
        guard let selectedItemID else { return displayedItems.first }
        return displayedItems.first { $0.id == selectedItemID }
    }

    var editingItem: ClipboardItem? {
        guard let editingItemID else { return nil }
        return items.first { $0.id == editingItemID }
            ?? searchResults.first { $0.id == editingItemID }
    }

    var renamingItem: ClipboardItem? {
        guard let renamingItemID else { return nil }
        return items.first { $0.id == renamingItemID }
            ?? searchResults.first { $0.id == renamingItemID }
    }

    var displayedItems: [ClipboardItem] {
        let source = isSearchActive && !searchQuery.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            ? searchResults
            : items
        return source.filter { item in
            switch (selectedCategory, item.payload) {
            case (.all, _), (.text, .text), (.images, .image), (.files, .files): true
            case let (.automatic(categoryID), _): item.category?.id == categoryID
            default: false
            }
        }
    }

    func insert(_ item: ClipboardItem) {
        items.removeAll { $0.payload.hasSameContent(as: item.payload) }
        items.insert(item, at: 0)
        if items.count > capacity {
            items.removeLast(items.count - capacity)
        }
        selectedCategory = .all
        selectedItemID = item.id
    }

    /// Replaces the immediately displayed transient capture with its persisted,
    /// classified form without moving the card or changing its selection.
    /// Distinguishes a user deletion from a newer capture of the same payload so
    /// persistence cleanup cannot delete the newer duplicate's shared row.
    func reconcileCapturedItem(_ item: ClipboardItem) -> CaptureReconciliation {
        guard let index = items.firstIndex(where: { $0.id == item.id }) else {
            return items.contains(where: { $0.payload.hasSameContent(as: item.payload) })
                ? .superseded
                : .removed
        }
        items[index] = item
        if let searchIndex = searchResults.firstIndex(where: { $0.id == item.id }) {
            searchResults[searchIndex] = item
        }
        return .reconciled
    }

    func selectPrevious() {
        moveSelection(by: -1)
    }

    func selectNext() {
        moveSelection(by: 1)
    }

    @discardableResult
    func deleteSelection() -> ClipboardItem? {
        guard let item = selectedItem else { return nil }
        if let databaseID = item.databaseID {
            items.removeAll { $0.databaseID == databaseID }
            searchResults.removeAll { $0.databaseID == databaseID }
        } else {
            items.removeAll { $0.id == item.id }
            searchResults.removeAll { $0.id == item.id }
        }
        selectedItemID = displayedItems.first?.id
        onDelete?(item)
        return item
    }

    func selectCategory(_ category: HistoryCategory) {
        guard category != selectedCategory else { return }
        selectedCategory = category
        if !displayedItems.contains(where: { $0.id == selectedItemID }) {
            selectedItemID = displayedItems.first?.id
        }
        revealSelection()
    }

    func selectPreviousCategory() {
        moveCategory(by: -1)
    }

    func selectNextCategory() {
        moveCategory(by: 1)
    }

    func setQuickPasteOverlayVisible(_ visible: Bool) {
        isQuickPasteOverlayVisible = visible
    }

    func revealSelection() {
        selectionRevealToken &+= 1
    }

    func beginSearch() {
        if !isSearchEditing {
            isSearchFieldFocused = false
        }
        isSearchActive = true
        isSearchEditing = true
        selectedItemID = displayedItems.first?.id
    }

    func setSearchEditing(_ editing: Bool) {
        guard isSearchActive || !editing else { return }
        isSearchEditing = editing
    }

    func setSearchFieldFocused(_ focused: Bool) {
        isSearchFieldFocused = focused
    }

    func finishSearchEditing() {
        isSearchEditing = false
        isSearchFieldFocused = false
    }

    func resumeSearchEditingAndDeleteBackward() {
        guard isSearchActive else { return }
        isSearchFieldFocused = false
        isSearchEditing = true
        if !searchQuery.isEmpty {
            searchQuery.removeLast()
        }
        collapseSearchIfQueryWasDeleted()
    }

    func collapseSearchIfQueryWasDeleted() {
        guard isSearchActive, searchQuery.isEmpty else { return }
        endSearch()
    }

    func replaceSearchResults(_ results: [ClipboardItem]) {
        searchResults = results
        if !displayedItems.contains(where: { $0.id == selectedItemID }) {
            selectedItemID = displayedItems.first?.id
        }
    }

    @discardableResult
    func applyLocalSearch(_ query: String) -> [ClipboardItem] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            replaceSearchResults([])
            return []
        }
        let matches = items.filter { item in
            if item.title.localizedCaseInsensitiveContains(trimmed) { return true }
            switch item.payload {
            case let .text(text, _): return text.localizedCaseInsensitiveContains(trimmed)
            case let .files(files):
                return files.contains { $0.name.localizedCaseInsensitiveContains(trimmed)
                    || $0.path.localizedCaseInsensitiveContains(trimmed) }
            case .image: return false
            }
        }
        replaceSearchResults(matches)
        return matches
    }

    func endSearch() {
        isSearchActive = false
        isSearchEditing = false
        isSearchFieldFocused = false
        searchQuery = ""
        searchResults = []
        selectedItemID = items.first?.id
    }

    func togglePreviewForSelection() {
        guard let selectedItemID else { return }
        previewedItemID = previewedItemID == selectedItemID ? nil : selectedItemID
    }

    func dismissPreview() {
        previewedItemID = nil
    }

    func beginEditing(_ item: ClipboardItem) {
        if case .files = item.payload { return }
        previewedItemID = nil
        renamingItemID = nil
        editingItemID = item.id
    }

    func beginEditingSelection() {
        guard let selectedItem else { return }
        beginEditing(selectedItem)
    }

    func cancelEditing() {
        editingItemID = nil
    }

    func beginRenaming(_ item: ClipboardItem) {
        previewedItemID = nil
        editingItemID = nil
        renamingItemID = item.id
    }

    func beginRenamingSelection() {
        guard let selectedItem else { return }
        beginRenaming(selectedItem)
    }

    func cancelRenaming() {
        renamingItemID = nil
    }

    func updateRenamingItem(title: String) {
        guard let current = renamingItem else { return }
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        let updated = ClipboardItem(
            id: current.id,
            databaseID: current.databaseID,
            title: trimmed.isEmpty ? nil : trimmed,
            linkIconData: current.linkIconData,
            linkPreviewData: current.linkPreviewData,
            hasFetchedLinkPreview: current.hasFetchedLinkPreview,
            imageAngle: current.imageAngle,
            payload: current.payload,
            sourceApplication: current.sourceApplication,
            category: current.category,
            copiedAt: current.copiedAt
        )
        replace(current, with: updated, in: &items)
        replace(current, with: updated, in: &searchResults)
        renamingItemID = nil
        selectedItemID = updated.id
        onUpdate?(updated)
    }

    func updateEditingItem(payload: ClipboardPayload, imageAngle: Int? = nil) {
        guard let current = editingItem else { return }
        let updated = ClipboardItem(
            id: current.id,
            databaseID: current.databaseID,
            title: current.title,
            linkIconData: current.linkIconData,
            linkPreviewData: current.linkPreviewData,
            hasFetchedLinkPreview: current.hasFetchedLinkPreview,
            imageAngle: imageAngle ?? current.imageAngle,
            payload: payload,
            sourceApplication: current.sourceApplication,
            category: current.category,
            copiedAt: current.copiedAt
        )
        replace(current, with: updated, in: &items)
        replace(current, with: updated, in: &searchResults)
        editingItemID = nil
        selectedItemID = updated.id
        onUpdate?(updated)
    }

    func updateLinkMetadata(
        itemID: ClipboardItem.ID,
        title: String?,
        iconData: Data?,
        previewData: Data?
    ) {
        guard let current = items.first(where: { $0.id == itemID }) else { return }
        let cleanTitle = title?.trimmingCharacters(in: .whitespacesAndNewlines)
        let resolvedTitle = current.title == "文本" && cleanTitle?.isEmpty == false
            ? cleanTitle!
            : current.title
        let resolvedIconData = iconData ?? current.linkIconData
        let resolvedPreviewData = previewData ?? current.linkPreviewData
        guard !current.hasFetchedLinkPreview
                || resolvedTitle != current.title
                || resolvedIconData != current.linkIconData
                || resolvedPreviewData != current.linkPreviewData else { return }
        let updated = ClipboardItem(
            id: current.id,
            databaseID: current.databaseID,
            title: resolvedTitle,
            linkIconData: resolvedIconData,
            linkPreviewData: resolvedPreviewData,
            hasFetchedLinkPreview: true,
            imageAngle: current.imageAngle,
            payload: current.payload,
            sourceApplication: current.sourceApplication,
            category: current.category,
            copiedAt: current.copiedAt
        )
        replace(current, with: updated, in: &items)
        replace(current, with: updated, in: &searchResults)
        onUpdate?(updated)
    }

    private func moveSelection(by offset: Int) {
        let visibleItems = displayedItems
        guard !visibleItems.isEmpty else { return }
        let currentIndex = selectedItemID.flatMap { id in
            visibleItems.firstIndex { $0.id == id }
        } ?? 0
        let nextIndex = min(max(currentIndex + offset, 0), visibleItems.count - 1)
        selectedItemID = visibleItems[nextIndex].id
    }

    private func replace(
        _ current: ClipboardItem,
        with updated: ClipboardItem,
        in collection: inout [ClipboardItem]
    ) {
        guard let index = collection.firstIndex(where: { item in
            if let databaseID = current.databaseID {
                return item.databaseID == databaseID
            }
            return item.id == current.id
        }) else { return }
        collection[index] = updated
    }

    private func moveCategory(by offset: Int) {
        let categories: [HistoryCategory] = [.all, .text, .images, .files]
            + automaticCategories.map { .automatic($0.id) }
        guard let currentIndex = categories.firstIndex(of: selectedCategory) else { return }
        let nextIndex = (currentIndex + offset + categories.count) % categories.count
        selectCategory(categories[nextIndex])
    }
}
