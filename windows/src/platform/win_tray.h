#pragma once

#include <QIcon>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

#include <memory>

class QAction;
class QMenu;

// Port of AppCoordinator.configureStatusItem / makeStatusMenu: left click
// toggles the Deck, right click opens a menu with "设置…", "背景模糊" (as on
// Linux) and "退出 ClipDeck".
class WinTray final : public QObject {
    Q_OBJECT

public:
    explicit WinTray(const QIcon &icon, QObject *parent = nullptr);
    ~WinTray() override;

    void show();
    void hide();
    bool isVisible() const;

    // "左键显示 ClipDeck，右键打开菜单", or the hot key failure text.
    void setHotkeyError(const QString &error);
    QString toolTip() const { return tray_.toolTip(); }

    // Win10 collapses new tray icons into the overflow area; tell the user once.
    bool showFirstRunHintIfNeeded(const QString &hotkeyName);
    // mac copiedOnly feedback when Ctrl+V cannot be sent (elevated target).
    void showCopiedOnlyMessage(const QString &reason);

    // Drives the same path as a shell click (used by tests).
    void simulateActivation(QSystemTrayIcon::ActivationReason reason) { onActivated(reason); }

    QMenu *menu() const { return menu_.get(); }
    QAction *settingsAction() const { return settingsAction_; }
    QAction *quitAction() const { return quitAction_; }
    QAction *blurAction() const { return blurAction_; }
    QMenu *syncMenu() const { return syncMenu_; }
    // "局域网同步" submenu (docs/lan-sync-design.md §8): status line, visibility
    // with its countdown, pause, sync now, and the sync panel.
    void setSyncState(const QString &status, bool visible, int secondsLeft, bool paused, bool available);
    // Reflects the stored setting without emitting blurToggled.
    void setBlurChecked(bool checked);

Q_SIGNALS:
    void toggleRequested();
    void settingsRequested();
    void quitRequested();
    void blurToggled(bool enabled);
    void syncVisibilityToggled(bool visible);
    void syncPauseToggled(bool paused);
    void syncNowRequested();
    void syncPanelRequested();

private:
    void onActivated(QSystemTrayIcon::ActivationReason reason);

    QSystemTrayIcon tray_;
    std::unique_ptr<QMenu> menu_;
    QAction *settingsAction_ = nullptr;
    QAction *quitAction_ = nullptr;
    QAction *blurAction_ = nullptr;
    QMenu *syncMenu_ = nullptr;
    QAction *syncStatusAction_ = nullptr;
    QAction *syncVisibleAction_ = nullptr;
    QAction *syncPauseAction_ = nullptr;
    QAction *syncNowAction_ = nullptr;
    QAction *syncPanelAction_ = nullptr;
};
