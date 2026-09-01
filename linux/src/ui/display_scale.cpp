#include "display_scale.h"

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace {

qreal globalScaleFactor = 1.0;
bool initialized = false;

qreal normalizedScale(qreal value) {
    if (!std::isfinite(value)) return 1.0;
    value = std::max<qreal>(1.0, std::min<qreal>(4.0, value));
    // 100 DPI is a common font-DPI default, not an intentional 104% desktop
    // scale. Ignore that small tolerance and snap real fractional desktop
    // scales to stable 5% steps (1.25 remains exactly 1.25).
    if (value < 1.10) return 1.0;
    return qRound(value * 20.0) / 20.0;
}

} // namespace

namespace DisplayScale {

qreal factor() {
    return globalScaleFactor;
}

qreal factorForScreen(const QScreen *screen) {
    if (!screen) return 1.0;
    // Qt reports fractional scaling either as DPR (QT_SCALE_FACTOR and some
    // compositors) or as logical DPI (notably some UOS/DDE configurations).
    // Taking the larger ratio covers both without multiplying the same scale
    // twice. 96 DPI is Qt's logical 1x baseline on X11.
    const qreal deviceRatio = screen->devicePixelRatio();
    const qreal logicalDpiRatio = screen->logicalDotsPerInch() / 96.0;
    return normalizedScale(std::max(deviceRatio, logicalDpiRatio));
}

void initialize(QScreen *screen) {
    if (initialized) return;
    initialized = true;
    globalScaleFactor = factorForScreen(screen);
    qInfo().noquote() << "[ClipDeck][DisplayScale] screen="
                      << (screen ? screen->name() : QStringLiteral("unknown"))
                      << "factor=" << globalScaleFactor
                      << "dpr=" << (screen ? screen->devicePixelRatio() : 1.0)
                      << "logicalDpi=" << (screen ? screen->logicalDotsPerInch() : 96.0);
}

QSize rasterSize(const QSize &logicalSize, qreal scale) {
    if (scale <= 0.0) scale = factor();
    scale = normalizedScale(scale);
    return QSize(static_cast<int>(std::ceil(logicalSize.width() * scale)),
                 static_cast<int>(std::ceil(logicalSize.height() * scale)));
}

} // namespace DisplayScale
