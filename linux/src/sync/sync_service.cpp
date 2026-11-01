#include "sync_service.h"
#include "cpace.h"
#include "network_interfaces.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QNetworkProxy>
#include <QSettings>
#include <QSysInfo>
#include <QTcpSocket>

#include <algorithm>

class CPaceSession final : public SyncCrypto::CPace {
public:
    using SyncCrypto::CPace::CPace;
};

namespace {

const QByteArray kPairingChannelId = QByteArrayLiteral("ClipDeck pairing v1");
constexpr int kPingIntervalMs = 15000;
constexpr int kIdleTimeoutMs = 45000;
constexpr int kPairingCodeLifetimeMs = 120000;
constexpr int kBatchSize = 100;
constexpr qint64 kMaxBlobBytes = 64LL * 1024 * 1024;

QString b64(const QByteArray &data) { return QString::fromLatin1(data.toBase64()); }
QByteArray unb64(const QJsonValue &value) { return QByteArray::fromBase64(value.toString().toLatin1()); }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QByteArray publicKeyMac(const QByteArray &isk, const QByteArray &publicKey) {
    return SyncCrypto::hmacBlake2b(isk, QByteArrayLiteral("ClipDeck public key") + publicKey);
}

QString relativeTime(qint64 ms) {
    if (ms <= 0) return QStringLiteral("从未连接");
    const qint64 seconds = (nowMs() - ms) / 1000;
    if (seconds < 60) return QStringLiteral("刚刚");
    if (seconds < 3600) return QStringLiteral("%1 分钟前").arg(seconds / 60);
    if (seconds < 86400) return QStringLiteral("%1 小时前").arg(seconds / 3600);
    return QStringLiteral("%1 天前").arg(seconds / 86400);
}

} // namespace

SyncService::SyncService(HistoryRepository *repository, const Options &options, QObject *parent)
    : QObject(parent), repository_(repository), options_(options), keyStore_(options.keyStorePath) {
    paused_ = QSettings().value(QStringLiteral("sync/paused"), false).toBool();
    tickTimer_.setInterval(1000);
    connect(&tickTimer_, &QTimer::timeout, this, &SyncService::tick);
    pushTimer_.setSingleShot(true);
    pushTimer_.setInterval(500);
    connect(&pushTimer_, &QTimer::timeout, this, [this] {
        for (Peer &peer : peers_) pushChanges(peer);
    });
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(5000);
    connect(&searchTimer_, &QTimer::timeout, this, &SyncService::devicesChanged);
    syncingOffTimer_.setSingleShot(true);
    syncingOffTimer_.setInterval(600); // the Deck animation shows for at least this long
    connect(&syncingOffTimer_, &QTimer::timeout, this, [this] {
        if (!syncing_) return;
        syncing_ = false;
        Q_EMIT syncingChanged();
    });
    connect(&server_, &QTcpServer::newConnection, this, &SyncService::onNewConnection);
    connect(&discovery_, &SyncDiscovery::replied, this, &SyncService::onReplied);
    connect(&discovery_, &SyncDiscovery::located, this, &SyncService::onLocated);
    discovery_.setIncludeLoopback(options.allowLoopback);
}

SyncService::~SyncService() {
    for (Peer &peer : peers_) {
        if (peer.channel) peer.channel->close();
    }
}

void SyncService::loadIdentity() {
    keyStore_.load();
    QByteArray privateKey = keyStore_.privateKey();
    if (privateKey.size() != 32) {
        privateKey = SyncCrypto::generateKeyPair().privateKey;
        keyStore_.setPrivateKey(privateKey);
        keyStore_.save();
    }
    identity_ = {SyncCrypto::publicKeyFor(privateKey), privateKey};
    deviceID_ = SyncCrypto::deviceIdFor(identity_.publicKey);
    deviceName_ = options_.deviceNameOverride;
    if (deviceName_.isEmpty())
        deviceName_ = QSettings().value(QStringLiteral("sync/deviceName"), QSysInfo::machineHostName()).toString();
}

