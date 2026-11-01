#include "cpace.h"
#include "noise.h"

#include <sodium.h>

#include <algorithm>

namespace SyncCrypto {
namespace {

const QByteArray kDsi = QByteArrayLiteral("CPaceRistretto255");
const QByteArray kDsiIsk = QByteArrayLiteral("CPaceRistretto255_ISK");
constexpr int kHashInputBlock = 128; // SHA-512 s_in_bytes

const unsigned char *bytes(const QByteArray &data) {
    return reinterpret_cast<const unsigned char *>(data.constData());
}
unsigned char *bytes(QByteArray &data) { return reinterpret_cast<unsigned char *>(data.data()); }

QByteArray sha512(const QByteArray &data) {
    QByteArray out(crypto_hash_sha512_BYTES, Qt::Uninitialized);
    crypto_hash_sha512(bytes(out), bytes(data), data.size());
    return out;
}

QByteArray hmacSha512(const QByteArray &key, const QByteArray &data) {
    QByteArray out(crypto_auth_hmacsha512_BYTES, Qt::Uninitialized);
    crypto_auth_hmacsha512_state state;
    crypto_auth_hmacsha512_init(&state, bytes(key), key.size());
    crypto_auth_hmacsha512_update(&state, bytes(data), data.size());
    crypto_auth_hmacsha512_final(&state, bytes(out));
    return out;
}

} // namespace

QByteArray CPace::lengthValue(const QByteArray &data) {
    QByteArray out;
    quint64 length = quint64(data.size());
    do {
        unsigned char byte = length & 0x7f;
        length >>= 7;
        if (length) byte |= 0x80;
        out.append(char(byte));
    } while (length);
    return out + data;
}

QByteArray CPace::generatorString(const QByteArray &prs, const QByteArray &ci, const QByteArray &sid) {
    const int used = 1 + lengthValue(prs).size() + lengthValue(kDsi).size();
    const int zeroPad = std::max(0, kHashInputBlock - used);
    return lengthValue(kDsi) + lengthValue(prs) + lengthValue(QByteArray(zeroPad, '\0')) + lengthValue(ci)
        + lengthValue(sid);
}

std::optional<QByteArray> CPace::generator(const QByteArray &prs, const QByteArray &ci, const QByteArray &sid) {
    initialize();
    const QByteArray hash = sha512(generatorString(prs, ci, sid));
    QByteArray point(kPointLength, Qt::Uninitialized);
    crypto_core_ristretto255_from_hash(bytes(point), bytes(hash));
    return point;
}

CPace::CPace(bool initiator, const QByteArray &prs, const QByteArray &ci, const QByteArray &sid,
             const QByteArray &ad, const QByteArray &fixedScalar)
    : initiator_(initiator), sid_(sid), ad_(ad) {
    initialize();
    const auto g = generator(prs, ci, sid);
    if (!g) return;
    scalar_ = fixedScalar;
    if (scalar_.size() != crypto_core_ristretto255_SCALARBYTES) {
        scalar_ = QByteArray(crypto_core_ristretto255_SCALARBYTES, Qt::Uninitialized);
        crypto_core_ristretto255_scalar_random(bytes(scalar_));
    }
    y_ = QByteArray(kPointLength, Qt::Uninitialized);
    ok_ = crypto_scalarmult_ristretto255(bytes(y_), bytes(scalar_), bytes(*g)) == 0;
}

std::optional<QByteArray> CPace::sharedKey(const QByteArray &peerMessage, const QByteArray &peerAd) const {
    if (!ok_ || peerMessage.size() != kPointLength) return std::nullopt;
    QByteArray k(kPointLength, Qt::Uninitialized);
    // scalar_mult_vfy: an invalid encoding or the identity element aborts.
    if (!crypto_core_ristretto255_is_valid_point(bytes(peerMessage))
        || crypto_scalarmult_ristretto255(bytes(k), bytes(scalar_), bytes(peerMessage)) != 0) {
        return std::nullopt;
    }
    const QByteArray &ya = initiator_ ? y_ : peerMessage;
    const QByteArray &ada = initiator_ ? ad_ : peerAd;
    const QByteArray &yb = initiator_ ? peerMessage : y_;
    const QByteArray &adb = initiator_ ? peerAd : ad_;
    const QByteArray transcript = lengthValue(ya) + lengthValue(ada) + lengthValue(yb) + lengthValue(adb);
    return sha512(lengthValue(kDsiIsk) + lengthValue(sid_) + lengthValue(k) + transcript);
}

QByteArray CPace::confirmation(const QByteArray &isk, bool fromInitiator) {
    initialize();
    return hmacSha512(isk, fromInitiator ? QByteArrayLiteral("ClipDeck pairing confirm initiator")
                                         : QByteArrayLiteral("ClipDeck pairing confirm responder"));
}

bool CPace::verifyConfirmation(const QByteArray &isk, bool fromInitiator, const QByteArray &tag) {
    const QByteArray expected = confirmation(isk, fromInitiator);
    return tag.size() == expected.size() && crypto_verify_64(bytes(tag), bytes(expected)) == 0;
}

} // namespace SyncCrypto
