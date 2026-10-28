#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QtGlobal>

// Owns its bytes. No QClipboard/QMimeData object crosses a thread boundary.
struct ClipboardSnapshot {
    quint64 sequence = 0;
    QDateTime copiedAt;
    quint32 ownerWindow = 0;
    quint32 sourceWindow = 0;
    QByteArray text;
    QByteArray html;
    QByteArray png;
    bool latin1 = false;
    bool explicitChange = true;
    bool baseline = false;

    qint64 byteCount() const { return qint64(text.size()) + html.size() + png.size(); }
};