bool SyncService::start() {
    if (!SyncCrypto::initialize() || !repository_ || !repository_->isOpen()) return false;
    loadIdentity();
    server_.setProxy(QNetworkProxy::NoProxy);
    if (!server_.listen(QHostAddress(QHostAddress::AnyIPv4), options_.preferredPort)
        && !server_.listen(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        return false;
    }
    for (const SyncPeer &stored : repository_->syncPeers()) {
        Peer &peer = peers_[stored.deviceID];
        peer.stored = stored;
    }
    if (options_.enableDiscovery) discovery_.start();
    available_ = true;
    sinceTick_.start();
    tickTimer_.start();
    checkNetwork();
    if (!paused_) {
        for (Peer &peer : peers_) connectPeer(peer);
    }
    // The UI may have bound before start(): name, availability, devices.
    Q_EMIT stateChanged();
    Q_EMIT devicesChanged();
    return true;
}

quint16 SyncService::port() const { return server_.serverPort(); }

// MARK: - Settings and status

void SyncService::setVisible(bool visible) {
    if (!available_ || visible == visible_) return;
    visible_ = visible;
    if (visible) {
        tag_ = SyncProtocol::randomTag();
        visibleUntilMs_ = nowMs() + SyncProtocol::kVisibilitySeconds * 1000LL;
        if (options_.enableDiscovery) discovery_.start();
    }
    discovery_.setVisible(visible, tag_, port());
    Q_EMIT stateChanged();
}

int SyncService::visibleSecondsLeft() const {
    return visible_ ? int(std::max<qint64>(0, (visibleUntilMs_ - nowMs() + 999) / 1000)) : 0;
}

void SyncService::setPaused(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    QSettings().setValue(QStringLiteral("sync/paused"), paused);
    for (Peer &peer : peers_) {
        if (paused) {
            if (peer.channel) peer.channel->close();
            if (peer.reconnectTimer) peer.reconnectTimer->stop();
        } else {
            peer.failures = 0;
            connectPeer(peer);
        }
    }
    Q_EMIT stateChanged();
    Q_EMIT devicesChanged();
}

void SyncService::setDeviceName(const QString &name) {
    const QString trimmed = name.trimmed().left(64);
    if (trimmed.isEmpty() || trimmed == deviceName_) return;
    deviceName_ = trimmed;
    QSettings().setValue(QStringLiteral("sync/deviceName"), trimmed);
    for (Peer &peer : peers_) {
        if (peer.channel && peer.channel->isEstablished())
            sendJson(peer.channel, {{QStringLiteral("type"), QStringLiteral("hello")},
                                    {QStringLiteral("deviceId"), deviceID_},
                                    {QStringLiteral("hostname"), deviceName_},
                                    {QStringLiteral("port"), int(port())}});
    }
    Q_EMIT stateChanged();
}

QString SyncService::statusText() const {
    if (!available_) return QStringLiteral("同步不可用");
    if (paused_) return QStringLiteral("已暂停同步");
    int connected = 0;
    for (const Peer &peer : peers_) connected += peer.channel && peer.channel->isEstablished();
    if (connected > 0) return QStringLiteral("已连接 %1 台设备").arg(connected);
    return peers_.isEmpty() ? QStringLiteral("未配对设备") : QStringLiteral("设备均离线");
}

QVariantList SyncService::devices() const {
    QVariantList list;
    for (const Peer &peer : peers_) {
        const bool connected = peer.channel && peer.channel->isEstablished();
        const QString state = !connected ? QStringLiteral("offline")
            : (peer.pushInFlight || peer.receiving) ? QStringLiteral("syncing") : QStringLiteral("connected");
        list.append(QVariantMap{
            {QStringLiteral("deviceId"), peer.stored.deviceID},
            {QStringLiteral("hostname"), peer.stored.hostname},
            {QStringLiteral("address"), peer.stored.addresses.value(0).section(QLatin1Char(':'), 0, 0)},
            {QStringLiteral("state"), state},
            {QStringLiteral("lastSeen"), connected ? QStringLiteral("已连接") : relativeTime(peer.stored.lastSeenMs)},
        });
    }
    std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("hostname")).toString()
            < b.toMap().value(QStringLiteral("hostname")).toString();
    });
    return list;
}

QVariantList SyncService::nearby() const {
    QVariantList list;
    for (const Nearby &device : nearby_) {
        if (peers_.contains(device.deviceID)) continue;
        list.append(QVariantMap{
            {QStringLiteral("key"), device.key},
            {QStringLiteral("hostname"), device.hostname.isEmpty() ? QStringLiteral("ClipDeck") : device.hostname},
            {QStringLiteral("address"), device.address},
            {QStringLiteral("tag"), QString::fromLatin1(device.tag)},
            {QStringLiteral("pairable"), peers_.size() < SyncProtocol::kMaxPeers},
        });
    }
    return list;
}

QVariantMap SyncService::pairing() const {
    if (pairing_.role == Pairing::Role::None) return {{QStringLiteral("active"), false}};
    static const char *stages[] = {"connecting", "code", "verifying", "finishing", "failed", "done"};
    return {
        {QStringLiteral("active"), true},
        {QStringLiteral("role"), pairing_.role == Pairing::Role::Outgoing ? QStringLiteral("outgoing")
                                                                          : QStringLiteral("incoming")},
        {QStringLiteral("stage"), QLatin1String(stages[int(pairing_.stage)])},
        {QStringLiteral("key"), pairing_.key},
        {QStringLiteral("hostname"), pairing_.hostname},
        {QStringLiteral("address"), pairing_.address},
        {QStringLiteral("code"), pairing_.role == Pairing::Role::Incoming ? pairing_.code : QString()},
        {QStringLiteral("attemptsLeft"), pairing_.attemptsLeft},
        {QStringLiteral("error"), pairing_.error},
    };
}

QString SyncService::incomingPairingCode() const {
    return pairing_.role == Pairing::Role::Incoming ? pairing_.code : QString();
}

bool SyncService::isConnectedTo(const QString &deviceID) const {
    const auto it = peers_.constFind(deviceID);
    return it != peers_.constEnd() && it->channel && it->channel->isEstablished();
}

void SyncService::connectToPeerNow(const QString &deviceID) {
    auto it = peers_.find(deviceID);
    if (it == peers_.end()) return;
    it->failures = 0;
    connectPeer(*it);
}

// MARK: - Timers and network

void SyncService::tick() {
    const qint64 elapsed = sinceTick_.restart();
    const qint64 now = nowMs();
    if (visible_) {
        if (now >= visibleUntilMs_ && pairing_.role != Pairing::Role::Incoming) setVisible(false);
        else Q_EMIT stateChanged();
    }
    static int counter = 0;
    if (++counter % 10 == 0 || elapsed > 5000) {
        // Every 10 s, or at once after a sleep: the network may have changed.
        if (elapsed > 5000) {
            for (Peer &peer : peers_) peer.failures = 0;
        }
        checkNetwork();
    }
    for (Peer &peer : peers_) {
        if (!peer.channel || !peer.channel->isEstablished()) {
            // A device that should connect to us may not know our address:
            // remind it at most once a minute.
            if (!paused_ && !weInitiate(peer.stored.deviceID) && now - peer.lastLocateMs > 60000) sendLocate(peer);
            continue;
        }
        if (now - peer.lastActivityMs > kIdleTimeoutMs) {
            peer.channel->close();
        } else if (now - peer.lastActivityMs > kPingIntervalMs && counter % 5 == 0) {
            sendJson(peer.channel, {{QStringLiteral("type"), QStringLiteral("ping")}});
        }
    }
}

