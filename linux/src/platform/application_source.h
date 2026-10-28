#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QtGlobal>

struct SourceApplicationInfo {
    QString identifier = QStringLiteral("unknown");
    QString name = QStringLiteral("Unknown");
    QByteArray iconData;
    QByteArray iconVariantsData;
    QImage icon;
};

// Uses the windows recorded at capture time, never the current UI focus.
// Opens an independent X connection owned by the processing thread.
SourceApplicationInfo sourceApplicationForWindows(quint32 sourceWindow, quint32 ownerWindow);
QStringList applicationSourceSelfTestFailures();
