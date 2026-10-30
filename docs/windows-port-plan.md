# ClipDeck Windows 10 复刻方案

> 基准：`mac/` 当前工作区代码（含未提交的文件卡片、顶栏四入口改动）。
> 目标：Windows 10（1809+ x64）上的 UI 与交互与 macOS 版**逐项一致**；只有在操作系统规则不允许时才做映射，所有映射都在本文中逐条列出。
>
> 注意：`docs/images/*.png` 截图是旧版本（顶栏仍是彩色圆点 Text / Images），复刻以**代码**为准，不以截图为准。

---

## 1. 结论速览

| 项 | 决策 |
|---|---|
| 技术栈 | **C++17 + Qt 6.5 LTS**；Deck、弹层、设置页使用 **Qt Quick (QML)**；Win32 API 处理剪贴板、热键、焦点和粘贴 |
| 复用 | 复用 Linux 版的数据层（SQLite 仓储、剪贴板后台管线、文件引用）和 `script-editor/` 的 CodeMirror 编辑器；**状态机与 UI 按 Swift 源码 1:1 移植**，不照搬 Linux UI（Linux 版有“固定”按钮等差异） |
| 视觉一致性手段 | 从 macOS 导出设计令牌 JSON（颜色、尺寸、字号、动画时长）→ 两端共用；用同一份虚拟演示数据做截图像素对比 |
| 最大风险 | Win10 上“亚克力模糊 + 24px 圆角 + 阴影”无边框窗口；“不抢焦点却能收键盘”的面板。两者放在第 0 阶段先做技术验证 |
| 需要你拍板 | ① 技术栈（推荐 Qt 6）② 全局快捷键默认值（推荐 `Alt+Shift+V`，与 Linux 一致）③ 编辑器运行时（QtWebEngine 或 WebView2） |

### 为什么选 Qt 6 而不是 WinUI 3 / WPF / Electron