void SyncService::checkNetwork() {
    QStringList signature;
    const auto interfaces = SyncNetwork::eligibleInterfaces();
    for (const auto &candidate : interfaces) signature.append(candidate.address.toString());
    signature.sort();
    const QString hint = interfaces.isEmpty() && !options_.allowLoopback ? QStringLiteral("未连接到局域网") : QString();
    if (hint != networkHint_) {
        networkHint_ = hint;
        Q_EMIT stateChanged();
    }
    // Windows can drop a group membership without any address change (Wi-Fi
    // power saving, a proxy's TUN adapter resetting the stack): join again
    // every check; joining a group we are already in is a no-op.
    if (options_.enableDiscovery) discovery_.rejoin();
    if (networkChecked_ && signature == interfaceSignature_) return;
    // The first check only records the networks. Starting with none (launched
    // at login before Wi-Fi is up) still counts, so the network that appears
    // next is handled as a change.
    const bool firstCheck = !networkChecked_;
    networkChecked_ = true;
    interfaceSignature_ = signature;
    if (firstCheck) return;
    // A new or lost network: rejoin the multicast group and reconnect now.
    if (options_.enableDiscovery) {
        discovery_.stop();
        discovery_.start();
        discovery_.setVisible(visible_, tag_, port());
    }
    if (paused_) return;
    for (Peer &peer : peers_) {
        peer.failures = 0;
        if (!peer.channel || !peer.channel->isEstablished()) connectPeer(peer);
    }
}

// MARK: - Connections

QVector<SecureChannel::Candidate> SyncService::syncCandidates() const {
    QVector<SecureChannel::Candidate> candidates;
    for (const Peer &peer : peers_) candidates.append({peer.stored.deviceID, peer.stored.publicKey});
    const auto unpaired = keyStore_.unpairedKeys();
    for (auto it = unpaired.cbegin(); it != unpaired.cend(); ++it) candidates.append({it.key(), it.value()});
    return candidates;
}

void SyncService::onNewConnection() {
    while (QTcpSocket *socket = server_.nextPendingConnection()) {
        // Only the LAN interfaces sync uses (no VPN or public address).
        bool mapped = false;
        const QHostAddress local(socket->localAddress().toIPv4Address(&mapped));
        bool allowed = options_.allowLoopback && local.isLoopback();
        for (const auto &candidate : SyncNetwork::eligibleInterfaces()) allowed = allowed || candidate.address == local;
        if (!allowed) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        SecureChannel *channel = SecureChannel::accept(socket, identity_, syncCandidates(), this);
        connect(channel, &SecureChannel::established, this, [this, channel] {
            if (channel->kind() == SecureChannel::Kind::Pairing) return; // waits for its first message
            const QString deviceID = channel->peerDeviceID();
            if (keyStore_.unpairedKeys().contains(deviceID)) {
                // Unpaired while it was offline: tell it once, then forget it.
                sendJson(channel, {{QStringLiteral("type"), QStringLiteral("unpair")}});
                keyStore_.forgetUnpaired(deviceID);
                keyStore_.save();
                QTimer::singleShot(500, channel, &SecureChannel::close);
                return;
            }
            auto it = peers_.find(deviceID);
            if (it == peers_.end() || paused_) {
                channel->close();
                return;
            }
            attachSyncChannel(*it, channel);
        });
        connect(channel, &SecureChannel::received, this,
                [this, channel](const QByteArray &plaintext) { onChannelReceived(channel, plaintext); });
        connect(channel, &SecureChannel::closed, this, [this, channel] { onChannelClosed(channel); });
    }
}

void SyncService::connectPeer(Peer &peer) {
    if (paused_ || !available_ || !weInitiate(peer.stored.deviceID)) return;
    if (peer.channel && peer.channel->isEstablished()) return;
    for (const auto &attempt : peer.attempts) {
        if (attempt) return; // already trying
    }
    const QStringList addresses = SyncNetwork::rankAddresses(peer.stored.addresses, SyncNetwork::eligibleInterfaces());
    if (addresses.isEmpty()) {
        sendLocate(peer);
        scheduleReconnect(peer);
        return;
    }
    const QString deviceID = peer.stored.deviceID;
    peer.attempts.clear();
    // Staggered by 250 ms; the first address that answers wins.
    for (int index = 0; index < addresses.size(); ++index) {
        const QString host = addresses[index].section(QLatin1Char(':'), 0, 0);
        const quint16 portNumber = quint16(addresses[index].section(QLatin1Char(':'), 1, 1).toUInt());
        QTimer::singleShot(index * 250, this, [this, deviceID, host, portNumber] {
            auto it = peers_.find(deviceID);
            if (it == peers_.end() || paused_ || (it->channel && it->channel->isEstablished())) return;
            SecureChannel *channel = SecureChannel::connectTo(host, portNumber, SecureChannel::Kind::Sync, identity_,
                                                              {deviceID, it->stored.publicKey}, this);
            it->attempts.append(channel);
            connect(channel, &SecureChannel::established, this, [this, channel, deviceID] {
                auto peer = peers_.find(deviceID);
                if (peer == peers_.end()) {
                    channel->close();
                    return;
                }
                for (const auto &other : peer->attempts) {
                    if (other && other != channel) other->close();
                }
                peer->attempts.clear();
                attachSyncChannel(*peer, channel);
            });
            connect(channel, &SecureChannel::received, this,
                    [this, channel](const QByteArray &plaintext) { onChannelReceived(channel, plaintext); });
            connect(channel, &SecureChannel::closed, this, [this, channel] { onChannelClosed(channel); });
        });
    }
}

void SyncService::scheduleReconnect(Peer &peer) {
    if (paused_ || !weInitiate(peer.stored.deviceID)) return;
    if (!peer.reconnectTimer) {
        peer.reconnectTimer = new QTimer(this);
        peer.reconnectTimer->setSingleShot(true);
        const QString deviceID = peer.stored.deviceID;
        connect(peer.reconnectTimer, &QTimer::timeout, this, [this, deviceID] {
            auto it = peers_.find(deviceID);
            if (it != peers_.end()) connectPeer(*it);
        });
    }
    // 1, 2, 4 … 60 s, ±20 %.
    const int base = std::min(60000, 1000 << std::min(peer.failures, 6));
    const int jitter = int(base * 0.2);
    peer.reconnectTimer->start(base - jitter + int(QRandomGenerator::global()->bounded(2 * jitter + 1)));
    ++peer.failures;
}

