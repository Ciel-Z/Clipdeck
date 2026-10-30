#pragma once

#include <QIcon>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

#include <memory>

class QAction;
class QMenu;

// Port of AppCoordinator.configureStatusItem / makeStatusMenu: left click
// toggles the Deck, right click opens a menu with "设置…" and "退出 ClipDeck".
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

Q_SIGNALS:
    void toggleRequested();
    void settingsRequested();
    void quitRequested();

private:
    void onActivated(QSystemTrayIcon::ActivationReason reason);

    QSystemTrayIcon tray_;
    std::unique_ptr<QMenu> menu_;
    QAction *settingsAction_ = nullptr;
    QAction *quitAction_ = nullptr;
};
