import AppKit
import SwiftUI

@MainActor
final class CategoryManager: ObservableObject {
    @Published var categories: [ClipboardCategory] = []
    @Published var selectedID: Int64?
    @Published var name = ""
    @Published var icon = "curlybraces"
    @Published var script = "function classify(item) {\n    return false;\n}"
    @Published var previewLanguage = "text"
    @Published var testText = ""
    @Published var testResult: String?
    @Published var errorMessage: String?

    private let repository: HistoryRepository
    private let onChange: () -> Void

    init(repository: HistoryRepository, onChange: @escaping () -> Void) {
        self.repository = repository
        self.onChange = onChange
        do {
            let values = try repository.fetchCategories()
            categories = values
            select(values.first?.id)
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func reload(select id: Int64? = nil) {
        Task {
            do {
                let values = try await Task.detached { [repository] in try repository.fetchCategories() }.value
                categories = values
                select(id ?? selectedID ?? values.first?.id)
                onChange()
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func select(_ id: Int64?) {
        selectedID = id
        guard let item = categories.first(where: { $0.id == id }) else { return }
        name = item.name
        icon = item.icon ?? ""
        script = item.script
        previewLanguage = item.previewLanguage ?? "text"
        testResult = nil
        errorMessage = item.lastErrorMessage
    }

    func add() {
        Task {
            do {
                let newItem = try await Task.detached { [repository, categories] in
                    try repository.createCategory(
                        name: "新分类",
                        icon: "curlybraces",
                        script: "function classify(item) {\n    return false;\n}",
                        previewLanguage: "text",
                        sortOrder: categories.count
                    )
                }.value
                reload(select: newItem.id)
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func save() {
        guard let selectedID, !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        Task {
            do {
                try await Task.detached { [repository, name, icon, script, previewLanguage] in
                    try repository.updateCategory(
                        id: selectedID,
                        name: name,
                        icon: icon.isEmpty ? nil : icon,
                        script: script,
                        previewLanguage: previewLanguage == "text" ? nil : previewLanguage
                    )
                }.value
                reload(select: selectedID)
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func deleteSelected() {
        guard let selectedID else { return }
        delete(id: selectedID)
    }

    func delete(id: Int64) {
        Task {
            do {
                try await Task.detached { [repository] in try repository.deleteCategory(id: id) }.value
                if selectedID == id { selectedID = nil }
                reload()
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func move(id: Int64, _ offset: Int) {
        guard let index = categories.firstIndex(where: { $0.id == id }) else { return }
        let destination = min(max(index + offset, 0), categories.count - 1)
        guard destination != index else { return }
        var reordered = categories
        reordered.swapAt(index, destination)
        categories = reordered
        Task {
            do {
                try await Task.detached { [repository, reordered] in
                    try repository.reorderCategories(reordered.map(\.id))
                }.value
                reload(select: id)
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func testScript() {
        let script = script
        let text = testText
        Task {
            let result = await Task.detached { ClassificationService.test(script: script, text: text) }.value
            testResult = result.error.map { "错误：\($0)" } ?? (result.matches ? "匹配：true" : "匹配：false")
        }
    }

    func formatScript() {
        script = JavaScriptFormatter.format(script)
    }
}

struct CategoryManagementView: View {
    private enum SettingsPane {
        case general
        case categories
    }

    @StateObject var manager: CategoryManager
    @State private var categoryPendingDeletion: ClipboardCategory?
    @State private var selectedPane: SettingsPane = .categories
    @AppStorage(PasteService.preserveFormattingKey) private var preserveFormatting = false

    var body: some View {
        HStack(spacing: 0) {
            sidebar
                .frame(width: 190)
                .background(Color(nsColor: .underPageBackgroundColor))

            Divider()

            if selectedPane == .general {
                generalSettings
            } else {
                categoryWorkspace
            }
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .frame(minWidth: 920, minHeight: 640)
        .alert(
            "删除分类？",
            isPresented: Binding(
                get: { categoryPendingDeletion != nil },
                set: { if !$0 { categoryPendingDeletion = nil } }
            ),
            presenting: categoryPendingDeletion
        ) { category in
            Button("取消", role: .cancel) {}
            Button("删除", role: .destructive) {
                manager.delete(id: category.id)
                categoryPendingDeletion = nil
            }
        } message: { category in
            Text("确定删除“\(category.name)”吗？已有历史内容会保留，但会失去这个分类。")
        }
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("设置")
                .font(.system(size: 22, weight: .bold, design: .rounded))
                .padding(.horizontal, 20)
                .padding(.top, 22)
                .padding(.bottom, 14)

            VStack(spacing: 5) {
                sidebarItem("通用", systemImage: "gearshape", pane: .general)
                sidebarItem("分类", systemImage: "square.grid.2x2", pane: .categories)
            }
            .padding(.horizontal, 12)

            Spacer()
        }
    }

    private func sidebarItem(_ title: String, systemImage: String, pane: SettingsPane) -> some View {
        Button { selectedPane = pane } label: {
            HStack(spacing: 11) {
                Image(systemName: systemImage)
                    .font(.system(size: 17, weight: .medium))
                    .frame(width: 22)
                Text(title)
                    .font(.system(size: 15, weight: .semibold))
                Spacer(minLength: 0)
            }
            .foregroundStyle(selectedPane == pane ? .primary : .secondary)
            .padding(.horizontal, 13)
            .frame(height: 42)
            .background(
                selectedPane == pane ? Color.white.opacity(0.12) : .clear,
                in: RoundedRectangle(cornerRadius: 10, style: .continuous)
            )
            .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
        .buttonStyle(.plain)
        .focusEffectDisabled()
    }

    private var generalSettings: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("通用")
                        .font(.system(size: 25, weight: .bold, design: .rounded))
                    Text("设置 ClipDeck 的默认行为")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                }

                settingsSection("粘贴") {
                    VStack(spacing: 0) {
                        settingsRow("默认粘贴格式") {
                            Picker("", selection: $preserveFormatting) {
                                Text("纯文本").tag(false)
                                Text("保留原格式").tag(true)
                            }
                            .labelsHidden()
                            .frame(width: 170)
                        }
                        Divider().padding(.leading, 16)
                        HStack {
                            Text(preserveFormatting
                                ? "存在 RTF 时同时写入富文本；没有富文本时自动使用纯文本。"
                                : "始终以纯文本粘贴，避免携带字体、颜色和段落样式。")
                                .font(.system(size: 12.5))
                                .foregroundStyle(.secondary)
                            Spacer()
                        }
                        .padding(.horizontal, 16)
                        .frame(minHeight: 56)
                    }
                }
            }
            .padding(.horizontal, 38)
            .padding(.vertical, 26)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
    }

    private var categoryWorkspace: some View {
        HStack(spacing: 0) {
            categoryList
                .frame(width: 210)
                .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            mainContent
        }
    }

    private var categoryList: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("分类")
                    .font(.system(size: 18, weight: .bold, design: .rounded))
                Spacer()
                Button(action: manager.add) {
                    Image(systemName: "plus")
                        .frame(width: 28, height: 28)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .focusEffectDisabled()
                .help("新增分类")
            }
            .padding(.horizontal, 16)
            .frame(height: 62)

            ScrollView {
                LazyVStack(spacing: 5) {
                    ForEach(manager.categories) { category in
                        categoryRow(category)
                    }
                }
                .padding(.horizontal, 10)
                .padding(.bottom, 12)
            }
        }
    }

    private func categoryRow(_ category: ClipboardCategory) -> some View {
        HStack(spacing: 4) {
            Button {
                manager.select(category.id)
            } label: {
                HStack(spacing: 9) {
                    Image(systemName: category.icon ?? "tag")
                        .font(.system(size: 15, weight: .medium))
                        .frame(width: 20)
                    Text(category.name)
                        .font(.system(size: 14, weight: .semibold))
                        .lineLimit(1)
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .focusEffectDisabled()

            Menu {
                Button("上移") { manager.move(id: category.id, -1) }
                    .disabled(manager.categories.first?.id == category.id)
                Button("下移") { manager.move(id: category.id, 1) }
                    .disabled(manager.categories.last?.id == category.id)
                Divider()
                Button("删除", role: .destructive) {
                    categoryPendingDeletion = category
                }
            } label: {
                Image(systemName: "ellipsis")
                    .font(.system(size: 13, weight: .semibold))
                    .frame(width: 26, height: 30)
                    .contentShape(Rectangle())
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .focusEffectDisabled()
        }
        .foregroundStyle(manager.selectedID == category.id ? .primary : .secondary)
        .padding(.leading, 11)
        .padding(.trailing, 5)
        .frame(height: 40)
        .background(
            manager.selectedID == category.id ? Color.white.opacity(0.12) : .clear,
            in: RoundedRectangle(cornerRadius: 9, style: .continuous)
        )
    }

    @ViewBuilder
    private var mainContent: some View {
        if manager.selectedID == nil {
            ContentUnavailableView("选择一个分类", systemImage: "tag")
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            editor
        }
    }

    private var editor: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("编辑分类")
                        .font(.system(size: 25, weight: .bold, design: .rounded))
                    Text("调整名称、内容预览方式和自动分类规则")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                }

                settingsSection("基本信息") {
                    VStack(spacing: 0) {
                        settingsRow("分类名称") {
                            TextField("分类名称", text: $manager.name)
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 270)
                        }
                        Divider().padding(.leading, 16)
                        settingsRow("预览语言") {
                            Picker("", selection: $manager.previewLanguage) {
                                ForEach(["text", "json", "sql", "javascript", "shell", "markdown", "xml", "yaml"], id: \.self) { Text($0) }
                            }
                            .labelsHidden()
                            .frame(width: 170)
                        }
                    }
                }

                settingsSection("JavaScript 分类规则") {
                    VStack(spacing: 0) {
                        HStack {
                            Text("函数需返回 true 或 false，首个匹配的分类生效")
                                .font(.system(size: 12.5))
                                .foregroundStyle(.secondary)
                            Spacer()
                            Button("格式化", action: manager.formatScript)
                                .buttonStyle(.bordered)
                        }
                        .padding(.horizontal, 16)
                        .frame(height: 48)

                        Divider().padding(.leading, 16)

                        JavaScriptEditor(text: $manager.script)
                            .frame(minHeight: 230)
                    }
                }

                settingsSection("规则测试") {
                    VStack(spacing: 0) {
                        TextEditor(text: $manager.testText)
                            .font(.system(size: 13, design: .monospaced))
                            .scrollContentBackground(.hidden)
                            .frame(height: 86)
                            .padding(10)

                        Divider().padding(.leading, 16)

                        HStack {
                            Button("测试脚本", action: manager.testScript)
                                .buttonStyle(.bordered)
                            if let result = manager.testResult {
                                Text(result)
                                    .font(.system(size: 12.5, weight: .medium))
                                    .foregroundStyle(result.hasPrefix("错误") ? .red : .secondary)
                            }
                            Spacer()
                        }
                        .padding(.horizontal, 16)
                        .frame(height: 50)
                    }
                }

                if let error = manager.errorMessage, !error.isEmpty {
                    Text("最近错误：\(error)")
                        .font(.caption)
                        .foregroundStyle(.red)
                }

                HStack {
                    Spacer()
                    Button("保存更改", action: manager.save)
                        .buttonStyle(.borderedProminent)
                        .keyboardShortcut("s", modifiers: .command)
                }
            }
            .padding(.horizontal, 38)
            .padding(.vertical, 26)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
    }

    private func settingsSection<Content: View>(
        _ title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title)
                .font(.system(size: 16, weight: .bold, design: .rounded))
            content()
                .background(
                    Color(nsColor: .controlBackgroundColor),
                    in: RoundedRectangle(cornerRadius: 14, style: .continuous)
                )
                .overlay {
                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                        .stroke(.white.opacity(0.045), lineWidth: 1)
                }
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
        }
    }

    private func settingsRow<Content: View>(
        _ title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        HStack {
            Text(title)
                .font(.system(size: 14, weight: .semibold))
            Spacer()
            content()
        }
        .padding(.horizontal, 16)
        .frame(height: 52)
    }
}

enum JavaScriptFormatter {
    private enum Mode {
        case code
        case singleQuote
        case doubleQuote
        case template
        case lineComment
        case blockComment
        case regularExpression
    }

    static func format(_ source: String) -> String {
        let characters = Array(source)
        var output = ""
        var mode = Mode.code
        var indentation = 0
        var parenthesisDepth = 0
        var escaped = false
        var regularExpressionCharacterClass = false
        var index = 0

        func nextCharacter(after position: Int) -> Character? {
            let next = position + 1
            return next < characters.count ? characters[next] : nil
        }

        func trimmedOutput() -> String {
            output.replacingOccurrences(of: "[ \\t]+$", with: "", options: .regularExpression)
        }

        func appendIndentIfNeeded() {
            if output.isEmpty || output.last == "\n" {
                output += String(repeating: "    ", count: max(0, indentation))
            }
        }

        func appendNewline() {
            output = trimmedOutput()
            if !output.isEmpty, output.last != "\n" { output.append("\n") }
        }

        func appendCollapsedSpace() {
            guard !output.isEmpty, output.last != " ", output.last != "\n" else { return }
            output.append(" ")
        }

        while index < characters.count {
            let character = characters[index]
            let next = nextCharacter(after: index)

            switch mode {
            case .singleQuote, .doubleQuote, .template:
                output.append(character)
                if escaped {
                    escaped = false
                } else if character == "\\" {
                    escaped = true
                } else if (mode == .singleQuote && character == "'")
                    || (mode == .doubleQuote && character == "\"")
                    || (mode == .template && character == "`") {
                    mode = .code
                }

            case .lineComment:
                if character == "\n" {
                    appendNewline()
                    mode = .code
                } else {
                    output.append(character)
                }

            case .blockComment:
                output.append(character)
                if character == "*", next == "/" {
                    output.append("/")
                    index += 1
                    appendNewline()
                    mode = .code
                }

            case .regularExpression:
                output.append(character)
                if escaped {
                    escaped = false
                } else if character == "\\" {
                    escaped = true
                } else if character == "[" {
                    regularExpressionCharacterClass = true
                } else if character == "]" {
                    regularExpressionCharacterClass = false
                } else if character == "/", !regularExpressionCharacterClass {
                    mode = .code
                }

            case .code:
                if character.isWhitespace {
                    appendCollapsedSpace()
                } else if character == "'" || character == "\"" || character == "`" {
                    appendIndentIfNeeded()
                    output.append(character)
                    mode = character == "'" ? .singleQuote : (character == "\"" ? .doubleQuote : .template)
                } else if character == "/", next == "/" {
                    appendIndentIfNeeded()
                    output += "//"
                    index += 1
                    mode = .lineComment
                } else if character == "/", next == "*" {
                    appendIndentIfNeeded()
                    output += "/*"
                    index += 1
                    mode = .blockComment
                } else if character == "/", startsRegularExpression(after: output) {
                    appendIndentIfNeeded()
                    output.append(character)
                    regularExpressionCharacterClass = false
                    mode = .regularExpression
                } else if character == "{" {
                    output = trimmedOutput()
                    if let last = output.last, last != " ", last != "\n", last != "(" && last != "[" {
                        output.append(" ")
                    }
                    appendIndentIfNeeded()
                    output.append("{")
                    indentation += 1
                    appendNewline()
                } else if character == "}" {
                    output = trimmedOutput()
                    if !output.isEmpty, output.last != "\n" { appendNewline() }
                    indentation = max(0, indentation - 1)
                    appendIndentIfNeeded()
                    output.append("}")
                    appendNewline()
                } else if character == ";" {
                    output = trimmedOutput()
                    appendIndentIfNeeded()
                    output.append(";")
                    if parenthesisDepth == 0 { appendNewline() }
                } else if character == "(" {
                    appendIndentIfNeeded()
                    output.append(character)
                    parenthesisDepth += 1
                } else if character == ")" {
                    output = trimmedOutput()
                    output.append(character)
                    parenthesisDepth = max(0, parenthesisDepth - 1)
                } else if character == "," {
                    output = trimmedOutput()
                    output += ", "
                } else {
                    appendIndentIfNeeded()
                    output.append(character)
                }
            }

            index += 1
        }

        return output.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private static func startsRegularExpression(after output: String) -> Bool {
        let trimmed = output.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let last = trimmed.last else { return true }
        if "=([{!?:;,".contains(last) { return true }
        return ["return", "case", "throw", "typeof", "instanceof"].contains {
            trimmed.hasSuffix($0)
        }
    }
}

struct JavaScriptEditor: NSViewRepresentable {
    @Binding var text: String

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.borderType = .noBorder
        let textView = NSTextView()
        textView.delegate = context.coordinator
        textView.isRichText = false
        textView.allowsUndo = true
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.font = .monospacedSystemFont(ofSize: 13, weight: .regular)
        textView.textContainerInset = NSSize(width: 10, height: 10)
        textView.string = text
        scrollView.documentView = textView
        context.coordinator.highlight(textView)
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        guard let textView = scrollView.documentView as? NSTextView, textView.string != text else { return }
        textView.string = text
        context.coordinator.highlight(textView)
    }

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        private var parent: JavaScriptEditor

        init(_ parent: JavaScriptEditor) { self.parent = parent }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView else { return }
            parent.text = textView.string
            highlight(textView)
        }

        func highlight(_ textView: NSTextView) {
            guard let storage = textView.textStorage else { return }
            let selection = textView.selectedRanges
            let fullRange = NSRange(location: 0, length: storage.length)
            storage.setAttributes([
                .font: NSFont.monospacedSystemFont(ofSize: 13, weight: .regular),
                .foregroundColor: NSColor.labelColor,
            ], range: fullRange)
            apply("\\b(function|return|true|false|null|undefined|const|let|var|if|else|try|catch|throw|new)\\b", color: .systemPurple, storage: storage)
            apply("(['\"])(?:\\\\.|(?!\\1).)*\\1", color: .systemOrange, storage: storage)
            apply("//.*|/\\*[\\s\\S]*?\\*/", color: .secondaryLabelColor, storage: storage)
            apply("\\b\\d+(?:\\.\\d+)?\\b", color: .systemTeal, storage: storage)
            textView.selectedRanges = selection
        }

        private func apply(_ pattern: String, color: NSColor, storage: NSTextStorage) {
            guard let regex = try? NSRegularExpression(pattern: pattern) else { return }
            let range = NSRange(location: 0, length: storage.length)
            for match in regex.matches(in: storage.string, range: range) {
                storage.addAttribute(.foregroundColor, value: color, range: match.range)
            }
        }
    }
}

@MainActor
final class CategorySettingsController: NSWindowController {
    var isVisible: Bool { window?.isVisible == true }

    init(repository: HistoryRepository, onChange: @escaping () -> Void) {
        let manager = CategoryManager(repository: repository, onChange: onChange)
        let window = NSWindow(contentViewController: NSHostingController(rootView: CategoryManagementView(manager: manager)))
        window.title = "ClipDeck 设置"
        window.styleMask = [.titled, .closable, .miniaturizable, .resizable]
        window.setContentSize(NSSize(width: 980, height: 680))
        window.center()
        super.init(window: window)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func show() {
        NSApp.activate()
        window?.makeKeyAndOrderFront(nil)
    }

    func hide() {
        window?.orderOut(nil)
    }
}
