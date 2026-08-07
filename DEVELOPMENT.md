# ClipDeck 开发进度

本文件按 [readme.md](readme.md) 第 19 节的推荐顺序跟踪实现。需求与产品边界始终以 `readme.md` 为准。

## 第一阶段：系统能力原型

- [x] 原生 Swift/AppKit + SwiftUI 工程骨架
- [x] 使用 `NSPasteboard.changeCount` 每 400ms 监听剪贴板
- [x] 读取纯文本和 RTF
- [x] 读取 PNG/TIFF 图片，并执行 60,000,000 像素安全限制
- [x] 忽略文件剪贴板内容
- [x] 内存历史、横向卡片与键盘选择/删除
- [x] 记录来源应用名称、Bundle ID 和图标
- [x] 鼠标所在屏幕底部的 `NSPanel`
- [x] Carbon 全局快捷键 `Command + Shift + V`
- [x] 标准 Xcode `.app` target 与本地 ad-hoc 签名
- [x] 写回文本或原格式图片、激活原应用并触发粘贴
- [x] 无辅助功能权限时降级为仅写入剪贴板
- [x] 核心单元测试

## 第二阶段：本地历史

- [x] 引入 GRDB.swift 与 SQLite migration
- [x] Application、Content 表和 Repository 层
- [x] 10 秒哈希去重与持久化历史列表
- [x] 文本、RTF、图片 BLOB 存储
- [ ] 图片字节大小设置（已实现默认 10 MB 限制）
- [x] 删除与默认 30 天分批过期清理
- [ ] WAL checkpoint 与 VACUUM

## 第三阶段：分类

- [x] Category 表与默认分类
- [x] JavaScriptCore 分类器和错误隔离
- [x] 首个命中停止、排序和删除回退
- [x] 分类管理、JS 语法高亮编辑与脚本测试界面

## 第四阶段：搜索与预览

- [x] FTS5 文本索引和搜索
- [x] 完整预览与轻量语法高亮
- [x] 默认纯文本/保留 RTF 粘贴设置

## 第五阶段：体验优化

- [ ] NSCollectionView、缩略图缓存和预加载
- [ ] 快捷键自定义、搜索键盘流和完整预览键盘流
- [ ] 多屏幕、全屏与 Space 场景验证
- [ ] 动画、数据库性能、内存和 CPU 分析
