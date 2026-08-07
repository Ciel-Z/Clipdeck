# ClipDeck macOS 版需求说明

## 1. 项目概述

### 1.1 软件名称

**ClipDeck**

### 1.2 产品定位

ClipDeck 是一款面向 macOS 的本地剪贴板历史管理工具。

产品整体样式与核心交互参考 Paste，包括：

* 从屏幕底部唤出剪贴板面板
* 横向卡片式展示历史内容
* 键盘快速选择
* 搜索、预览和快速粘贴
* 显示内容来源应用
* 粘贴完成后自动收起
>> 样式及功能交互参考: https://pasteapp.io/use-cases#everyone、https://pasteapp.io/use-cases、https://pasteapp.io/use-cases#designers
>> 本机已安装并付费相关的正版软件，可触发按键 F12 呼出 paste deck，观察软件实机效果。一些样式及比较细节的交互 ui 可根据 paste 实机效果复刻。

ClipDeck 不追求完整复刻 Paste 的全部功能，而是在保留其优秀交互体验的基础上，裁剪不需要的能力，并重点增加面向程序员的自动分类和代码预览功能。

### 1.3 首期平台

首期仅开发：

* macOS

后续可能根据首版功能再次裁剪后开发 Linux 版本，但当前设计和开发只需要聚焦 macOS，不需要为了 Linux 提前采用跨平台 UI 框架。

---

# 2. 技术选型

## 2.1 开发工具

使用：

* Xcode
* Swift

## 2.2 UI 技术

采用 AppKit 与 SwiftUI 混合开发。

AppKit 主要负责：

* 底部悬浮窗口
* NSPanel
* 窗口层级
* 焦点控制
* 多屏幕定位
* 全局快捷键
* 菜单栏
* 剪贴板监听
* 粘贴到原应用
* 辅助功能权限
* 拖拽和键盘事件

SwiftUI 主要负责：

* 设置页面
* 分类管理页面
* 分类脚本编辑页面
* 普通卡片内容
* 表单和配置项
* 空状态和错误状态

核心横向卡片列表优先考虑使用：

* NSCollectionView

卡片内部可使用 SwiftUI 或 AppKit 自定义视图。

## 2.3 数据存储

使用：

* SQLite
* GRDB.swift

全文搜索可使用：

* SQLite FTS5

## 2.4 分类脚本

使用：

* JavaScriptCore

首版允许先在独立后台执行队列中运行脚本。

后续为了处理死循环、内存异常和脚本崩溃，可将脚本执行迁移到独立 XPC Service。

---

# 3. 首版功能范围

## 3.1 支持的剪贴板内容

首版只记录：

* 文本
* 图片

不记录：

* 文件
* 文件夹
* 音频
* 视频
* 其他任意二进制剪贴板内容

检测到不支持的剪贴板类型时，直接忽略，不生成历史记录。

---

# 4. 文本内容

## 4.1 纯文本与富文本

文本内容需要同时保存：

* 纯文本
* 富文本

纯文本用于：

* 搜索
* 分类脚本
* 普通预览
* 默认粘贴
* 降级粘贴

富文本用于：

* 用户选择保留原格式粘贴
* 恢复原始文本样式

建议保存格式：

* `plain_text`：SQLite TEXT
* `rich_text`：SQLite BLOB
* `rich_text_type`：记录富文本格式，例如 RTF

首版优先支持 RTF。

如果剪贴板中只有纯文本，则 `rich_text` 可以为空。

## 4.2 默认粘贴格式

设置中提供：

* 纯文本
* 保留原格式

默认值：

* 纯文本

当设置为保留原格式时：

* 如果存在富文本，则同时写入富文本和纯文本表示
* 如果不存在富文本，则自动退化为纯文本

图片粘贴不受该配置影响。

---

# 5. 图片内容

## 5.1 图片存储

图片直接以二进制数据保存到 SQLite BLOB。

不使用 Base64。

