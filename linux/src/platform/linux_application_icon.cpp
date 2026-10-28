#include "linux_application_icon.h"

#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QMap>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <limits>

namespace {

constexpr quint32 kIconSetMagic = 0x43444943; // CDIC
constexpr quint16 kIconSetVersion = 1;

struct IconCandidate {
    QString path;
    int score = 0;
    int width = 0;
    int height = 0;
};

QByteArray readLimitedFile(const QString &path, qint64 maximumBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximumBytes) return {};
    return file.readAll();
}

QImage fittedImage(const QImage &source, const QSize &target, qreal dpr) {
    if (source.isNull() || !target.isValid()) return {};
    const QImage scaled = source.size() == target
        ? source
        : source.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage canvas(target, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QPoint((target.width() - scaled.width()) / 2,
                             (target.height() - scaled.height()) / 2), scaled);
    painter.end();
    canvas.setDevicePixelRatio(std::max<qreal>(1.0, dpr));
    return canvas;
}

int candidateScore(const QString &path, int rootIndex, const QString &theme) {
    const QString lower = path.toLower();
    int score = 1000 - rootIndex * 50;
    if (!theme.isEmpty() && lower.contains(QLatin1Char('/') + theme + QLatin1Char('/'))) score += 500;
    if (lower.contains(QStringLiteral("/hicolor/"))) score += 300;
    if (lower.contains(QStringLiteral("/apps/"))) score += 40;
    return score;
}

QVector<IconCandidate> iconCandidates(const QString &iconValue) {
    QVector<IconCandidate> result;
    const QString icon = iconValue.trimmed();
    if (icon.isEmpty()) return result;
    const QFileInfo direct(icon);
    if (direct.isAbsolute() && direct.isFile()) {
        result.append({direct.canonicalFilePath(), 5000, 0, 0});
        return result;
    }

    QString iconName = QFileInfo(icon).fileName();
    const QString suffix = QFileInfo(iconName).suffix().toLower();
    if (suffix == QStringLiteral("png") || suffix == QStringLiteral("svg")
        || suffix == QStringLiteral("xpm")) iconName.chop(suffix.size() + 1);
    const QStringList roots = {
        QDir::home().filePath(QStringLiteral(".local/share/icons")),
        QDir::home().filePath(QStringLiteral(".icons")),
        QStringLiteral("/usr/share/icons"), QStringLiteral("/usr/local/share/icons"),
        QStringLiteral("/usr/share/pixmaps")
    };
    const QString theme = QIcon::themeName().toLower();
    QSet<QString> visited;
    for (int rootIndex = 0; rootIndex < roots.size(); ++rootIndex) {
        if (!QFileInfo(roots[rootIndex]).isDir()) continue;
        QDirIterator iterator(roots[rootIndex],
            QStringList{iconName + QStringLiteral(".png"), iconName + QStringLiteral(".svg"),
                        iconName + QStringLiteral(".xpm")},
            QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty() || visited.contains(canonical)) continue;
            visited.insert(canonical);
            result.append({canonical, candidateScore(canonical, rootIndex, theme), 0, 0});
        }
    }
    return result;
}

} // namespace

bool LinuxApplicationIconSet::isEmpty() const {
    return svgData.isEmpty() && pngVariants.isEmpty();
}

QByteArray LinuxApplicationIconSet::serialize() const {
    if (isEmpty()) return {};
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_11);
    stream << kIconSetMagic << kIconSetVersion << svgData;
    stream << static_cast<quint16>(pngVariants.size());
    for (const LinuxPngIconVariant &variant : pngVariants) {
        stream << static_cast<quint16>(variant.width)
               << static_cast<quint16>(variant.height) << variant.data;
    }
    return stream.status() == QDataStream::Ok ? result : QByteArray();
}

LinuxApplicationIconSet LinuxApplicationIconSet::deserialize(const QByteArray &data) {
    LinuxApplicationIconSet result;
    if (data.isEmpty()) return result;
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_5_11);
    quint32 magic = 0;
    quint16 version = 0;
    quint16 count = 0;
    stream >> magic >> version;
    if (magic != kIconSetMagic || version != kIconSetVersion) return {};
    stream >> result.svgData >> count;
    if (count > 32) return {};
    for (quint16 index = 0; index < count; ++index) {
        quint16 width = 0;
        quint16 height = 0;
        QByteArray png;
        stream >> width >> height >> png;
        if (width > 0 && height > 0 && !png.isEmpty()) {
            result.pngVariants.append({width, height, std::move(png)});
        }
    }
    if (stream.status() != QDataStream::Ok) return {};
    return result;
}

