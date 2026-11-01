#pragma once

#include <QByteArray>

#include <optional>

// CPace (draft-irtf-cfrg-cpace-13) with ristretto255 and SHA-512: turns the
// 6-digit pairing code into a strong shared key without the code crossing the
// network, so a recorded pairing cannot be brute-forced offline
// (docs/lan-sync-design.md §9.4). Initiator/responder ordering ("ir").
namespace SyncCrypto {

class CPace {
public:
    static constexpr int kPointLength = 32;
    static constexpr int kKeyLength = 64;

    // prs: the pairing code; ci: channel identifier; sid: session ID (the NN
    // handshake hash); ad: this side's associated data (its device ID).
    // `fixedScalar` is for test vectors only.
    CPace(bool initiator, const QByteArray &prs, const QByteArray &ci, const QByteArray &sid,
          const QByteArray &ad, const QByteArray &fixedScalar = {});

    bool ok() const { return ok_; }
    // This side's public message Y (32 bytes).
    QByteArray message() const { return y_; }
    // The intermediate session key from the other side's message and AD;
    // nullopt for an invalid or identity point.
    std::optional<QByteArray> sharedKey(const QByteArray &peerMessage, const QByteArray &peerAd) const;

    // Key confirmation (ClipDeck's addition): each side proves it holds the
    // same ISK; a wrong pairing code fails here.
    static QByteArray confirmation(const QByteArray &isk, bool fromInitiator);
    static bool verifyConfirmation(const QByteArray &isk, bool fromInitiator, const QByteArray &tag);

    // Exposed for test vectors.
    static QByteArray generatorString(const QByteArray &prs, const QByteArray &ci, const QByteArray &sid);
    static std::optional<QByteArray> generator(const QByteArray &prs, const QByteArray &ci, const QByteArray &sid);
    static QByteArray lengthValue(const QByteArray &data); // prepend_len (LEB128 length)

private:
    bool initiator_;
    QByteArray sid_;
    QByteArray ad_;
    QByteArray scalar_;
    QByteArray y_;
    bool ok_ = false;
};

} // namespace SyncCrypto
