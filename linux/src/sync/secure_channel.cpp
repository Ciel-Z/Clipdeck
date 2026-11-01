#include "secure_channel.h"
#include "sync_protocol.h"

#include <QNetworkProxy>

namespace {

const QByteArray kHeaderMagic = QByteArrayLiteral("CDK1");
const QByteArray kPrologue = QByteArrayLiteral("ClipDeck LAN sync v1");

} // namespace

SecureChannel::SecureChannel(QTcpSocket *socket, bool initiator, const SyncCrypto::KeyPair &localStatic,
                             QObject *parent)
    : QObject(parent), socket_(socket), initiator_(initiator), localStatic_(localStatic) {
    socket_->setParent(this);
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket_->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    handshakeTimer_.setSingleShot(true);
    handshakeTimer_.setInterval(kHandshakeTimeoutMs);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this] { fail(QStringLiteral("handshake timed out")); });
    connect(socket_, &QTcpSocket::readyRead, this, &SecureChannel::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, [this] { fail(QStringLiteral("disconnected")); });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket_, &QTcpSocket::errorOccurred, this, [this] { fail(socket_->errorString()); });
#else
    connect(socket_, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this,
            [this] { fail(socket_->errorString()); });
#endif
    handshakeTimer_.start();
}

SecureChannel::~SecureChannel() {
    if (socket_) socket_->disconnect(this);
}

SecureChannel *SecureChannel::connectTo(const QString &host, quint16 port, Kind kind,
                                        const SyncCrypto::KeyPair &localStatic, const Candidate &peer,
                                        QObject *parent) {
    auto *socket = new QTcpSocket;
    // A direct LAN connection: a system HTTP proxy (Clash and the like) cannot
    // carry it, and Qt would otherwise apply one and fail at once.
    socket->setProxy(QNetworkProxy::NoProxy);
    auto *channel = new SecureChannel(socket, true, localStatic, parent);
    channel->kind_ = kind;
    channel->peer_ = peer;
    connect(socket, &QTcpSocket::connected, channel, [channel] {
        QByteArray header = kHeaderMagic;
        header.append(char(channel->kind_));
        channel->writeFrame(header);
        channel->startHandshake();
    });
    // Next turn of the event loop, so an error raised at once still reaches
    // the caller's `closed` handler, which it connects after this returns.
    QMetaObject::invokeMethod(socket, [socket, host, port] { socket->connectToHost(host, port); },
                              Qt::QueuedConnection);
    return channel;
}

SecureChannel *SecureChannel::accept(QTcpSocket *socket, const SyncCrypto::KeyPair &localStatic,
                                     const QVector<Candidate> &candidates, QObject *parent) {
    auto *channel = new SecureChannel(socket, false, localStatic, parent);
    channel->candidates_ = candidates;
    if (socket->bytesAvailable() > 0) QMetaObject::invokeMethod(channel, &SecureChannel::onReadyRead, Qt::QueuedConnection);
    return channel;
}

QString SecureChannel::peerAddress() const {
    if (!socket_) return {};
    QHostAddress address = socket_->peerAddress();
    bool mapped = false;
    const quint32 ipv4 = address.toIPv4Address(&mapped);
    return mapped ? QHostAddress(ipv4).toString() : address.toString();
}
quint16 SecureChannel::peerPort() const { return socket_ ? socket_->peerPort() : 0; }
QString SecureChannel::localAddress() const {
    if (!socket_) return {};
    bool mapped = false;
    const quint32 ipv4 = socket_->localAddress().toIPv4Address(&mapped);
    return mapped ? QHostAddress(ipv4).toString() : socket_->localAddress().toString();
}

void SecureChannel::startHandshake() {
    using Pattern = SyncCrypto::HandshakeState::Pattern;
    if (kind_ == Kind::Pairing) {
        handshake_ = std::make_unique<SyncCrypto::HandshakeState>(Pattern::NN, true, kPrologue);
    } else {
        handshake_ = std::make_unique<SyncCrypto::HandshakeState>(Pattern::KK, true, kPrologue, localStatic_,
                                                                 peer_.publicKey);
    }
    const auto message = handshake_->writeMessage({});
    if (!message) {
        fail(QStringLiteral("handshake could not start"));
        return;
    }
    writeFrame(*message);
}

