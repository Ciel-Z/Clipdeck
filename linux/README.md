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

当前预览版提供：

- Qt 私有运行库可以在 UOS 中加载
- 托盘图标左键显示 Deck、右键显示菜单
- 透明无底色托盘图标和深色半透明无边框窗口
- Deck 在鼠标所在显示器底部出现，距离左右屏幕边缘各 8 像素
- SQLite/WAL 持久化文本和图片历史，按内容去重并记录复制次数
- 独立 X11 剪贴板读取线程，后台队列处理持久化和来源应用识别，减少界面阻塞
- 最近十条历史缓存与异步查询，预览、编辑和粘贴时按需加载完整内容
- History、Text、Images 与 JSON、URL、SQL、Markdown、代码、色彩分类
- 分类筛选和文本搜索
- 卡片预览、文本编辑、重命名、图片旋转、访问链接与删除
- 来源应用、复制时间、字符数和图片尺寸展示
- `Ctrl+Shift+V` 全局呼出 Deck
- 左右方向键选择，双击卡片或按 `Enter` 自动粘贴
- `Ctrl+1` 至 `Ctrl+9` 快速粘贴前九条记录
- `Delete` 删除当前记录，`Esc` 收起 Deck
- 点击 Deck 右上角“固定”，或按 `Ctrl+P` 固定 Deck；固定后截图工具或其他窗口
  获取焦点时 Deck 不会收起。固定成功后按钮会变为蓝色“已固定”
- 图片大小、历史保留期限、数据库清理和登录自动启动设置

数据库保存在当前用户的 XDG 应用数据目录。macOS 版的 Command 快捷键在 UOS
上统一映射为 Ctrl。实时背景模糊取决于 UOS 窗口合成器；不支持时使用深色半透明
材质降级。

## 公共资源与自检

应用 SVG 和图标 PNG 统一来自仓库根目录 `shared/assets/`；分类脚本编辑器使用
`script-editor/dist/`，通过 Qt WebEngine 加载。更新编辑器后应先重新构建其 `dist/`。

可通过 `CLIPDECK_VERSION` 指定打包版本。构建后的二进制支持内置自检：

```bash
QT_QPA_PLATFORM=offscreen QTWEBENGINE_DISABLE_SANDBOX=1 \
  linux/.build/amd64-release/clipdeck-linux --self-test
```

自检覆盖历史存储、分类、后台剪贴板队列、异步内容加载和粘贴期间的新剪贴板内容保护。
