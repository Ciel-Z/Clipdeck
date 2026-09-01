#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

struct LinuxPngIconVariant {
    int width = 0;
    int height = 0;
    QByteArray data;
};

struct LinuxApplicationIconSet {
    QByteArray svgData;
    QVector<LinuxPngIconVariant> pngVariants;
    QString selectedPath;

    bool isEmpty() const;
    QByteArray serialize() const;
    QImage rasterize(const QSize &targetPhysicalSize, qreal devicePixelRatio) const;

    static LinuxApplicationIconSet deserialize(const QByteArray &data);
};

LinuxApplicationIconSet resolveLinuxApplicationIconSet(const QString &iconValue);