void SecureChannel::onReadyRead() {
    if (!socket_) return;
    buffer_.append(socket_->readAll());
    QList<QByteArray> frames;
    if (!SyncProtocol::takeFrames(buffer_, frames)) {
        fail(QStringLiteral("malformed frame"));
        return;
    }
    for (const QByteArray &frame : frames) {
        if (closing_) return;
        handleFrame(frame);
    }
}

void SecureChannel::handleFrame(const QByteArray &frame) {
    using Pattern = SyncCrypto::HandshakeState::Pattern;
    if (established_) {
        const auto plaintext = receive_.decryptWithAd({}, frame);
        if (!plaintext) {
            fail(QStringLiteral("frame failed authentication"));
            return;
        }
        if (++receivedSinceRekey_ >= kRekeyInterval) {
            receive_.rekey();
            receivedSinceRekey_ = 0;
        }
        Q_EMIT received(*plaintext);
        return;
    }
    if (initiator_) {
        // Message 2 of the handshake.
        if (!handshake_ || !handshake_->readMessage(frame)) {
            fail(QStringLiteral("handshake rejected"));
            return;
        }
        finishHandshake(*handshake_);
        return;
    }
    if (!headerSeen_) {
        if (frame.size() != 5 || !frame.startsWith(kHeaderMagic)
            || (frame[4] != char(Kind::Pairing) && frame[4] != char(Kind::Sync))) {
            fail(QStringLiteral("not a ClipDeck connection"));
            return;
        }
        kind_ = Kind(frame[4]);
        headerSeen_ = true;
        return;
    }
    // Message 1: find the handshake that accepts it.
    if (kind_ == Kind::Pairing) {
        handshake_ = std::make_unique<SyncCrypto::HandshakeState>(Pattern::NN, false, kPrologue);
        if (!handshake_->readMessage(frame)) {
            fail(QStringLiteral("handshake rejected"));
            return;
        }
    } else {
        for (const Candidate &candidate : candidates_) {
            auto attempt = std::make_unique<SyncCrypto::HandshakeState>(Pattern::KK, false, kPrologue, localStatic_,
                                                                       candidate.publicKey);
            if (attempt->readMessage(frame)) {
                handshake_ = std::move(attempt);
                peer_ = candidate;
                break;
            }
        }
        if (!handshake_) {
            fail(QStringLiteral("unknown device"));
            return;
        }
    }
    const auto reply = handshake_->writeMessage({});
    if (!reply) {
        fail(QStringLiteral("handshake failed"));
        return;
    }
    writeFrame(*reply);
    finishHandshake(*handshake_);
}

void SecureChannel::finishHandshake(SyncCrypto::HandshakeState &state) {
    if (!state.isComplete()) {
        fail(QStringLiteral("handshake incomplete"));
        return;
    }
    send_ = state.sendCipher();
    receive_ = state.receiveCipher();
    handshakeHash_ = state.handshakeHash();
    handshake_.reset();
    handshakeTimer_.stop();
    established_ = true;
    Q_EMIT established();
}

bool SecureChannel::writeFrame(const QByteArray &payload) {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) return false;
    return socket_->write(SyncProtocol::frameBytes(payload)) >= 0;
}

bool SecureChannel::send(const QByteArray &plaintext) {
    if (!established_ || closing_ || plaintext.size() > SyncProtocol::kMaxPlaintext + 1) return false;
    const auto ciphertext = send_.encryptWithAd({}, plaintext);
    if (!ciphertext || !writeFrame(*ciphertext)) return false;
    if (++sentSinceRekey_ >= kRekeyInterval) {
        send_.rekey();
        sentSinceRekey_ = 0;
    }
    return true;
}

void SecureChannel::close() {
    if (closing_) return;
    closing_ = true;
    handshakeTimer_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->flush();
        socket_->disconnectFromHost();
    }
    Q_EMIT closed(QStringLiteral("closed"));
}

void SecureChannel::fail(const QString &reason) {
    if (closing_) return;
    closing_ = true;
    handshakeTimer_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
    Q_EMIT closed(reason);
}