不将图片保存到独立文件目录。

这样可以保证：

* 内容与图片同时删除
* 不产生孤儿文件
* 数据备份简单
* 数据库事务一致
* 清理逻辑统一

## 5.2 图片大小限制

设置中提供：

* 是否记录图片
* 最大记录图片大小

可选值可包括：

* 不记录图片
* 1 MB
* 2 MB
* 5 MB
* 10 MB
* 20 MB

超过用户配置大小的图片：

* 直接忽略
* 不压缩
* 不转码
* 不尝试降低质量
* 不写入数据库

## 5.3 图片像素限制

为避免异常图片解码导致过高内存占用，需要设置一个程序内置的最大像素限制。

该限制：

* 不在设置页面显示
* 不允许用户修改
* 仅作为程序安全保护

可以暂定：

* 最大总像素数 60,000,000

判断时需要防止宽高相乘溢出。

## 5.4 图片搜索

首版图片不支持搜索。

搜索时只返回文本类型内容。

---

# 6. 剪贴板监听

## 6.1 监听方式

使用：

* `NSPasteboard.general`
* `changeCount`

通过定时轮询检测剪贴板变化。

建议轮询间隔：

* 300～500 毫秒

剪贴板读取、图片解析、数据库写入和分类执行不能阻塞 UI 主线程。

## 6.2 内容处理流程

处理流程：

1. 检测剪贴板 `changeCount` 变化。
2. 判断是否包含文本或图片。
3. 不支持的类型直接忽略。
4. 读取并标准化内容。
5. 检查图片大小和内置像素限制。
6. 计算内容哈希。
7. 判断是否为短时间内重复内容。
8. 保存内容到数据库。
9. 文本内容异步执行自动分类。
10. 更新 UI。

必须先保存内容，再执行分类。

即使分类脚本报错、卡顿或失败，也不能影响内容记录。

---

# 7. 来源应用

## 7.1 来源记录

每条内容需要记录复制时的前台应用。

需要保存：

* 应用名称
* Bundle ID
* 应用图标

来源应用用于：

* 卡片展示
* 搜索或筛选
* 分类脚本判断
* 帮助用户回忆内容来源

## 7.2 Application 表

应用信息独立保存，避免每条内容重复保存图标。

同一个 Bundle ID 只保存一条应用记录。

如果应用名称或图标发生变化，可以在后续复制时更新。

---

# 8. 自动分类

## 8.1 分类定位

自动分类是 ClipDeck 相比 Paste 的主要扩展功能。

分类可以结合：

* 内容文本
* 来源应用名称
* 来源应用 Bundle ID
* 文本长度
* 复制时间
* 自定义 JavaScript 逻辑

系统初次启动时可以预置几个分类，例如：

* JSON
* SQL
* Java
* JavaScript
* Shell
* Markdown
* XML
* YAML
* URL

系统预置分类和用户新建分类没有任何区别。

用户可以直接：

* 修改分类名称
* 修改图标
* 修改 JS 脚本
* 修改代码预览语言
* 调整分类顺序
* 删除分类

不需要保存系统分类与用户分类的来源区别。

不提供分类停用功能。

用户不需要某个分类时可以直接删除。

## 8.2 单分类原则

首版一条文本内容最多命中一个分类。

分类按照 `sort_order` 顺序依次执行。

规则：

1. 从第一条分类开始执行。
2. 第一个脚本返回 `true` 的分类即为命中分类。
3. 命中后停止执行后续分类。
4. 所有分类均未命中时，内容为普通文本。
5. 单个脚本报错时，将该脚本视为返回 `false`，继续执行后续分类。

这样可以避免：

* 一条卡片展示多个分类标签
* 与 Paste 横向卡片样式冲突
* 分类优先级不明确
* 预览语言无法确定

## 8.3 分类删除

内容表直接保存可空的 `category_id`。

分类删除后：

