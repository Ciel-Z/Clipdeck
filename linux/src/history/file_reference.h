#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <QUrl>
class QMimeData;

struct ClipboardFileReference {
    QString path;
    QString name;
    QString mimeType;
    bool isDirectory = false;
};

namespace FileReferences {
QVector<QUrl> urls(const QByteArray &uriList);
QByteArray encode(const QVector<ClipboardFileReference> &files);
QVector<ClipboardFileReference> decode(const QByteArray &data);
QByteArray identity(const QVector<ClipboardFileReference> &files);
QString searchableText(const QVector<ClipboardFileReference> &files);
QMimeData *mimeData(const QVector<ClipboardFileReference> &files);
}
