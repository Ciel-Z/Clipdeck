#include "win_tray.h"

#include <QAction>
#include <QMenu>
#include <QSettings>

WinTray::WinTray(const QIcon &icon, QObject *parent) : QObject(parent), tray_(icon), menu_(new QMenu) {
    // The shortcut text mirrors the mac menu key equivalents (⌘, and ⌘Q).
    settingsAction_ = menu_->addAction(QStringLiteral("设置…\tCtrl+,"));
    menu_->addSeparator();
    quitAction_ = menu_->addAction(QStringLiteral("退出 ClipDeck\tCtrl+Q"));
    connect(settingsAction_, &QAction::triggered, this, &WinTray::settingsRequested);
    connect(quitAction_, &QAction::triggered, this, &WinTray::quitRequested);

    tray_.setContextMenu(menu_.get());
    setHotkeyError(QString());
    connect(&tray_, &QSystemTrayIcon::activated, this, &WinTray::onActivated);
}

WinTray::~WinTray() {
    tray_.setContextMenu(nullptr);
    tray_.hide();
}

void WinTray::show() { tray_.show(); }
void WinTray::hide() { tray_.hide(); }
bool WinTray::isVisible() const { return tray_.isVisible(); }

void WinTray::setHotkeyError(const QString &error) {
    tray_.setToolTip(error.isEmpty() ? QStringLiteral("左键显示 ClipDeck，右键打开菜单")
                                     : QStringLiteral("全局快捷键注册失败：%1").arg(error));
}

bool WinTray::showFirstRunHintIfNeeded(const QString &hotkeyName) {
    QSettings settings;
    if (settings.value(QStringLiteral("app/trayHintShown"), false).toBool()) return false;
    settings.setValue(QStringLiteral("app/trayHintShown"), true);
    tray_.showMessage(QStringLiteral("ClipDeck 已在后台运行"),
                      QStringLiteral("按 %1 或点击托盘图标呼出。若看不到图标，请在任务栏 ^ 溢出区把它拖出来。")
                          .arg(hotkeyName),
                      QSystemTrayIcon::Information, 8000);
    return true;
}

void WinTray::showCopiedOnlyMessage(const QString &reason) {
    tray_.showMessage(QStringLiteral("内容已复制到剪贴板"), reason, QSystemTrayIcon::Information, 4000);
}

void WinTray::onActivated(QSystemTrayIcon::ActivationReason reason) {
    // Windows reports Trigger and then DoubleClick for a double click; the
    // Deck toggles on the first and ignores the second, like a single mac click.
    if (reason == QSystemTrayIcon::Trigger) Q_EMIT toggleRequested();
}
