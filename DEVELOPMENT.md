# ClipDeck 开发说明

本文档记录当前代码边界、核心约束和验证方式。产品能力以 [readme.md](readme.md) 为准。

## 技术栈

- Swift 6
- AppKit + SwiftUI
- SQLite + GRDB.swift + FTS5
- JavaScriptCore
- 本地打包的 highlight.js
- Carbon 全局快捷键

最低系统版本为 macOS 14。

## 目录职责

### App

- `ClipDeckApp.swift`：应用入口。
- `AppCoordinator.swift`：组装服务、窗口和菜单栏交互，不承载内容解析规则。
- `AppPreferences.swift`：设置键、默认值、快捷键匹配和偏好读取。

### Models

- `ClipboardItem.swift`：剪贴板内容、来源应用和分类模型。

### Persistence

- `ClipDeckDatabase.swift`：数据库初始化与仅追加的 schema/data migration。
- `HistoryRepository.swift`：历史、分类和应用记录的数据访问。
- `DefaultCategoryScripts.swift`：系统预制分类与兼容旧版本所需的规则快照。

### Services

- `ClipboardReader.swift`：读取系统剪贴板并执行类型、体积和像素限制。
- `ClipboardMonitor.swift`：监听 changeCount、调度保存和网页元数据补全。
- `ClassificationService.swift`：构造分类脚本输入、依次执行分类并隔离错误。
- `CodeHighlightService.swift`：加权代码检测与 highlight.js 高亮桥接。
- `ClipboardColorParser.swift`：Hex、RGB、RGBA 的统一解析。
- `PasteService.swift`：写回剪贴板、恢复目标应用并触发粘贴。
- `GlobalHotKeyController.swift`：注册和释放全局快捷键。
- `InMemoryHistoryStore.swift`：Deck 当前列表、过滤、选中和预览状态。

### UI

- `HistoryPanelView.swift`：Deck、卡片、预览和内容编辑界面。
- `PanelController.swift`：NSPanel 生命周期、焦点、键盘事件、多屏定位和高度调整。
- `CategoryManagementView.swift`：设置、分类 CRUD、脚本编辑和快捷键配置。

## 必须保持的行为

调整结构或优化性能时，以下行为不得退化：

- Deck 在鼠标所在显示器底部弹出，并记住用户调整后的高度。
- 默认焦点位于卡片；开始输入或按回退键后进入搜索框。
- 搜索实时更新；结束搜索后焦点返回卡片。
- 编辑器持有焦点时，回车、回退和方向键只操作编辑内容。
- 粘贴目标是呼出 Deck 前持有焦点的外部应用。
- 菜单栏图标左键切换 Deck，右键仅打开菜单。
- 预览以 Deck 上方浮层展示，切换卡片或粘贴后消失。
- 图片和颜色卡片保持沉浸式头部，不压缩主要展示区域。
- 分类按用户顺序执行，第一个命中项生效；系统分类与自定义分类具有同等可编辑性。
- 所有历史默认保存在本机；代码检测与语法高亮不请求网络。

## 分类与迁移约束

- 分类规则由用户编辑，不能在启动时无条件覆盖。
- 修改系统默认规则时，新增数据库 migration，只更新与旧预设脚本完全相同的分类。
- 用户自定义过的同名分类必须保持不变。
- 新安装使用 `DefaultCategoryScripts.presets`；“恢复预制分类”同样以该列表为准。
- 图片不进入文本分类；文本分类首个命中后停止继续执行。

## 验证

运行全部单元测试：

```bash
CLANG_MODULE_CACHE_PATH=/tmp/clipdeck-clang-cache \
SWIFTPM_MODULECACHE_OVERRIDE=/tmp/clipdeck-swiftpm-cache \
swift test
```

验证 Xcode 应用 target：

```bash
xcodebuild \
  -project ClipDeck.xcodeproj \
  -scheme ClipDeck \
  -configuration Debug \
  -derivedDataPath .build/XcodeDerivedData \
  build
```

新增 Swift 文件时必须同时确认 Swift Package 与 Xcode Sources Build Phase 均能编译。

## 后续整理方向

- 继续拆分 `HistoryPanelView.swift` 中的卡片、编辑器和预览组件。
- 继续拆分 `CategoryManagementView.swift` 中的设置页、快捷键录制器和脚本编辑器。
- 将网页元数据解析从 `ClipboardMonitor.swift` 移入独立服务文件。
- 为 Panel 焦点、菜单栏左右键和多显示器定位补充可自动化的集成测试。
- 使用 Instruments 持续检查拖动 Deck 时的布局、CPU 和图片解码开销。