void SyncService::attachSyncChannel(Peer &peer, SecureChannel *channel) {
    if (peer.channel && peer.channel != channel && peer.channel->isEstablished()) {
        // Both sides connected at once: keep the one the smaller ID opened.
        const bool newIsPreferred = channel->isInitiator() == weInitiate(peer.stored.deviceID);
        if (!newIsPreferred) {
            channel->close();
            return;
        }
        peer.channel->close();
    }
    peer.channel = channel;
    peer.failures = 0;
    peer.pushedCursor = peer.stored.sentCursor;
    peer.pushInFlight = false;
    peer.receiving = false;
    peer.lastActivityMs = nowMs();
    peer.stored.lastSeenMs = nowMs();
    if (peer.reconnectTimer) peer.reconnectTimer->stop();
    sendJson(channel, {{QStringLiteral("type"), QStringLiteral("hello")},
                       {QStringLiteral("version"), SyncProtocol::kProtocolVersion},
                       {QStringLiteral("deviceId"), deviceID_},
                       {QStringLiteral("hostname"), deviceName_},
                       {QStringLiteral("port"), int(port())}});
    savePeer(peer);
    Q_EMIT stateChanged();
    Q_EMIT devicesChanged();
    pushChanges(peer);
}

void SyncService::onChannelClosed(SecureChannel *channel) {
    if (pairing_.channel == channel && pairing_.stage != Pairing::Stage::Done) {
        if (pairing_.stage != Pairing::Stage::Failed) failPairing(QStringLiteral("连接已断开"));
    }
    for (Nearby &device : nearby_) {
        if (device.infoChannel == channel) device.infoChannel.clear();
    }
    for (Peer &peer : peers_) {
        const bool wasAttempt = peer.attempts.removeAll(channel) > 0;
        const bool wasChannel = peer.channel == channel;
        if (!wasAttempt && !wasChannel) continue; // another device's connection
        if (wasChannel) {
            peer.channel.clear();
            peer.pushInFlight = false;
            peer.receiving = false;
            peer.pendingBlobs.clear();
            peer.blobData.clear();
            peer.stored.lastSeenMs = nowMs();
            savePeer(peer);
            Q_EMIT stateChanged();
            Q_EMIT devicesChanged();
            updateSyncing();
        }
        bool trying = false;
        for (const auto &attempt : peer.attempts) trying = trying || attempt;
        if (!trying && !peer.channel && weInitiate(peer.stored.deviceID)) {
            if (peer.failures >= 2) sendLocate(peer);
            scheduleReconnect(peer);
        }
    }
    channel->deleteLater();
}

void SyncService::recordAddress(Peer &peer, const QString &address, quint16 portNumber) {
    if (address.isEmpty() || !portNumber) return;
    const QString entry = QStringLiteral("%1:%2").arg(address).arg(portNumber);
    peer.stored.addresses.removeAll(entry);
    peer.stored.addresses.prepend(entry);
    while (peer.stored.addresses.size() > 4) peer.stored.addresses.removeLast();
}

void SyncService::savePeer(const Peer &peer) { repository_->saveSyncPeer(peer.stored); }

void SyncService::sendJson(SecureChannel *channel, const QJsonObject &object) {
    if (channel) channel->send(SyncProtocol::jsonFrame(object));
}

void SyncService::sendLocate(Peer &peer, const QHostAddress &target) {
    if (!options_.enableDiscovery) return;
    const QByteArray key = keyStore_.locateKey(peer.stored.deviceID);
    if (key.isEmpty()) return;
    peer.lastLocateMs = nowMs();
    discovery_.locate(SyncProtocol::locateToken(key, nowMs() / 1000), port(), target);
}

void SyncService::onLocated(const QHostAddress &from, quint16 portNumber, const QByteArray &token) {
    const qint64 seconds = nowMs() / 1000;
    for (Peer &peer : peers_) {
        const QByteArray key = keyStore_.locateKey(peer.stored.deviceID);
        if (key.isEmpty()) continue;
        bool matches = false;
        for (int slot = -1; slot <= 1; ++slot)
            matches = matches || SyncProtocol::locateToken(key, seconds + slot * 30) == token;
        if (!matches) continue;
        recordAddress(peer, from.toString(), portNumber);
        savePeer(peer);
        if (paused_ || (peer.channel && peer.channel->isEstablished())) return;
        if (weInitiate(peer.stored.deviceID)) {
            peer.failures = 0;
            connectPeer(peer);
        } else if (nowMs() - peer.lastLocateMs > 2000) {
            sendLocate(peer, from); // so it learns where we are and connects
        }
        return;
    }
}

void SyncService::syncNow() {
    if (paused_) return;
    for (Peer &peer : peers_) {
        if (peer.channel && peer.channel->isEstablished()) {
            pushChanges(peer);
        } else {
            peer.failures = 0;
            if (weInitiate(peer.stored.deviceID)) connectPeer(peer);
            else sendLocate(peer);
        }
    }
}

void SyncService::notifyLocalChange() {
    if (!paused_) pushTimer_.start();
}

// MARK: - Sync protocol

void SyncService::onChannelReceived(SecureChannel *channel, const QByteArray &plaintext) {
    if (channel->kind() == SecureChannel::Kind::Pairing) {
        if (const auto message = SyncProtocol::parseJsonFrame(plaintext)) handlePairingMessage(channel, *message);
        else channel->close();
        return;
    }
    for (Peer &peer : peers_) {
        if (peer.channel != channel) continue;
        peer.lastActivityMs = nowMs();
        if (const auto message = SyncProtocol::parseJsonFrame(plaintext)) handleSyncMessage(peer, *message);
        else if (const auto chunk = SyncProtocol::parseBlobFrame(plaintext)) handleBlob(peer, *chunk);
        else channel->close();
        return;
    }
}