* 原来命中该分类的内容保留
* 对应内容的 `category_id` 自动变为空
* 内容重新显示为普通文本
* 不删除任何历史内容

数据库外键使用：

```sql
ON DELETE SET NULL
```

## 8.4 分类脚本接口

脚本只接收标准 JSON 数据，不暴露 AppKit 或 Swift 对象。

建议脚本格式：

```javascript
function classify(item) {
    return true;
}
```

输入示例：

```json
{
  "text": "SELECT * FROM user",
  "textLength": 18,
  "sourceApplication": {
    "bundleId": "com.jetbrains.intellij",
    "name": "IntelliJ IDEA"
  },
  "copiedAt": 1785814560000
}
```

脚本返回：

* `true`：命中当前分类
* `false`：不命中当前分类

首版不支持复杂对象返回。

## 8.5 脚本容错

脚本出现以下问题时不能影响主程序：

* 语法错误
* 运行异常
* 返回值不是 Boolean
* 正则表达式异常
* 空指针
* 数据类型错误

处理原则：

* 当前分类视为未命中
* 继续执行后续分类
* 内容正常保存
* 更新分类的最近错误信息
* 不弹出阻塞式错误窗口

分类表可以保存：

* `last_error_message`
* `last_error_at`

不需要保存 `error_count`。

脚本执行必须在后台线程或后台队列中完成。

---

# 9. 分类代码预览

## 9.1 预览方式

对于代码类和结构化文本分类，使用类似 Markdown 代码块的语法高亮效果进行预览。

例如 SQL 分类视觉效果等价于：

````markdown
```sql
SELECT *
FROM user;
````

````

但该 Markdown 结构只是一种预览表达，不会修改或包装原始内容。

数据库中始终保存：

- 原始纯文本
- 原始富文本

不会保存：

- Markdown 代码块标记
- 高亮后的文本
- 格式化后的文本
- 语法高亮结果

## 9.2 预览语言

分类增加：

- `preview_language`

可选值例如：

- 普通文本
- JSON
- SQL
- Java
- JavaScript
- Shell
- Markdown
- XML
- YAML

`preview_language` 只影响 UI 渲染，不参与分类判断。

分类名称与预览语言没有绑定关系。

例如分类名称可以叫：

- 数据库脚本

但预览语言仍然选择：

- SQL

## 9.3 预览渲染

建议使用原生文本组件实现：

- NSTextView
- NSAttributedString
- 轻量语法高亮方案
- Tree-sitter 或其他原生高亮库

不建议仅为了语法高亮引入完整 WKWebView。

卡片预览建议：

- 使用等宽字体
- 展示前若干行
- 超长内容截断
- 默认不显示行号
- 保留原始换行
- 不改变原内容

完整预览可以显示更多内容。

---

# 10. 搜索

## 10.1 搜索范围

首版只搜索：

- `content.plain_text`

不搜索：

- 富文本数据
- 图片
- 图片元信息
- 应用图标
- JavaScript 脚本

搜索结果可以显示：

- 内容预览
- 分类
- 来源应用
- 最后复制时间

## 10.2 搜索实现

使用 SQLite FTS5 建立文本索引。

FTS 索引只同步文本类型内容。

图片不进入 FTS。

可增加虚拟表：

```sql
content_fts
````

索引字段首版可以只包括：

* `plain_text`

后续如增加内容重命名，再加入标题字段。

由于程序员内容主要包括：

* SQL
* JSON
* Java
* 英文报错
* URL
* 标识符
* 数字
* Shell 命令

FTS5 可以覆盖主要使用场景。

中文搜索需要在后续单独评估 tokenizer。

---

# 11. 去重

## 11.1 内容哈希

文本和图片均计算内容哈希。

文本哈希基于：

* 标准化后的纯文本

图片哈希基于：

* 原始入库图片二进制

建议使用：

* SHA-256

## 11.2 去重范围

首版不做永久全局合并。

只处理短时间内重复写入剪贴板的情况，例如：

