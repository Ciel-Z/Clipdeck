#pragma once

#include <QString>

// Port of LaunchAtLoginController: a value under
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run. Task Manager's
// "Startup apps" can disable the entry without removing it; that state lives
// in Explorer\StartupApproved\Run and is reported, not overridden.
class WinLaunchAtLogin {
public:
    explicit WinLaunchAtLogin(QString executablePath, QString valueName = QStringLiteral("ClipDeck"));

    bool isRegistered() const;           // the Run value points at this executable
    bool isDisabledInStartupApps() const;
    bool isOn() const { return isRegistered() && !isDisabledInStartupApps(); }
    // Empty on success, else a user-facing error.
    QString setEnabled(bool enabled);
    // mac LaunchAtLoginController.statusMessage
    QString statusMessage() const;

    static void openStartupAppsSettings();

private:
    QString command() const;
    QString executablePath_;
    QString valueName_;
};