QImage LinuxApplicationIconSet::rasterize(const QSize &targetPhysicalSize, qreal dpr) const {
    if (!targetPhysicalSize.isValid()) return {};
    if (!svgData.isEmpty()) {
        QBuffer buffer;
        buffer.setData(svgData);
        if (buffer.open(QIODevice::ReadOnly)) {
            QImageReader reader(&buffer, "svg");
            reader.setScaledSize(targetPhysicalSize);
            const QImage svg = reader.read();
            if (!svg.isNull()) return fittedImage(svg, targetPhysicalSize, dpr);
        }
    }
    if (pngVariants.isEmpty()) return {};

    const int targetPixels = std::max(targetPhysicalSize.width(), targetPhysicalSize.height());
    const LinuxPngIconVariant *selected = nullptr;
    int selectedPixels = std::numeric_limits<int>::max();
    for (const LinuxPngIconVariant &variant : pngVariants) {
        const int pixels = std::max(variant.width, variant.height);
        if (pixels >= targetPixels && pixels < selectedPixels) {
            selected = &variant;
            selectedPixels = pixels;
        }
    }
    if (!selected) {
        selectedPixels = -1;
        for (const LinuxPngIconVariant &variant : pngVariants) {
            const int pixels = std::max(variant.width, variant.height);
            if (pixels > selectedPixels) {
                selected = &variant;
                selectedPixels = pixels;
            }
        }
    }
    if (!selected) return {};
    return fittedImage(QImage::fromData(selected->data, "PNG"), targetPhysicalSize, dpr);
}

LinuxApplicationIconSet resolveLinuxApplicationIconSet(const QString &iconValue) {
    static thread_local QMap<QString, LinuxApplicationIconSet> cache;
    if (cache.contains(iconValue)) return cache.value(iconValue);

    LinuxApplicationIconSet result;
    QVector<IconCandidate> candidates = iconCandidates(iconValue);
    IconCandidate bestSvg;
    bestSvg.score = std::numeric_limits<int>::min();
    QMap<QPair<int, int>, IconCandidate> bestPngBySize;
    IconCandidate bestFallback;
    bestFallback.score = std::numeric_limits<int>::min();
    for (IconCandidate candidate : candidates) {
        const QString suffix = QFileInfo(candidate.path).suffix().toLower();
        if (suffix == QStringLiteral("svg")) {
            if (candidate.score > bestSvg.score) bestSvg = candidate;
            continue;
        }
        QImageReader reader(candidate.path);
        const QSize size = reader.size();
        if (!size.isValid()) continue;
        candidate.width = size.width();
        candidate.height = size.height();
        if (suffix == QStringLiteral("png")) {
            const QPair<int, int> key(size.width(), size.height());
            if (!bestPngBySize.contains(key) || candidate.score > bestPngBySize[key].score) {
                bestPngBySize[key] = candidate;
            }
        } else if (candidate.score > bestFallback.score) {
            bestFallback = candidate;
        }
    }

    if (!bestSvg.path.isEmpty()) {
        result.svgData = readLimitedFile(bestSvg.path, 2 * 1024 * 1024);
        if (!result.svgData.isEmpty()) result.selectedPath = bestSvg.path;
    }
    QList<IconCandidate> pngs = bestPngBySize.values();
    std::sort(pngs.begin(), pngs.end(), [](const IconCandidate &left, const IconCandidate &right) {
        return std::max(left.width, left.height) < std::max(right.width, right.height);
    });
    for (const IconCandidate &candidate : pngs) {
        const QByteArray data = readLimitedFile(candidate.path, 4 * 1024 * 1024);
        if (data.isEmpty()) continue;
        result.pngVariants.append({candidate.width, candidate.height, data});
        if (result.selectedPath.isEmpty()) result.selectedPath = candidate.path;
    }
    if (result.pngVariants.isEmpty() && result.svgData.isEmpty() && !bestFallback.path.isEmpty()) {
        const QImage fallback(bestFallback.path);
        if (!fallback.isNull()) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            fallback.save(&buffer, "PNG");
            result.pngVariants.append({fallback.width(), fallback.height(), png});
            result.selectedPath = bestFallback.path;
        }
    }
    cache.insert(iconValue, result);
    return result;
}
