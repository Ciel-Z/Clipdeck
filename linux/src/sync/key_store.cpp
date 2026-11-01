#include "key_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace {

QJsonObject encodeKeys(const QHash<QString, QByteArray> &keys) {
    QJsonObject object;
    for (auto it = keys.cbegin(); it != keys.cend(); ++it)
        object.insert(it.key(), QString::fromLatin1(it.value().toBase64()));
    return object;
}

QHash<QString, QByteArray> decodeKeys(const QJsonObject &object) {
    QHash<QString, QByteArray> keys;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        keys.insert(it.key(), QByteArray::fromBase64(it.value().toString().toLatin1()));
    return keys;
}

} // namespace

SyncKeyStore::SyncKeyStore(QString path) : path_(std::move(path)) {}

void SyncKeyStore::markUnpaired(const QString &deviceID, const QByteArray &publicKey) {
    locateKeys_.remove(deviceID);
    unpairedKeys_.insert(deviceID, publicKey);
}

bool SyncKeyStore::load() {
    QFile file(path_);
    if (!file.exists()) return true; // nothing stored yet
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray plain = unprotect(file.readAll());
    const QJsonDocument document = QJsonDocument::fromJson(plain);
    if (!document.isObject()) return false;
    const QJsonObject root = document.object();
    privateKey_ = QByteArray::fromBase64(root.value(QStringLiteral("privateKey")).toString().toLatin1());
    locateKeys_ = decodeKeys(root.value(QStringLiteral("locateKeys")).toObject());
    unpairedKeys_ = decodeKeys(root.value(QStringLiteral("unpaired")).toObject());
    return true;
}

bool SyncKeyStore::save() const {
    QDir().mkpath(QFileInfo(path_).absolutePath());
    const QJsonObject root{
        {QStringLiteral("privateKey"), QString::fromLatin1(privateKey_.toBase64())},
        {QStringLiteral("locateKeys"), encodeKeys(locateKeys_)},
        {QStringLiteral("unpaired"), encodeKeys(unpairedKeys_)},
    };
    const QByteArray sealed = protect(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (sealed.isEmpty()) return false;
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(sealed);
    if (!file.commit()) return false;
    QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QByteArray SyncKeyStore::protect(const QByteArray &plain) {
#ifdef Q_OS_WIN
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"ClipDeck sync keys", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return {};
    const QByteArray sealed(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    LocalFree(output.pbData);
    return sealed;
#else
    return plain; // the file itself is 0600
#endif
}

QByteArray SyncKeyStore::unprotect(const QByteArray &sealed) {
#ifdef Q_OS_WIN
    DATA_BLOB input{DWORD(sealed.size()), reinterpret_cast<BYTE *>(const_cast<char *>(sealed.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return {};
    const QByteArray plain(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plain;
#else
    return sealed;
#endif
}