void SyncService::handleSyncMessage(Peer &peer, const QJsonObject &message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("hello")) {
        const QString hostname = message.value(QStringLiteral("hostname")).toString().left(64);
        if (!hostname.isEmpty()) peer.stored.hostname = hostname;
        if (peer.channel)
            recordAddress(peer, peer.channel->peerAddress(), quint16(message.value(QStringLiteral("port")).toInt()));
        savePeer(peer);
        Q_EMIT devicesChanged();
    } else if (type == QStringLiteral("item")) {
        if (!peer.receiving) {
            peer.receiving = true;
            peer.receivedChanges = false;
            updateSyncing();
            Q_EMIT devicesChanged();
        }
        const auto wire = SyncProtocol::decodeItem(message);
        if (!wire) return;
        if (wire->blobId.isEmpty()) {
            importWire(peer, *wire);
        } else if (wire->bodySize <= kMaxBlobBytes) {
            peer.pendingBlobs.insert(wire->blobId, *wire);
            peer.blobData.insert(wire->blobId, QByteArray());
        }
    } else if (type == QStringLiteral("batch_end")) {
        if (peer.receivedChanges) Q_EMIT historyChanged();
        peer.receiving = false;
        peer.receivedChanges = false;
        sendJson(peer.channel, {{QStringLiteral("type"), QStringLiteral("ack")},
                                {QStringLiteral("hlc"), message.value(QStringLiteral("hlc"))}});
        updateSyncing();
        Q_EMIT devicesChanged();
    } else if (type == QStringLiteral("ack")) {
        const quint64 hlc = message.value(QStringLiteral("hlc")).toString().toULongLong();
        peer.stored.sentCursor = std::max(peer.stored.sentCursor, hlc);
        peer.pushInFlight = false;
        savePeer(peer);
        pushChanges(peer); // more, if the batch was full
        updateSyncing();
        Q_EMIT devicesChanged();
    } else if (type == QStringLiteral("ping")) {
        sendJson(peer.channel, {{QStringLiteral("type"), QStringLiteral("pong")}});
    } else if (type == QStringLiteral("unpair")) {
        const QString deviceID = peer.stored.deviceID;
        SecureChannel *channel = peer.channel;
        repository_->removeSyncPeer(deviceID);
        keyStore_.markUnpaired(deviceID, QByteArray());
        keyStore_.forgetUnpaired(deviceID);
        keyStore_.save();
        if (peer.reconnectTimer) peer.reconnectTimer->deleteLater();
        peers_.remove(deviceID);
        if (channel) channel->close();
        Q_EMIT stateChanged();
        Q_EMIT devicesChanged();
    }
}

void SyncService::handleBlob(Peer &peer, const SyncProtocol::BlobChunk &chunk) {
    auto wire = peer.pendingBlobs.find(chunk.blobId);
    if (wire == peer.pendingBlobs.end()) return;
    QByteArray &data = peer.blobData[chunk.blobId];
    if (chunk.offset != quint32(data.size()) || data.size() + chunk.data.size() > wire->bodySize) {
        peer.pendingBlobs.erase(wire);
        peer.blobData.remove(chunk.blobId);
        return;
    }
    data.append(chunk.data);
    if (data.size() < wire->bodySize) return;
    SyncProtocol::WireItem complete = *wire;
    const QByteArray body = data;
    peer.pendingBlobs.erase(wire);
    peer.blobData.remove(chunk.blobId);
    if (SyncProtocol::applyBody(complete, body)) importWire(peer, complete);
}

void SyncService::importWire(Peer &peer, const SyncProtocol::WireItem &wire) {
    ClipboardItem item = wire.item;
    if (item.originDevice.isEmpty()) item.originDevice = peer.stored.deviceID;
    // Images over this device's own limit are not kept (§7).
    if (item.isImage()) {
        QSettings settings;
        const bool records = settings.value(QStringLiteral("clipboard/recordsImages"), true).toBool();
        const qint64 limit = settings.value(QStringLiteral("clipboard/maximumImageMegabytes"), 10).toLongLong() * 1024 * 1024;
        if (!records || item.imageData.size() > limit) return;
    }
    const auto result = repository_->importRemoteItem(item);
    if (result == HistoryRepository::ImportResult::Inserted || result == HistoryRepository::ImportResult::Updated
        || result == HistoryRepository::ImportResult::Merged) {
        peer.receivedChanges = true;
    }
}

void SyncService::pushChanges(Peer &peer) {
    if (paused_ || !peer.channel || !peer.channel->isEstablished() || peer.pushInFlight) return;
    const QVector<ClipboardItem> changes = repository_->syncChanges(peer.pushedCursor, SyncProtocol::kSyncWindowMs, kBatchSize);
    if (changes.isEmpty()) return;
    peer.pushInFlight = true;
    updateSyncing();
    quint64 last = peer.pushedCursor;
    for (const ClipboardItem &item : changes) {
        ClipboardItem outgoing = item;
        if (outgoing.originDevice.isEmpty()) outgoing.originDevice = deviceID_;
        const SyncProtocol::WireItem wire = SyncProtocol::wireItemFor(outgoing);
        sendJson(peer.channel, SyncProtocol::encodeItem(wire));
        if (!wire.blobId.isEmpty()) {
            const int chunkSize = SyncProtocol::kMaxPlaintext - 20;
            for (int offset = 0; offset < wire.body.size(); offset += chunkSize)
                peer.channel->send(SyncProtocol::blobFrame(wire.blobId, quint32(offset), wire.body.mid(offset, chunkSize)));
        }
        last = std::max(last, item.updatedHlc);
    }
    peer.pushedCursor = last;
    sendJson(peer.channel, {{QStringLiteral("type"), QStringLiteral("batch_end")},
                            {QStringLiteral("hlc"), QString::number(last)}});
    Q_EMIT devicesChanged();
}

