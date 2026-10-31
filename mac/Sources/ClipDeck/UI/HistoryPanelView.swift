import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct HistoryPanelView: View {
    @ObservedObject var store: InMemoryHistoryStore
    let onPaste: (ClipboardItem, Bool) -> Void
    let onClose: () -> Void
    let onManageCategories: () -> Void
    let onSearch: (String) -> Void
    @FocusState private var searchFieldFocused: Bool
    @AppStorage(AppPreferences.deckBackgroundBlurKey) private var backgroundBlur = true
    @State private var categoryPageStart = 0
    @State private var isResizing = false

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            content
        }
        .background {
            ZStack {
                // 背景模糊 off shows the live desktop through black 37 %, the
                // Linux default; the panel itself is clear and non-opaque.
                if backgroundBlur {
                    DeckMaterialView()
                    Color.black.opacity(0.20)
                } else {
                    Color.black.opacity(0.37)
                }
                LinearGradient(
                    colors: [.white.opacity(0.06), .clear, .black.opacity(0.06)],
                    startPoint: .top,
                    endPoint: .bottom
                )
            }
        }
        .overlay(alignment: .top) {
            DeckResizeHandle()
                .frame(maxWidth: .infinity)
                .frame(height: 18)
                .contentShape(Rectangle())
        }
        .onExitCommand {
            if store.filePasteFeedback != nil {
                store.filePasteFeedback = nil
            } else if store.renamingItemID != nil {
                store.cancelRenaming()
            } else if store.editingItemID != nil {
                store.cancelEditing()
            } else {
                onClose()
            }
        }
        .onMoveCommand { direction in
            guard store.editingItemID == nil, store.renamingItemID == nil else { return }
            if direction == .left { store.selectPrevious() }
            if direction == .right { store.selectNext() }
        }
        .onChange(of: store.isSearchEditing) { _, editing in
            searchFieldFocused = editing
        }
        .onChange(of: searchFieldFocused) { _, focused in
            store.setSearchFieldFocused(focused)
        }
        .onChange(of: store.searchQuery) { _, query in
            onSearch(query)
        }
        .onReceive(NotificationCenter.default.publisher(for: .clipDeckResizeStarted)) { _ in
            isResizing = true
        }
        .onReceive(NotificationCenter.default.publisher(for: .clipDeckResizeEnded)) { _ in
            isResizing = false
        }
        .transaction { transaction in
            if isResizing {
                transaction.animation = nil
            }
        }
    }

    @ViewBuilder
    private var content: some View {
        if store.displayedItems.isEmpty {
            emptyState
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            GeometryReader { geometry in
                let cardSide = max(166, geometry.size.height - 24)

                ScrollViewReader { proxy in
                    ScrollView(.horizontal, showsIndicators: false) {
                        LazyHStack(spacing: 24) {
                            ForEach(Array(store.displayedItems.enumerated()), id: \.element.id) { entry in
                                let item = entry.element
                                ClipboardCardView(
                                    item: item,
                                    isSelected: item.id == store.selectedItemID,
                                    width: cardSide,
                                    height: cardSide,
                                    isResizing: isResizing,
                                    quickPasteNumber: store.isQuickPasteOverlayVisible && entry.offset < 9
                                        ? entry.offset + 1
                                        : nil,
                                    fileFeedback: store.filePasteFeedback?.itemID == item.id
                                        ? store.filePasteFeedback : nil
                                )
                                .id(item.id)
                                .overlay(alignment: .bottom) {
                                    if let feedback = store.filePasteFeedback, feedback.itemID == item.id {
                                        FilePasteFeedbackView(
                                            feedback: feedback,
                                            onCancel: { store.filePasteFeedback = nil },
                                            onConfirm: { onPaste(item, false) },
                                            isSingleFile: item.payload.fileReferences?.count == 1
                                        )
                                        .padding(item.payload.fileReferences?.count == 1 ? 4 : 10)
                                    }
                                }
                                .onTapGesture {
                                    store.finishSearchEditing()
                                    store.selectedItemID = item.id
                                }
                                .onTapGesture(count: 2) {
                                    store.selectedItemID = item.id
                                    paste(
                                        item,
                                        forcePlainText: AppPreferences.isPlainTextModifierActive(
                                            in: NSApp.currentEvent?.modifierFlags ?? []
                                        )
                                    )
                                }
                                .popover(
                                    isPresented: floatingContentBinding(for: item),
                                    attachmentAnchor: .rect(.bounds),
                                    arrowEdge: .bottom
                                ) {
                                    if store.renamingItemID == item.id {
                                        RenameItemView(
                                            initialTitle: item.title,
                                            onCancel: { store.cancelRenaming() },
                                            onSave: { title in
                                                store.updateRenamingItem(title: title)
                                            }
                                        )
                                    } else if store.editingItemID == item.id {
                                        ContentEditorView(
                                            item: item,
                                            onCancel: { store.cancelEditing() },
                                            onSave: { payload, imageAngle in
                                                store.updateEditingItem(
                                                    payload: payload,
                                                    imageAngle: imageAngle
                                                )
                                            }
                                        )
                                    } else {
                                        ContentPreviewView(item: item)
                                    }
                                }
                                .contextMenu {
                                    Button {
                                        store.selectedItemID = item.id
                                        paste(item)
                                    } label: {
                                        Label("粘贴", systemImage: "doc.on.clipboard")
                                    }
                                    .keyboardShortcut(.return, modifiers: [])

                                    if let url = item.webURL {
                                        Button {
                                            store.selectedItemID = item.id
                                            NSWorkspace.shared.open(url)
                                        } label: {
                                            Label("访问", systemImage: "safari")
                                        }
                                        .keyboardShortcut("o", modifiers: .command)
                                    }

                                    Divider()

                                    Button {
                                        store.selectedItemID = item.id
                                        store.beginEditing(item)
                                    } label: {
                                        Label("编辑", systemImage: "pencil")
                                    }
                                    .keyboardShortcut("e", modifiers: .command)
                                    .disabled(item.payload.fileReferences != nil)

                                    Button {
                                        store.selectedItemID = item.id
                                        store.beginRenaming(item)
                                    } label: {
                                        Label("重命名", systemImage: "character.cursor.ibeam")
                                    }
                                    .keyboardShortcut("r", modifiers: .command)

                                    Button {
                                        store.selectedItemID = item.id
                                        store.togglePreviewForSelection()
                                    } label: {
                                        Label("预览", systemImage: "eye")
                                    }
                                    .keyboardShortcut(.space, modifiers: [])

                                    Divider()

                                    Button(role: .destructive) {
                                        store.selectedItemID = item.id
                                        store.deleteSelection()
                                    } label: {
                                        Label("删除", systemImage: "trash")
                                    }
                                    .keyboardShortcut(.delete, modifiers: .command)
                                }
                            }
                        }
                        .padding(.horizontal, 22)
                        .padding(.vertical, 12)
                    }
                    .onChange(of: store.selectedItemID) { _, id in
                        guard let id else { return }
                        withAnimation(.easeOut(duration: 0.18)) {
                            proxy.scrollTo(id, anchor: .center)
                        }
                    }
                    .onChange(of: store.selectionRevealToken) { _, _ in
                        guard let id = store.selectedItemID else { return }
                        withAnimation(.easeOut(duration: 0.18)) {
                            proxy.scrollTo(id, anchor: .center)
                        }
                    }
                    .onReceive(NotificationCenter.default.publisher(for: .clipDeckResizeEnded)) { _ in
                        guard let id = store.selectedItemID else { return }
                        DispatchQueue.main.async {
                            proxy.scrollTo(id, anchor: .center)
                        }
                    }
                }
            }
        }
    }

    private func paste(_ item: ClipboardItem, forcePlainText: Bool = false) {
        store.dismissPreview()
        onPaste(item, forcePlainText)
    }

    private func floatingContentBinding(for item: ClipboardItem) -> Binding<Bool> {
        Binding(
            get: {
                store.previewedItemID == item.id
                    || store.editingItemID == item.id
                    || store.renamingItemID == item.id
            },
            set: { presented in
                if !presented, store.previewedItemID == item.id {
                    store.dismissPreview()
                }
                if !presented, store.editingItemID == item.id {
                    store.cancelEditing()
                }
                if !presented, store.renamingItemID == item.id {
                    store.cancelRenaming()
                }
            }
        )
    }

    private var toolbar: some View {
        ZStack {
            HStack {
                Spacer()

                Button(action: onManageCategories) {
                    Image(systemName: "ellipsis")
                        .font(.system(size: 18, weight: .bold))
                        .foregroundStyle(.white.opacity(0.82))
                        .frame(width: 38, height: 38)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .focusEffectDisabled()
            }

            HStack(spacing: 20) {
                if store.isSearchEditing || !store.searchQuery.isEmpty {
                    HStack(spacing: 8) {
                        Image(systemName: "magnifyingglass")
                        TextField("搜索文本", text: $store.searchQuery)
                            .textFieldStyle(.plain)
                            .focused($searchFieldFocused)
                            .simultaneousGesture(TapGesture().onEnded {
                                store.setSearchEditing(true)
                            })
                            .onSubmit {
                                store.finishSearchEditing()
                            }
                        Button {
                            store.endSearch()
                        } label: {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundStyle(.white.opacity(0.48))
                        }
                        .buttonStyle(.plain)
                        .focusEffectDisabled()
                    }
                    .font(.system(size: 14, weight: .medium))
                    .padding(.horizontal, 12)
                    .frame(width: 230, height: 34)
                    .background(.white.opacity(0.14), in: Capsule())
                } else {
                    Button {
                        store.beginSearch()
                    } label: {
                        Image(systemName: "magnifyingglass")
                            .font(.system(size: 19, weight: .medium))
                            .frame(width: 28, height: 34)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .focusEffectDisabled()
                }

                categoryStrip

                Button(action: onManageCategories) {
                    Image(systemName: "plus")
                        .font(.system(size: 19, weight: .medium))
                        .frame(width: 34, height: 34)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .focusEffectDisabled()
                .padding(.leading, 5)
            }
            .foregroundStyle(.white.opacity(0.92))
            .padding(.trailing, 54)
        }
        .padding(.horizontal, 24)
        .padding(.top, 7)
        .frame(height: 58)
    }

    private var categoryStrip: some View {
        ScrollViewReader { proxy in
            HStack(spacing: 7) {
                HStack(spacing: 4) {
                    typeButton(.all, icon: .history, label: "History")
                    typeButton(.text, icon: .text, label: "文本")
                    typeButton(.images, icon: .image, label: "图片")
                    typeButton(.files, icon: .folder, label: "文件")
                }
                .fixedSize(horizontal: true, vertical: false)
                Rectangle()
                    .fill(.white.opacity(0.18))
                    .frame(width: 1, height: 22)
                    .padding(.horizontal, 7)
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 12) {
                        ForEach(Array(store.automaticCategories.enumerated()), id: \.element.id) { position, item in
                            category(
                                color: CategoryColor.color(icon: item.icon, position: position),
                                label: item.name,
                                value: .automatic(item.id)
                            )
                        }
                    }
                }
                .frame(minWidth: 240, idealWidth: 760, maxWidth: 760)

                if categoryValues.count > 6 {
                    HStack(spacing: 0) {
                        categoryPageButton(
                            systemName: "chevron.left",
                            isEnabled: categoryPageStart > 0
                        ) {
                            categoryPageStart = max(0, categoryPageStart - 5)
                            scrollCategoryPage(using: proxy)
                        }
                        categoryPageButton(
                            systemName: "chevron.right",
                            isEnabled: categoryPageStart + 6 < categoryValues.count
                        ) {
                            categoryPageStart = min(
                                max(0, categoryValues.count - 6),
                                categoryPageStart + 5
                            )
                            scrollCategoryPage(using: proxy)
                        }
                    }
                    .padding(.leading, 2)
                }
            }
            .onChange(of: store.selectedCategory) { _, category in
                revealCategory(category, using: proxy)
            }
            .onChange(of: store.automaticCategories.count) { _, _ in
                categoryPageStart = min(
                    categoryPageStart,
                    max(0, categoryValues.count - 6)
                )
                scrollCategoryPage(using: proxy)
            }
        }
    }

    private var categoryValues: [HistoryCategory] {
        store.automaticCategories.map { .automatic($0.id) }
    }

    private func typeButton(_ value: HistoryCategory, icon: DeckNavigationIcon.Kind, label: String) -> some View {
        Button {
            store.selectCategory(value)
        } label: {
            HStack(spacing: 6) {
                DeckNavigationIcon(kind: icon)
                    .frame(width: 19, height: 19)
                if value == .all || store.selectedCategory == value {
                    Text(label).font(.system(size: 13, weight: .semibold))
                }
            }
            .padding(.horizontal, 10)
            .frame(height: 35)
            .foregroundStyle(.white.opacity(store.selectedCategory == value ? 1 : 0.65))
            .background(store.selectedCategory == value ? .white.opacity(0.16) : .clear,
                        in: RoundedRectangle(cornerRadius: 9))
            .shadow(color: .black.opacity(store.selectedCategory == value ? 0.24 : 0), radius: 4, y: 1)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .focusEffectDisabled()
        .focusable(false)
        .help(label)
        .accessibilityLabel(label)
        .accessibilityAddTraits(store.selectedCategory == value ? .isSelected : [])
    }

    private func categoryPageButton(
        systemName: String,
        isEnabled: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 13, weight: .bold))
                .frame(width: 23, height: 31)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .focusEffectDisabled()
        .disabled(!isEnabled)
        .opacity(isEnabled ? 0.82 : 0.20)
    }

    private func revealCategory(
        _ category: HistoryCategory,
        using proxy: ScrollViewProxy
    ) {
        guard let index = categoryValues.firstIndex(of: category) else { return }
        if index < categoryPageStart {
            categoryPageStart = index
        } else if index >= categoryPageStart + 6 {
            categoryPageStart = max(0, index - 5)
        }
        withAnimation(.easeOut(duration: 0.18)) {
            proxy.scrollTo(category, anchor: .center)
        }
    }

    private func scrollCategoryPage(using proxy: ScrollViewProxy) {
        guard categoryValues.indices.contains(categoryPageStart) else { return }
        withAnimation(.easeOut(duration: 0.20)) {
            proxy.scrollTo(categoryValues[categoryPageStart], anchor: .leading)
        }
    }

    private func category(color: Color, label: String, value: HistoryCategory) -> some View {
        HStack(spacing: 7) {
            Circle().fill(color).frame(width: 11, height: 11)
            Text(label)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .font(.system(size: 13, weight: .semibold))
        .frame(width: 96, height: 31)
        .background(store.selectedCategory == value ? .white.opacity(0.14) : .clear, in: Capsule())
        .shadow(color: .black.opacity(store.selectedCategory == value ? 0.20 : 0), radius: 4, y: 1)
        .contentShape(Capsule())
        .onTapGesture { store.selectCategory(value) }
        .help(label)
        .id(value)
    }

    private var emptyState: some View {
        VStack(spacing: 13) {
            Image(systemName: "doc.on.clipboard")
                .font(.system(size: 30, weight: .light))
            Text(store.isSearchActive ? "没有找到匹配内容" : "Copy something to get started")
                .font(.system(size: 16, weight: .semibold, design: .rounded))
            Text(store.isSearchActive ? "尝试其他关键词" : "复制的文本、图片和文件会显示在这里")
                .font(.system(size: 12.5))
                .foregroundStyle(.secondary)
        }
        .foregroundStyle(.white.opacity(0.88))
        .padding(.horizontal, 34)
        .padding(.vertical, 28)
        .background(.white.opacity(0.10), in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }
}

private struct ClipboardCardView: View {
    let item: ClipboardItem
    let isSelected: Bool
    let width: CGFloat
    let height: CGFloat
    let isResizing: Bool
    let quickPasteNumber: Int?
    let fileFeedback: FilePasteFeedback?

    var body: some View {
        ZStack(alignment: .top) {
            if isExpanded {
                Group {
                    if usesImmersiveHeader {
                        cardContent(showFooter: true)
                        expandedHeader
                    } else {
                        VStack(spacing: 0) {
                            expandedHeader
                                .background(headerBackground)
                            cardContent(showFooter: true)
                        }
                    }
                }
                .transition(.opacity)
            } else {
                cardContent(showFooter: false)
                compactHeader
                    .transition(.opacity)
            }
        }
        .frame(width: width, height: height)
        .background(Color(red: 0.075, green: 0.075, blue: 0.082))
        .clipShape(RoundedRectangle(cornerRadius: cardCornerRadius, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: cardCornerRadius, style: .continuous)
                .stroke(
                    isSelected ? Color(red: 0.04, green: 0.56, blue: 1.0) : .white.opacity(0.09),
                    lineWidth: isSelected ? 4 : 1
                )
        }
        .overlay(alignment: .bottomTrailing) {
            if let quickPasteNumber {
                Text("\(quickPasteNumber)")
                    .font(.system(size: 14, weight: .bold, design: .rounded))
                    .foregroundStyle(.white.opacity(0.94))
                    .frame(minWidth: 25, minHeight: 25)
                    .background(.black.opacity(isSelected ? 0.66 : 0.48), in: RoundedRectangle(cornerRadius: 7))
                    .padding(9)
                    .transition(.opacity.combined(with: .scale(scale: 0.88)))
            }
        }
        .shadow(
            color: isResizing
                ? .clear
                : (isSelected ? Color.blue.opacity(0.24) : .black.opacity(0.28)),
            radius: isResizing ? 0 : 9,
            y: isResizing ? 0 : 4
        )
        .contentShape(RoundedRectangle(cornerRadius: cardCornerRadius, style: .continuous))
        .animation(.easeOut(duration: 0.14), value: isSelected)
        .animation(.easeOut(duration: 0.10), value: quickPasteNumber)
    }

    private var expandedHeader: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 7) {
                    Text(cardHeaderTitle)
                        .font(.system(size: 14, weight: .bold, design: .rounded))
                        .lineLimit(1)
                    if let categoryLabel {
                        Text(categoryLabel)
                            .font(.system(size: 12, weight: .semibold, design: .rounded))
                            .opacity(0.72)
                            .lineLimit(1)
                    }
                }
                Text(relativeTimeText)
                    .font(.system(size: 11.5, weight: .semibold, design: .rounded))
                    .opacity(0.78)
            }
            Spacer(minLength: 8)
            if item.payload.fileReferences != nil {
                DeckNavigationIcon(kind: .folder)
                    .frame(width: 30, height: 30)
            } else {
                sourceBadge(size: sourceIconSize)
            }
        }
        .foregroundStyle(.white)
        .padding(.leading, 14)
        .padding(.trailing, 10)
        .frame(height: headerHeight)
        .shadow(color: usesImmersiveHeader ? .black.opacity(0.42) : .clear, radius: 3, y: 1)
    }

    private var compactHeader: some View {
        HStack(spacing: 5) {
            Text(cardHeaderTitle)
                .fontWeight(.bold)
                .lineLimit(1)
            if let categoryLabel {
                Text(categoryLabel)
                    .foregroundStyle(.white.opacity(0.70))
                    .lineLimit(1)
            }
            Text(relativeTimeText)
                .foregroundStyle(.white.opacity(0.62))
            Spacer(minLength: 6)
            sourceBadge(size: 18)
        }
        .font(.system(size: 11.5, weight: .semibold, design: .rounded))
        .foregroundStyle(.white.opacity(0.90))
        .padding(.horizontal, 11)
        .padding(.top, 10)
        .shadow(color: usesImmersiveHeader ? .black.opacity(0.55) : .clear, radius: 3, y: 1)
    }

    @ViewBuilder
    private func cardContent(showFooter: Bool) -> some View {
        switch item.payload {
        case let .text(plainText, _):
            if item.category?.previewLanguage?.lowercased() == "color",
               let color = ClipboardColorParser.color(from: plainText) {
                colorContent(color, value: plainText, showFooter: showFooter)
            } else if item.webURL != nil {
                linkContent(showFooter: showFooter)
            } else {
                ZStack(alignment: .bottom) {
                    Text(SyntaxHighlighter.highlighted(
                        String(plainText.prefix(3_000)),
                        language: item.category?.previewLanguage,
                        fontSize: 13.5
                    ))
                        .foregroundStyle(.white.opacity(0.90))
                        .lineSpacing(2)
                        .padding(.horizontal, 14)
                        .padding(.top, showFooter ? 13 : 37)
                        .padding(.bottom, showFooter ? 34 : 12)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)

                    if showFooter {
                        Text("\(plainText.count) 个字符")
                            .font(.system(size: 11, weight: .medium, design: .rounded))
                            .foregroundStyle(.white.opacity(0.52))
                            .padding(.bottom, 10)
                    }
                }
            }

        case let .image(data, width, height, _):
            ZStack(alignment: .bottom) {
                ImageCheckerboard()
                if let image = ClipboardImageCache.image(key: item.id.uuidString, data: data) {
                    RotatedImageView(image: image, angle: item.imageAngle)
                        .padding(8)
                }
                if showFooter {
                    Text(item.imageAngle.isMultiple(of: 180)
                        ? "\(width) × \(height)"
                        : "\(height) × \(width)")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.white.opacity(0.82))
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5)
                        .background(.black.opacity(0.58), in: Capsule())
                        .padding(.bottom, 9)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        case let .files(files):
            FileReferenceList(files: files, compact: true, feedback: fileFeedback)
                .padding(.top, showFooter ? 0 : 30)
                .padding(.bottom, fileFeedback == nil ? 0 : 92)
        }
    }

    private func colorContent(_ color: NSColor, value: String, showFooter: Bool) -> some View {
        ZStack(alignment: .bottom) {
            Color(nsColor: color)
            if showFooter {
                Text(value.trimmingCharacters(in: .whitespacesAndNewlines))
                    .font(.system(size: 13, weight: .bold, design: .monospaced))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(.black.opacity(0.48), in: Capsule())
                    .shadow(color: .black.opacity(0.35), radius: 3, y: 1)
                    .padding(.bottom, 12)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    @ViewBuilder
    private func linkContent(showFooter: Bool) -> some View {
        if showFooter {
            VStack(spacing: 0) {
                GeometryReader { geometry in
                    if let data = item.linkPreviewData,
                       let image = ClipboardImageCache.image(key: "preview:\(item.id)", data: data) {
                        Image(nsImage: image)
                            .resizable()
                            .scaledToFill()
                            .frame(width: geometry.size.width, height: geometry.size.height)
                            .clipped()
                    } else {
                        linkIcon(size: min(58, max(42, height * 0.25)))
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }

                linkFooter
                    .frame(height: linkFooterHeight)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            linkIcon(size: 36)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding(.top, 18)
        }
    }

    private var linkFooter: some View {
        HStack(alignment: .center, spacing: 10) {
            linkIcon(size: 32)
            VStack(alignment: .leading, spacing: 2) {
                Text(linkDisplayTitle)
                    .font(.system(size: 13.5, weight: .semibold, design: .rounded))
                    .foregroundStyle(.white.opacity(0.92))
                    .lineLimit(2)
                if let host = item.linkHost {
                    Text(host)
                        .font(.system(size: 11.5, weight: .medium, design: .rounded))
                        .foregroundStyle(.white.opacity(0.50))
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
    }

    private var cardHeaderTitle: String {
        if let files = item.payload.fileReferences { return "文件 · \(files.count) 项" }
        return item.webURL == nil ? item.title : "链接"
    }

    private var linkFooterHeight: CGFloat {
        min(88, max(72, height * 0.23))
    }

    private func linkIcon(size: CGFloat) -> some View {
        Group {
            if let data = item.linkIconData,
               let image = ClipboardImageCache.image(key: "link:\(item.id)", data: data) {
                Image(nsImage: image)
                    .resizable()
                    .scaledToFit()
                    .padding(size * 0.13)
            } else {
                Image(systemName: "safari")
                    .font(.system(size: size * 0.55, weight: .regular))
                    .foregroundStyle(.white.opacity(0.42))
            }
        }
        .frame(width: size, height: size)
        .background(.white.opacity(0.94), in: RoundedRectangle(cornerRadius: size * 0.23, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: size * 0.23, style: .continuous)
                .stroke(.white.opacity(0.18), lineWidth: 1)
        }
        .shadow(color: .black.opacity(0.24), radius: 5, y: 2)
    }

    private var linkDisplayTitle: String {
        if item.title != "文本", !item.title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return item.title
        }
        return item.linkHost ?? item.webURL?.absoluteString ?? "链接"
    }

    private func sourceBadge(size: CGFloat) -> some View {
        Group {
            if let data = item.sourceApplication.iconData,
               let image = ClipboardImageCache.image(
                   key: "app:\(item.sourceApplication.bundleIdentifier ?? item.sourceApplication.name)",
                   data: data
               ) {
                Image(nsImage: image)
                    .resizable()
                    .frame(width: size, height: size)
                    .clipShape(RoundedRectangle(cornerRadius: max(4, size * 0.20), style: .continuous))
            } else {
                Image(systemName: "app")
                    .font(.system(size: size * 0.66, weight: .medium))
                    .foregroundStyle(.white.opacity(0.9))
            }
        }
        .frame(width: size, height: size)
        .shadow(color: .black.opacity(0.35), radius: 2, y: 1)
    }

    private var categoryLabel: String? {
        guard let category = item.category else { return nil }
        guard category.previewLanguage?.lowercased() == "code",
              case let .text(text, _) = item.payload else { return category.name }
        let detected = CodeHighlightService.shared.highlight(text, language: "code").language
        guard let detected else { return category.name }
        return CodeHighlightService.displayName(for: detected)
    }

    private var usesImmersiveHeader: Bool {
        if case .image = item.payload { return true }
        return item.category?.previewLanguage?.lowercased() == "color"
    }

    private var headerBackground: LinearGradient {
        switch item.payload {
        case .text:
            LinearGradient(
                colors: isSelected
                    ? [Color(red: 0.01, green: 0.14, blue: 0.35), Color(red: 0.02, green: 0.26, blue: 0.60)]
                    : [Color(red: 0.27, green: 0.62, blue: 0.93), Color(red: 0.13, green: 0.47, blue: 0.88)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
        case .files:
            LinearGradient(
                colors: [Color(red: 0.141, green: 0.420, blue: 0.725), Color(red: 0.13, green: 0.38, blue: 0.68)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
        case .image:
            LinearGradient(
                colors: [Color(red: 0.36, green: 0.42, blue: 0.50), Color(red: 0.22, green: 0.27, blue: 0.34)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
        }
    }

    private var headerHeight: CGFloat { min(58, max(48, height * 0.20)) }

    private var sourceIconSize: CGFloat { headerHeight - 8 }

    private var isExpanded: Bool { height >= 180 }

    private var cardCornerRadius: CGFloat { isExpanded ? 18 : 15 }

    private var relativeTimeText: String {
        let seconds = max(0, Date().timeIntervalSince(item.copiedAt))
        if seconds < 60 {
            return "现在"
        }
        if seconds < 3_600 {
            return "\(Int(seconds / 60)) 分钟"
        }
        if seconds < 86_400 {
            return "\(Int(seconds / 3_600)) 小时"
        }
        if seconds < 604_800 {
            return "\(Int(seconds / 86_400)) 天"
        }
        return item.copiedAt.formatted(.dateTime.month().day())
    }
}

private struct DeckMaterialView: NSViewRepresentable {
    func makeNSView(context: Context) -> NSVisualEffectView {
        let view = NSVisualEffectView()
        view.material = .underWindowBackground
        view.blendingMode = .behindWindow
        view.state = .active
        view.alphaValue = 0.62
        return view
    }

    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {}
}

private struct ImageCheckerboard: View {
    private let cellSize: CGFloat = 12

    var body: some View {
        Canvas { context, size in
            let columns = Int(ceil(size.width / cellSize))
            let rows = Int(ceil(size.height / cellSize))
            for row in 0..<rows {
                for column in 0..<columns {
                    let color = (row + column).isMultiple(of: 2)
                        ? Color.white.opacity(0.055)
                        : Color.black.opacity(0.12)
                    context.fill(
                        Path(CGRect(
                            x: CGFloat(column) * cellSize,
                            y: CGFloat(row) * cellSize,
                            width: cellSize,
                            height: cellSize
                        )),
                        with: .color(color)
                    )
                }
            }
        }
        .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}

private struct RotatedImageView: View {
    let image: NSImage
    let angle: Int

    var body: some View {
        GeometryReader { geometry in
            let quarterTurn = !angle.isMultiple(of: 180)
            Image(nsImage: image)
                .resizable()
                .scaledToFit()
                .frame(
                    width: quarterTurn ? geometry.size.height : geometry.size.width,
                    height: quarterTurn ? geometry.size.width : geometry.size.height
                )
                .rotationEffect(.degrees(Double(angle)))
                .frame(width: geometry.size.width, height: geometry.size.height)
        }
    }
}

private struct DeckResizeHandle: NSViewRepresentable {
    func makeNSView(context: Context) -> ResizeTrackingView { ResizeTrackingView() }
    func updateNSView(_ nsView: ResizeTrackingView, context: Context) {}

    final class ResizeTrackingView: NSView {
        private var startMouseY: CGFloat = 0
        private var startFrame: NSRect = .zero
        private var pendingFrame: NSRect?
        private var frameUpdateScheduled = false
        private var lastFrameUpdateTime: TimeInterval = 0
        private let indicatorLayer = CALayer()
        private var resizeTrackingArea: NSTrackingArea?
        private var isDragging = false

        override init(frame frameRect: NSRect) {
            super.init(frame: frameRect)
            configureIndicator()
        }

        required init?(coder: NSCoder) {
            super.init(coder: coder)
            configureIndicator()
        }

        private func configureIndicator() {
            identifier = NSUserInterfaceItemIdentifier("deck-resize-handle")
            wantsLayer = true
            indicatorLayer.backgroundColor = NSColor.white.withAlphaComponent(0.32).cgColor
            indicatorLayer.cornerRadius = 1.5
            indicatorLayer.isHidden = true
            layer?.addSublayer(indicatorLayer)
        }

        override func layout() {
            super.layout()
            indicatorLayer.frame = CGRect(
                x: floor((bounds.width - 44) / 2),
                y: bounds.height - 8,
                width: 44,
                height: 3
            )
            window?.invalidateCursorRects(for: self)
        }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window?.acceptsMouseMovedEvents = true
            window?.invalidateCursorRects(for: self)
            updateTrackingAreas()
        }

        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            if let resizeTrackingArea { removeTrackingArea(resizeTrackingArea) }
            let area = NSTrackingArea(
                rect: .zero,
                options: [.activeAlways, .inVisibleRect, .mouseEnteredAndExited,
                          .mouseMoved, .cursorUpdate, .enabledDuringMouseDrag],
                owner: self,
                userInfo: nil
            )
            addTrackingArea(area)
            resizeTrackingArea = area
        }

        override func resetCursorRects() {
            super.resetCursorRects()
            addCursorRect(visibleRect, cursor: .resizeUpDown)
        }

        override func cursorUpdate(with event: NSEvent) {
            NSCursor.resizeUpDown.set()
        }

        override func mouseEntered(with event: NSEvent) {
            NSCursor.resizeUpDown.set()
        }

        override func mouseMoved(with event: NSEvent) {
            NSCursor.resizeUpDown.set()
        }

        override func mouseExited(with event: NSEvent) {
            if !isDragging { NSCursor.arrow.set() }
        }

        override func mouseDown(with event: NSEvent) {
            guard let window else { return }
            isDragging = true
            NSCursor.resizeUpDown.set()
            startMouseY = NSEvent.mouseLocation.y
            startFrame = window.frame
            pendingFrame = nil
            lastFrameUpdateTime = 0
            indicatorLayer.isHidden = false
            window.hasShadow = false
            NotificationCenter.default.post(name: .clipDeckResizeStarted, object: nil)
        }

        override func mouseDragged(with event: NSEvent) {
            NSCursor.resizeUpDown.set()
            guard let window, let screen = window.screen ?? NSScreen.main else { return }
            let delta = NSEvent.mouseLocation.y - startMouseY
            let maximum = max(250, min(420, floor(screen.visibleFrame.height * 0.42)))
            let minimum = min(250, maximum)
            let newHeight = min(max(startFrame.height + delta, minimum), maximum)
            pendingFrame = NSRect(
                x: startFrame.minX,
                y: startFrame.minY,
                width: startFrame.width,
                height: newHeight
            )
            scheduleFrameUpdate(for: window, screen: screen)
        }

        override func mouseUp(with event: NSEvent) {
            isDragging = false
            if let window {
                applyPendingFrame(to: window, display: true)
                window.hasShadow = true
                window.invalidateShadow()
                NSLog("ClipDeck panel resized to %.0f pt", window.frame.height)
                UserDefaults.standard.set(Double(window.frame.height), forKey: "deck.preferredHeight")
            }
            indicatorLayer.isHidden = true
            NotificationCenter.default.post(name: .clipDeckResizeEnded, object: nil)
            let mouse = convert(event.locationInWindow, from: nil)
            if visibleRect.contains(mouse) {
                NSCursor.resizeUpDown.set()
            } else {
                NSCursor.arrow.set()
            }
        }

        private func scheduleFrameUpdate(for window: NSWindow, screen: NSScreen) {
            guard !frameUpdateScheduled else { return }
            frameUpdateScheduled = true
            let framesPerSecond = max(60, screen.maximumFramesPerSecond)
            let frameInterval = 1.0 / Double(framesPerSecond)
            let elapsed = ProcessInfo.processInfo.systemUptime - lastFrameUpdateTime
            let delay = max(0, frameInterval - elapsed)
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self, weak window] in
                guard let self else { return }
                self.frameUpdateScheduled = false
                guard let window else { return }
                self.applyPendingFrame(to: window, display: false)
            }
        }

        private func applyPendingFrame(to window: NSWindow, display: Bool) {
            guard let frame = pendingFrame else { return }
            pendingFrame = nil
            lastFrameUpdateTime = ProcessInfo.processInfo.systemUptime
            window.setFrame(frame, display: display)
        }
    }
}

private extension Notification.Name {
    static let clipDeckResizeStarted = Notification.Name("ClipDeck.resizeStarted")
    static let clipDeckResizeEnded = Notification.Name("ClipDeck.resizeEnded")
}

@MainActor
enum SyntaxHighlighter {
    private struct CacheKey: Hashable {
        let text: String
        let language: String?
        let fontSizeTenths: Int
    }

    private static var cache: [CacheKey: AttributedString] = [:]

    static func highlighted(_ text: String, language: String?, fontSize: CGFloat) -> AttributedString {
        let normalizedLanguage = language?.lowercased()
        let shouldCache = text.count <= 5_000
        let cacheKey = CacheKey(
            text: text,
            language: normalizedLanguage,
            fontSizeTenths: Int((fontSize * 10).rounded())
        )
        if shouldCache, let cached = cache[cacheKey] {
            return cached
        }

        let highlighted = AttributedString(makeHighlighted(text, language: normalizedLanguage, fontSize: fontSize))
        if shouldCache {
            if cache.count >= 256 {
                cache.removeAll(keepingCapacity: true)
            }
            cache[cacheKey] = highlighted
        }
        return highlighted
    }

    static func attributedText(_ text: String, language: String?, fontSize: CGFloat) -> NSAttributedString {
        makeHighlighted(text, language: language?.lowercased(), fontSize: fontSize)
    }

    private static func makeHighlighted(
        _ text: String,
        language normalizedLanguage: String?,
        fontSize: CGFloat
    ) -> NSAttributedString {
        let isCode = normalizedLanguage != nil && normalizedLanguage != "text"
        let font = isCode
            ? NSFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
            : NSFont.systemFont(ofSize: fontSize)
        let result = NSMutableAttributedString(
            string: text,
            attributes: [
                .font: font,
                .foregroundColor: NSColor.labelColor,
            ]
        )

        guard isCode else { return result }

        if normalizedLanguage == "code" {
            let highlighted = CodeHighlightService.shared.highlight(text, language: "code")
            guard highlighted.text == text else { return result }
            for span in highlighted.spans where NSMaxRange(span.range) <= result.length {
                result.addAttribute(
                    .foregroundColor,
                    value: tokenColor(for: span.scope),
                    range: span.range
                )
            }
            return result
        }

        let keywords = keywordPattern(for: normalizedLanguage)
        apply(keywords, color: .systemPurple, to: result)
        apply("\\b(?:true|false|null|undefined|nil|NULL)\\b", color: .systemPurple, to: result)
        apply("\\b\\d+(?:\\.\\d+)?\\b", color: .systemTeal, to: result)
        apply("(['\"])(?:\\\\.|(?!\\1).)*\\1", color: .systemOrange, to: result)
        apply("//.*|/\\*[\\s\\S]*?\\*/|--.*|#(?![# ]).*", color: .secondaryLabelColor, to: result)

        if normalizedLanguage == "json" {
            apply("\"(?:\\\\.|[^\"])*\"(?=\\s*:)", color: .systemBlue, to: result)
        } else if normalizedLanguage == "markdown" {
            apply("(?m)^#{1,6}\\s+.*$", color: .systemBlue, to: result)
            apply("`[^`]+`|\\*\\*[^*]+\\*\\*", color: .systemOrange, to: result)
        }
        return result
    }

    private static func keywordPattern(for language: String?) -> String {
        let words: [String]
        switch language {
        case "sql":
            words = ["select", "from", "where", "join", "insert", "update", "delete", "create", "alter", "drop", "group", "order", "by", "limit", "as", "and", "or", "not", "into", "values", "set"]
        case "shell":
            words = ["if", "then", "else", "fi", "for", "while", "do", "done", "case", "esac", "function", "export", "local", "return"]
        case "javascript", "js", "json":
            words = ["function", "return", "const", "let", "var", "if", "else", "for", "while", "class", "new", "try", "catch", "throw", "import", "export", "async", "await"]
        case "java":
            words = ["class", "interface", "public", "private", "protected", "static", "final", "void", "return", "new", "if", "else", "for", "while", "try", "catch", "throw", "package", "import"]
        default:
            words = []
        }
        guard !words.isEmpty else { return "(?!)" }
        return "(?i)\\b(?:\(words.joined(separator: "|")))\\b"
    }

    private static func apply(_ pattern: String, color: NSColor, to text: NSMutableAttributedString) {
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return }
        let range = NSRange(location: 0, length: text.length)
        for match in regex.matches(in: text.string, range: range) {
            text.addAttribute(.foregroundColor, value: color, range: match.range)
        }
    }

    private static func tokenColor(for scope: String) -> NSColor {
        switch scope {
        case "keyword", "selector-tag", "meta-keyword": .systemPurple
        case "string", "regexp", "template-tag", "template-variable": .systemOrange
        case "comment", "quote": .secondaryLabelColor
        case "number", "literal", "symbol", "bullet": .systemTeal
        case "title", "section", "name", "selector-id": .systemBlue
        case "type", "built_in", "class", "attribute": .systemMint
        case "variable", "params", "attr", "property": .systemCyan
        case "meta", "doctag": .systemPink
        default: .labelColor
        }
    }
}

@MainActor
private enum ClipboardImageCache {
    private static var images: [String: NSImage] = [:]

    static func image(key: String, data: Data) -> NSImage? {
        if let cached = images[key] {
            return cached
        }
        guard let image = NSImage(data: data) else { return nil }
        if images.count >= 96 {
            images.removeAll(keepingCapacity: true)
        }
        images[key] = image
        return image
    }
}

private struct RenameItemView: View {
    let onCancel: () -> Void
    let onSave: (String) -> Void
    @State private var title: String
    @FocusState private var titleFocused: Bool

    init(
        initialTitle: String,
        onCancel: @escaping () -> Void,
        onSave: @escaping (String) -> Void
    ) {
        self.onCancel = onCancel
        self.onSave = onSave
        _title = State(initialValue: initialTitle)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("重命名")
                .font(.system(size: 15, weight: .semibold))

            TextField("标题", text: $title)
                .textFieldStyle(.plain)
                .font(.system(size: 16))
                .padding(.horizontal, 12)
                .frame(height: 38)
                .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 9))
                .focused($titleFocused)
                .onSubmit(save)

            HStack {
                Text("空标题将恢复为默认名称")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("取消", action: onCancel)
                    .keyboardShortcut(.cancelAction)
                Button("保存", action: save)
                    .keyboardShortcut("s", modifiers: .command)
            }
        }
        .padding(18)
        .frame(width: 380)
        .onAppear { titleFocused = true }
    }

    private func save() {
        onSave(title)
    }
}

private struct ContentEditorView: View {
    let item: ClipboardItem
    let onCancel: () -> Void
    let onSave: (ClipboardPayload, Int?) -> Void
    @State private var text: String
    @State private var imageAngle: Int
    @FocusState private var textEditorFocused: Bool

    init(
        item: ClipboardItem,
        onCancel: @escaping () -> Void,
        onSave: @escaping (ClipboardPayload, Int?) -> Void
    ) {
        self.item = item
        self.onCancel = onCancel
        self.onSave = onSave
        if case let .text(plainText, _) = item.payload {
            _text = State(initialValue: plainText)
        } else {
            _text = State(initialValue: "")
        }
        _imageAngle = State(initialValue: item.imageAngle)
    }

    var body: some View {
        VStack(spacing: 0) {
            editorToolbar

            switch item.payload {
            case .text:
                textEditor
            case .image:
                imageEditor
            case let .files(files):
                FileReferenceList(files: files, compact: false)
            }
        }
        .frame(width: 560, height: 430)
        .background(editorBackground)
        .onAppear {
            if case .text = item.payload {
                textEditorFocused = true
            }
        }
    }

    private var editorToolbar: some View {
        ZStack {
            if case .image = item.payload {
                HStack(spacing: 22) {
                    editorIconButton("rotate.left", help: "向左旋转") {
                        rotateImage(clockwise: false)
                    }
                    .keyboardShortcut("[", modifiers: .command)

                    editorIconButton("rotate.right", help: "向右旋转") {
                        rotateImage(clockwise: true)
                    }
                    .keyboardShortcut("]", modifiers: .command)
                }
            } else {
                HStack(spacing: 10) {
                    Text("编辑文本")
                    if editableLanguage == "json" {
                        Button("格式化", action: formatJSON)
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                    }
                }
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(.white.opacity(0.76))
            }

            HStack {
                Button(action: onCancel) {
                    Text("取消")
                        .frame(width: 68, height: 34)
                        .background(.white.opacity(0.07), in: Capsule())
                }
                .buttonStyle(.plain)
                .keyboardShortcut(.cancelAction)

                Spacer()

                Button(action: save) {
                    Text("保存")
                        .fontWeight(.semibold)
                        .foregroundStyle(.white)
                        .frame(width: 68, height: 34)
                        .background(Color.accentColor, in: Capsule())
                }
                .buttonStyle(.plain)
                .keyboardShortcut("s", modifiers: .command)
            }
        }
        .padding(.horizontal, 14)
        .frame(height: 62)
        .background(.white.opacity(0.035))
        .overlay(alignment: .bottom) { Divider().opacity(0.55) }
    }

    private var textEditor: some View {
        VStack(spacing: 0) {
            Group {
                if let language = editableLanguage {
                    SyntaxTextEditor(
                        text: $text,
                        language: language,
                        fontSize: 15,
                        autofocus: true
                    )
                } else {
                    TextEditor(text: $text)
                        .font(.system(size: 17))
                        .focused($textEditorFocused)
                        .scrollContentBackground(.hidden)
                }
            }
                .padding(16)
                .background(Color.black.opacity(0.56))
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .overlay {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(.white.opacity(0.09), lineWidth: 1)
                }
                .padding(.horizontal, 12)
                .padding(.top, 12)
                .padding(.bottom, 8)

            HStack(spacing: 8) {
                Text("\(text.count) 个字符")
                Text("·")
                Text("\(text.split(whereSeparator: \.isWhitespace).count) 个单词")
                Text("·")
                Text("\(max(1, text.split(separator: "\n", omittingEmptySubsequences: false).count)) 行")
                Spacer()
            }
            .font(.system(size: 12, weight: .medium))
            .foregroundStyle(.white.opacity(0.50))
            .padding(.horizontal, 18)
            .frame(height: 44)
            .background(.white.opacity(0.025))
        }
    }

    private var editableLanguage: String? {
        let language = item.category?.previewLanguage?.lowercased()
        return ["code", "json", "sql", "java", "javascript", "js"].contains(language) ? language : nil
    }

    @ViewBuilder
    private var imageEditor: some View {
        if case let .image(data, width, height, _) = item.payload,
           let image = NSImage(data: data) {
            VStack(spacing: 10) {
                ZStack {
                    ImageCheckerboard()
                    RotatedImageView(image: image, angle: imageAngle)
                        .padding(12)
                }
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .overlay {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(.white.opacity(0.10), lineWidth: 1)
                }
                .padding(.horizontal, 12)
                .padding(.top, 12)

                HStack {
                    Text(displayedDimensions(width: width, height: height))
                        .font(.system(size: 13, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.white.opacity(0.56))
                    Spacer()
                }
                .padding(.horizontal, 18)
                .padding(.bottom, 12)
            }
        } else {
            ContentUnavailableView("无法读取图片", systemImage: "photo.badge.exclamationmark")
        }
    }

    private var editorBackground: some ShapeStyle {
        LinearGradient(
            colors: [
                Color(red: 0.18, green: 0.18, blue: 0.21),
                Color(red: 0.13, green: 0.13, blue: 0.15),
            ],
            startPoint: .top,
            endPoint: .bottom
        )
    }

    private func editorIconButton(
        _ systemName: String,
        help: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 17, weight: .medium))
                .foregroundStyle(.white.opacity(0.88))
                .frame(width: 34, height: 34)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .help(help)
    }

    private func save() {
        switch item.payload {
        case .text:
            onSave(.text(plainText: text, rtfData: nil), nil)
        case .image:
            onSave(item.payload, imageAngle)
        case .files:
            break
        }
    }

    private func rotateImage(clockwise: Bool) {
        imageAngle = (imageAngle + (clockwise ? 90 : 270)) % 360
    }

    private func formatJSON() {
        guard let formatted = JSONTextFormatter.format(text) else {
            NSSound.beep()
            return
        }
        text = formatted
    }

    private func displayedDimensions(width: Int, height: Int) -> String {
        imageAngle.isMultiple(of: 180) ? "\(width) × \(height)" : "\(height) × \(width)"
    }
}

enum JSONTextFormatter {
    static func format(_ source: String) -> String? {
        guard let data = source.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              JSONSerialization.isValidJSONObject(object),
              let formatted = try? JSONSerialization.data(
                  withJSONObject: object,
                  options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
              ) else { return nil }
        return String(data: formatted, encoding: .utf8)
    }
}

@MainActor
enum ImageRotation {
    static func render(_ data: Data, angle: Int) -> (data: Data, width: Int, height: Int)? {
        let turns = ((angle % 360) + 360) % 360 / 90
        guard turns > 0 else {
            guard let image = NSBitmapImageRep(data: data) else { return nil }
            return (data, image.pixelsWide, image.pixelsHigh)
        }
        var renderedData = data
        var result: (data: Data, width: Int, height: Int)?
        for _ in 0 ..< turns {
            guard let rotated = rotate(renderedData, clockwise: true) else { return nil }
            renderedData = rotated.data
            result = rotated
        }
        return result
    }

    static func rotate(_ data: Data, clockwise: Bool) -> (data: Data, width: Int, height: Int)? {
        guard let source = NSBitmapImageRep(data: data),
              let sourceImage = source.cgImage else { return nil }
        let sourceWidth = source.pixelsWide
        let sourceHeight = source.pixelsHigh
        guard let target = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: sourceHeight,
            pixelsHigh: sourceWidth,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0
        ), let graphicsContext = NSGraphicsContext(bitmapImageRep: target) else { return nil }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = graphicsContext
        let context = graphicsContext.cgContext
        context.interpolationQuality = .high
        if clockwise {
            context.translateBy(x: CGFloat(sourceHeight), y: 0)
            context.rotate(by: .pi / 2)
        } else {
            context.translateBy(x: 0, y: CGFloat(sourceWidth))
            context.rotate(by: -.pi / 2)
        }
        context.draw(
            sourceImage,
            in: CGRect(x: 0, y: 0, width: sourceWidth, height: sourceHeight)
        )
        NSGraphicsContext.restoreGraphicsState()

        guard let output = target.representation(using: .png, properties: [:]) else { return nil }
        return (output, sourceHeight, sourceWidth)
    }
}

struct ContentPreviewView: View {
    let item: ClipboardItem

    private var previewSize: CGSize {
        switch item.payload {
        case .text, .files:
            return ContentPreviewLayout.textSize
        case let .image(_, width, height, _):
            return ContentPreviewLayout.imageSize(
                pixelWidth: width,
                pixelHeight: height,
                angle: item.imageAngle
            )
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text(categoryName)
                    .font(.system(size: item.payload.fileReferences == nil ? 18 : 15,
                                  weight: .semibold))
                Text(item.sourceApplication.name)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(.secondary)
                Spacer()
                Text(item.copiedAt.formatted(date: .abbreviated, time: .shortened))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 22)
            .frame(height: item.payload.fileReferences == nil ? 58 : 52)

            Divider()

            switch item.payload {
            case let .text(text, _):
                PreviewTextScrollView(
                    text: String(text.prefix(200_000)),
                    language: item.category?.previewLanguage
                )
            case let .image(data, width, height, _):
                VStack(spacing: 12) {
                    if let image = ClipboardImageCache.image(key: item.id.uuidString, data: data) {
                        RotatedImageView(image: image, angle: item.imageAngle)
                    }
                    Text(item.imageAngle.isMultiple(of: 180)
                        ? "\(width) × \(height)"
                        : "\(height) × \(width)")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                .padding(24)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            case let .files(files):
                FileReferenceList(files: files, compact: false)
            }
        }
        .frame(width: previewSize.width, height: previewSize.height)
        .background(item.payload.fileReferences == nil ? .clear : Color(red: 0.075, green: 0.075, blue: 0.082))
    }

    private var categoryName: String {
        if let files = item.payload.fileReferences { return "文件 · \(files.count) 项" }
        return item.category?.name ?? (item.payload.isText ? "文本" : "图片")
    }
}

// The same 24-point outline geometry as the navigation mockup: history, type,
// image and folder. Using paths keeps these icons consistent across OS versions.
struct DeckNavigationIcon: View {
    enum Kind { case history, text, image, folder }
    let kind: Kind

    var body: some View {
        GeometryReader { geometry in
            Path { path in
                switch kind {
                case .history:
                    path.addArc(center: CGPoint(x: 12, y: 12), radius: 9,
                                startAngle: .degrees(200), endAngle: .degrees(160), clockwise: false)
                    path.move(to: CGPoint(x: 3, y: 3))
                    path.addLine(to: CGPoint(x: 3, y: 8))
                    path.addLine(to: CGPoint(x: 8, y: 8))
                    path.move(to: CGPoint(x: 12, y: 7))
                    path.addLine(to: CGPoint(x: 12, y: 12))
                    path.addLine(to: CGPoint(x: 16, y: 14))
                case .text:
                    path.move(to: CGPoint(x: 4, y: 7))
                    path.addLine(to: CGPoint(x: 4, y: 4))
                    path.addLine(to: CGPoint(x: 20, y: 4))
                    path.addLine(to: CGPoint(x: 20, y: 7))
                    path.move(to: CGPoint(x: 12, y: 4)); path.addLine(to: CGPoint(x: 12, y: 20))
                    path.move(to: CGPoint(x: 8, y: 20)); path.addLine(to: CGPoint(x: 16, y: 20))
                case .image:
                    path.addRoundedRect(in: CGRect(x: 3, y: 3, width: 18, height: 18), cornerSize: CGSize(width: 2, height: 2))
                    path.addEllipse(in: CGRect(x: 7, y: 7, width: 3, height: 3))
                    path.move(to: CGPoint(x: 21, y: 15))
                    path.addLine(to: CGPoint(x: 16, y: 10))
                    path.addLine(to: CGPoint(x: 5, y: 21))
                case .folder:
                    path.move(to: CGPoint(x: 20, y: 20))
                    path.addLine(to: CGPoint(x: 4, y: 20))
                    path.addQuadCurve(to: CGPoint(x: 2, y: 18), control: CGPoint(x: 2, y: 20))
                    path.addLine(to: CGPoint(x: 2, y: 6))
                    path.addQuadCurve(to: CGPoint(x: 4, y: 4), control: CGPoint(x: 2, y: 4))
                    path.addLine(to: CGPoint(x: 9, y: 4))
                    path.addLine(to: CGPoint(x: 11, y: 7))
                    path.addLine(to: CGPoint(x: 20, y: 7))
                    path.addQuadCurve(to: CGPoint(x: 22, y: 9), control: CGPoint(x: 22, y: 7))
                    path.addLine(to: CGPoint(x: 22, y: 18))
                    path.addQuadCurve(to: CGPoint(x: 20, y: 20), control: CGPoint(x: 22, y: 20))
                    path.closeSubpath()
                }
            }
            .transform(CGAffineTransform(scaleX: geometry.size.width / 24, y: geometry.size.height / 24))
            .stroke(style: StrokeStyle(lineWidth: 1.7, lineCap: .round, lineJoin: .round))
        }
        .accessibilityHidden(true)
    }
}

private struct FileReferenceTypeIcon: View {
    let file: ClipboardFileReference
    var size: CGFloat = 28

    var body: some View {
        Group {
            if file.isDirectory {
                DeckNavigationIcon(kind: .folder)
            } else {
                GeometryReader { geometry in
                    Path { path in
                        path.move(to: CGPoint(x: 14, y: 2))
                        path.addLine(to: CGPoint(x: 6, y: 2))
                        path.addQuadCurve(to: CGPoint(x: 4, y: 4), control: CGPoint(x: 4, y: 2))
                        path.addLine(to: CGPoint(x: 4, y: 20))
                        path.addQuadCurve(to: CGPoint(x: 6, y: 22), control: CGPoint(x: 4, y: 22))
                        path.addLine(to: CGPoint(x: 18, y: 22))
                        path.addQuadCurve(to: CGPoint(x: 20, y: 20), control: CGPoint(x: 20, y: 22))
                        path.addLine(to: CGPoint(x: 20, y: 8))
                        path.closeSubpath()
                        path.move(to: CGPoint(x: 14, y: 2))
                        path.addLine(to: CGPoint(x: 14, y: 8))
                        path.addLine(to: CGPoint(x: 20, y: 8))
                        let isImage = UTType(file.typeIdentifier)?.conforms(to: .image) == true
                            || UTType(filenameExtension: file.url.pathExtension)?.conforms(to: .image) == true
                        if isImage {
                            path.addEllipse(in: CGRect(x: 7, y: 11, width: 2, height: 2))
                            path.move(to: CGPoint(x: 6, y: 20))
                            path.addLine(to: CGPoint(x: 13, y: 14))
                            path.addLine(to: CGPoint(x: 19, y: 19))
                        } else if UTType(file.typeIdentifier)?.conforms(to: .sourceCode) == true
                                    || ["py", "js", "ts", "swift", "java", "sh", "cpp", "c", "go", "rs"].contains(file.url.pathExtension.lowercased()) {
                            path.move(to: CGPoint(x: 10, y: 12))
                            path.addLine(to: CGPoint(x: 7, y: 15))
                            path.addLine(to: CGPoint(x: 10, y: 18))
                            path.move(to: CGPoint(x: 14, y: 12))
                            path.addLine(to: CGPoint(x: 17, y: 15))
                            path.addLine(to: CGPoint(x: 14, y: 18))
                        } else {
                            for y in [12.0, 16.0] {
                                path.move(to: CGPoint(x: 8, y: y))
                                path.addLine(to: CGPoint(x: 16, y: y))
                            }
                        }
                    }
                    .transform(CGAffineTransform(scaleX: geometry.size.width / 24, y: geometry.size.height / 24))
                    .stroke(style: StrokeStyle(lineWidth: 1.7, lineCap: .round, lineJoin: .round))
                }
            }
        }
        .foregroundStyle(Color(red: 0.49, green: 0.73, blue: 1))
        .frame(width: size, height: size)
    }
}

private struct FileReferenceList: View {
    let files: [ClipboardFileReference]
    let compact: Bool
    var feedback: FilePasteFeedback? = nil
    @State private var visibleFileID: String?

    private var commonDirectory: String? {
        let parents = Set(files.map { ($0.path as NSString).deletingLastPathComponent })
        return parents.count == 1 ? parents.first : nil
    }

    var body: some View {
        Group {
            if files.count == 1, let file = files.first {
                singleFile(file)
            } else if compact {
                GeometryReader { geometry in
                    let availableHeight = max(0, geometry.size.height - 24)
                    let allRowsHeight = CGFloat(files.count * 26 + max(0, files.count - 1) * 4)
                    let footerHeight: CGFloat = allRowsHeight <= availableHeight ? 0 : 22
                    let count = min(files.count, max(0, Int((max(0, availableHeight - footerHeight) + 4) / 30)))
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(Array(files.prefix(count))) { file in
                            fileRow(file)
                        }
                        Spacer(minLength: 0)
                        if files.count > count {
                            HStack {
                                Text("+\(files.count - count) 项")
                                Spacer()
                                Text("空格预览全部")
                            }
                            .font(.system(size: 10.5)).foregroundStyle(.secondary)
                        }
                    }
                    .padding(12)
                    .frame(width: geometry.size.width, height: geometry.size.height, alignment: .topLeading)
                    .clipped()
                }
            } else {
                VStack(spacing: 0) {
                    HStack(spacing: 7) {
                        Image(systemName: "folder").font(.system(size: 11))
                        Text(commonDirectory.map { ($0 as NSString).abbreviatingWithTildeInPath }
                             ?? "多个文件位置")
                            .lineLimit(1).truncationMode(.middle)
                            .help(commonDirectory ?? "完整路径可在文件名上悬停查看")
                        Spacer(minLength: 0)
                    }
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                    .padding(.horizontal, 20).padding(.vertical, 9)
                    Divider()
                    ScrollViewReader { proxy in
                        ScrollView(.vertical, showsIndicators: true) {
                            LazyVStack(alignment: .leading, spacing: 4) {
                                ForEach(Array(files.enumerated()), id: \.element.id) { index, file in
                                    HStack(alignment: .center, spacing: 10) {
                                        fileRow(file)
                                    }
                                    .padding(.horizontal, 10)
                                    .frame(height: commonDirectory == nil ? 48 : 36)
                                    .background(.white.opacity(index.isMultiple(of: 2) ? 0.035 : 0),
                                                in: RoundedRectangle(cornerRadius: 7))
                                    .id(file.id)
                                }
                            }
                            .scrollTargetLayout()
                            .padding(.horizontal, 10).padding(.vertical, 8)
                        }
                        .scrollPosition(id: $visibleFileID, anchor: .top)
                        .onReceive(NotificationCenter.default.publisher(for: .clipDeckPreviewScroll)) { notification in
                            guard let direction = notification.userInfo?["direction"] as? Int,
                                  !files.isEmpty else { return }
                            let current = files.firstIndex { $0.id == visibleFileID } ?? 0
                            let next = max(0, min(files.count - 1, current + direction))
                            visibleFileID = files[next].id
                            withAnimation(.easeOut(duration: 0.16)) {
                                proxy.scrollTo(files[next].id, anchor: .top)
                            }
                        }
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func singleFile(_ file: ClipboardFileReference) -> some View {
        GeometryReader { geometry in
            let short = geometry.size.height < 170
            let tiny = geometry.size.height < 120
            let padding: CGFloat = compact ? (short ? 12 : 20) : 28
            VStack(spacing: tiny ? 6 : (short ? 10 : 20)) {
                FileReferenceTypeIcon(file: file, size: tiny ? 24 : (short ? 34 : 56))
                Text(file.name)
                    .font(.system(size: short ? 14 : 18, weight: .semibold))
                    .multilineTextAlignment(.center).lineLimit(short ? 1 : 2)
                    .truncationMode(.middle)
                if geometry.size.height >= 100 {
                    Text((file.path as NSString).abbreviatingWithTildeInPath)
                        .font(.system(size: compact ? 12 : 13))
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center).lineLimit(short ? 1 : (compact ? 2 : 4))
                        .truncationMode(.middle)
                }
            }
            .help(file.path)
            .padding(padding)
            .frame(width: geometry.size.width, height: geometry.size.height)
            .clipped()
        }
    }

    private func fileRow(_ file: ClipboardFileReference) -> some View {
        HStack(alignment: .center, spacing: compact ? 8 : 10) {
            FileReferenceTypeIcon(file: file, size: compact ? 20 : 22)
            VStack(alignment: .leading, spacing: 4) {
                Text(file.name).font(.system(size: compact ? 12.5 : 13, weight: .medium))
                    .lineLimit(1).truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if !compact, commonDirectory == nil {
                    Text(((file.path as NSString).deletingLastPathComponent as NSString).abbreviatingWithTildeInPath)
                        .font(.system(size: 10.5)).foregroundStyle(.secondary)
                        .lineLimit(1).truncationMode(.middle).textSelection(.enabled)
                }
            }
            .help(file.path)
            if !compact {
                Text(file.isDirectory ? "文件夹" : file.url.pathExtension.uppercased())
                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                    .foregroundStyle(.tertiary)
                    .padding(.horizontal, 6).padding(.vertical, 3)
                    .background(.white.opacity(0.045), in: RoundedRectangle(cornerRadius: 4))
            }
            if let feedback, case let .partial(available, _) = feedback.phase {
                Image(systemName: available.contains(where: { $0.path == file.path })
                      ? "checkmark" : "exclamationmark.triangle")
                    .font(.system(size: 13))
                    .foregroundStyle(available.contains(where: { $0.path == file.path })
                                     ? Color.green : Color(red: 1, green: 0.69, blue: 0.54))
            }
        }
        .frame(height: compact ? 26 : nil)
    }
}

private struct FilePasteFeedbackView: View {
    let feedback: FilePasteFeedback
    let onCancel: () -> Void
    let onConfirm: () -> Void
    var isSingleFile = false

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            switch feedback.phase {
            case .checking:
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("正在检查源文件…").font(.system(size: 12))
                }
            case let .unavailable(message):
                Label(message, systemImage: "exclamationmark.circle")
                    .font(.system(size: 12, weight: .medium)).foregroundStyle(Color(red: 1, green: 0.69, blue: 0.54))
                Text("文件可能已移动或挂载已断开")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            case let .partial(available, unavailable):
                Label("\(unavailable.count) 个文件不可访问", systemImage: "exclamationmark.circle")
                    .font(.system(size: 12, weight: .medium)).foregroundStyle(Color(red: 1, green: 0.69, blue: 0.54))
                Text("是否仅粘贴其余 \(available.count) 个文件？")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                HStack(spacing: 7) {
                    Button(action: onCancel) {
                        Text("取消").padding(.horizontal, 10).padding(.vertical, 6)
                            .background(.white.opacity(0.10), in: RoundedRectangle(cornerRadius: 6))
                    }
                    Button(action: onConfirm) {
                        Text("粘贴可用文件").foregroundStyle(.white)
                            .padding(.horizontal, 10).padding(.vertical, 6)
                            .background(Color(red: 0.04, green: 0.48, blue: 1), in: RoundedRectangle(cornerRadius: 6))
                    }
                }
                .font(.system(size: 11, weight: .medium))
                .buttonStyle(.plain)
                .controlSize(.small)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(isSingleFile ? Color(red: 0.31, green: 0.18, blue: 0.14)
                    : Color(red: 0.09, green: 0.10, blue: 0.12).opacity(0.98))
        .clipShape(UnevenRoundedRectangle(
            topLeadingRadius: isSingleFile ? 0 : 10,
            bottomLeadingRadius: isSingleFile ? 15 : 10,
            bottomTrailingRadius: isSingleFile ? 15 : 10,
            topTrailingRadius: isSingleFile ? 0 : 10
        ))
    }
}

enum ContentPreviewLayout {
    static let textSize = CGSize(width: 560, height: 360)

    private static let minimumPopoverSize = CGSize(width: 360, height: 300)
    private static let maximumImageViewportSize = CGSize(width: 512, height: 270)
    private static let horizontalChrome: CGFloat = 48
    private static let verticalChrome: CGFloat = 135

    static func imageSize(pixelWidth: Int, pixelHeight: Int, angle: Int) -> CGSize {
        guard pixelWidth > 0, pixelHeight > 0 else { return textSize }

        let quarterTurn = !angle.isMultiple(of: 180)
        let width = CGFloat(quarterTurn ? pixelHeight : pixelWidth)
        let height = CGFloat(quarterTurn ? pixelWidth : pixelHeight)
        let scale = min(
            maximumImageViewportSize.width / width,
            maximumImageViewportSize.height / height
        )
        let viewport = CGSize(width: width * scale, height: height * scale)

        return CGSize(
            width: max(minimumPopoverSize.width, viewport.width + horizontalChrome),
            height: max(minimumPopoverSize.height, viewport.height + verticalChrome)
        )
    }
}

private struct PreviewTextScrollView: NSViewRepresentable {
    let text: String
    let language: String?

    func makeNSView(context: Context) -> PreviewKeyboardScrollView {
        let scrollView = PreviewKeyboardScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = false
        scrollView.autohidesScrollers = true
        scrollView.drawsBackground = true
        scrollView.backgroundColor = .textBackgroundColor

        let textView = NSTextView()
        textView.isEditable = false
        textView.isSelectable = true
        textView.isRichText = true
        textView.drawsBackground = false
        textView.textContainerInset = NSSize(width: 22, height: 22)
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.autoresizingMask = [.width]
        textView.textContainer?.widthTracksTextView = true
        textView.textContainer?.containerSize = NSSize(
            width: 0,
            height: CGFloat.greatestFiniteMagnitude
        )
        scrollView.documentView = textView
        update(textView, in: scrollView)
        return scrollView
    }

    func updateNSView(_ scrollView: PreviewKeyboardScrollView, context: Context) {
        guard let textView = scrollView.documentView as? NSTextView else { return }
        update(textView, in: scrollView)
    }

    private func update(_ textView: NSTextView, in scrollView: NSScrollView) {
        let attributed = NSAttributedString(SyntaxHighlighter.highlighted(
            text,
            language: language,
            fontSize: 14
        ))
        if !textView.attributedString().isEqual(to: attributed) {
            textView.textStorage?.setAttributedString(attributed)
        }
        let width = max(1, scrollView.contentSize.width)
        textView.setFrameSize(NSSize(width: width, height: textView.frame.height))
        if let textContainer = textView.textContainer,
           let layoutManager = textView.layoutManager {
            textContainer.containerSize = NSSize(
                width: max(1, width - textView.textContainerInset.width * 2),
                height: CGFloat.greatestFiniteMagnitude
            )
            layoutManager.ensureLayout(for: textContainer)
            let usedHeight = layoutManager.usedRect(for: textContainer).height
                + textView.textContainerInset.height * 2
            textView.setFrameSize(NSSize(
                width: width,
                height: max(scrollView.contentSize.height, usedHeight)
            ))
        }
    }
}

private final class PreviewKeyboardScrollView: NSScrollView {
    private var keyboardScrollTargetY: CGFloat?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handlePreviewScroll(_:)),
            name: .clipDeckPreviewScroll,
            object: nil
        )
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
    }

    override func layout() {
        super.layout()
        guard let textView = documentView as? NSTextView else { return }
        let width = max(1, contentSize.width)
        guard abs(textView.frame.width - width) > 0.5 else { return }
        textView.setFrameSize(NSSize(width: width, height: textView.frame.height))
        textView.textContainer?.containerSize.width = max(
            1,
            width - textView.textContainerInset.width * 2
        )
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    @objc private func handlePreviewScroll(_ notification: Notification) {
        scrollVertically(direction: notification.userInfo?["direction"] as? Int ?? 0)
    }

    override func scrollWheel(with event: NSEvent) {
        keyboardScrollTargetY = nil
        super.scrollWheel(with: event)
    }

    private func scrollVertically(direction: Int) {
        guard direction != 0, let documentView else { return }
        let current = contentView.bounds.origin
        let maximumY = max(0, documentView.frame.height - contentView.bounds.height)
        let startingY = keyboardScrollTargetY ?? current.y
        let targetY = min(max(0, startingY + CGFloat(direction) * 48), maximumY)
        keyboardScrollTargetY = targetY

        if NSWorkspace.shared.accessibilityDisplayShouldReduceMotion {
            contentView.scroll(to: NSPoint(x: current.x, y: targetY))
        } else {
            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0.14
                context.timingFunction = CAMediaTimingFunction(name: .easeOut)
                contentView.animator().setBoundsOrigin(NSPoint(x: current.x, y: targetY))
            }
        }
        reflectScrolledClipView(contentView)
    }
}

private extension ClipboardPayload {
    var isText: Bool {
        if case .text = self { return true }
        return false
    }
}
