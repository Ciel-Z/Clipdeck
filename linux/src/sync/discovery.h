#pragma once

#include "network_interfaces.h"
#include "sync_protocol.h"

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QUdpSocket>

// UDP discovery on demand (docs/lan-sync-design.md §4–5). Nothing is sent
// unless the sync panel asks for nearby devices (three queries, then quiet)
// or a paired device cannot be reached (one locate packet). A visible device
// answers queries by unicast only; an invisible one does not answer at all.
class SyncDiscovery final : public QObject {
    Q_OBJECT
public:
    explicit SyncDiscovery(QObject *parent = nullptr);

    bool start();
    void stop();
    // Joins the group again on every eligible interface; the OS can drop a
    // membership without telling us.
    void rejoin();
    bool isListening() const { return socket_.state() == QAbstractSocket::BoundState; }

    // While visible, queries are answered with `tag` and `tcpPort`.
    void setVisible(bool visible, const QByteArray &tag, quint16 tcpPort);
    // Sends a query on every eligible interface at 0 s, 1 s and 3 s.
    void query();
    // One locate packet per interface; `token` comes from a paired device's
    // locate key. `target`, when set, receives it by unicast instead.
    void locate(const QByteArray &token, quint16 tcpPort, const QHostAddress &target = QHostAddress());

    // Tests on one machine: also use the loopback interface.
    void setIncludeLoopback(bool include) { includeLoopback_ = include; }

Q_SIGNALS:
    void replied(const QHostAddress &from, quint16 tcpPort, const QByteArray &tag);
    void located(const QHostAddress &from, quint16 tcpPort, const QByteArray &token);

private:
    void onReadyRead();
    void onQueryReplies();
    void sendQueryRound();
    void sendToGroup(QUdpSocket &socket, const QByteArray &datagram);
    QVector<SyncNetwork::Interface> interfaces() const;

    QUdpSocket socket_;      // the shared discovery port: queries in, locates in and out
    QUdpSocket querySocket_; // our own queries go out here, so replies reach only us
    bool visible_ = false;
    bool includeLoopback_ = false;
    QByteArray tag_;
    quint16 tcpPort_ = 0;
    QSet<QByteArray> ownNonces_;
    QHash<QString, qint64> lastReplyTo_;
    QHash<QString, qint64> lastLocateFrom_;
};