void SyncService::updateSyncing() {
    bool busy = false;
    for (const Peer &peer : peers_) busy = busy || peer.pushInFlight || peer.receiving;
    if (busy) {
        syncingOffTimer_.stop();
        if (!syncing_) {
            syncing_ = true;
            Q_EMIT syncingChanged();
        }
    } else if (syncing_ && !syncingOffTimer_.isActive()) {
        syncingOffTimer_.start();
    }
}

// MARK: - Discovery

void SyncService::searchNearby() {
    if (!available_ || !options_.enableDiscovery) return;
    // Keep devices still being identified; drop stale ones.
    nearby_.erase(std::remove_if(nearby_.begin(), nearby_.end(), [](const Nearby &device) { return !device.infoChannel; }),
                  nearby_.end());
    // Searching means pairing is under way: a visible device stays visible
    // for another full period instead of running out mid-pairing.
    if (visible_) {
        visibleUntilMs_ = std::max(visibleUntilMs_, nowMs() + SyncProtocol::kVisibilitySeconds * 1000LL);
        Q_EMIT stateChanged();
    }
    discovery_.query();
    searchTimer_.start();
    Q_EMIT devicesChanged();
}

void SyncService::onReplied(const QHostAddress &from, quint16 portNumber, const QByteArray &tag) {
    const QString key = QStringLiteral("%1:%2").arg(from.toString()).arg(portNumber);
    for (const Nearby &device : nearby_) {
        if (device.key == key) return;
    }
    Nearby device;
    device.key = key;
    device.address = from.toString();
    device.port = portNumber;
    device.tag = tag;
    // The host name travels over an encrypted channel, never in UDP (§4).
    SecureChannel *channel = SecureChannel::connectTo(device.address, portNumber, SecureChannel::Kind::Pairing,
                                                      identity_, {}, this);
    device.infoChannel = channel;
    connect(channel, &SecureChannel::established, this,
            [this, channel] { sendJson(channel, {{QStringLiteral("type"), QStringLiteral("info_request")}}); });
    connect(channel, &SecureChannel::received, this,
            [this, channel](const QByteArray &plaintext) { onChannelReceived(channel, plaintext); });
    connect(channel, &SecureChannel::closed, this, [this, channel] { onChannelClosed(channel); });
    nearby_.append(device);
    Q_EMIT devicesChanged();
}

// MARK: - Pairing

void SyncService::pair(const QString &nearbyKey) {
    for (const Nearby &device : nearby_) {
        if (device.key != nearbyKey) continue;
        pairWithAddress(device.address, device.port);
        pairing_.key = nearbyKey;
        pairing_.hostname = device.hostname;
        Q_EMIT pairingChanged();
        return;
    }
}

void SyncService::pairWithAddress(const QString &host, quint16 portNumber) {
    if (!available_ || pairing_.role != Pairing::Role::None || peers_.size() >= SyncProtocol::kMaxPeers) return;
    clearPairing();
    pairing_.role = Pairing::Role::Outgoing;
    pairing_.stage = Pairing::Stage::Connecting;
    pairing_.address = host;
    SecureChannel *channel = SecureChannel::connectTo(host, portNumber, SecureChannel::Kind::Pairing, identity_, {}, this);
    pairing_.channel = channel;
    connect(channel, &SecureChannel::established, this, [this, channel] {
        sendJson(channel, {{QStringLiteral("type"), QStringLiteral("pair_request")},
                           {QStringLiteral("deviceId"), deviceID_},
                           {QStringLiteral("hostname"), deviceName_}});
    });
    connect(channel, &SecureChannel::received, this,
            [this, channel](const QByteArray &plaintext) { onChannelReceived(channel, plaintext); });
    connect(channel, &SecureChannel::closed, this, [this, channel] { onChannelClosed(channel); });
    Q_EMIT pairingChanged();
}

