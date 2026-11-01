#include "discovery.h"
#include "noise.h"

#include <QDateTime>
#include <QNetworkDatagram>
#include <QTimer>

SyncDiscovery::SyncDiscovery(QObject *parent) : QObject(parent) {
    connect(&socket_, &QUdpSocket::readyRead, this, &SyncDiscovery::onReadyRead);
    connect(&querySocket_, &QUdpSocket::readyRead, this, &SyncDiscovery::onQueryReplies);
}

QVector<SyncNetwork::Interface> SyncDiscovery::interfaces() const {
    QVector<SyncNetwork::Interface> result = SyncNetwork::eligibleInterfaces();
    if (includeLoopback_) {
        for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
            if (!(networkInterface.flags() & QNetworkInterface::IsLoopBack)) continue;
            SyncNetwork::Interface loopback;
            loopback.networkInterface = networkInterface;
            loopback.address = QHostAddress(QHostAddress::LocalHost);
            result.append(loopback);
        }
    }
    return result;
}

bool SyncDiscovery::start() {
    if (isListening()) return true;
    if (!socket_.bind(QHostAddress(QHostAddress::AnyIPv4), SyncProtocol::kDiscoveryPort,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        return false;
    }
    socket_.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    socket_.setSocketOption(QAbstractSocket::MulticastLoopbackOption, includeLoopback_ ? 1 : 0);
    for (const SyncNetwork::Interface &candidate : interfaces())
        socket_.joinMulticastGroup(SyncProtocol::discoveryGroup(), candidate.networkInterface);
    if (querySocket_.state() != QAbstractSocket::BoundState) {
        querySocket_.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
        querySocket_.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
        querySocket_.setSocketOption(QAbstractSocket::MulticastLoopbackOption, includeLoopback_ ? 1 : 0);
    }
    return true;
}

void SyncDiscovery::stop() {
    socket_.close();
    querySocket_.close();
    visible_ = false;
}

void SyncDiscovery::setVisible(bool visible, const QByteArray &tag, quint16 tcpPort) {
    visible_ = visible;
    tag_ = tag;
    tcpPort_ = tcpPort;
}

void SyncDiscovery::query() {
    if (!start()) return;
    sendQueryRound();
    QTimer::singleShot(1000, this, &SyncDiscovery::sendQueryRound);
    QTimer::singleShot(3000, this, &SyncDiscovery::sendQueryRound);
}

void SyncDiscovery::sendQueryRound() {
    SyncProtocol::DiscoveryPacket packet;
    packet.type = SyncProtocol::PacketType::Query;
    packet.nonce = SyncCrypto::randomBytes(8);
    ownNonces_.insert(packet.nonce);
    if (ownNonces_.size() > 64) ownNonces_.clear();
    sendToGroup(querySocket_, SyncProtocol::encodePacket(packet));
}

void SyncDiscovery::locate(const QByteArray &token, quint16 tcpPort, const QHostAddress &target) {
    if (!start()) return;
    SyncProtocol::DiscoveryPacket packet;
    packet.type = SyncProtocol::PacketType::Locate;
    packet.locate = token;
    packet.tcpPort = tcpPort;
    const QByteArray datagram = SyncProtocol::encodePacket(packet);
    if (!target.isNull()) socket_.writeDatagram(datagram, target, SyncProtocol::kDiscoveryPort);
    else sendToGroup(socket_, datagram);
}

void SyncDiscovery::sendToGroup(QUdpSocket &socket, const QByteArray &datagram) {
    // Each interface separately, so a machine on several LANs asks on all.
    for (const SyncNetwork::Interface &candidate : interfaces()) {
        socket.setMulticastInterface(candidate.networkInterface);
        socket.writeDatagram(datagram, SyncProtocol::discoveryGroup(), SyncProtocol::kDiscoveryPort);
    }
}

void SyncDiscovery::onQueryReplies() {
    while (querySocket_.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = querySocket_.receiveDatagram(256);
        const auto packet = SyncProtocol::decodePacket(datagram.data());
        if (!packet || packet->type != SyncProtocol::PacketType::Reply || !ownNonces_.contains(packet->nonce)) continue;
        Q_EMIT replied(QHostAddress(datagram.senderAddress().toIPv4Address()), packet->tcpPort, packet->tag);
    }
}

void SyncDiscovery::onReadyRead() {
    while (socket_.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket_.receiveDatagram(256);
        const auto packet = SyncProtocol::decodePacket(datagram.data());
        if (!packet) continue;
        const QHostAddress from(datagram.senderAddress().toIPv4Address());
        const QString key = from.toString();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        switch (packet->type) {
        case SyncProtocol::PacketType::Query: {
            if (!visible_ || ownNonces_.contains(packet->nonce)) break;
            if (now - lastReplyTo_.value(key) < 2000) break; // at most one reply per source every 2 s
            lastReplyTo_.insert(key, now);
            SyncProtocol::DiscoveryPacket reply;
            reply.type = SyncProtocol::PacketType::Reply;
            reply.nonce = packet->nonce;
            reply.tag = tag_;
            reply.tcpPort = tcpPort_;
            socket_.writeDatagram(SyncProtocol::encodePacket(reply), from, datagram.senderPort());
            break;
        }
        case SyncProtocol::PacketType::Reply:
            break; // replies arrive on querySocket_
        case SyncProtocol::PacketType::Locate:
            if (now - lastLocateFrom_.value(key) < 2000) break;
            lastLocateFrom_.insert(key, now);
            Q_EMIT located(from, packet->tcpPort, packet->locate);
            break;
        }
    }
}