| 方案 | 优点 | 问题 |
|---|---|---|
| **Qt 6 + QML（推荐）** | Linux 版已有 Qt 数据层、QJSEngine 分类、QWebChannel 编辑器桥接可直接复用；QML 的声明式动画、圆角裁切、阴影（MultiEffect）和 SwiftUI 写法对应关系最直接；可完全自绘，不受系统控件风格影响 | 需自行处理 Win10 亚克力；QtWebEngine 体积大 |
| WinUI 3 / WPF (C#) | 原生 Windows 观感 | 我们要的恰恰**不是** Windows 观感，而是 macOS 观感；Mica 只在 Win11；零代码复用 |
| Electron / Tauri | Web 技术自绘方便 | 常驻剪贴板工具内存占用过高；Win32 焦点/粘贴仍需原生模块；与现有两端无复用 |

---

## 2. 仓库结构

```text
windows/
  CMakeLists.txt
  src/
    app/            # AppCoordinator、AppPreferences、托盘、单实例
    core/           # 由 Linux 抽出的共享数据层（见第 8 节），Qt5/Qt6 双编译
    store/          # HistoryStore —— InMemoryHistoryStore.swift 的 1:1 移植
    platform/win/   # 剪贴板监听/读取/写入、全局热键、焦点、SendInput、来源应用图标、开机启动、亚克力
    ui/qml/         # Deck、卡片、弹层、设置页、分类管理
    ui/             # QML 注册的 C++ 模型与绘制组件（图片缓存、语法高亮）
  resources/        # 打包的字体、图标 SVG、提示音、highlight.min.js、script-editor/dist
  tests/            # Qt Test，移植 mac/Tests 的全部用例
  packaging/        # WiX / Inno Setup、签名脚本
shared/
  design-tokens.json   # 新增：由 macOS 导出的设计令牌
  assets/              # 已有：clipdeck.svg、应用图标
```

---

## 3. 平台能力映射（macOS → Windows 10）

| macOS 实现 | 文件 | Windows 10 实现 | 备注 |
|---|---|---|---|
| `NSPanel` `.nonactivatingPanel` `.floating`，`canJoinAllSpaces` | PanelController.swift | 无边框 `HWND`，`WS_EX_TOOLWINDOW \| WS_EX_TOPMOST \| WS_EX_NOREDIRECTIONBITMAP`，不出现在任务栏和 Alt+Tab | 见 4.1 焦点模型 |
| `NSVisualEffectView(.underWindowBackground, alpha 0.62)` | HistoryPanelView.swift `DeckMaterialView` | `SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)`（1803+），调整高度期间降级为 `ACCENT_ENABLE_BLURBEHIND`；不可用时（省电模式、关闭透明效果）降级为 `#1E1E20` 不透明 | Win10 1903+ 亚克力在窗口拖动/缩放时有已知卡顿，因此缩放时降级；macOS 版缩放时也会关闭阴影，行为对应 |
| `contentView.layer.cornerRadius = 24` | PanelController.swift | `DwmSetWindowAttribute` 圆角仅限 Win11；Win10 方案：`SetWindowRgn` 圆角区域裁切模糊层 + QML 层抗锯齿圆角描边遮住锯齿 | 第 0 阶段验证 |
| 窗口阴影 `hasShadow` | | 外层透明分层窗口自绘阴影（QML MultiEffect），或 DWM 1px frame 阴影 | 第 0 阶段二选一 |
| `NSScreen.visibleFrame`（鼠标所在屏） | `preferredScreen()` | `MonitorFromPoint(GetCursorPos)` + `GetMonitorInfo().rcWork`（任务栏以上区域），Per-Monitor DPI v2 | 任务栏在左/右/上时同样贴 rcWork 底部 |
| Carbon `RegisterEventHotKey` | GlobalHotKeyController.swift | `RegisterHotKey`；注册失败时托盘提示，与 macOS tooltip 文案一致 | |
| `NSStatusItem` 左键 / 右键 | AppCoordinator.swift | `Shell_NotifyIcon` 托盘图标：左键切换 Deck，右键菜单 “设置… / 退出 ClipDeck” | Win10 默认把新图标收进溢出区，首次启动气泡提示“拖到任务栏” |
| `NSPasteboard.changeCount` 每 200ms 轮询 | ClipboardMonitor.swift | `AddClipboardFormatListener` → `WM_CLIPBOARDUPDATE` 事件驱动；`GetClipboardSequenceNumber()` 作为 changeCount | 抑制自身写入：记录写入后的 sequence number，逻辑同 `suppressCurrentPasteboardChange()` |
| `.string` / `.rtf` | ClipboardReader.swift | `CF_UNICODETEXT` / 注册格式 `"Rich Text Format"` | |
| `.png` / `.tiff` / JPEG / HEIC / WebP / GIF | ClipboardReader.swift | 注册格式 `"PNG"` 优先 → `CF_DIBV5` → `CF_DIB`；全部规范化为 PNG 存储，保留大小和像素上限 | 浏览器、截图工具提供 `PNG`；DIBV5 需正确处理 alpha |
| 文件 URL `.fileURL` | ClipboardReader.swift | `CF_HDROP`；单个图片文件 → 图片卡片（与 macOS 的图片文件 URL 规则一致） | |
| 写回文件引用 URL | PasteService.swift | `CF_HDROP` + `"Preferred DropEffect" = DROPEFFECT_COPY` | |
| `CGEvent` 发送 ⌘V | PasteService.swift | `SendInput` 发送 Ctrl+V | 见 4.3 |
| `AXIsProcessTrusted()` 不通过 → 仅复制并弹窗 | AppCoordinator.swift | Windows 无辅助功能授权；对应失败场景是 **UIPI**：目标进程完整性级别高于 ClipDeck（管理员运行的程序）时无法注入按键 → 同样弹出“内容已复制到剪贴板”，文案改为“目标应用以管理员身份运行，请手动按 Ctrl + V” | |
| `NSWorkspace.frontmostApplication` | | `GetForegroundWindow()` → `GetWindowThreadProcessId` → `QueryFullProcessImageName` | UWP 应用需从 `ApplicationFrameHost` 解析真实子窗口 |
| 来源应用名 + 图标 | ClipboardMonitor.swift | 名称：`FileDescription` 版本资源，回退为 exe 名；图标：`SHGetFileInfo` / `IShellItemImageFactory` 取 64px PNG；`bundle_id` 列存 exe 完整路径小写 | 不改表结构 |
| JavaScriptCore（分类脚本、highlight.js） | ClassificationService / CodeHighlightService | `QJSEngine`（Linux 已用于分类），highlight.js 同样跑在 QJSEngine | 需移植 `console.*` 捕获与输入字段 |
| WKWebView + CodeMirror | ScriptEditorWebView.swift | QtWebEngine + QWebChannel（`bridge.ts` 已支持）；或 WebView2 + 为 `bridge.ts` 增加 `chrome.webview` 适配 | 见第 9 节决策 |
| `NSSound` Tink / Pop | FeedbackSoundPlayer | 自制两段等效短音 WAV，`PlaySound(SND_ASYNC)` | Apple 系统音效不能在 Windows 上分发 |
| `SMAppService` 登录项 | LaunchAtLoginController.swift | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`；MSIX 版用 StartupTask | “打开登录项设置”按钮改为打开 `ms-settings:startupapps` |
| UserDefaults | AppPreferences.swift | `QSettings`（`HKCU\Software\ClipDeck`），**键名保持一致** | |
| GRDB SQLite + FTS5 | ClipDeckDatabase.swift | Qt SQL（SQLite 驱动需启用 FTS5）；**迁移名与表结构 1:1**，数据库位于 `%LOCALAPPDATA%\ClipDeck\clipdeck.sqlite`，WAL | |
| `NSWorkspace.open(url)` | | `ShellExecuteW(L"open", url)` / `QDesktopServices::openUrl` | |
| SF Symbols | 多处 | **不能分发**（许可证限定 Apple 平台）。使用 MIT 许可的 Fluent System Icons / Lucide，按 SF Symbol 的视觉权重挑选并统一导出 SVG；`DeckNavigationIcon` 本来就是自绘路径，直接移植 | 见附录 B 图标清单 |

---

## 4. 交互规格（必须逐条一致）

### 4.1 Deck 呼出、焦点与收起

| 行为 | macOS | Windows 实现 |
|---|---|---|
| 呼出前记录目标应用 | `rememberFrontmostExternalApplication()` | 在 `WM_HOTKEY` / 托盘点击处理的第一时间 `GetForegroundWindow()` 保存为 `targetHwnd`（排除自身窗口） |
| 呼出 | 非激活面板成为 key window，原应用保持激活 | Windows 无法“不激活又收键盘”。做法：`SetForegroundWindow(deck)`（`WM_HOTKEY` 和托盘点击都会授予前台权限）；视觉上无差异，因为 Deck 是工具窗口、不进任务栏 |
| 在设置窗口获得焦点时呼出 | `targetsClipDeck = true`，粘贴回设置窗口输入框 | 同逻辑：记录设置窗口里获得焦点的 QML 控件，粘贴时写剪贴板 → 还原焦点 → 在进程内发送粘贴 |
| 再次按热键 / 再次左键托盘 | 收起 | 同 |
| 失去焦点 | `windowDidResignKey` 后下一轮事件循环：若仍在本应用内且正在搜索编辑 / 预览 / 编辑 / 重命名则保留，否则收起 | 处理 `WM_ACTIVATE(WA_INACTIVE)`，用 `QTimer::singleShot(0)` 延迟到下一轮；“本应用内”判定为新前台窗口属于本进程（弹层窗口为 Deck 的 owned window） |
| Esc 分层退出 | 文件反馈 → 重命名 → 编辑 → 搜索 → 收起 | 同，顺序不可变 |
| 每次显示时 | 重新计算 frame、选中保持或选第一项、`revealSelection()` 居中 | 同 |

### 4.2 Deck 几何（单位为 DIP，按显示器 DPI 缩放）

- 宽度 = rcWork 宽 − 2×8；x = rcWork.left + 8；底边贴 rcWork.bottom。
- 最大高度 = `max(250, min(420, floor(rcWork.h × 0.42)))`；最小高度 = `min(250, 最大高度)`；默认高度 = `min(最大高度, max(340, floor(rcWork.h × 0.385)))`。
- 用户调整的高度存在 `deck.preferredHeight`，每次显示时按当前屏幕重新夹取。
- 顶部 18px 整条为调整高度热区：光标 `IDC_SIZENS`；拖动时显示 44×3、圆角 1.5、白 32% 的指示条（距顶 5px）；按显示器刷新率节流 `SetWindowPos`；拖动期间关闭卡片阴影和动画，结束后把选中卡片重新滚动到中间。

### 4.3 粘贴流程（对应 `PasteService.paste` / `pasteAsSoonAsActive`）

1. 写剪贴板（`OpenClipboard` 失败时 10ms 间隔重试 5 次）；纯文本模式只写 `CF_UNICODETEXT`；“保留原格式”开启且有 RTF 时追加 RTF；图片有旋转角度时写渲染后的 PNG + DIBV5；文件写 `CF_HDROP` + DropEffect。
2. 记录写入后的 `GetClipboardSequenceNumber()`，并让监听器忽略这一次变化。
3. 收起 Deck，然后 `SetForegroundWindow(targetHwnd)`（因为 Deck 当前在前台，这个调用是允许的；失败时用 `AllowSetForegroundWindow` / `AttachThreadInput` 兜底）。
4. 每 30ms 轮询，最多 2s：剪贴板序号变化 → 取消（日志与 macOS 文案一致）；`GetForegroundWindow()` 的根窗口等于目标且 Deck 不可见 → 发送粘贴。
5. **Windows 专有**：发送前用 `GetAsyncKeyState` 检查物理按住的修饰键。快速粘贴时 Ctrl 本来就按着，无需处理；Shift（纯文本双击）和 Alt（来自热键）必须先补发 key-up，否则目标收到的是 Ctrl+Shift+V / Ctrl+Alt+V。补发的 key-up 只影响这一次注入。
6. 目标是更高完整性级别的进程 → 不发送，返回 `copiedOnly`，弹窗提示（见第 3 节 UIPI）。
7. 播放粘贴提示音。

文件卡片粘贴前的后台可访问性检查（`FilePasteController`）完整移植：2s 超时、检查期间剪贴板变化即作废、部分可用时提示“粘贴可用文件”。网络路径（UNC）和已断开的映射盘可能阻塞较久，必须在工作线程里执行。

### 4.4 键盘映射

原则：⌘ → **Ctrl**，⌥ → **Alt**，⌃ → **Win** 不开放（Win 组合键大多被系统占用），⌫ → Backspace，⌦ → Delete。

| 功能 | macOS | Windows |
|---|---|---|
| 全局呼出 | ⌘⇧V | **待定**：推荐 `Alt+Shift+V`（与 Linux 一致）。`Ctrl+Shift+V` 是浏览器、Office、VS Code 的“粘贴为纯文本”，全局注册会抢走它；`Win+V` 是系统剪贴板历史 |
| 快速粘贴 1–9 | 按住 ⌘ 显示序号，⌘1…9 | 按住 Ctrl 显示序号，Ctrl+1…9（主键盘区和小键盘都支持） |
| 上 / 下一个分类 | ⌘← / ⌘→ | Ctrl+← / Ctrl+→ |
| 切换卡片 | ← / → | ← / → |
| 粘贴 | Return / Enter；搜索编辑中则先结束编辑 | Enter（主键盘和小键盘） |
| 纯文本粘贴 | ⇧ + 双击 | Shift + 双击 |
| 预览 | Space（切换），预览中 ↑↓ 滚动 | 同 |
| 编辑 / 重命名 / 访问链接 | ⌘E / ⌘R / ⌘O | Ctrl+E / Ctrl+R / Ctrl+O |
| 删除 | ⌘⌫ | Ctrl+Backspace，另加 **Delete**（Windows 用户习惯；搜索编辑中 Delete 交给输入框） |
| 搜索 | 直接输入即搜索；⌘F | 同；Ctrl+F |
| 把剪贴板粘进搜索框 | 搜索激活时 ⌘V（空白折叠为单个空格） | 搜索激活时 Ctrl+V |
| 搜索框为空时 Backspace | 退出搜索 | 同（注意 IME 组字状态：组字时不触发） |
| 设置 | ⌘, | Ctrl+, |
| 退出 | ⌘Q（Deck 获得焦点时） | Ctrl+Q（Deck 获得焦点时） |
| 编辑器保存 / 取消 | ⌘S / Esc | Ctrl+S / Esc |
| 图片左 / 右旋转 | ⌘[ / ⌘] | Ctrl+[ / Ctrl+] |
| 脚本编辑器 | ⌘/ 注释、Tab/⇧Tab、⇧⌘F 格式化 | Ctrl+/、Tab/Shift+Tab、Shift+Ctrl+F（CodeMirror 的 `Mod` 本来就会映射） |
| 设置页修饰键选择器 | ⌘ Command / ⇧ Shift / ⌃ Control / ⌥ Option | Ctrl / Shift / Alt（三选一，默认快速粘贴 = Ctrl、纯文本 = Shift） |

界面里出现的所有快捷键提示文字（`⌘/ 注释` 等）同步替换为 Windows 写法。

### 4.5 鼠标与滚轮

- 竖向滚轮 → 卡片横向滚动（仅作用于卡片带，不影响分类条和预览）。普通滚轮：每格（delta 120）移动 44 DIP，0.14s ease-out，多次滚动叠加到目标位置；精确触控板（delta 不是 120 的倍数）：1:1 移动，0.08s。`SPI_GETCLIENTAREAANIMATION` 关闭时不做动画（对应 macOS“减弱动态效果”）。
- 横向滚轮（`WM_MOUSEHWHEEL`）直接横向滚动。
- 单击选中（并结束搜索编辑）；双击粘贴；右键菜单见 5.4。
- 选中变化：0.18s ease-out 把选中卡片滚动到中间。

### 4.6 搜索状态机

逐字移植 `InMemoryHistoryStore` 的 `beginSearch / setSearchEditing / finishSearchEditing / resumeSearchEditingAndDeleteBackward / collapseSearchIfQueryWasDeleted / endSearch`，以及：

- 本地即时过滤（标题、正文、文件名、路径，不区分大小写）+ 120ms 防抖后 FTS5 查询，两份结果按 databaseID 去重合并（`mergeSearchResults`）。
- 搜索词从空变为非空时，分类重置为 History。
- IME（微软拼音）：组字中不触发搜索、不触发 Backspace 退出搜索——对应 macOS 的 `hasMarkedText` 判断，用 `QInputMethodEvent::preeditString()` 判断。

---

## 5. 视觉规格

### 5.1 设计令牌导出（先做这一步）

新增 `mac/Tools/DesignTokenExport.swift`：在深色外观下把 `NSColor.systemPurple / systemOrange / systemTeal / systemBlue / systemMint / systemCyan / systemPink / systemIndigo / systemGreen / secondaryLabelColor / labelColor / accentColor` 以及 SwiftUI 的 `.purple .cyan .green .pink .indigo .mint` 解析为 sRGB 十六进制，连同下面所有尺寸常量一起写入 `shared/design-tokens.json`。Windows 端在编译期生成 `Tokens.qml`。**不要手抄系统色**：不同 macOS 版本的取值有差异，必须以导出结果为准。

### 5.2 Deck 容器

| 元素 | 规格 |
|---|---|
| 背景 | 模糊材质（透明度 0.62）+ 黑 20% + 竖向渐变（白 6% → 透明 → 黑 6%） |
| 圆角 | 24（四角） |
| 工具栏 | 高 58，上内边距 7，左右 24；居中组件距右 54 |
| 搜索按钮 | 放大镜 19pt medium，28×34；展开后为胶囊输入框 230×34、白 14% 底、14pt medium，占位“搜索文本”，右侧清除按钮（白 48%） |
| 固定入口 | History / 文本 / 图片 / 文件：图标 19×19（24 网格、1.7 线宽、圆角端点），间距 4；按钮高 35、左右 10、圆角 9；选中：白 16% 底、阴影黑 24% 半径 4 y1、文字 100%；未选：65%；**只有 History 和当前选中入口显示文字**（13 semibold） |
| 分隔线 | 1×22，白 18%，左右各 7 |
| 自定义分类 | 胶囊 96×31，圆点 11，13 semibold，间距 12；可横向滚动，宽度 240–760；选中白 14% + 阴影；圆点颜色按 sortOrder 循环 purple, cyan, green, pink, indigo, mint |
| 分页箭头 | 分类超过 6 个时显示；23×31，13 bold；可用 82% / 不可用 20%；每次翻 5 个 |
| “+” 按钮 | 19 medium，34×34，左边距 5 → 打开设置的分类页 |
| “…” 按钮 | 右侧 38×38，18 bold，白 82% → 打开设置的分类页 |
| 空状态 | 图标 30 light；标题 16 semibold rounded；副标题 12.5 次要色；内边距 34/28；白 10% 底、圆角 22。文案：无搜索时“Copy something to get started / 复制的文本、图片和文件会显示在这里”；搜索时“没有找到匹配内容 / 尝试其他关键词” |

### 5.3 卡片

| 元素 | 规格 |
|---|---|
| 尺寸 | 正方形，边长 = `max(166, 内容区高度 − 24)`；间距 24；外边距 左右 22 / 上下 12 |
| 模式 | 边长 ≥ 180 为展开模式，否则为紧凑模式；切换时 opacity 过渡 |
| 底色 / 圆角 | `#131315`（0.075, 0.075, 0.082）；圆角 展开 18 / 紧凑 15（continuous 曲线，Qt 用超椭圆路径近似） |
| 描边 | 选中 `rgb(10,143,255)` 4px；未选 白 9% 1px；0.14s ease-out |
| 阴影 | 选中 blue 24%、未选黑 28%，半径 9 y4；调整高度时关闭 |
| 快速粘贴序号 | 右下角，最小 25×25，14 bold rounded，黑 66%（选中）/ 48%，圆角 7，边距 9；出现时淡入 + 0.88 缩放，0.10s |
| 展开头部 | 高 = `clamp(48, 边长×0.20, 58)`；左 14 右 10；标题 14 bold + 分类 12 semibold 72%；时间 11.5 semibold 78%；右侧来源应用图标（头部高 −8，圆角 max(4, 20%)）；文件卡片显示文件夹图标 30 |
| 头部渐变（左上→右下） | 文本未选：`(0.27,0.62,0.93) → (0.13,0.47,0.88)`；文本选中：`(0.01,0.14,0.35) → (0.02,0.26,0.60)`；文件：`(0.141,0.420,0.725) → (0.13,0.38,0.68)`；图片：`(0.36,0.42,0.50) → (0.22,0.27,0.34)` |
| 沉浸式头部 | 图片和颜色卡片头部浮在内容上方，文字阴影黑 42%（紧凑模式 55%） |
| 紧凑头部 | 单行：标题 bold · 分类 70% · 时间 62% · 来源图标 18；11.5 semibold；左右 11，上 10 |
| 文本内容 | 前 3000 字符，13.5 等宽 + 语法高亮，行距 +2；左右 14，上 13（紧凑 37），下 34（紧凑 12）；底部“N 个字符”11 medium 52% |
| 图片 | 12px 棋盘格（白 5.5% / 黑 12%，底 `(0.10,0.10,0.11)`），内边距 8 等比适配并按 imageAngle 旋转；底部尺寸胶囊 11 等宽 semibold，黑 58% |
| 颜色 | 整卡填充颜色；底部数值胶囊 13 bold 等宽，黑 48% |
| 链接 | 有预览图时铺满；否则居中白底站点图标 `clamp(42, 边长×0.25, 58)`（圆角 23%，阴影）；底栏高 `clamp(72, 边长×0.23, 88)`：图标 32 + 标题 13.5 semibold（最多 2 行）+ 域名 11.5 50%；紧凑模式只显示 36 的图标 |
| 文件 | 移植 `FileReferenceList`：单文件居中大图标 + 名称 + 路径；多文件按可用高度计算可见行数 + “还有 N 项” |
| 相对时间 | 现在 / N 分钟 / N 小时 / N 天 / 超过 7 天显示“M月d日” |
| 标题规则 | 文件“文件 · N 项”；链接“链接”；其余为 item.title；代码分类显示检测到的语言名 |

### 5.4 右键菜单

顺序与图标：粘贴（Enter）· 访问（Ctrl+O，仅链接）· 分隔线 · 编辑（Ctrl+E，文件卡片不可用）· 重命名（Ctrl+R）· 预览（Space）· 分隔线 · 删除（Ctrl+Backspace，红色）。
菜单本身用 QML 自绘为 macOS 深色上下文菜单样式（半透明深色、右侧显示快捷键；圆角、行高和内边距从 macOS 截图量取），不使用 Win10 原生菜单。

### 5.5 弹层（预览 / 编辑 / 重命名）

SwiftUI `popover(arrowEdge: .bottom)`：出现在卡片上方，箭头朝下指向卡片。Windows 用 Deck 的 owned 无边框半透明窗口实现，带指向卡片的箭头、深色模糊材质；箭头尺寸、圆角和材质颜色在第 0 阶段从 macOS 截图中量取后写入设计令牌。

- 预览：文本 / 文件 560×360；图片按 `ContentPreviewLayout.imageSize` 计算（视口最大 512×270，最小 360×300，额外留出 48 / 135）。头部高 58（文件 52）：分类名 18 semibold、来源应用 13 次要色、右侧日期时间。
- 编辑：560×430；工具栏 62：左“取消”胶囊 68×34 白 7%，右“保存”胶囊 accent，中间“编辑文本”（JSON 显示“格式化”）或图片旋转按钮；文本编辑区黑 56%、圆角 12、17pt；底栏 44。
- 重命名：宽 380，内边距 18，输入框高 38、白 8%、圆角 9。
- 切换卡片或粘贴时自动关闭预览（`selectedItemID` 的 didSet 逻辑）。

### 5.6 设置窗口

- Win10 标准标题栏 + `DWMWA_USE_IMMERSIVE_DARK_MODE`（20H1+ 属性号 20，1809–1909 为 19），标题“ClipDeck 设置”。内容区 100% 自绘复刻：左侧栏 190 宽（标题“设置”22 bold rounded；项目高 42、圆角 10、图标 17、文字 15 semibold；选中白 12%），五个面板：通用 / 图片 / 键盘快捷键 / 分类 / 数据。
- 面板内容、确认对话框文案、保留期限滑块（7 天…1 年、永久，含主次刻度）、快捷键录制按钮、分类拖动排序、分类编辑页、展开式 JavaScript 编辑窗口（含 `item` 字段说明、测试控制台）全部按 `CategoryManagementView.swift` 移植。
- 系统对话框（`NSAlert` / `confirmationDialog`）改为 QML 自绘的 macOS 风格对话框，按钮顺序保持 macOS（取消在左，主操作在右）。这是唯一与 Windows 惯例冲突的地方，因要求“交互完全一致”而保留 macOS 顺序。

### 5.7 字体

| macOS | Windows 10 |
|---|---|
| SF Pro / SF Pro Rounded（拉丁） | 随应用打包一款 OFL 授权的圆体西文字体（如 Nunito），只用于 `.rounded` 的位置；其他位置用 Segoe UI |
| 苹方（中文） | Microsoft YaHei UI（系统自带；Win10 没有可替代的圆体中文） |
| SF Mono | 打包 Cascadia Mono（OFL；Win10 默认没有） |

字重映射：semibold → DemiBold (600)，bold → 700，medium → 500。Windows 的 ClearType 和 macOS 渲染存在差异，像素对比时文字区域允许一定容差。

---

## 6. 数据与业务逻辑

全部按 Swift 源码移植，逻辑不做重新设计：

- `HistoryStore`：内存容量 100；选中、分类、搜索、预览、编辑、重命名、快速粘贴遮罩、文件反馈全部状态及其互斥关系。
- 历史仓储：表结构与 mac 的 GRDB 迁移序列一致（`application / category / content / content_fts`、全部索引与触发器、`addContentTitle`、`addLinkIconData`、`addLinkPreviewData`、`addImageAngle`、`addFileReferences` 等）。24 小时内按 SHA-256 内容哈希去重（文件按排序后的路径列表哈希）。保留期限按 `last_copied_at` 计算，缩短时二次确认；“清理过期内容并回收空间”执行 WAL checkpoint + VACUUM。
- 启动加载：先加载 20 条快速显示，后台清理过期内容后再补全 —— 与 `loadPersistedHistory` 一致。
- 分类：`DefaultCategoryScripts.swift` 的预制脚本原样拷贝，首个匹配生效；脚本输入对象的字段（`text`、`textLength`、`sourceApplication.*` 等）保持同名。`bundleIdentifier` 在 Windows 上是 exe 路径，需在字段说明里注明。
- 链接元数据：标题、图标（含 manifest）、预览图（最大 8MB）的解析规则移植；使用 `QNetworkAccessManager`。
- 图片：大小上限（默认 10MB）、像素上限、开关“记录图片”。
- 隐私（Windows 增强，建议采纳）：遵守 `ExcludeClipboardContentFromMonitorProcessing`、`CanIncludeInClipboardHistory = 0`、`Clipboard Viewer Ignore` 这三个格式标记，不记录密码管理器写入的内容。

---

## 7. 实施阶段

| 阶段 | 内容 | 产出 / 验收 |
|---|---|---|
| **0 技术验证** | ① 亚克力 + 圆角 + 阴影无边框窗口在 Win10 1809 / 21H2、100% / 150% / 200% DPI、多屏混合 DPI 下的效果；② 热键呼出 → Deck 收键盘 → 还原焦点 → SendInput 粘贴，测试 Notepad、Word、Chrome、VS Code、微信、管理员运行的 cmd；③ QML 横向列表 100 张卡片滚动保持 60fps | 决定圆角/阴影方案；粘贴成功率表 |
| **1 地基** | 导出设计令牌；从 Linux 抽出 `core/`（Qt5/Qt6 双编译）；SQLite 迁移对齐；`HistoryStore` 移植 + 移植 `InMemoryHistoryStoreTests` | 单元测试通过 |
| **2 平台层** | 剪贴板监听 / 读取 / 写入（文本、RTF、PNG/DIB、HDROP）、来源应用与图标、热键、托盘、单实例、开机启动、提示音 | 移植 `PasteServiceTests`、`FileReferenceTests` |
| **3 Deck** | 容器、调整高度、工具栏、分类条、卡片全部类型、滚动、键盘映射、右键菜单、快速粘贴、搜索 | 与 macOS 同一份演示数据截图对比 |
| **4 弹层** | 预览、编辑（含语法高亮编辑、JSON 格式化、图片旋转）、重命名、文件粘贴反馈 | |
| **5 设置** | 五个面板、快捷键录制、保留期限、数据清理、分类管理、CodeMirror 编辑器与测试控制台 | 移植 `ClassificationServiceTests` |
| **6 收尾** | 安装包（WiX MSI 或 Inno Setup + 便携 zip）、代码签名、崩溃日志、`windows/README.md`、根 README 补充 Windows 部分 | 在干净的 Win10 虚拟机安装验证 |

### 一致性验收方法

1. 在 macOS 上用 `--show-on-launch` 加虚拟演示数据（与 `docs/images` 同一套）截取 Deck 展开 / 紧凑、四种入口、搜索、预览、编辑、重命名、文件反馈、设置五页的截图；Windows 端用同一份数据在 100% 和 200% DPI 下截图。
2. 按 DIP 对齐后做逐像素差异对比：几何和颜色差异视为缺陷；文字抗锯齿差异允许容差。
3. 按 4.4、4.6 的表格逐条执行键盘交互测试，并加入 IME 组字场景。

---

## 8. 与 Linux 版的复用边界

| 可复用（抽到 `core/`） | 不复用 |
|---|---|
| `history_repository.*`（需先确认与 mac 的迁移序列一致，差异以 mac 为准）、`history/clipboard_pipeline.*`、`capture_payload.*`、`file_reference.*`、`file_paste_controller.*`（去掉 X11 依赖）、`script_editor_widget` 的 QWebChannel 桥接思路 | `main.cpp` 中的 `DeckWindow` 等 Widgets UI（与 mac 存在差异，且 Windows 使用 QML）；`platform/x11_*` |

Linux 版是 Qt 5.11（Debian 10 基线），Windows 版用 Qt 6，`core/` 只能使用两者共有的 API（QtCore / QtSql / QtQml），并在 CI 上两种都编译。

---

## 9. 待决策事项

1. **技术栈**：推荐 Qt 6 + QML（理由见第 1 节）。
2. **全局快捷键默认值**：推荐 `Alt+Shift+V`；也可以用 `Ctrl+Shift+V`，代价是与浏览器 / Office 的“粘贴为纯文本”冲突。用户可在设置里修改。
3. **编辑器运行时**：QtWebEngine（与 Linux 一致、零改动，安装包增加约 100MB+）或 WebView2（Win10 绝大多数机器已预装 Evergreen Runtime，体积小，但需为 `bridge.ts` 增加约 30 行 `chrome.webview` 适配）。推荐 WebView2。
4. **对话框按钮顺序**：保持 macOS 顺序（本文默认）还是改为 Windows 顺序。
5. **最低系统版本**：推荐 Win10 1809（Qt 6.5 支持的下限，也能使用亚克力和深色标题栏）。

---

## 附录 A：Windows 10 已知差异（无法 100% 一致的地方）

| 项 | 原因 | 处理 |
|---|---|---|
| 模糊材质的观感 | Win10 亚克力的噪点和饱和度与 macOS vibrancy 不同 | 调整叠加层透明度，使截图对比的平均亮度一致 |
| 字体 | SF / 苹方不能分发 | 见 5.7 |
| SF Symbols | 许可证限制 | 用相近的开源图标替换 |
| 托盘图标默认被收起 | Win10 系统行为 | 首次启动提示 |
| 管理员应用无法自动粘贴 | UIPI | 退化为仅复制 + 提示，对应 macOS 未授权辅助功能的分支 |
| “焦点不离开原应用” | Windows 无非激活键盘窗口 | 呼出时 Deck 激活、粘贴时还原，用户感知一致 |

## 附录 B：SF Symbols 替换清单

`magnifyingglass` `xmark.circle.fill` `plus` `ellipsis` `chevron.left` `chevron.right` `doc.on.clipboard` `safari` `pencil` `character.cursor.ibeam` `eye` `trash` `app` `rotate.left` `rotate.right` `folder` `exclamationmark.circle` `gearshape` `power` `photo` `keyboard` `square.grid.2x2` `externaldrive`，以及设置页、分类图标选择器中用到的其余符号（实现时用 `grep -o 'systemName: "[^"]*"'` 生成完整列表）。分类图标存在数据库 `category.icon` 字段里（SF Symbol 名称），Windows 端需要一张“SF Symbol 名 → SVG”映射表，未覆盖的名称回退为默认图标。