* 同一应用重复触发写入
* 应用同时写入多个剪贴板表示
* 快速重复复制同一内容

可以设置一个内部去重时间窗口，例如：

* 5 秒
* 10 秒
* 30 秒

短时间命中重复内容时，可以：

* 更新 `last_copied_at`
* 增加 `copy_count`
* 更新来源应用
* 将内容移动到历史最前
* 重新执行分类

---

# 12. 历史过期与清理

## 12.1 保留时长

设置中提供历史保留时长。

范围：

* 1～90 天

建议默认值：

* 30 天

不支持永久保存。

## 12.2 不保存 expires_at

内容表不保存 `expires_at`。

过期判断统一基于：

```text
last_copied_at < 当前时间 - 保留天数
```

这样用户修改保留时长后，不需要更新历史内容。

下一次清理时直接使用新的保留时长。

## 12.3 清理方式

使用后台任务或 Swift Concurrency 执行清理。

触发时机：

* 应用启动后
* 每隔若干小时
* 用户修改保留时长后
* 用户点击清空缓存后

普通清理不能阻塞 UI。

建议每次分批删除，例如：

* 每批 200～500 条

文本、富文本和图片统一清理。

## 12.4 清空缓存

设置页面提供：

* 清空缓存

该功能表示：

1. 删除已经超过保留期限的内容。
2. 执行 WAL checkpoint。
3. 回收 SQLite 空闲空间。
4. 必要时执行完整 VACUUM。

清空缓存不能删除仍在有效期内的内容。

确认提示建议为：

> 将删除已超过保留期限的文本和图片，并回收占用的磁盘空间。有效期内的历史记录不会受到影响。

如果后续需要删除全部历史，必须使用另一个明确功能名称：

* 清空全部历史

不能与清空缓存共用一个名称。

---

# 13. 数据库设计

首版核心表：

1. `application`
2. `category`
3. `content`

另外使用：

4. `content_fts`

`content_fts` 是全文搜索虚拟表，不属于独立业务实体。

## 13.1 Application 表

```sql
CREATE TABLE application (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    bundle_id       TEXT NOT NULL UNIQUE,
    name            TEXT NOT NULL,
    icon_data       BLOB,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
);
```

## 13.2 Category 表

```sql
CREATE TABLE category (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    name                TEXT NOT NULL,
    icon                TEXT,
    script              TEXT NOT NULL,
    preview_language    TEXT,
    sort_order          INTEGER NOT NULL,
    last_error_message  TEXT,
    last_error_at       INTEGER,
    created_at          INTEGER NOT NULL,
    updated_at          INTEGER NOT NULL
);
```

## 13.3 Content 表

```sql
CREATE TABLE content (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,

    content_type        TEXT NOT NULL,

    plain_text          TEXT,
    rich_text           BLOB,
    rich_text_type      TEXT,

    image_data          BLOB,
    image_width         INTEGER,
    image_height        INTEGER,
    image_format        TEXT,

    content_hash        TEXT NOT NULL,
    content_size        INTEGER NOT NULL,

    application_id      INTEGER,
    category_id         INTEGER,

    copied_at           INTEGER NOT NULL,
    last_copied_at      INTEGER NOT NULL,
    copy_count          INTEGER NOT NULL DEFAULT 1,

    created_at          INTEGER NOT NULL,
    updated_at          INTEGER NOT NULL,

    FOREIGN KEY (application_id)
        REFERENCES application(id)
        ON DELETE SET NULL,

    FOREIGN KEY (category_id)
        REFERENCES category(id)
        ON DELETE SET NULL
);
```

## 13.4 内容约束

当 `content_type = TEXT`：

* `plain_text` 必须非空
* `rich_text` 可以为空
* `image_data` 必须为空

当 `content_type = IMAGE`：

* `image_data` 必须非空
* `plain_text` 为空
* `rich_text` 为空
* `category_id` 为空

首版分类仅适用于文本。

图片不执行分类脚本。

