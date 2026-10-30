#include "file_reference.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QMimeData>
#include <algorithm>

QVector<QUrl> FileReferences::urls(const QByteArray &data) {
    QVector<QUrl> result;
    QSet<QString> seen;
    for (const QByteArray &line : data.split('\n')) {
        const QByteArray value = line.trimmed();
        if (value.isEmpty() || value.startsWith('#')) continue;
        const QUrl url = QUrl::fromEncoded(value, QUrl::StrictMode);
        if (!url.isValid() || !url.isLocalFile()) continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (!QDir::isAbsolutePath(path) || seen.contains(path)) continue;
        seen.insert(path);
        result.append(QUrl::fromLocalFile(path));
    }
    return result;
}

QByteArray FileReferences::encode(const QVector<ClipboardFileReference> &files) {
    QJsonArray array;
    for (const auto &file : files) array.append(QJsonObject{
        {QStringLiteral("path"), file.path}, {QStringLiteral("name"), file.name},
        {QStringLiteral("mimeType"), file.mimeType}, {QStringLiteral("isDirectory"), file.isDirectory}});
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QVector<ClipboardFileReference> FileReferences::decode(const QByteArray &data) {
    QVector<ClipboardFileReference> files;
    const auto document = QJsonDocument::fromJson(data);
    if (!document.isArray()) return files;
    QSet<QString> seen;
    for (const auto &value : document.array()) {
        const auto object = value.toObject();
        const QString path = object.value(QStringLiteral("path")).toString();
        if (!QDir::isAbsolutePath(path) || seen.contains(path)) continue;
        seen.insert(path);
        files.append({path, object.value(QStringLiteral("name")).toString(),
            object.value(QStringLiteral("mimeType")).toString(),
            object.value(QStringLiteral("isDirectory")).toBool()});
    }
    return files;
}

QByteArray FileReferences::identity(const QVector<ClipboardFileReference> &files) {
    QStringList paths;
    for (const auto &file : files) paths.append(QDir::cleanPath(file.path));
    paths.removeDuplicates();
    std::sort(paths.begin(), paths.end());
    QJsonArray array;
    for (const auto &path : paths) array.append(path);
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QString FileReferences::searchableText(const QVector<ClipboardFileReference> &files) {
    QStringList values;
    for (const auto &file : files) values << file.name << file.path;
    return values.join(QLatin1Char('\n'));
}

QMimeData *FileReferences::mimeData(const QVector<ClipboardFileReference> &files) {
    auto *data = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome("copy");
    for (const auto &file : files) {
        const QUrl url = QUrl::fromLocalFile(file.path);
        urls.append(url); gnome += '\n'; gnome += url.toEncoded();
    }
    data->setUrls(urls);
    // Native file-manager interoperability; history replay always copies,
    // never replays a destructive cut/move from an older clipboard snapshot.
    // https://mail.gnome.org/archives/commits-list/2022-January/msg01303.html
    data->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
    data->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("0"));
    return data;
}
