# 运行 ClipDeck

ClipDeck 是 macOS GUI 应用，必须以 `.app` 运行。不要使用 `swift run ClipDeck`；SwiftPM 的裸可执行文件不具备稳定的应用 Bundle、签名和系统权限身份。

## Xcode

1. 打开 `ClipDeck.xcodeproj`，不要打开 `Package.swift`。
2. Scheme 选择 `ClipDeck`，Destination 选择 `My Mac`。
3. 点击 Run。
4. 菜单栏出现剪贴板图标后，按 `Command + Shift + V` 唤出或收起 Deck。

工程使用 `Sign to Run Locally` 的 ad-hoc 签名，不访问开发者证书私钥，因此正常运行不应要求输入钥匙串密码。

## 命令行

```bash
./scripts/run-debug.sh
```

## 辅助功能权限

唤出 Deck、查看历史和把内容写回剪贴板不需要辅助功能权限。只有自动向原应用发送 `Command + V` 时才需要在“系统设置 → 隐私与安全性 → 辅助功能”中授权 ClipDeck；未授权时会降级为写入剪贴板并提示手动粘贴。

## 快捷键诊断

菜单栏的 ClipDeck 菜单会显示快捷键注册状态。若显示注册失败，会同时显示 Carbon OSStatus；通常表示该组合键已被另一款常驻工具占用。
