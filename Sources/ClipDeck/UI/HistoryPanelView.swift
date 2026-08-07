import AppKit
import SwiftUI

struct HistoryPanelView: View {
    @ObservedObject var store: InMemoryHistoryStore
    let onPaste: (ClipboardItem) -> Void
    let onClose: () -> Void
    let onManageCategories: () -> Void
    let onSearch: (String) -> Void
    @FocusState private var searchFieldFocused: Bool
    @State private var categoryPageStart = 0

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            content
        }
        .background {
            ZStack {
                DeckMaterialView()
                Color.black.opacity(0.20)
                LinearGradient(
                    colors: [.white.opacity(0.06), .clear, .black.opacity(0.06)],
                    startPoint: .top,
                    endPoint: .bottom
                )
            }
        }
        .overlay(alignment: .top) {
            ZStack(alignment: .top) {
                Capsule()
                    .fill(.white.opacity(0.32))
                    .frame(width: 44, height: 3)
                    .padding(.top, 5)
                DeckResizeHandle()
            }
            .frame(height: 18)
        }
        .onExitCommand {
            if store.renamingItemID != nil {
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
                            ForEach(store.displayedItems) { item in
                                ClipboardCardView(
                                    item: item,
                                    isSelected: item.id == store.selectedItemID,
                                    width: cardSide,
                                    height: cardSide
                                )
                                .id(item.id)
                                .onTapGesture {
                                    store.finishSearchEditing()
                                    store.selectedItemID = item.id
                                }
                                .onTapGesture(count: 2) {
                                    store.selectedItemID = item.id
                                    paste(item)
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
                                            onSave: { payload in
                                                store.updateEditingItem(payload: payload)
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

                                    Divider()

                                    Button {
                                        store.selectedItemID = item.id
                                        store.beginEditing(item)
                                    } label: {
                                        Label("编辑", systemImage: "pencil")
                                    }
                                    .keyboardShortcut("e", modifiers: .command)

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

    private func paste(_ item: ClipboardItem) {
        store.dismissPreview()
        onPaste(item)
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
        }
        .padding(.horizontal, 24)
        .padding(.top, 7)
        .frame(height: 58)
    }

    private var categoryStrip: some View {
        ScrollViewReader { proxy in
            HStack(spacing: 7) {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 12) {
                        HStack(spacing: 7) {
                            Image(systemName: "clock.arrow.circlepath")
                            Text("History")
                                .lineLimit(1)
                        }
                        .font(.system(size: 14, weight: .semibold, design: .rounded))
                        .frame(width: 96, height: 35)
                        .background(
                            store.selectedCategory == .all ? .white.opacity(0.16) : .clear,
                            in: Capsule()
                        )
                        .contentShape(Capsule())
                        .onTapGesture { store.selectCategory(.all) }
                        .id(HistoryCategory.all)

                        category(
                            color: Color(red: 1.00, green: 0.22, blue: 0.28),
                            label: "Text",
                            value: .text
                        )
                        category(
                            color: Color(red: 1.00, green: 0.57, blue: 0.19),
                            label: "Images",
                            value: .images
                        )

                        ForEach(store.automaticCategories) { item in
                            category(
                                color: automaticCategoryColor(item.sortOrder),
                                label: item.name,
                                value: .automatic(item.id)
                            )
                        }
                    }
                }
                .frame(width: 636)

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
        [.all, .text, .images] + store.automaticCategories.map { .automatic($0.id) }
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
        .contentShape(Capsule())
        .onTapGesture { store.selectCategory(value) }
        .id(value)
    }

    private func automaticCategoryColor(_ order: Int) -> Color {
        let colors: [Color] = [.purple, .cyan, .green, .pink, .indigo, .mint]
        return colors[abs(order) % colors.count]
    }

    private var emptyState: some View {
        VStack(spacing: 13) {
            Image(systemName: "doc.on.clipboard")
                .font(.system(size: 30, weight: .light))
            Text(store.isSearchActive ? "没有找到匹配内容" : "Copy something to get started")
                .font(.system(size: 16, weight: .semibold, design: .rounded))
            Text(store.isSearchActive ? "尝试其他关键词" : "Text and images will appear here")
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

    var body: some View {
        ZStack(alignment: .top) {
            if isExpanded {
                VStack(spacing: 0) {
                    expandedHeader
                    cardContent(showFooter: true)
                }
                .transition(.opacity)
            } else {
                cardContent(showFooter: false)
                compactHeader
                    .transition(.opacity)
            }
        }
        .animation(.easeOut(duration: 0.12), value: isExpanded)
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
        .shadow(color: isSelected ? Color.blue.opacity(0.24) : .black.opacity(0.28), radius: 9, y: 4)
        .contentShape(RoundedRectangle(cornerRadius: cardCornerRadius, style: .continuous))
        .animation(.easeOut(duration: 0.14), value: isSelected)
    }

    private var expandedHeader: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 7) {
                    Text(item.title)
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
            sourceBadge(size: sourceIconSize)
        }
        .foregroundStyle(.white)
        .padding(.leading, 14)
        .padding(.trailing, 10)
        .frame(height: headerHeight)
        .background(headerBackground)
    }

    private var compactHeader: some View {
        HStack(spacing: 5) {
            Text(item.title)
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
    }

    @ViewBuilder
    private func cardContent(showFooter: Bool) -> some View {
        switch item.payload {
        case let .text(plainText, _):
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

        case let .image(data, width, height, _):
            ZStack(alignment: .bottom) {
                ImageCheckerboard()
                if let image = ClipboardImageCache.image(key: item.id.uuidString, data: data) {
                    Image(nsImage: image)
                        .resizable()
                        .scaledToFit()
                        .padding(8)
                }
                if showFooter {
                    Text("\(width) × \(height)")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.white.opacity(0.82))
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5)
                        .background(.black.opacity(0.58), in: Capsule())
                        .padding(.bottom, 9)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
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

    private var categoryLabel: String? { item.category?.name }

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

private struct DeckResizeHandle: NSViewRepresentable {
    func makeNSView(context: Context) -> ResizeTrackingView { ResizeTrackingView() }
    func updateNSView(_ nsView: ResizeTrackingView, context: Context) {}

    final class ResizeTrackingView: NSView {
        private var startMouseY: CGFloat = 0
        private var startFrame: NSRect = .zero

        override func resetCursorRects() {
            addCursorRect(bounds, cursor: .resizeUpDown)
        }

        override func mouseDown(with event: NSEvent) {
            guard let window else { return }
            startMouseY = NSEvent.mouseLocation.y
            startFrame = window.frame
        }

        override func mouseDragged(with event: NSEvent) {
            guard let window, let screen = window.screen ?? NSScreen.main else { return }
            let delta = NSEvent.mouseLocation.y - startMouseY
            let maximum = max(250, min(420, floor(screen.visibleFrame.height * 0.42)))
            let minimum = min(250, maximum)
            let newHeight = min(max(startFrame.height + delta, minimum), maximum)
            window.setFrame(
                NSRect(x: startFrame.minX, y: startFrame.minY, width: startFrame.width, height: newHeight),
                display: true
            )
        }

        override func mouseUp(with event: NSEvent) {
            if let window {
                NSLog("ClipDeck panel resized to %.0f pt", window.frame.height)
                UserDefaults.standard.set(Double(window.frame.height), forKey: "deck.preferredHeight")
            }
            NotificationCenter.default.post(name: .clipDeckResizeEnded, object: nil)
        }
    }
}

private extension Notification.Name {
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

        let highlighted = makeHighlighted(text, language: normalizedLanguage, fontSize: fontSize)
        if shouldCache {
            if cache.count >= 256 {
                cache.removeAll(keepingCapacity: true)
            }
            cache[cacheKey] = highlighted
        }
        return highlighted
    }

    private static func makeHighlighted(
        _ text: String,
        language normalizedLanguage: String?,
        fontSize: CGFloat
    ) -> AttributedString {
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

        guard isCode else { return AttributedString(result) }

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
        return AttributedString(result)
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
    let onSave: (ClipboardPayload) -> Void
    @State private var text: String
    @State private var imagePayload: ClipboardPayload?
    @FocusState private var textEditorFocused: Bool

    init(
        item: ClipboardItem,
        onCancel: @escaping () -> Void,
        onSave: @escaping (ClipboardPayload) -> Void
    ) {
        self.item = item
        self.onCancel = onCancel
        self.onSave = onSave
        if case let .text(plainText, _) = item.payload {
            _text = State(initialValue: plainText)
            _imagePayload = State(initialValue: nil)
        } else {
            _text = State(initialValue: "")
            _imagePayload = State(initialValue: item.payload)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            editorToolbar

            switch item.payload {
            case .text:
                textEditor
            case .image:
                imageEditor
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
                Text("编辑文本")
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
            TextEditor(text: $text)
                .font(.system(size: 17))
                .focused($textEditorFocused)
                .scrollContentBackground(.hidden)
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

    @ViewBuilder
    private var imageEditor: some View {
        if case let .image(data, width, height, _) = imagePayload,
           let image = NSImage(data: data) {
            VStack(spacing: 10) {
                ZStack {
                    ImageCheckerboard()
                    Image(nsImage: image)
                        .resizable()
                        .scaledToFit()
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
                    Text("\(width) × \(height)")
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
            onSave(.text(plainText: text, rtfData: nil))
        case .image:
            if let imagePayload { onSave(imagePayload) }
        }
    }

    private func rotateImage(clockwise: Bool) {
        guard case let .image(data, _, _, _) = imagePayload,
              let rotated = ImageRotation.rotate(data, clockwise: clockwise) else { return }
        imagePayload = .image(
            data: rotated.data,
            width: rotated.width,
            height: rotated.height,
            format: .png
        )
    }
}

@MainActor
private enum ImageRotation {
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

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text(categoryName)
                    .font(.system(size: 18, weight: .bold, design: .rounded))
                Text(item.sourceApplication.name)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(.secondary)
                Spacer()
                Text(item.copiedAt.formatted(date: .abbreviated, time: .shortened))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 22)
            .frame(height: 58)

            Divider()

            switch item.payload {
            case let .text(text, _):
                ScrollView([.horizontal, .vertical]) {
                    Text(SyntaxHighlighter.highlighted(
                        String(text.prefix(200_000)),
                        language: item.category?.previewLanguage,
                        fontSize: 14
                    ))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .topLeading)
                    .padding(22)
                }
                .background(Color(nsColor: .textBackgroundColor))
            case let .image(data, width, height, _):
                VStack(spacing: 12) {
                    if let image = ClipboardImageCache.image(key: item.id.uuidString, data: data) {
                        Image(nsImage: image)
                            .resizable()
                            .scaledToFit()
                    }
                    Text("\(width) × \(height)")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                .padding(24)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(width: 560, height: 360)
    }

    private var categoryName: String {
        item.category?.name ?? (item.payload.isText ? "文本" : "图片")
    }
}

private extension ClipboardPayload {
    var isText: Bool {
        if case .text = self { return true }
        return false
    }
}