## 13.5 索引建议

```sql
CREATE INDEX idx_content_last_copied_at
ON content(last_copied_at DESC);
```

```sql
CREATE INDEX idx_content_category_id
ON content(category_id);
```

```sql
CREATE INDEX idx_content_application_id
ON content(application_id);
```

```sql
CREATE INDEX idx_category_sort_order
ON category(sort_order);
```

可增加短时间去重索引：

```sql
CREATE INDEX idx_content_hash_last_copied
ON content(content_hash, last_copied_at DESC);
```

---

# 14. 界面与交互

## 14.1 主面板

主面板从当前屏幕底部弹出。

主要特征：

* 无普通标题栏
* 横向卡片列表
* 当前卡片有明显选中状态
* 支持键盘左右切换
* 支持鼠标横向滚动
* 支持搜索
* 支持分类切换
* 支持来源应用展示
* 粘贴后自动收起

需要考虑：

* 多显示器
* 当前鼠标所在屏幕
* 当前前台应用所在屏幕
* 全屏应用
* Space
* 窗口层级
* 是否抢焦点
* 搜索状态下接收键盘输入

## 14.2 卡片信息

文本卡片建议显示：

* 分类名称或图标
* 来源应用图标
* 来源应用名称
* 内容预览
* 最后复制时间

图片卡片建议显示：

* 图片缩略图
* 来源应用图标
* 来源应用名称
* 图片尺寸
* 最后复制时间

一条文本内容只显示一个分类。

未分类文本显示为普通文本。

## 14.3 完整预览

支持内容预览。

文本预览：

* 显示完整纯文本
* 分类内容使用语法高亮
* 不修改原文
* 可选择纯文本复制
* 可选择保留格式复制

图片预览：

* 显示原始图片
* 可查看图片尺寸
* 不提供 OCR
* 不提供压缩和编辑

---

# 15. 快捷键

首版至少支持：

* 全局快捷键唤出或收起 ClipDeck
* `Esc`：关闭面板
* `←` / `→`：选择上一条或下一条
* `Return`：按默认配置粘贴
* 可增加修饰键进行另一种格式粘贴
* `Delete`：删除当前内容
* `Command + F`：进入搜索
* `Space`：打开完整预览

快捷键需要支持设置中自定义。

全局快捷键实现可以使用：

* Carbon Hot Key API
* 或稳定的 macOS 快捷键库

---

# 16. 粘贴流程

粘贴流程：

1. 用户在其他应用中触发 ClipDeck。
2. 记录原前台应用。
3. 用户选择历史内容。
4. 根据默认粘贴设置，将内容写回系统剪贴板。
5. 激活原前台应用。
6. 通过辅助功能或 CGEvent 触发 `Command + V`。
7. 面板自动关闭。

如果没有辅助功能权限：

* 只将内容写入系统剪贴板
* 提示用户手动按 `Command + V`
* 引导用户授权辅助功能权限

---

# 17. 设置项

首版设置建议包括：

## 通用

* 开机启动
* 全局唤出快捷键
* 历史保留天数，1～90 天
* 默认粘贴格式

  * 纯文本
  * 保留原格式

## 图片

* 是否记录图片
* 最大图片大小

像素限制不在界面展示。

## 分类

* 分类列表
* 新建分类
* 编辑分类
* 删除分类
* 拖动排序
* JS 脚本编辑
* 预览语言选择
* 脚本测试

## 数据

* 当前数据库占用
* 清空缓存
* 清空全部历史

---

# 18. 明确不做的功能

首版及当前产品方向不考虑：

* OCR
* Pinboards
* Shared Pinboards
* 跨设备同步
* iCloud 同步
* iPhone 版
* iPad 版
* iPhone/iPad 自定义键盘
* Siri Shortcuts
* AI 集成
* MCP 集成
* 团队协作
* 文件剪贴板历史
* 音视频剪贴板历史
* 图片搜索
* 图片压缩
* 图片格式转换
* 一个内容对应多个分类
* 分类停用
* 系统分类保护
* 区分系统分类和用户分类
* 永久保存历史

