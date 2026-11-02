# ClipDeck Linux 构建环境

该目录使用 AMD64 Debian 10 容器为 UOS 20 构建 Linux 二进制。容器以
glibc 2.28 为兼容基线，不使用目标 UOS 中版本不一致的 Qt/DTK 开发包。

## 构建

Mac 需要安装并启动 Docker Desktop，然后在仓库根目录执行：

```bash
bash linux/scripts/build-linux.sh
```

首次执行会联网下载 Debian 10 基础镜像和归档软件包。镜像生成后，实际的
编译和打包步骤使用 `--network none`，保证源码没有隐藏的在线依赖。之后执行
脚本会直接复用本地镜像；需要更新构建环境时使用：

```bash
CLIPDECK_REBUILD_IMAGE=1 bash linux/scripts/build-linux.sh
```

产物位于：

```text
artifacts/linux/clipdeck_<版本号>_amd64.deb
```

## UOS 离线安装

将 `.deb` 复制到 UOS 20 AMD64 机器：

```bash
sudo apt install './clipdeck_<版本号>_amd64.deb'
clipdeck
```

若离线 `apt` 尝试访问软件源，可改用：

```bash
sudo dpkg -i './clipdeck_<版本号>_amd64.deb'
```

当前代码提供（不代表所有旧安装包均已包含）：

- Qt 私有运行库可以在 UOS 中加载
- 托盘图标左键显示 Deck、右键显示菜单
- 透明无底色托盘图标和深色半透明无边框窗口
- Deck 在鼠标所在显示器底部出现，距离左右屏幕边缘各 8 像素
- SQLite/WAL 持久化文本、富文本、图片与文件引用历史，按近 24 小时内容去重并记录复制次数
- 独立 X11 剪贴板读取线程，后台队列处理持久化和来源应用识别，减少界面阻塞
- 最近十条历史缓存与异步查询，预览、编辑和粘贴时按需加载完整内容
- History、文本、图片、文件四个固定入口，以及 JSON、URL、SQL、Markdown、代码、色彩分类
- 分类与关键词组合筛选；分类可调整顺序、颜色、卡片类型和 JavaScript 规则
- 卡片预览、文本编辑、重命名、图片旋转、访问链接与删除
- 来源应用、复制时间、字符数和图片尺寸展示
- `Alt+Shift+V` 默认全局呼出 Deck（可配置；已有用户设置优先）
- 左右方向键选择，双击卡片或按 `Enter` 自动粘贴
- `Ctrl+1` 至 `Ctrl+9` 快速粘贴前九条记录
- `Shift+Delete` 删除当前记录，`Esc` 分层退出 / 收起 Deck
- `Ctrl+C` 复制卡片并保留 Deck，`Ctrl+←/→` 切换分类，`Ctrl+,` 打开设置
- 普通文本自动换行与富文本预览；图片预览按宽高比缩放，保留最大尺寸边界
- 文件 / 文件夹按一次复制生成一张卡片，仅记录引用，不备份文件；粘贴前检查可用性，部分可用时确认后粘贴
- 已配对设备间加密的局域网同步，范围为最近 24 小时文本与图片；文件引用不同步
- 支持登录自动启动、历史保留、容量限制和数据库清理

数据库保存在当前用户的 XDG 应用数据目录。三端默认快捷键对照见
[根 README](../readme.md#默认快捷键)，不能将 Command 全部机械替换为 Ctrl。
实时背景模糊取决于 UOS 窗口合成器；不支持时使用深色半透明材质降级。
当前实现面向 X11，不代表已支持 Wayland 或所有 Linux 发行版。

## 公共资源与自检

应用 SVG 和图标 PNG 统一来自仓库根目录 `shared/assets/`；分类脚本编辑器使用
`script-editor/dist/`，通过 Qt WebEngine 加载。更新编辑器后应先重新构建其 `dist/`。

可通过 `CLIPDECK_VERSION` 指定打包版本。构建后的二进制支持内置自检：

```bash
QT_QPA_PLATFORM=offscreen QTWEBENGINE_DISABLE_SANDBOX=1 \
  linux/.build/amd64-release/clipdeck-linux --self-test
```

自检覆盖历史存储、分类、后台剪贴板队列、异步内容加载和粘贴期间的新剪贴板内容保护。
