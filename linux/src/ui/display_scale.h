#pragma once

#include <QPoint>
#include <QSize>
#include <QtGlobal>

class QScreen;

namespace DisplayScale {

qreal factor();
qreal factorForScreen(const QScreen *screen);
void initialize(QScreen *screen);
QSize rasterSize(const QSize &logicalSize, qreal scale = 0.0);

} // namespace DisplayScale
