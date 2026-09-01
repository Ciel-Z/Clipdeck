import AppKit
import Carbon
import SwiftUI
import UniformTypeIdentifiers

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
    @Published var testConsole: [ClassificationService.ConsoleEntry] = []
    @Published var errorMessage: String?
    @Published var databaseByteCount: Int64 = 0
    @Published var maintenanceStatus: String?
    @Published var isMaintaining = false

    private let repository: HistoryRepository
    private let onChange: () -> Void

    init(repository: HistoryRepository, onChange: @escaping () -> Void) {
        self.repository = repository
        self.onChange = onChange
        do {
            let values = try repository.fetchCategories()
            categories = values
            select(values.first?.id)
            refreshDatabaseSize()
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
        testConsole = []
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

    func previewMove(id: Int64, over targetID: Int64) {
        guard let index = categories.firstIndex(where: { $0.id == id }),
              let destination = categories.firstIndex(where: { $0.id == targetID }),
              index != destination else { return }
        var reordered = categories
        let category = reordered.remove(at: index)
        reordered.insert(category, at: destination)
        categories = reordered
    }

    func persistOrder(select id: Int64?) {
        let reordered = categories
        Task {
            do {
                try await Task.detached { [repository, reordered] in
                    try repository.reorderCategories(reordered.map(\.id))
                }.value
                onChange()
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func restorePresets() {
        Task {
            do {
                try await Task.detached { [repository] in
                    try repository.restoreDefaultCategories()
                }.value
                reload()
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func testScript() {
        let script = script
        let text = testText
        let previewLanguage = previewLanguage
        testResult = nil
        testConsole = []
        Task {
            let result = await Task.detached {
                ClassificationService.test(
                    script: script,
                    text: text,
                    previewLanguage: previewLanguage
                )
            }.value
            testConsole = result.console
            testResult = result.error.map { "错误：\($0)" } ?? (result.matches ? "匹配：true" : "匹配：false")
        }
    }

    func formatScript() {
        script = JavaScriptFormatter.format(script)
    }

    func refreshDatabaseSize() {
        let repository = repository
        Task {
            databaseByteCount = await Task.detached {
                repository.databaseByteCount()
            }.value
        }
    }

    func applyRetention(days: Int) {
        let repository = repository
        Task {
            do {
                let deleted = try await Task.detached {
                    try repository.deleteExpired(retentionDays: days)
                }.value
                if deleted > 0 {
                    maintenanceStatus = "已清理 \(deleted) 条过期历史"
                    onChange()
                }
                refreshDatabaseSize()
            } catch {
                maintenanceStatus = "清理失败：\(error.localizedDescription)"
            }
        }
    }

    func clearCache(retentionDays: Int) {
        guard !isMaintaining else { return }
        isMaintaining = true
        maintenanceStatus = "正在清理过期内容并回收空间…"
        let repository = repository
        Task {
            do {
                let deleted = try await Task.detached {
                    try repository.clearExpiredAndReclaim(retentionDays: retentionDays)
                }.value
                maintenanceStatus = "已删除 \(deleted) 条过期历史并回收数据库空间"
                onChange()
            } catch {
                maintenanceStatus = "清理失败：\(error.localizedDescription)"
            }
            isMaintaining = false
            refreshDatabaseSize()
        }
    }

    func clearAllHistory() {
        guard !isMaintaining else { return }
        isMaintaining = true
        maintenanceStatus = "正在删除全部历史…"
        let repository = repository
        Task {
            do {
                let deleted = try await Task.detached {
                    try repository.clearAllHistoryAndReclaim()
                }.value
                maintenanceStatus = "已删除 \(deleted) 条历史并回收数据库空间"
                onChange()
            } catch {
                maintenanceStatus = "删除失败：\(error.localizedDescription)"
            }
            isMaintaining = false
            refreshDatabaseSize()
        }
    }
}

private enum SettingsPane {
    case general
    case images
    case keyboard
    case data
    case categories
}

@MainActor
private final class SettingsNavigation: ObservableObject {
    @Published var selectedPane: SettingsPane = .general
}

private struct CategoryManagementView: View {

    private enum DataAction {
        case clearCache
        case clearAll
    }

    private struct RetentionOption {
        let days: Int
        let title: String
        let position: Double
        let isMajor: Bool
    }

    @StateObject var manager: CategoryManager
    @ObservedObject var navigation: SettingsNavigation
    @StateObject private var launchAtLogin = LaunchAtLoginController()
    @State private var categoryPendingDeletion: ClipboardCategory?
    @State private var pendingDataAction: DataAction?
    @State private var pendingRetentionDays: Int?
    @State private var draftRetentionDays: Int?
    @State private var confirmsShortcutReset = false
    @State private var confirmsPresetRestore = false
    @State private var draggedCategoryID: Int64?
    @State private var isScriptEditorExpanded = false
    @AppStorage(PasteService.preserveFormattingKey) private var preserveFormatting = false
    @AppStorage(AppPreferences.retentionDaysKey) private var retentionDays = 30
    @AppStorage(AppPreferences.recordsImagesKey) private var recordsImages = true
    @AppStorage(AppPreferences.maximumImageMegabytesKey) private var maximumImageMegabytes = 10
    @AppStorage(AppPreferences.hotKeyCodeKey) private var hotKeyCode = Int(kVK_ANSI_V)
    @AppStorage(AppPreferences.hotKeyModifiersKey) private var hotKeyModifiers = Int(cmdKey | shiftKey)
    @AppStorage(AppPreferences.nextCategoryKeyCodeKey) private var nextCategoryKeyCode = Int(kVK_RightArrow)
    @AppStorage(AppPreferences.nextCategoryModifiersKey) private var nextCategoryModifiers = Int(cmdKey)
    @AppStorage(AppPreferences.previousCategoryKeyCodeKey) private var previousCategoryKeyCode = Int(kVK_LeftArrow)
    @AppStorage(AppPreferences.previousCategoryModifiersKey) private var previousCategoryModifiers = Int(cmdKey)
    @AppStorage(AppPreferences.quickPasteModifierKey) private var quickPasteModifier = Int(cmdKey)
    @AppStorage(AppPreferences.plainTextModifierKey) private var plainTextModifier = Int(shiftKey)

    var body: some View {
        HStack(spacing: 0) {
            sidebar
                .frame(width: 190)
                .background(Color(nsColor: .underPageBackgroundColor))

            Divider()

            if navigation.selectedPane == .general {
                generalSettings
            } else if navigation.selectedPane == .images {
                imageSettings
            } else if navigation.selectedPane == .keyboard {
                keyboardSettings
            } else if navigation.selectedPane == .data {
                dataSettings
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
        .alert(
            "缩短历史保留期限？",
            isPresented: Binding(
                get: { pendingRetentionDays != nil },
                set: { if !$0 { pendingRetentionDays = nil } }
            ),
            presenting: pendingRetentionDays
        ) { days in
            Button("取消", role: .cancel) {
                pendingRetentionDays = nil
            }
            Button("删除并应用", role: .destructive) {
                retentionDays = days
                pendingRetentionDays = nil
                manager.applyRetention(days: days)
            }
        } message: { days in
            Text("将永久删除最近复制时间早于\(retentionTitle(for: days))的历史项目，然后应用新期限。此操作无法撤销。")
        }
        .confirmationDialog(
            pendingDataAction == .clearAll ? "清空全部历史？" : "清空缓存？",
            isPresented: Binding(
                get: { pendingDataAction != nil },
                set: { if !$0 { pendingDataAction = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("取消", role: .cancel) { pendingDataAction = nil }
            if pendingDataAction == .clearAll {
                Button("清空全部历史", role: .destructive) {
                    pendingDataAction = nil
                    manager.clearAllHistory()
                }
            } else {
                Button("清理过期内容并回收空间") {
                    pendingDataAction = nil
                    manager.clearCache(retentionDays: retentionDays)
                }
            }
        } message: {
            if pendingDataAction == .clearAll {
                Text("所有文本和图片历史都将永久删除，此操作无法撤销。")
            } else {
                Text("只删除超过当前保留期限的内容，并执行 WAL checkpoint 和 VACUUM；有效期内历史不受影响。")
            }
        }
        .confirmationDialog(
            "重置键盘快捷键？",
            isPresented: $confirmsShortcutReset,
            titleVisibility: .visible
        ) {
            Button("取消", role: .cancel) {}
            Button("恢复默认快捷键", role: .destructive) {
                AppPreferences.resetShortcutDefaults()
                hotKeyCode = Int(kVK_ANSI_V)
                hotKeyModifiers = Int(cmdKey | shiftKey)
                nextCategoryKeyCode = Int(kVK_RightArrow)
                nextCategoryModifiers = Int(cmdKey)
                previousCategoryKeyCode = Int(kVK_LeftArrow)
                previousCategoryModifiers = Int(cmdKey)
                quickPasteModifier = Int(cmdKey)
                plainTextModifier = Int(shiftKey)
                NotificationCenter.default.post(name: .clipDeckShortcutChanged, object: nil)
            }
        } message: {
            Text("启动 ClipDeck、类型切换、快速粘贴和纯文本模式都将恢复为默认快捷键。")
        }
        .confirmationDialog(
            "还原预制分类？",
            isPresented: $confirmsPresetRestore,
            titleVisibility: .visible
        ) {
            Button("取消", role: .cancel) {}
            Button("还原预制分类") { manager.restorePresets() }
        } message: {
            Text("缺失的预制分类会被重新创建，同名预制分类的图标、规则和预览语言会恢复默认；其他自建分类会保留。")
        }
        .sheet(isPresented: $isScriptEditorExpanded) {
            ExpandedJavaScriptEditor(manager: manager)
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
                sidebarItem("图片", systemImage: "photo", pane: .images)
                sidebarItem("键盘快捷键", systemImage: "keyboard", pane: .keyboard)
                sidebarItem("分类", systemImage: "square.grid.2x2", pane: .categories)
                sidebarItem("数据", systemImage: "externaldrive", pane: .data)
            }
            .padding(.horizontal, 12)

            Spacer()
        }
    }

    private func sidebarItem(_ title: String, systemImage: String, pane: SettingsPane) -> some View {
        Button { navigation.selectedPane = pane } label: {
            HStack(spacing: 11) {
                Image(systemName: systemImage)
                    .font(.system(size: 17, weight: .medium))
                    .frame(width: 22)
                Text(title)
                    .font(.system(size: 15, weight: .semibold))
                Spacer(minLength: 0)
            }
            .foregroundStyle(navigation.selectedPane == pane ? .primary : .secondary)
            .padding(.horizontal, 13)
            .frame(height: 42)
            .background(
                navigation.selectedPane == pane ? Color.white.opacity(0.12) : .clear,
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

                settingsSection("启动") {
                    VStack(spacing: 0) {
                        settingsRow("登录时自动启动") {
                            Toggle(
                                "",
                                isOn: Binding(
                                    get: { launchAtLogin.isOn },
                                    set: { launchAtLogin.setEnabled($0) }
                                )
                            )
                            .labelsHidden()
                            .disabled(launchAtLogin.isUpdating)
                        }

                        if let message = launchAtLogin.statusMessage {
                            Divider().padding(.leading, 16)
                            HStack(spacing: 12) {
                                Text(message)
                                    .font(.system(size: 12.5))
                                    .foregroundStyle(launchAtLogin.errorMessage == nil ? Color.secondary : Color.red)
                                Spacer()
                                if launchAtLogin.requiresApproval {
                                    Button("打开登录项设置") {
                                        launchAtLogin.openSystemSettings()
                                    }
                                    .buttonStyle(.bordered)
                                }
                            }
                            .padding(.horizontal, 16)
                            .frame(minHeight: 56)
                        }
                    }
                }

                settingsSection("保留历史") {
                    VStack(spacing: 0) {
                        VStack(spacing: 5) {
                            retentionSlider

                            GeometryReader { geometry in
                                ForEach(retentionMajorLabels, id: \.title) { marker in
                                    Text(marker.title)
                                        .font(.system(size: 13))
                                        .foregroundStyle(.secondary)
                                        .fixedSize()
                                        .position(
                                            x: 10 + max(0, geometry.size.width - 20) * marker.position,
                                            y: 8
                                        )
                                }
                            }
                            .frame(height: 18)
                        }
                        .padding(.horizontal, 16)
                        .padding(.vertical, 15)

                        Divider().padding(.leading, 16)

                        HStack {
                            Text("期限以项目最近一次复制时间为起点")
                                .font(.system(size: 12.5))
                                .foregroundStyle(.secondary)
                            Spacer()
                            Button("删除历史…", role: .destructive) {
                                pendingDataAction = .clearAll
                            }
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
        .onAppear {
            launchAtLogin.refresh()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            launchAtLogin.refresh()
        }
    }

    private var imageSettings: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("图片")
                        .font(.system(size: 25, weight: .bold, design: .rounded))
                    Text("控制图片剪贴板内容的记录方式")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                }

                settingsSection("图片历史") {
                    VStack(spacing: 0) {
                        settingsRow("记录图片") {
                            Toggle("", isOn: $recordsImages)
                                .labelsHidden()
                        }
                        Divider().padding(.leading, 16)
                        settingsRow("最大图片大小") {
                            Picker("", selection: $maximumImageMegabytes) {
                                ForEach([1, 2, 5, 10, 20], id: \.self) { size in
                                    Text("\(size) MB").tag(size)
                                }
                            }
                            .labelsHidden()
                            .frame(width: 130)
                            .disabled(!recordsImages)
                        }
                    }
                }

                Text("PNG 超过限制时会被忽略；TIFF 会先转为 PNG，再按转换后的大小判断是否保存。")
                    .font(.system(size: 12.5))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 38)
            .padding(.vertical, 26)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
    }

    private var dataSettings: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("数据")
                        .font(.system(size: 25, weight: .bold, design: .rounded))
                    Text("查看数据库占用并管理本地历史")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                }

                settingsSection("数据库") {
                    VStack(spacing: 0) {
                        settingsRow("当前占用") {
                            Text(formattedDatabaseSize)
                                .foregroundStyle(.secondary)
                        }
                        Divider().padding(.leading, 16)
                        settingsRow("过期缓存") {
                            Button("清空缓存…") {
                                pendingDataAction = .clearCache
                            }
                            .disabled(manager.isMaintaining)
                        }
                        Divider().padding(.leading, 16)
                        settingsRow("全部历史") {
                            Button("清空全部历史…", role: .destructive) {
                                pendingDataAction = .clearAll
                            }
                            .disabled(manager.isMaintaining)
                        }
                    }
                }

                if let status = manager.maintenanceStatus {
                    Text(status)
                        .font(.system(size: 12.5, weight: .medium))
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 38)
            .padding(.vertical, 26)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .onAppear { manager.refreshDatabaseSize() }
    }

    private var keyboardSettings: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("键盘快捷键")
                        .font(.system(size: 25, weight: .bold, design: .rounded))
                    Text("点击快捷键区域，然后按下新的组合键")
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                }

                settingsSection("全局") {
                    settingsRow("启动 ClipDeck") {
                        KeyboardShortcutRecorder(
                            keyCode: shortcutKeyCodeBinding,
                            modifiers: shortcutModifiersBinding
                        ) {
                            NotificationCenter.default.post(
                                name: .clipDeckShortcutChanged,
                                object: nil
                            )
                        }
                        .frame(width: 190, height: 34)
                    }
                }

                settingsSection("Deck") {
                    settingsRow("显示下一个类型") {
                        KeyboardShortcutRecorder(
                            keyCode: uint32Binding($nextCategoryKeyCode),
                            modifiers: uint32Binding($nextCategoryModifiers),
                            onChange: {}
                        )
                        .frame(width: 190, height: 34)
                    }
                    Divider().padding(.leading, 16)
                    settingsRow("显示上一个类型") {
                        KeyboardShortcutRecorder(
                            keyCode: uint32Binding($previousCategoryKeyCode),
                            modifiers: uint32Binding($previousCategoryModifiers),
                            onChange: {}
                        )
                        .frame(width: 190, height: 34)
                    }
                    Divider().padding(.leading, 16)
                    settingsRow("快速粘贴") {
                        HStack(spacing: 9) {
                            modifierPicker(selection: $quickPasteModifier)
                            Text("+ 1…9")
                                .font(.system(size: 14, weight: .semibold, design: .rounded))
                                .foregroundStyle(.secondary)
                        }
                    }
                    Divider().padding(.leading, 16)
                    settingsRow("纯文本模式") {
                        modifierPicker(selection: $plainTextModifier)
                    }
                }

                HStack {
                    Spacer()
                    Button("将快捷方式重置为默认…") {
                        confirmsShortcutReset = true
                    }
                }
            }
            .padding(.horizontal, 38)
            .padding(.vertical, 26)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
    }

    private var formattedDatabaseSize: String {
        ByteCountFormatter.string(
            fromByteCount: manager.databaseByteCount,
            countStyle: .file
        )
    }

    private var retentionOptions: [RetentionOption] {
        var options: [RetentionOption] = []
        for day in 1 ... 6 {
            let position = Double(day - 1) / 6.0 * 0.25
            options.append(RetentionOption(
                days: day,
                title: "\(day)天",
                position: position,
                isMajor: day == 1
            ))
        }
        options.append(RetentionOption(days: 7, title: "1周", position: 0.25, isMajor: true))
        for week in 2 ... 3 {
            let position = 0.25 + Double(week - 1) / 3.0 * 0.25
            options.append(RetentionOption(days: week * 7, title: "\(week)周", position: position, isMajor: false))
        }
        options.append(RetentionOption(days: 30, title: "1个月", position: 0.5, isMajor: true))
        for month in 2 ... 11 {
            let position = 0.5 + Double(month - 1) / 11.0 * 0.25
            options.append(RetentionOption(days: month * 30, title: "\(month)个月", position: position, isMajor: false))
        }
        options.append(RetentionOption(days: 365, title: "1年", position: 0.75, isMajor: true))
        options.append(RetentionOption(days: 0, title: "永久", position: 1, isMajor: true))
        return options
    }

    private var retentionMajorLabels: [(title: String, position: Double)] {
        [
            ("天", 0),
            ("周", 0.25),
            ("月", 0.5),
            ("年", 0.75),
            ("永久", 1),
        ]
    }

    private var retentionOptionIndex: Int {
        if retentionDays == 0 { return retentionOptions.count - 1 }
        return retentionOptions.indices.min {
            abs(retentionOptions[$0].days - retentionDays) < abs(retentionOptions[$1].days - retentionDays)
        } ?? 2
    }

    private var displayedRetentionDays: Int {
        draftRetentionDays ?? retentionDays
    }

    private var displayedRetentionOptionIndex: Int {
        if displayedRetentionDays == 0 { return retentionOptions.count - 1 }
        return retentionOptions.indices.min {
            abs(retentionOptions[$0].days - displayedRetentionDays)
                < abs(retentionOptions[$1].days - displayedRetentionDays)
        } ?? retentionOptionIndex
    }

    private var retentionSlider: some View {
        GeometryReader { geometry in
            let usableWidth = max(0, geometry.size.width - 20)
            let selectedPosition = retentionOptions[displayedRetentionOptionIndex].position
            let selectedX = 10 + usableWidth * selectedPosition

            Text(retentionOptions[displayedRetentionOptionIndex].title)
                .font(.system(size: 13, weight: .semibold, design: .rounded))
                .fixedSize()
                .position(x: min(max(selectedX, 28), geometry.size.width - 28), y: 9)

            Slider(
                value: retentionSelectionBinding,
                in: 0 ... 1,
                onEditingChanged: { isEditing in
                    if isEditing {
                        draftRetentionDays = displayedRetentionDays
                    } else if let days = draftRetentionDays {
                        draftRetentionDays = nil
                        requestRetentionChange(to: days)
                    }
                }
            )
            .position(x: geometry.size.width / 2, y: 34)

            ForEach(Array(retentionOptions.enumerated()), id: \.offset) { _, option in
                Circle()
                    .fill(Color.secondary.opacity(option.isMajor ? 0.65 : 0.32))
                    .frame(width: option.isMajor ? 3.5 : 2, height: option.isMajor ? 3.5 : 2)
                    .position(x: 10 + usableWidth * option.position, y: 51)
            }
        }
        .frame(height: 57)
    }

    private var retentionSelectionBinding: Binding<Double> {
        Binding(
            get: { retentionOptions[displayedRetentionOptionIndex].position },
            set: { value in
                let index = retentionOptions.indices.min {
                    abs(retentionOptions[$0].position - value) < abs(retentionOptions[$1].position - value)
                } ?? retentionOptionIndex
                draftRetentionDays = retentionOptions[index].days
            }
        )
    }

    private func requestRetentionChange(to days: Int) {
        guard days != retentionDays else { return }
        let isShorter = retentionDays == 0
            ? days != 0
            : days != 0 && days < retentionDays
        if isShorter {
            pendingRetentionDays = days
        } else {
            retentionDays = days
        }
    }

    private func retentionTitle(for days: Int) -> String {
        if let option = retentionOptions.first(where: { $0.days == days }) {
            return option.title
        }
        return "\(days)天"
    }

    private var shortcutKeyCodeBinding: Binding<UInt32> {
        Binding(
            get: { UInt32(hotKeyCode) },
            set: { hotKeyCode = Int($0) }
        )
    }

    private var shortcutModifiersBinding: Binding<UInt32> {
        Binding(
            get: { UInt32(hotKeyModifiers) },
            set: { hotKeyModifiers = Int($0) }
        )
    }

    private func uint32Binding(_ binding: Binding<Int>) -> Binding<UInt32> {
        Binding(
            get: { UInt32(binding.wrappedValue) },
            set: { binding.wrappedValue = Int($0) }
        )
    }

    private func modifierPicker(selection: Binding<Int>) -> some View {
        Picker("", selection: selection) {
            Text("⌘ Command").tag(Int(cmdKey))
            Text("⇧ Shift").tag(Int(shiftKey))
            Text("⌃ Control").tag(Int(controlKey))
            Text("⌥ Option").tag(Int(optionKey))
        }
        .labelsHidden()
        .pickerStyle(.menu)
        .frame(width: 145)
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
                Button {
                    confirmsPresetRestore = true
                } label: {
                    Image(systemName: "arrow.counterclockwise")
                        .frame(width: 28, height: 28)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .focusEffectDisabled()
                .help("还原预制分类")
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

            Image(systemName: "line.3.horizontal")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(.tertiary)
                .frame(width: 24, height: 30)
                .help("拖动调整顺序")

            Menu {
                Button("删除", role: .destructive) { categoryPendingDeletion = category }
            } label: {
                Image(systemName: "ellipsis")
                    .font(.system(size: 13, weight: .semibold))
                    .frame(width: 24, height: 30)
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
        .onDrag {
            draggedCategoryID = category.id
            return NSItemProvider(object: String(category.id) as NSString)
        }
        .onDrop(
            of: [UTType.text],
            delegate: CategoryDropDelegate(
                targetID: category.id,
                manager: manager,
                draggedID: $draggedCategoryID
            )
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
                                ForEach(["text", "code", "json", "sql", "java", "javascript", "color", "shell", "markdown", "xml", "yaml"], id: \.self) { Text($0) }
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
                            Button {
                                isScriptEditorExpanded = true
                            } label: {
                                Label("展开编辑", systemImage: "arrow.up.left.and.arrow.down.right")
                            }
                            .buttonStyle(.bordered)
                        }
                        .padding(.horizontal, 16)
                        .frame(height: 48)

                        Divider().padding(.leading, 16)

                        ScriptEditorWebView(
                            text: $manager.script,
                            onSave: manager.save,
                            onRun: manager.testScript,
                            onFormat: manager.formatScript
                        )
                        .frame(minHeight: 280)

                        Divider().padding(.leading, 16)

                        HStack(spacing: 8) {
                            Text("⌘/ 注释")
                            Text("·")
                            Text("Tab 缩进")
                            Spacer()
                            Text("\(scriptLineCount) 行")
                            Text("·")
                            Text("\(manager.script.count) 个字符")
                        }
                        .font(.system(size: 11.5, weight: .medium))
                        .foregroundStyle(.tertiary)
                        .padding(.horizontal, 16)
                        .frame(height: 36)
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
                            Spacer()
                        }
                        .padding(.horizontal, 16)
                        .frame(height: 50)

                        if manager.testResult != nil || !manager.testConsole.isEmpty {
                            Divider().padding(.leading, 16)
                            ScrollView {
                                ScriptTestOutputView(
                                    entries: manager.testConsole,
                                    outcome: manager.testResult
                                )
                                .padding(12)
                            }
                            .frame(maxHeight: 150)
                            .background(Color(nsColor: .textBackgroundColor).opacity(0.28))
                        }
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

    private var scriptLineCount: Int {
        max(1, manager.script.split(separator: "\n", omittingEmptySubsequences: false).count)
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

private struct CategoryDropDelegate: DropDelegate {
    let targetID: Int64
    let manager: CategoryManager
    @Binding var draggedID: Int64?

    func dropEntered(info: DropInfo) {
        guard let draggedID else { return }
        manager.previewMove(id: draggedID, over: targetID)
    }

    func performDrop(info: DropInfo) -> Bool {
        manager.persistOrder(select: draggedID)
        draggedID = nil
        return true
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: .move)
    }
}

private struct KeyboardShortcutRecorder: NSViewRepresentable {
    @Binding var keyCode: UInt32
    @Binding var modifiers: UInt32
    let onChange: () -> Void

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> ShortcutCaptureButton {
        let button = ShortcutCaptureButton(title: "", target: nil, action: nil)
        button.bezelStyle = .rounded
        button.font = .systemFont(ofSize: 14, weight: .semibold)
        button.target = context.coordinator
        button.action = #selector(Coordinator.beginRecording(_:))
        button.onCapture = { [weak coordinator = context.coordinator] code, flags in
            coordinator?.capture(keyCode: code, modifiers: flags)
        }
        button.onCancel = { [weak coordinator = context.coordinator] in
            coordinator?.finishWithoutChange()
        }
        button.title = ShortcutFormatter.label(keyCode: keyCode, modifiers: modifiers)
        return button
    }

    func updateNSView(_ button: ShortcutCaptureButton, context: Context) {
        context.coordinator.parent = self
        if !button.isRecording {
            button.title = ShortcutFormatter.label(keyCode: keyCode, modifiers: modifiers)
        }
    }

    @MainActor
    final class Coordinator: NSObject {
        var parent: KeyboardShortcutRecorder
        weak var button: ShortcutCaptureButton?

        init(_ parent: KeyboardShortcutRecorder) {
            self.parent = parent
        }

        @objc func beginRecording(_ sender: ShortcutCaptureButton) {
            button = sender
            sender.isRecording = true
            sender.title = "请按新的快捷键…"
            sender.window?.makeFirstResponder(sender)
        }

        func capture(keyCode: UInt32, modifiers: UInt32) {
            parent.keyCode = keyCode
            parent.modifiers = modifiers
            button?.isRecording = false
            button?.title = ShortcutFormatter.label(keyCode: keyCode, modifiers: modifiers)
            parent.onChange()
        }

        func finishWithoutChange() {
            button?.isRecording = false
            button?.title = ShortcutFormatter.label(
                keyCode: parent.keyCode,
                modifiers: parent.modifiers
            )
        }
    }
}

private final class ShortcutCaptureButton: NSButton {
    var onCapture: ((UInt32, UInt32) -> Void)?
    var onCancel: (() -> Void)?
    var isRecording = false

    override var acceptsFirstResponder: Bool { true }

    override func keyDown(with event: NSEvent) {
        guard isRecording else {
            super.keyDown(with: event)
            return
        }
        if event.keyCode == 53 {
            onCancel?()
            return
        }
        let modifiers = ShortcutFormatter.carbonModifiers(from: event.modifierFlags)
        guard modifiers != 0 || ShortcutFormatter.isFunctionKey(event.keyCode) else {
            NSSound.beep()
            return
        }
        onCapture?(UInt32(event.keyCode), modifiers)
    }
}

private enum ShortcutFormatter {
    static func carbonModifiers(from flags: NSEvent.ModifierFlags) -> UInt32 {
        var result: UInt32 = 0
        if flags.contains(.control) { result |= UInt32(controlKey) }
        if flags.contains(.option) { result |= UInt32(optionKey) }
        if flags.contains(.shift) { result |= UInt32(shiftKey) }
        if flags.contains(.command) { result |= UInt32(cmdKey) }
        return result
    }

    static func label(keyCode: UInt32, modifiers: UInt32) -> String {
        var value = ""
        if modifiers & UInt32(controlKey) != 0 { value += "⌃" }
        if modifiers & UInt32(optionKey) != 0 { value += "⌥" }
        if modifiers & UInt32(shiftKey) != 0 { value += "⇧" }
        if modifiers & UInt32(cmdKey) != 0 { value += "⌘" }
        return value + keyName(UInt16(keyCode))
    }

    static func isFunctionKey(_ keyCode: UInt16) -> Bool {
        keyName(keyCode).hasPrefix("F")
    }

    private static func keyName(_ code: UInt16) -> String {
        let names: [UInt16: String] = [
            0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X",
            8: "C", 9: "V", 11: "B", 12: "Q", 13: "W", 14: "E", 15: "R",
            16: "Y", 17: "T", 18: "1", 19: "2", 20: "3", 21: "4", 22: "6",
            23: "5", 24: "=", 25: "9", 26: "7", 27: "-", 28: "8", 29: "0",
            30: "]", 31: "O", 32: "U", 33: "[", 34: "I", 35: "P", 36: "↩",
            37: "L", 38: "J", 39: "'", 40: "K", 41: ";", 42: "\\", 43: ",",
            44: "/", 45: "N", 46: "M", 47: ".", 48: "⇥", 49: "Space",
            50: "`", 51: "⌫", 53: "Esc", 96: "F5", 97: "F6", 98: "F7",
            99: "F3", 100: "F8", 101: "F9", 103: "F11", 105: "F13",
            107: "F14", 109: "F10", 111: "F12", 113: "F15", 118: "F4",
            120: "F2", 122: "F1", 123: "←", 124: "→", 125: "↓", 126: "↑",
        ]
        return names[code] ?? "Key \(code)"
    }
}

private struct ExpandedJavaScriptEditor: View {
    @ObservedObject var manager: CategoryManager
    @Environment(\.dismiss) private var dismiss
    @State private var initialScript: String

    init(manager: CategoryManager) {
        self.manager = manager
        _initialScript = State(initialValue: manager.script)
    }

    private var lineCount: Int {
        max(1, manager.script.split(separator: "\n", omittingEmptySubsequences: false).count)
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("\(manager.name) · JavaScript 分类规则")
                        .font(.system(size: 17, weight: .semibold))
                    Text("编辑分类规则并使用右侧样本验证结果")
                        .font(.system(size: 11.5))
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button("取消") {
                    manager.script = initialScript
                    dismiss()
                }
                    .keyboardShortcut(.cancelAction)
                Button("保存并完成") {
                    manager.save()
                    dismiss()
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut("s", modifiers: .command)
            }
            .padding(.horizontal, 18)
            .frame(height: 62)
            .background(.bar)

            Divider()

            HSplitView {
                VStack(spacing: 0) {
                    HStack {
                        Label("分类脚本", systemImage: "curlybraces")
                            .font(.system(size: 12.5, weight: .semibold))
                            .foregroundStyle(.secondary)
                        Spacer()
                        Button("格式化", action: manager.formatScript)
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                            .keyboardShortcut("f", modifiers: [.command, .shift])
                    }
                    .padding(.horizontal, 12)
                    .frame(height: 42)

                    Divider()

                    ScriptEditorWebView(
                        text: $manager.script,
                        autofocus: true,
                        onSave: manager.save,
                        onRun: manager.testScript,
                        onFormat: manager.formatScript
                    )
                    .background(Color(nsColor: .textBackgroundColor).opacity(0.42))

                    Divider()

                    HStack(spacing: 8) {
                        Text("⌘/ 注释")
                        Text("·")
                        Text("Tab / ⇧Tab 缩进")
                        Text("·")
                        Text("⇧⌘F 格式化")
                        Spacer()
                        Text("\(lineCount) 行")
                        Text("·")
                        Text("\(manager.script.count) 个字符")
                    }
                    .font(.system(size: 11.5, weight: .medium))
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 14)
                    .frame(height: 36)
                }
                .frame(minWidth: 560)

                VStack(alignment: .leading, spacing: 0) {
                    HStack {
                        Text("规则测试")
                            .font(.system(size: 13, weight: .semibold))
                        Spacer()
                        Button("运行测试", action: manager.testScript)
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                            .keyboardShortcut(.return, modifiers: .command)
                    }
                        .padding(.horizontal, 14)
                        .frame(height: 42)

                    Divider()

                    TextEditor(text: $manager.testText)
                        .font(.system(size: 12.5, design: .monospaced))
                        .scrollContentBackground(.hidden)
                        .frame(height: 160)
                        .padding(8)

                    Divider()

                    HStack {
                        Text("控制台输出")
                            .font(.system(size: 12.5, weight: .semibold))
                        Spacer()
                    }
                    .padding(.horizontal, 14)
                    .frame(height: 36)

                    ScrollView {
                        ScriptTestOutputView(
                            entries: manager.testConsole,
                            outcome: manager.testResult
                        )
                        .padding(12)
                    }
                    .frame(height: 124)
                    .background(Color(nsColor: .textBackgroundColor).opacity(0.28))

                    Divider()

                    Text("item 可用字段")
                        .font(.system(size: 13, weight: .semibold))
                        .padding(.horizontal, 14)
                        .frame(height: 42)

                    Divider()

                    ScrollView {
                        VStack(alignment: .leading, spacing: 12) {
                            JavaScriptFieldRow(name: "text", detail: "复制的文本内容")
                            JavaScriptFieldRow(name: "textLength", detail: "文本字符数")
                            JavaScriptFieldRow(name: "sourceApplication.bundleId", detail: "来源应用标识")
                            JavaScriptFieldRow(name: "sourceApplication.name", detail: "来源应用名称")
                            JavaScriptFieldRow(name: "copiedAt", detail: "毫秒时间戳")
                            if manager.previewLanguage == "code" {
                                JavaScriptFieldRow(name: "detectedLanguage", detail: "识别的代码语言")
                                JavaScriptFieldRow(name: "codeScore", detail: "代码识别分数")
                                JavaScriptFieldRow(name: "codeEngineMatched", detail: "是否识别为代码")
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(14)
                    }
                }
                .frame(minWidth: 270, idealWidth: 300, maxWidth: 360)
            }
        }
        .frame(minWidth: 900, minHeight: 620)
    }
}

private struct JavaScriptFieldRow: View {
    let name: String
    let detail: String

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("item.\(name)")
                .font(.system(size: 11.5, weight: .medium, design: .monospaced))
                .textSelection(.enabled)
            Text(detail)
                .font(.system(size: 11.5))
                .foregroundStyle(.secondary)
        }
    }
}

private struct ScriptTestOutputView: View {
    let entries: [ClassificationService.ConsoleEntry]
    let outcome: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            if entries.isEmpty, outcome == nil {
                Label("等待测试", systemImage: "circle.dashed")
                    .foregroundStyle(.tertiary)
            }

            ForEach(Array(entries.enumerated()), id: \.offset) { _, entry in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(entry.level.rawValue.uppercased())
                        .font(.system(size: 9.5, weight: .bold, design: .monospaced))
                        .foregroundStyle(color(for: entry.level))
                        .frame(width: 38, alignment: .leading)
                    Text(entry.message.isEmpty ? "（空）" : entry.message)
                        .font(.system(size: 11.5, design: .monospaced))
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            if let outcome {
                Divider()
                Label(
                    outcome,
                    systemImage: outcome.hasPrefix("错误")
                        ? "xmark.circle.fill"
                        : (outcome.contains("true") ? "checkmark.circle.fill" : "minus.circle.fill")
                )
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(outcome.hasPrefix("错误") ? .red : .secondary)
                .textSelection(.enabled)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func color(for level: ClassificationService.ConsoleEntry.Level) -> Color {
        switch level {
        case .warn: .orange
        case .error: .red
        case .info: .blue
        case .debug: .purple
        case .log: .secondary
        }
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

struct SyntaxTextEditor: NSViewRepresentable {
    @Binding var text: String
    let language: String
    var fontSize: CGFloat = 13
    var autofocus = false
    var showsLineNumbers = false

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.borderType = .noBorder
        scrollView.drawsBackground = false
        let textView = SyntaxEditorTextView()
        textView.delegate = context.coordinator
        textView.isRichText = false
        textView.allowsUndo = true
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.font = .monospacedSystemFont(ofSize: fontSize, weight: .regular)
        textView.textContainerInset = NSSize(width: 10, height: 10)
        textView.drawsBackground = false
        textView.isHorizontallyResizable = true
        textView.isVerticallyResizable = true
        textView.autoresizingMask = [.width]
        textView.maxSize = NSSize(
            width: CGFloat.greatestFiniteMagnitude,
            height: CGFloat.greatestFiniteMagnitude
        )
        textView.textContainer?.widthTracksTextView = false
        textView.textContainer?.containerSize = NSSize(
            width: CGFloat.greatestFiniteMagnitude,
            height: CGFloat.greatestFiniteMagnitude
        )
        textView.string = text
        scrollView.documentView = textView
        if showsLineNumbers {
            scrollView.hasVerticalRuler = true
            scrollView.rulersVisible = true
            scrollView.verticalRulerView = LineNumberRulerView(textView: textView)
        }
        context.coordinator.highlight(textView)
        if autofocus {
            DispatchQueue.main.async { textView.window?.makeFirstResponder(textView) }
        }
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        context.coordinator.update(self)
        guard let textView = scrollView.documentView as? NSTextView, textView.string != text else { return }
        textView.string = text
        context.coordinator.highlight(textView)
        if autofocus, textView.window?.firstResponder !== textView {
            DispatchQueue.main.async { textView.window?.makeFirstResponder(textView) }
        }
    }

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        private var parent: SyntaxTextEditor
        private var isApplyingHighlight = false

        init(_ parent: SyntaxTextEditor) { self.parent = parent }

        func update(_ parent: SyntaxTextEditor) {
            self.parent = parent
        }

        func textDidChange(_ notification: Notification) {
            guard !isApplyingHighlight, let textView = notification.object as? NSTextView else { return }
            parent.text = textView.string
            highlight(textView)
        }

        func highlight(_ textView: NSTextView) {
            guard let storage = textView.textStorage else { return }
            let selection = textView.selectedRanges
            isApplyingHighlight = true
            storage.setAttributedString(SyntaxHighlighter.attributedText(
                storage.string,
                language: parent.language,
                fontSize: parent.fontSize
            ))
            textView.selectedRanges = selection
            isApplyingHighlight = false
        }
    }
}

private final class SyntaxEditorTextView: NSTextView {
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if isSelectAllShortcut(event) {
            selectAll(nil)
            return true
        }
        return super.performKeyEquivalent(with: event)
    }

    override func keyDown(with event: NSEvent) {
        if isSelectAllShortcut(event) {
            selectAll(nil)
            return
        }
        let modifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
        if modifiers == .command, event.charactersIgnoringModifiers == "/" {
            toggleLineComments()
            return
        }
        if event.keyCode == UInt16(kVK_Tab), modifiers == [] || modifiers == .shift {
            changeIndent(outdent: modifiers == .shift)
            return
        }
        super.keyDown(with: event)
    }

    override func insertNewline(_ sender: Any?) {
        let source = string as NSString
        let selection = selectedRange()
        let lineRange = source.lineRange(for: NSRange(location: selection.location, length: 0))
        let line = source.substring(with: lineRange)
        let indentation = String(line.prefix { $0 == " " || $0 == "\t" })
        let beforeCursor = source.substring(with: NSRange(
            location: lineRange.location,
            length: max(0, selection.location - lineRange.location)
        )).trimmingCharacters(in: .whitespacesAndNewlines)
        let extraIndent = beforeCursor.hasSuffix("{") ? "    " : ""
        insertText("\n\(indentation)\(extraIndent)", replacementRange: selection)
    }

    private func isSelectAllShortcut(_ event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection([.command, .shift, .option, .control])
        return event.type == .keyDown
            && modifiers == .command
            && event.charactersIgnoringModifiers?.lowercased() == "a"
    }

    private func changeIndent(outdent: Bool) {
        let source = string as NSString
        let selection = selectedRange()
        if selection.length == 0, !outdent {
            insertText("    ", replacementRange: selection)
            return
        }

        let lines = source.lineRange(for: selection)
        var replacement = source.substring(with: lines)
        if outdent {
            replacement = replacement.replacingOccurrences(
                of: #"(?m)^( {1,4}|\t)"#,
                with: "",
                options: .regularExpression
            )
        } else {
            replacement = replacement.replacingOccurrences(
                of: #"(?m)^"#,
                with: "    ",
                options: .regularExpression
            )
        }
        insertText(replacement, replacementRange: lines)
        setSelectedRange(NSRange(location: lines.location, length: replacement.utf16.count))
    }

    private func toggleLineComments() {
        let source = string as NSString
        let lines = source.lineRange(for: selectedRange())
        let original = source.substring(with: lines)
        let values = original.components(separatedBy: "\n")
        let nonEmpty = values.filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
        let shouldUncomment = !nonEmpty.isEmpty && nonEmpty.allSatisfy {
            $0.trimmingCharacters(in: .whitespaces).hasPrefix("//")
        }
        let changed = values.map { line -> String in
            guard !line.trimmingCharacters(in: .whitespaces).isEmpty else { return line }
            let indentation = String(line.prefix { $0 == " " || $0 == "\t" })
            let content = String(line.dropFirst(indentation.count))
            if shouldUncomment {
                let uncommented = content.dropFirst(2)
                return indentation + (uncommented.first == " " ? String(uncommented.dropFirst()) : String(uncommented))
            }
            return indentation + "// " + content
        }.joined(separator: "\n")
        insertText(changed, replacementRange: lines)
        setSelectedRange(NSRange(location: lines.location, length: changed.utf16.count))
    }
}

private final class LineNumberRulerView: NSRulerView {
    private weak var textView: NSTextView?

    init(textView: NSTextView) {
        self.textView = textView
        super.init(scrollView: textView.enclosingScrollView, orientation: .verticalRuler)
        clientView = textView
        ruleThickness = 42
        textView.enclosingScrollView?.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(invalidateRuler),
            name: NSText.didChangeNotification,
            object: textView
        )
        if let clipView = textView.enclosingScrollView?.contentView {
            NotificationCenter.default.addObserver(
                self,
                selector: #selector(invalidateRuler),
                name: NSView.boundsDidChangeNotification,
                object: clipView
            )
        }
    }

    required init(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    @objc private func invalidateRuler(_ notification: Notification) {
        needsDisplay = true
    }

    override func drawHashMarksAndLabels(in rect: NSRect) {
        guard let textView,
              let layoutManager = textView.layoutManager,
              let textContainer = textView.textContainer,
              let scrollView = textView.enclosingScrollView else { return }

        NSColor.textBackgroundColor.withAlphaComponent(0.28).setFill()
        rect.fill()

        let source = textView.string as NSString
        let visibleRect = scrollView.contentView.bounds
        let glyphRange = layoutManager.glyphRange(forBoundingRect: visibleRect, in: textContainer)
        let characterRange = layoutManager.characterRange(forGlyphRange: glyphRange, actualGlyphRange: nil)
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10.5, weight: .regular),
            .foregroundColor: NSColor.secondaryLabelColor,
        ]

        var lineNumber = 1
        var lineStart = 0
        while lineStart < min(characterRange.location, source.length) {
            lineStart = NSMaxRange(source.lineRange(for: NSRange(location: lineStart, length: 0)))
            lineNumber += 1
        }

        let upperBound = min(source.length, NSMaxRange(characterRange) + 1)
        repeat {
            let location = min(lineStart, source.length)
            let glyphIndex = location < source.length
                ? layoutManager.glyphIndexForCharacter(at: location)
                : layoutManager.numberOfGlyphs
            var y = textView.textContainerInset.height
            if glyphIndex < layoutManager.numberOfGlyphs {
                y += layoutManager.lineFragmentRect(forGlyphAt: glyphIndex, effectiveRange: nil).minY
            } else if layoutManager.numberOfGlyphs > 0 {
                let last = layoutManager.lineFragmentRect(
                    forGlyphAt: layoutManager.numberOfGlyphs - 1,
                    effectiveRange: nil
                )
                y += last.maxY
            }
            y -= visibleRect.minY
            let label = "\(lineNumber)" as NSString
            let size = label.size(withAttributes: attributes)
            label.draw(at: NSPoint(x: ruleThickness - size.width - 8, y: y), withAttributes: attributes)

            guard lineStart < source.length else { break }
            let next = NSMaxRange(source.lineRange(for: NSRange(location: lineStart, length: 0)))
            guard next > lineStart else { break }
            lineStart = next
            lineNumber += 1
        } while lineStart <= upperBound
    }
}

@MainActor
final class CategorySettingsController: NSWindowController {
    var isVisible: Bool { window?.isVisible == true }
    var ownsFocusedWindow: Bool {
        guard NSApp.isActive, let settingsWindow = window, var focusedWindow = NSApp.keyWindow else {
            return false
        }
        while let parent = focusedWindow.sheetParent {
            focusedWindow = parent
        }
        return focusedWindow === settingsWindow
    }
    private let navigation: SettingsNavigation
    private weak var deckReturnWindow: NSWindow?
    private weak var deckReturnResponder: NSResponder?

    init(repository: HistoryRepository, onChange: @escaping () -> Void) {
        let navigation = SettingsNavigation()
        self.navigation = navigation
        let manager = CategoryManager(repository: repository, onChange: onChange)
        let window = NSWindow(contentViewController: NSHostingController(
            rootView: CategoryManagementView(manager: manager, navigation: navigation)
        ))
        window.title = "ClipDeck 设置"
        window.styleMask = [.titled, .closable, .miniaturizable, .resizable]
        window.setContentSize(NSSize(width: 980, height: 680))
        window.center()
        super.init(window: window)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func showGeneral() {
        navigation.selectedPane = .general
        show()
    }

    func showCategories() {
        navigation.selectedPane = .categories
        show()
    }

    private func show() {
        NSApp.activate()
        window?.makeKeyAndOrderFront(nil)
    }

    func hide() {
        window?.orderOut(nil)
    }

    func rememberFocusForDeck() {
        guard ownsFocusedWindow, let focusedWindow = NSApp.keyWindow else { return }
        deckReturnWindow = focusedWindow
        deckReturnResponder = focusedWindow.firstResponder
    }

    func restoreFocusAndPaste() {
        guard let settingsWindow = window, settingsWindow.isVisible else { return }
        let targetWindow = deckReturnWindow?.isVisible == true ? deckReturnWindow : settingsWindow
        let previousResponder = deckReturnResponder ?? targetWindow?.firstResponder
        deckReturnWindow = nil
        deckReturnResponder = nil
        NSApp.activate()
        targetWindow?.makeKeyAndOrderFront(nil)
        if let previousResponder {
            targetWindow?.makeFirstResponder(previousResponder)
        }
        DispatchQueue.main.async { [weak targetWindow, weak previousResponder] in
            guard let targetWindow, targetWindow.isKeyWindow else { return }
            if let previousResponder,
               previousResponder.tryToPerform(#selector(NSText.paste(_:)), with: nil) {
                return
            }
            NSApp.sendAction(#selector(NSText.paste(_:)), to: nil, from: nil)
        }
    }
}
