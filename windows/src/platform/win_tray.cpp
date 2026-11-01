#include "win_tray.h"
#include "win_window_effects.h"

#include <QAction>
#include <QMenu>
#include <QSettings>

namespace {

// The menu style shared with the macOS menu bar menu (docs/lan-sync-design.md
// §8.1): dark, 24 px rows, a 16 px check column, thin separators. DWM adds the
// rounded corners and shadow.
void styleMenu(QMenu *menu) {
    menu->setStyleSheet(QStringLiteral(
        "QMenu{background:#2b2b2e;color:#eeeeef;border:1px solid rgba(255,255,255,30);padding:4px;"
        "font-family:'Segoe UI';font-size:13px;}"
        "QMenu::item{padding:4px 24px 4px 26px;min-height:16px;border-radius:5px;}"
        "QMenu::item:selected{background:#0A84FF;color:white;}"
        "QMenu::item:disabled{color:#8e8e93;background:transparent;}"
        "QMenu::separator{height:1px;background:rgba(255,255,255,30);margin:4px 8px;}"
        "QMenu::indicator{width:14px;height:14px;left:7px;}"
        "QMenu::right-arrow{width:10px;height:10px;right:8px;}"));
    QObject::connect(menu, &QMenu::aboutToShow, menu,
                     [menu] { WinEffects::setRoundedPopup(reinterpret_cast<HWND>(menu->winId())); });
}

} // namespace

WinTray::WinTray(const QIcon &icon, QObject *parent) : QObject(parent), tray_(icon), menu_(new QMenu) {
    // The shortcut text mirrors the mac menu key equivalents (⌘, and ⌘Q).
    settingsAction_ = menu_->addAction(QStringLiteral("设置…\tCtrl+,"));
    // Same item as the Linux tray: frosted backdrop on, or the live desktop.
    blurAction_ = menu_->addAction(QStringLiteral("背景模糊"));
    blurAction_->setCheckable(true);
    syncMenu_ = new QMenu(QStringLiteral("局域网同步"), menu_.get());
    syncStatusAction_ = syncMenu_->addAction(QStringLiteral("未配对设备"));
    syncStatusAction_->setEnabled(false);
    syncMenu_->addSeparator();
    syncVisibleAction_ = syncMenu_->addAction(QStringLiteral("局域网可见"));
    syncVisibleAction_->setCheckable(true);
    syncPauseAction_ = syncMenu_->addAction(QStringLiteral("暂停同步"));
    syncPauseAction_->setCheckable(true);
    syncNowAction_ = syncMenu_->addAction(QStringLiteral("立即同步"));
    syncMenu_->addSeparator();
    syncPanelAction_ = syncMenu_->addAction(QStringLiteral("同步设备…"));
    menu_->insertMenu(settingsAction_, syncMenu_)->setText(QStringLiteral("局域网同步"));
    menu_->insertSeparator(settingsAction_);
    connect(syncVisibleAction_, &QAction::toggled, this, &WinTray::syncVisibilityToggled);
    connect(syncPauseAction_, &QAction::toggled, this, &WinTray::syncPauseToggled);
    connect(syncNowAction_, &QAction::triggered, this, &WinTray::syncNowRequested);
    connect(syncPanelAction_, &QAction::triggered, this, &WinTray::syncPanelRequested);
    styleMenu(menu_.get());
    styleMenu(syncMenu_);
    menu_->addSeparator();
    quitAction_ = menu_->addAction(QStringLiteral("退出 ClipDeck\tCtrl+Q"));
    connect(settingsAction_, &QAction::triggered, this, &WinTray::settingsRequested);
    connect(blurAction_, &QAction::toggled, this, &WinTray::blurToggled);
    connect(quitAction_, &QAction::triggered, this, &WinTray::quitRequested);

    tray_.setContextMenu(menu_.get());
    setHotkeyError(QString());
    connect(&tray_, &QSystemTrayIcon::activated, this, &WinTray::onActivated);
}

WinTray::~WinTray() {
    tray_.setContextMenu(nullptr);
    tray_.hide();
}

void WinTray::setSyncState(const QString &status, bool visible, int secondsLeft, bool paused, bool available) {
    syncStatusAction_->setText(status);
    const QSignalBlocker visibleBlocker(syncVisibleAction_);
    syncVisibleAction_->setChecked(visible);
    syncVisibleAction_->setText(visible ? QStringLiteral("局域网可见 (%1:%2)")
                                              .arg(secondsLeft / 60)
                                              .arg(secondsLeft % 60, 2, 10, QLatin1Char('0'))
                                        : QStringLiteral("局域网可见"));
    const QSignalBlocker pauseBlocker(syncPauseAction_);
    syncPauseAction_->setChecked(paused);
    for (QAction *action : {syncVisibleAction_, syncPauseAction_, syncNowAction_, syncPanelAction_})
        action->setEnabled(available);
    syncNowAction_->setEnabled(available && !paused);
}

void WinTray::setBlurChecked(bool checked) {
    const QSignalBlocker blocker(blurAction_);
    blurAction_->setChecked(checked);
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
