#pragma once

#include "discovery.h"
#include "history_repository.h"
#include "key_store.h"
#include "noise.h"
#include "secure_channel.h"
#include "sync_protocol.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>

class CPaceSession;

// LAN sync (docs/lan-sync-design.md): discovery, pairing, and one encrypted
// connection per paired device that exchanges the last 24 hours of non-file
// history. Shared by the Windows and Linux clients; the UI binds to the
// properties below (QML on Windows, widgets on Linux).
class SyncService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ isAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY stateChanged)
    Q_PROPERTY(int visibleSecondsLeft READ visibleSecondsLeft NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY stateChanged)
    Q_PROPERTY(bool syncing READ isSyncing NOTIFY syncingChanged)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY stateChanged)
    Q_PROPERTY(QString visibilityTag READ visibilityTag NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList nearby READ nearby NOTIFY devicesChanged)
    Q_PROPERTY(bool searching READ isSearching NOTIFY devicesChanged)
    Q_PROPERTY(QVariantMap pairing READ pairing NOTIFY pairingChanged)
    Q_PROPERTY(QString networkHint READ networkHint NOTIFY stateChanged)

public:
    struct Options {
        QString keyStorePath;
        quint16 preferredPort = 47821;
        bool allowLoopback = false;    // tests on one machine
        bool enableDiscovery = true;   // tests can run without UDP
        QString deviceNameOverride;    // tests
    };

    SyncService(HistoryRepository *repository, const Options &options, QObject *parent = nullptr);
    ~SyncService() override;

    bool start();
    bool isAvailable() const { return available_; }
    QString deviceID() const { return deviceID_; }
    quint16 port() const;

    bool isVisible() const { return visible_; }
    void setVisible(bool visible);
    int visibleSecondsLeft() const;
    bool isPaused() const { return paused_; }
    void setPaused(bool paused);
    bool isSyncing() const { return syncing_; }
    QString deviceName() const { return deviceName_; }
    void setDeviceName(const QString &name);
    QString visibilityTag() const { return visible_ ? QString::fromLatin1(tag_) : QString(); }
    QString statusText() const;
    QString networkHint() const { return networkHint_; }
    QVariantList devices() const;
    QVariantList nearby() const;
    bool isSearching() const { return searchTimer_.isActive(); }
    QVariantMap pairing() const;

    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void searchNearby();
    Q_INVOKABLE void pair(const QString &nearbyKey);
    // Tests and manual entry: pair with an address directly.
    void pairWithAddress(const QString &host, quint16 port);
    Q_INVOKABLE void submitPairingCode(const QString &code);
    Q_INVOKABLE void cancelPairing();
    Q_INVOKABLE void rejectPairing();
    Q_INVOKABLE void unpair(const QString &deviceID);
    // Call after any local history change; changes go out in about 500 ms.
    Q_INVOKABLE void notifyLocalChange();

    // Tests: the code shown on the responder.
    QString incomingPairingCode() const;
    bool isConnectedTo(const QString &deviceID) const;
    void connectToPeerNow(const QString &deviceID);

Q_SIGNALS:
    void stateChanged();
    void devicesChanged();
    void pairingChanged();
    void syncingChanged();
    // Items from other devices were written to the history database.
    void historyChanged();
    // A device asked to pair; the UI should show the sync panel.
    void pairingRequested();

private:
    struct Peer {
        SyncPeer stored;
        QPointer<SecureChannel> channel;
        QVector<QPointer<SecureChannel>> attempts;
        QTimer *reconnectTimer = nullptr;
        int failures = 0;
        quint64 pushedCursor = 0;
        bool pushInFlight = false;
        bool receiving = false;
        bool receivedChanges = false;
        qint64 lastActivityMs = 0;
        qint64 lastLocateMs = 0;
        QHash<QByteArray, SyncProtocol::WireItem> pendingBlobs;
        QHash<QByteArray, QByteArray> blobData;
    };
    struct Nearby {
        QString key; // "ip:port"
        QString address;
        quint16 port = 0;
        QByteArray tag;
        QString hostname;
        QString deviceID;
        QPointer<SecureChannel> infoChannel;
    };
    struct Pairing {
        enum class Role { None, Outgoing, Incoming } role = Role::None;
        enum class Stage { Connecting, WaitingForCode, Verifying, Finishing, Failed, Done } stage = Stage::Connecting;
        QPointer<SecureChannel> channel;
        QString key;
        QString address;
        QString hostname;
        QString peerDeviceID;
        QString code; // incoming: shown to the user
        QByteArray peerMessage;
        std::unique_ptr<CPaceSession> cpace;
        QByteArray isk;
        int attemptsLeft = 5;
        QString error;
        QTimer *expiry = nullptr;
    };

    void loadIdentity();
    void onNewConnection();
    void onChannelReceived(SecureChannel *channel, const QByteArray &plaintext);
    void onChannelClosed(SecureChannel *channel);
    void attachSyncChannel(Peer &peer, SecureChannel *channel);
    void handleSyncMessage(Peer &peer, const QJsonObject &message);
    void handleBlob(Peer &peer, const SyncProtocol::BlobChunk &chunk);
    void importWire(Peer &peer, const SyncProtocol::WireItem &wire);
    void pushChanges(Peer &peer);
    void sendJson(SecureChannel *channel, const QJsonObject &object);
    void connectPeer(Peer &peer);
    void scheduleReconnect(Peer &peer);
    bool weInitiate(const QString &peerDeviceID) const { return deviceID_ < peerDeviceID; }
    void recordAddress(Peer &peer, const QString &address, quint16 port);
    void savePeer(const Peer &peer);
    void updateSyncing();
    void tick();
    void checkNetwork();
    void sendLocate(Peer &peer, const QHostAddress &target = QHostAddress());
    void onReplied(const QHostAddress &from, quint16 port, const QByteArray &tag);
    void onLocated(const QHostAddress &from, quint16 port, const QByteArray &token);

    // Pairing
    void handlePairingMessage(SecureChannel *channel, const QJsonObject &message);
    void startIncomingPairing(SecureChannel *channel, const QJsonObject &request);
    void sendChallenge();
    void finishPairing(const QString &deviceID, const QString &hostname, const QByteArray &publicKey,
                       const QString &address, quint16 port);
    void failPairing(const QString &error);
    void clearPairing();
    QVector<SecureChannel::Candidate> syncCandidates() const;

    HistoryRepository *repository_;
    Options options_;
    SyncKeyStore keyStore_;
    SyncCrypto::KeyPair identity_;
    QString deviceID_;
    QString deviceName_;
    bool available_ = false;
    QTcpServer server_;
    SyncDiscovery discovery_;
    QHash<QString, Peer> peers_;
    QVector<Nearby> nearby_;
    Pairing pairing_;
    QHash<SecureChannel *, QString> pendingChannels_; // accepted, not yet identified
    bool visible_ = false;
    qint64 visibleUntilMs_ = 0;
    QByteArray tag_;
    bool paused_ = false;
    bool syncing_ = false;
    QString networkHint_;
    QTimer tickTimer_;
    QTimer pushTimer_;
    QTimer searchTimer_;
    QTimer syncingOffTimer_;
    QElapsedTimer sinceTick_;
    QStringList interfaceSignature_;
};