---

# 19. 推荐开发顺序

## 第一阶段：系统能力原型

先验证三个最关键能力：

1. 监听剪贴板并读取纯文本、富文本和图片。
2. 使用全局快捷键从屏幕底部唤出 NSPanel。
3. 选择内容后粘贴回原前台应用。

该阶段可以先使用内存数组，不接数据库和分类。

## 第二阶段：本地历史

实现：

* SQLite
* GRDB
* Application 表
* Content 表
* 历史列表
* 文本和图片存储
* 删除内容
* 历史过期清理
* 图片大小限制

## 第三阶段：分类

实现：

* Category 表
* 默认分类初始化
* JavaScriptCore
* 分类排序
* 首个命中即停止
* 脚本异常容错
* 删除分类后内容回归普通文本
* 分类设置页面

## 第四阶段：搜索与预览

实现：

* FTS5
* 文本搜索
* 图片不参与搜索
* 分类语法高亮
* 完整内容预览
* 默认粘贴格式

## 第五阶段：体验优化

实现：

* NSCollectionView 复用
* 缩略图缓存
* 列表预加载
* 键盘操作
* 多屏幕
* Space 和全屏应用
* 动画
* 数据库性能优化
* 内存和 CPU 分析

---

# 20. 开发约束

Codex 在生成和修改代码时需要遵循以下原则：

1. 主线程只负责 UI，不执行数据库重操作、图片解析和分类脚本。
2. 内容必须先成功保存，再异步分类。
3. 单个分类脚本失败不能影响其他分类。
4. 单个分类脚本失败不能导致内容丢失。
5. 分类只影响内容归属和预览，不修改原始文本。
6. 纯文本搜索不能解析富文本。
7. 图片不能进入全文搜索。
8. 图片超过大小限制直接忽略，不压缩。
9. 图片像素限制为内部保护，不暴露设置项。
10. 分类删除后只能清空内容的 `category_id`，不能删除内容。
11. 文本最多命中一个分类。
12. 分类排序同时代表匹配优先级。
13. 系统初始分类和用户分类完全一致。
14. 不增加当前需求之外的跨平台抽象。
15. macOS 首版优先保证启动速度、唤出速度、滚动流畅度和低资源占用。
16. 避免在核心卡片列表中使用 WKWebView。
17. 数据库操作统一通过 Repository 或 Store 层，不在 View 中直接执行 SQL。
18. AppKit 窗口逻辑与 SwiftUI 页面状态需要分层管理。
19. 所有后台任务应支持取消或避免重复执行。
20. 每次实现新功能时同步补充测试。

---

# 21. 首版验收标准

首版完成后至少应满足：

* 可以持续监听 macOS 剪贴板
* 可以正确记录纯文本
* 可以正确记录 RTF 富文本
* 可以正确记录限制范围内的图片
* 不记录文件等其他类型
* 可以显示来源应用名称和图标
* 可以通过快捷键快速打开主面板
* 主面板从屏幕底部展示
* 历史记录横向卡片展示
* 可以使用键盘切换内容
* 可以按照默认设置粘贴纯文本或富文本
* 可以恢复图片到剪贴板并粘贴
* 可以搜索纯文本内容
* 图片不会出现在关键字搜索结果中
* 可以新建、编辑、排序和删除分类
* 可以通过 JS 脚本自动分类
* JS 脚本错误不会导致内容丢失
* 同一内容最多命中一个分类
* 分类删除后历史内容仍然存在
* 分类内容可以使用对应语言进行语法高亮预览
* 语法高亮不会修改原始内容
* 可以配置 1～90 天历史保留时间
* 文本与图片都会被过期清理
* 清空缓存可以删除过期内容并回收磁盘空间
* 应用在大量历史内容下仍能快速唤出和流畅滚动
