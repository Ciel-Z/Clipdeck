#include "win_process_util.h"

#include <QByteArray>
#include <QFileInfo>

#include <iterator>

namespace WinProcess {

long integrityLevel(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return -1;
    long rid = -1;
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        if (size > 0) {
            QByteArray buffer(int(size), Qt::Uninitialized);
            auto *label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer.data());
            if (GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) {
                const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
                rid = long(*GetSidSubAuthority(label->Label.Sid, count - 1));
            }
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return rid;
}

QString integrityName(long rid) {
    if (rid < 0) return QStringLiteral("unknown");
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID) return QStringLiteral("system");
    if (rid >= SECURITY_MANDATORY_HIGH_RID) return QStringLiteral("high");
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return QStringLiteral("medium");
    if (rid >= SECURITY_MANDATORY_LOW_RID) return QStringLiteral("low");
    return QStringLiteral("untrusted");
}

QString imagePath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return QString();
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = DWORD(std::size(buffer));
    QString path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) path = QString::fromWCharArray(buffer, int(size));
    CloseHandle(process);
    return path;
}

QString describeWindow(HWND window) {
    if (!window) return QStringLiteral("<none>");
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[128] = {};
    GetClassNameW(window, className, int(std::size(className)));
    const QString image = QFileInfo(imagePath(processId)).fileName();
    return QStringLiteral("hwnd=0x%1 pid=%2 exe=%3 class=%4 integrity=%5")
        .arg(quintptr(window), 0, 16)
        .arg(processId)
        .arg(image.isEmpty() ? QStringLiteral("?") : image, QString::fromWCharArray(className),
             integrityName(integrityLevel(processId)));
}

bool belongsToCurrentProcess(HWND window) {
    if (!window) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

HWND rootOwner(HWND window) { return window ? GetAncestor(window, GA_ROOTOWNER) : nullptr; }

} // namespace WinProcess
