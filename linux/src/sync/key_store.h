#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

// Secrets for LAN sync (docs/lan-sync-design.md §9.7): this device's private
// key, each paired device's locate key, and the public keys of devices
// unpaired while offline (so they can be told once they reconnect).
//
// Stored as one file in the app data directory. On Windows its contents are
// encrypted for the current user with DPAPI; on Linux the file is readable
// and writable by its owner only (0600).
class SyncKeyStore final {
public:
    explicit SyncKeyStore(QString path);

    bool load();
    bool save() const;

    QByteArray privateKey() const { return privateKey_; }
    void setPrivateKey(const QByteArray &key) { privateKey_ = key; }

    QByteArray locateKey(const QString &deviceID) const { return locateKeys_.value(deviceID); }
    QHash<QString, QByteArray> locateKeys() const { return locateKeys_; }
    void setLocateKey(const QString &deviceID, const QByteArray &key) { locateKeys_.insert(deviceID, key); }

    QHash<QString, QByteArray> unpairedKeys() const { return unpairedKeys_; }
    void markUnpaired(const QString &deviceID, const QByteArray &publicKey);
    void forgetUnpaired(const QString &deviceID) { unpairedKeys_.remove(deviceID); }

    QString path() const { return path_; }

    // Exposed for tests: the platform protection applied to the file body.
    static QByteArray protect(const QByteArray &plain);
    static QByteArray unprotect(const QByteArray &sealed);

private:
    QString path_;
    QByteArray privateKey_;
    QHash<QString, QByteArray> locateKeys_;
    QHash<QString, QByteArray> unpairedKeys_;
};