void SyncService::handlePairingMessage(SecureChannel *channel, const QJsonObject &message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    // Answers to a nearby device's request for our name.
    if (type == QStringLiteral("info_request")) {
        if (visible_) {
            sendJson(channel, {{QStringLiteral("type"), QStringLiteral("info")},
                               {QStringLiteral("deviceId"), deviceID_},
                               {QStringLiteral("hostname"), deviceName_}});
        }
        QTimer::singleShot(500, channel, &SecureChannel::close);
        return;
    }
    if (type == QStringLiteral("info")) {
        for (Nearby &device : nearby_) {
            if (device.infoChannel != channel) continue;
            device.hostname = message.value(QStringLiteral("hostname")).toString().left(64);
            device.deviceID = message.value(QStringLiteral("deviceId")).toString();
            if (peers_.contains(device.deviceID)) {
                // Already paired: a useful fresh address for it.
                Peer &peer = peers_[device.deviceID];
                recordAddress(peer, device.address, device.port);
                savePeer(peer);
            }
        }
        channel->close();
        Q_EMIT devicesChanged();
        return;
    }
    if (type == QStringLiteral("pair_request")) {
        startIncomingPairing(channel, message);
        return;
    }
    if (pairing_.channel != channel) {
        channel->close();
        return;
    }

    if (pairing_.role == Pairing::Role::Outgoing) {
        if (type == QStringLiteral("pair_challenge") || type == QStringLiteral("pair_retry")) {
            pairing_.peerDeviceID = message.value(QStringLiteral("deviceId")).toString(pairing_.peerDeviceID);
            const QString hostname = message.value(QStringLiteral("hostname")).toString();
            if (!hostname.isEmpty()) pairing_.hostname = hostname.left(64);
            pairing_.peerMessage = unb64(message.value(QStringLiteral("cpace")));
            pairing_.attemptsLeft = message.value(QStringLiteral("attemptsLeft")).toInt(pairing_.attemptsLeft);
            pairing_.error = type == QStringLiteral("pair_retry") ? QStringLiteral("配对码不正确") : QString();
            pairing_.stage = Pairing::Stage::WaitingForCode;
            Q_EMIT pairingChanged();
        } else if (type == QStringLiteral("pair_accept")) {
            const QByteArray publicKey = unb64(message.value(QStringLiteral("publicKey")));
            if (pairing_.isk.isEmpty()
                || !SyncCrypto::CPace::verifyConfirmation(pairing_.isk, false, unb64(message.value(QStringLiteral("confirm"))))
                || publicKeyMac(pairing_.isk, publicKey) != unb64(message.value(QStringLiteral("mac")))
                || SyncCrypto::deviceIdFor(publicKey) != pairing_.peerDeviceID) {
                failPairing(QStringLiteral("对方的身份校验失败"));
                return;
            }
            sendJson(channel, {{QStringLiteral("type"), QStringLiteral("pair_finish")},
                               {QStringLiteral("publicKey"), b64(identity_.publicKey)},
                               {QStringLiteral("mac"), b64(publicKeyMac(pairing_.isk, identity_.publicKey))},
                               {QStringLiteral("port"), int(port())}});
            finishPairing(pairing_.peerDeviceID, pairing_.hostname, publicKey, pairing_.address,
                          quint16(message.value(QStringLiteral("port")).toInt()));
        } else if (type == QStringLiteral("pair_reject")) {
            const QString reason = message.value(QStringLiteral("reason")).toString();
            failPairing(reason == QStringLiteral("attempts") ? QStringLiteral("配对码错误次数过多")
                        : reason == QStringLiteral("full")   ? QStringLiteral("对方的已配对设备已满")
                        : reason == QStringLiteral("busy")   ? QStringLiteral("对方正在与其他设备配对")
                        : reason == QStringLiteral("expired") ? QStringLiteral("配对码已过期")
                                                             : QStringLiteral("对方拒绝了配对"));
        }
        return;
    }

    // Incoming: the initiator's answer to the challenge, then its public key.
    if (type == QStringLiteral("pair_response") && pairing_.stage == Pairing::Stage::WaitingForCode) {
        const auto isk = pairing_.cpace ? pairing_.cpace->sharedKey(unb64(message.value(QStringLiteral("cpace"))),
                                                                    pairing_.peerDeviceID.toLatin1())
                                        : std::nullopt;
        if (isk && SyncCrypto::CPace::verifyConfirmation(*isk, true, unb64(message.value(QStringLiteral("confirm"))))) {
            pairing_.isk = *isk;
            pairing_.stage = Pairing::Stage::Finishing;
            sendJson(channel, {{QStringLiteral("type"), QStringLiteral("pair_accept")},
                               {QStringLiteral("confirm"), b64(SyncCrypto::CPace::confirmation(*isk, false))},
                               {QStringLiteral("publicKey"), b64(identity_.publicKey)},
                               {QStringLiteral("mac"), b64(publicKeyMac(*isk, identity_.publicKey))},
                               {QStringLiteral("port"), int(port())}});
            Q_EMIT pairingChanged();
            return;
        }
        if (--pairing_.attemptsLeft <= 0) {
            sendJson(channel, {{QStringLiteral("type"), QStringLiteral("pair_reject")},
                               {QStringLiteral("reason"), QStringLiteral("attempts")}});
            failPairing(QStringLiteral("配对码错误次数过多"));
            return;
        }
        // A fresh CPace exchange for the next attempt (same code).
        sendChallenge();
        return;
    }
    if (type == QStringLiteral("pair_finish") && pairing_.stage == Pairing::Stage::Finishing) {
        const QByteArray publicKey = unb64(message.value(QStringLiteral("publicKey")));
        if (publicKeyMac(pairing_.isk, publicKey) != unb64(message.value(QStringLiteral("mac")))
            || SyncCrypto::deviceIdFor(publicKey) != pairing_.peerDeviceID) {
            failPairing(QStringLiteral("对方的身份校验失败"));
            return;
        }
        finishPairing(pairing_.peerDeviceID, pairing_.hostname, publicKey, channel->peerAddress(),
                      quint16(message.value(QStringLiteral("port")).toInt()));
    }
}

void SyncService::startIncomingPairing(SecureChannel *channel, const QJsonObject &request) {
    auto reject = [this, channel](const QString &reason) {
        sendJson(channel, {{QStringLiteral("type"), QStringLiteral("pair_reject")}, {QStringLiteral("reason"), reason}});
        QTimer::singleShot(500, channel, &SecureChannel::close);
    };
    if (!visible_) return reject(QStringLiteral("rejected"));
    if (pairing_.role != Pairing::Role::None) return reject(QStringLiteral("busy"));
    if (peers_.size() >= SyncProtocol::kMaxPeers) return reject(QStringLiteral("full"));
    const QString peerID = request.value(QStringLiteral("deviceId")).toString();
    if (peerID.size() != 32 || peerID == deviceID_) return reject(QStringLiteral("rejected"));
    clearPairing();
    pairing_.role = Pairing::Role::Incoming;
    pairing_.channel = channel;
    pairing_.peerDeviceID = peerID;
    pairing_.hostname = request.value(QStringLiteral("hostname")).toString().left(64);
    pairing_.address = channel->peerAddress();
    pairing_.code = QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(1000000), 6, 10, QLatin1Char('0'));
    pairing_.attemptsLeft = 5;
    pairing_.expiry = new QTimer(this);
    pairing_.expiry->setSingleShot(true);
    connect(pairing_.expiry, &QTimer::timeout, this, [this] {
        if (pairing_.role != Pairing::Role::Incoming || pairing_.stage == Pairing::Stage::Done) return;
        sendJson(pairing_.channel, {{QStringLiteral("type"), QStringLiteral("pair_reject")},
                                    {QStringLiteral("reason"), QStringLiteral("expired")}});
        failPairing(QStringLiteral("配对码已过期"));
    });
    pairing_.expiry->start(kPairingCodeLifetimeMs);
    visibleUntilMs_ = std::max(visibleUntilMs_, nowMs() + kPairingCodeLifetimeMs);
    sendChallenge();
    Q_EMIT pairingRequested();
}

