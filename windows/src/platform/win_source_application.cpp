#include "win_source_application.h"

#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <windows.h>
#include <shellapi.h>

namespace {

QMutex cacheMutex;
QHash<QString, SourceApplicationInfo> cache;

QString processPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return QString();
    wchar_t buffer[MAX_PATH * 4];
    DWORD size = DWORD(sizeof(buffer) / sizeof(buffer[0]));
    const bool ok = QueryFullProcessImageNameW(process, 0, buffer, &size) != FALSE;
    CloseHandle(process);
    return ok ? QString::fromWCharArray(buffer, int(size)) : QString();
}

QString pathForWindow(quint32 handle) {
    if (!handle) return QString();
    HWND window = reinterpret_cast<HWND>(quintptr(handle));
    if (!IsWindow(window)) return QString();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return QString();
    const QString path = processPath(pid);
    // ApplicationFrameHost only hosts UWP chrome; the owner window names the app.
    if (path.endsWith(QLatin1String("ApplicationFrameHost.exe"), Qt::CaseInsensitive)) return QString();
    return path;
}

QImage iconForPath(const QString &path) {
    SHFILEINFOW info{};
    const std::wstring native = path.toStdWString();
    if (!SHGetFileInfoW(native.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon) {
        return QImage();
    }
    QImage image = QImage::fromHICON(info.hIcon);
    DestroyIcon(info.hIcon);
    return image;
}

} // namespace

QString windowsExecutableDescription(const QString &path) {
    const std::wstring native = path.toStdWString();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(native.c_str(), &handle);
    if (size == 0) return QString();
    QByteArray block(int(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(native.c_str(), 0, size, block.data())) return QString();
    struct Translation { WORD language; WORD codePage; } *translation = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translation), &length)
        || length < sizeof(Translation)) {
        return QString();
    }
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", translation->language, translation->codePage);
    wchar_t *value = nullptr;
    UINT valueLength = 0;
    if (!VerQueryValueW(block.data(), key, reinterpret_cast<void **>(&value), &valueLength) || !value) return QString();
    return QString::fromWCharArray(value).trimmed();
}

SourceApplicationInfo windowsSourceApplication(quint32 sourceWindow, quint32 ownerWindow) {
    QString path = pathForWindow(ownerWindow);
    if (path.isEmpty()) path = pathForWindow(sourceWindow);
    if (path.isEmpty()) return SourceApplicationInfo();

    {
        QMutexLocker locker(&cacheMutex);
        const auto found = cache.constFind(path);
        if (found != cache.constEnd()) return found.value();
    }
    SourceApplicationInfo info;
    info.identifier = path;
    QString name = windowsExecutableDescription(path);
    if (name.isEmpty()) name = QFileInfo(path).completeBaseName();
    info.name = name.isEmpty() ? QStringLiteral("Unknown") : name;
    QImage icon = iconForPath(path);
    if (!icon.isNull()) {
        info.icon = icon;
        QBuffer buffer(&info.iconData);
        buffer.open(QIODevice::WriteOnly);
        icon.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG");
    }
    QMutexLocker locker(&cacheMutex);
    cache.insert(path, info);
    return info;
}
