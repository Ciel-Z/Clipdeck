#include "win_launch_at_login.h"

#include <QDir>

#include <windows.h>
#include <shellapi.h>

namespace {

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

} // namespace

WinLaunchAtLogin::WinLaunchAtLogin(QString executablePath, QString valueName)
    : executablePath_(std::move(executablePath)), valueName_(std::move(valueName)) {}

QString WinLaunchAtLogin::command() const {
    return QLatin1Char('"') + QDir::toNativeSeparators(executablePath_) + QLatin1Char('"');
}

bool WinLaunchAtLogin::isRegistered() const {
    wchar_t buffer[2048];
    DWORD size = sizeof(buffer);
    const std::wstring name = valueName_.toStdWString();
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, name.c_str(), RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return false;
    }
    return QString::fromWCharArray(buffer).compare(command(), Qt::CaseInsensitive) == 0;
}

bool WinLaunchAtLogin::isDisabledInStartupApps() const {
    BYTE data[12] = {};
    DWORD size = sizeof(data);
    const std::wstring name = valueName_.toStdWString();
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, name.c_str(), RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS
        || size == 0) {
        return false;
    }
    return (data[0] & 0x01) != 0; // 0x02 enabled, 0x03 disabled by the user
}

QString WinLaunchAtLogin::setEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return QStringLiteral("无法打开启动项注册表");
    }
    const std::wstring name = valueName_.toStdWString();
    LSTATUS status;
    if (enabled) {
        const std::wstring value = command().toStdWString();
        status = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                                DWORD((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, name.c_str());
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? QString() : QStringLiteral("更新启动项失败（%1）").arg(status);
}

QString WinLaunchAtLogin::statusMessage() const {
    if (isRegistered() && isDisabledInStartupApps()) {
        return QStringLiteral("已在“设置 → 应用 → 启动”中被关闭，需要在那里重新允许。");
    }
    return QString();
}

void WinLaunchAtLogin::openStartupAppsSettings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL);
}
