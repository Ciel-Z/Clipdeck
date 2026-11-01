#pragma once

#include "noise.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

#include <memory>

// One encrypted TCP connection (docs/lan-sync-design.md §7 and §9.5).
//
// The connection opens with one plaintext frame, "CDK1" and a kind byte:
// 'P' for pairing (Noise NN, no identities yet) or 'S' for sync (Noise KK,
// both sides paired). Every later frame is encrypted. A KK responder does
// not know who is connecting, so it tries each paired device's public key
// until the first handshake message decrypts (at most a handful).
class SecureChannel final : public QObject {
    Q_OBJECT
public:
    enum class Kind : char { Pairing = 'P', Sync = 'S' };

    struct Candidate {
        QString deviceID;
        QByteArray publicKey;
    };

    // Initiator: knows the kind and, for sync, the peer.
    static SecureChannel *connectTo(const QString &host, quint16 port, Kind kind,
                                    const SyncCrypto::KeyPair &localStatic, const Candidate &peer,
                                    QObject *parent = nullptr);
    // Responder: accepts any kind; for sync, `candidates` are the known peers.
    static SecureChannel *accept(QTcpSocket *socket, const SyncCrypto::KeyPair &localStatic,
                                 const QVector<Candidate> &candidates, QObject *parent = nullptr);
    ~SecureChannel() override;

    bool isEstablished() const { return established_; }
    bool isInitiator() const { return initiator_; }
    Kind kind() const { return kind_; }
    QString peerDeviceID() const { return peer_.deviceID; }
    QByteArray peerPublicKey() const { return peer_.publicKey; }
    QByteArray handshakeHash() const { return handshakeHash_; }
    QString peerAddress() const;
    quint16 peerPort() const;
    QString localAddress() const;

    bool send(const QByteArray &plaintext);
    void close();

    static constexpr int kHandshakeTimeoutMs = 10000;
    static constexpr int kRekeyInterval = 10000; // frames

Q_SIGNALS:
    void established();
    void received(const QByteArray &plaintext);
    void closed(const QString &reason);

private:
    SecureChannel(QTcpSocket *socket, bool initiator, const SyncCrypto::KeyPair &localStatic, QObject *parent);
    void onReadyRead();
    void handleFrame(const QByteArray &frame);
    void startHandshake();
    void finishHandshake(SyncCrypto::HandshakeState &state);
    void fail(const QString &reason);
    bool writeFrame(const QByteArray &payload);

    QPointer<QTcpSocket> socket_;
    bool initiator_;
    SyncCrypto::KeyPair localStatic_;
    Kind kind_ = Kind::Pairing;
    Candidate peer_;
    QVector<Candidate> candidates_;
    std::unique_ptr<SyncCrypto::HandshakeState> handshake_;
    SyncCrypto::CipherState send_;
    SyncCrypto::CipherState receive_;
    QByteArray handshakeHash_;
    QByteArray buffer_;
    bool headerSeen_ = false;
    bool established_ = false;
    bool closing_ = false;
    int sentSinceRekey_ = 0;
    int receivedSinceRekey_ = 0;
    QTimer handshakeTimer_;
};
