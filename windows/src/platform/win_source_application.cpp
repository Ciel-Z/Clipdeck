#include "win_source_application.h"

#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace {

// Stored icon size: the 58 px header badge at 200% scaling stays sharp.
constexpr int kSourceIconPixels = 128;

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

// The shell returns a 32-bit DIB section. QImage::fromHBITMAP drops its alpha
// (transparent margins turn black), so copy the bits directly. Icon frames
// come back with straight alpha; reading them as premultiplied brightens the
// anti-aliased edge into a light rim.
QImage imageFromShellBitmap(HBITMAP bitmap) {
    DIBSECTION section{};
    if (GetObjectW(bitmap, sizeof(section), &section) != sizeof(section) || section.dsBm.bmBitsPixel != 32
        || !section.dsBm.bmBits) {
        return QImage::fromHBITMAP(bitmap);
    }
    const int width = section.dsBm.bmWidth;
    const int height = section.dsBm.bmHeight;
    const bool bottomUp = section.dsBmih.biHeight > 0;
    QImage image(width, height, QImage::Format_ARGB32);
    const auto *bits = static_cast<const uchar *>(section.dsBm.bmBits);
    bool hasAlpha = false;
    bool straight = false;
    for (int y = 0; y < height; ++y) {
        const uchar *source = bits + qsizetype(bottomUp ? height - 1 - y : y) * section.dsBm.bmWidthBytes;
        memcpy(image.scanLine(y), source, size_t(width) * 4);
        const auto *pixels = reinterpret_cast<const QRgb *>(source);
        for (int x = 0; x < width; ++x) {
            const int alpha = qAlpha(pixels[x]);
            hasAlpha = hasAlpha || alpha != 0;
            straight = straight || qRed(pixels[x]) > alpha || qGreen(pixels[x]) > alpha || qBlue(pixels[x]) > alpha;
        }
    }
    // Icons without an alpha mask come back with alpha 0 everywhere.
    if (!hasAlpha) return image.convertToFormat(QImage::Format_RGB32);
    // A premultiplied bitmap never has a channel above its alpha.
    if (!straight) image.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// The shell picks the largest frame in the executable (up to 256 px) and
// scales it to the requested size; SHGFI_LARGEICON is only 32 px, which looks
// soft once the badge is drawn at 40+ device pixels.
QImage shellIcon(const std::wstring &native, int size) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    QImage image;
    IShellItemImageFactory *factory = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(native.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(factory->GetImage(SIZE{size, size}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) && bitmap) {
            image = imageFromShellBitmap(bitmap);
            DeleteObject(bitmap);
        }
        factory->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return image;
}

QImage iconForPath(const QString &path) {
    const std::wstring native = path.toStdWString();
    QImage image = shellIcon(native, kSourceIconPixels);
    if (!image.isNull()) return image;
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(native.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon) {
        return QImage();
    }
    image = QImage::fromHICON(info.hIcon);
    DestroyIcon(info.hIcon);
    return image;
}

} // namespace

QImage windowsApplicationIcon(const QString &path) {
    {
        QMutexLocker locker(&cacheMutex);
        const auto found = cache.constFind(path);
        if (found != cache.constEnd()) return found.value().icon;
    }
    return iconForPath(path);
}

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
        if (icon.width() > kSourceIconPixels) {
            icon = icon.scaled(kSourceIconPixels, kSourceIconPixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        icon.save(&buffer, "PNG");
    }
    QMutexLocker locker(&cacheMutex);
    cache.insert(path, info);
    return info;
}