void SyncService::sendChallenge() {
    pairing_.cpace = std::make_unique<CPaceSession>(false, pairing_.code.toLatin1(), kPairingChannelId,
                                                     pairing_.channel->handshakeHash(), deviceID_.toLatin1());
    pairing_.stage = Pairing::Stage::WaitingForCode;
    sendJson(pairing_.channel, {{QStringLiteral("type"), pairing_.attemptsLeft < 5 ? QStringLiteral("pair_retry")
                                                                                      : QStringLiteral("pair_challenge")},
                                {QStringLiteral("deviceId"), deviceID_},
                                {QStringLiteral("hostname"), deviceName_},
                                {QStringLiteral("cpace"), b64(pairing_.cpace->message())},
                                {QStringLiteral("attemptsLeft"), pairing_.attemptsLeft}});
    Q_EMIT pairingChanged();
}

void SyncService::submitPairingCode(const QString &code) {
    if (pairing_.role != Pairing::Role::Outgoing || pairing_.stage != Pairing::Stage::WaitingForCode || !pairing_.channel)
        return;
    const QString digits = code.trimmed();
    if (digits.size() != 6) return;
    CPaceSession session(true, digits.toLatin1(), kPairingChannelId, pairing_.channel->handshakeHash(),
                         deviceID_.toLatin1());
    const auto isk = session.sharedKey(pairing_.peerMessage, pairing_.peerDeviceID.toLatin1());
    if (!isk) {
        failPairing(QStringLiteral("配对数据无效"));
        return;
    }
    pairing_.isk = *isk;
    pairing_.stage = Pairing::Stage::Verifying;
    pairing_.error.clear();
    sendJson(pairing_.channel, {{QStringLiteral("type"), QStringLiteral("pair_response")},
                                {QStringLiteral("cpace"), b64(session.message())},
                                {QStringLiteral("confirm"), b64(SyncCrypto::CPace::confirmation(*isk, true))}});
    Q_EMIT pairingChanged();
}

void SyncService::finishPairing(const QString &deviceID, const QString &hostname, const QByteArray &publicKey,
                                const QString &address, quint16 portNumber) {
    Peer &peer = peers_[deviceID];
    peer.stored.deviceID = deviceID;
    peer.stored.hostname = hostname.isEmpty() ? QStringLiteral("ClipDeck") : hostname;
    peer.stored.publicKey = publicKey;
    peer.stored.sentCursor = 0;
    peer.stored.lastSeenMs = nowMs();
    recordAddress(peer, address, portNumber);
    keyStore_.forgetUnpaired(deviceID);
    keyStore_.setLocateKey(deviceID, SyncCrypto::hmacBlake2b(pairing_.isk, QByteArrayLiteral("ClipDeck locate key")).left(32));
    keyStore_.save();
    savePeer(peer);
    pairing_.stage = Pairing::Stage::Done;
    SecureChannel *channel = pairing_.channel;
    Q_EMIT pairingChanged();
    if (channel) QTimer::singleShot(300, channel, &SecureChannel::close);
    QTimer::singleShot(1500, this, [this] {
        if (pairing_.stage == Pairing::Stage::Done) {
            clearPairing();
            Q_EMIT pairingChanged();
        }
    });
    Q_EMIT stateChanged();
    Q_EMIT devicesChanged();
    // The smaller device ID opens the sync connection.
    QTimer::singleShot(400, this, [this, deviceID] { connectToPeerNow(deviceID); });
}

void SyncService::failPairing(const QString &error) {
    if (pairing_.role == Pairing::Role::None) return;
    pairing_.stage = Pairing::Stage::Failed;
    pairing_.error = error;
    pairing_.isk.clear();
    if (pairing_.expiry) pairing_.expiry->stop();
    if (pairing_.channel) QTimer::singleShot(300, pairing_.channel, &SecureChannel::close);
    Q_EMIT pairingChanged();
}

void SyncService::cancelPairing() {
    if (pairing_.channel) pairing_.channel->close();
    clearPairing();
    Q_EMIT pairingChanged();
}

void SyncService::rejectPairing() {
    if (pairing_.role != Pairing::Role::Incoming) return;
    sendJson(pairing_.channel, {{QStringLiteral("type"), QStringLiteral("pair_reject")},
                                {QStringLiteral("reason"), QStringLiteral("rejected")}});
    if (pairing_.channel) QTimer::singleShot(300, pairing_.channel, &SecureChannel::close);
    clearPairing();
    Q_EMIT pairingChanged();
}

void SyncService::clearPairing() {
    if (pairing_.expiry) pairing_.expiry->deleteLater();
    pairing_.role = Pairing::Role::None;
    pairing_.stage = Pairing::Stage::Connecting;
    pairing_.channel.clear();
    pairing_.key.clear();
    pairing_.address.clear();
    pairing_.hostname.clear();
    pairing_.peerDeviceID.clear();
    pairing_.code.clear();
    pairing_.peerMessage.clear();
    pairing_.cpace.reset();
    pairing_.isk.clear();
    pairing_.attemptsLeft = 5;
    pairing_.error.clear();
    pairing_.expiry = nullptr;
}

void SyncService::unpair(const QString &deviceID) {
    auto it = peers_.find(deviceID);
    if (it == peers_.end()) return;
    SecureChannel *channel = it->channel;
    if (channel && channel->isEstablished()) {
        sendJson(channel, {{QStringLiteral("type"), QStringLiteral("unpair")}});
        QTimer::singleShot(500, channel, &SecureChannel::close);
        keyStore_.markUnpaired(deviceID, QByteArray());
        keyStore_.forgetUnpaired(deviceID);
    } else {
        // Offline: remember its key so it can be told when it reconnects.
        keyStore_.markUnpaired(deviceID, it->stored.publicKey);
    }
    keyStore_.save();
    repository_->removeSyncPeer(deviceID);
    for (const auto &attempt : it->attempts) {
        if (attempt) attempt->close();
    }
    if (it->reconnectTimer) it->reconnectTimer->deleteLater();
    peers_.erase(it);
    Q_EMIT stateChanged();
    Q_EMIT devicesChanged();
}
