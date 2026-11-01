#include "noise.h"

#include <sodium.h>

#include <mutex>

namespace SyncCrypto {
namespace {

constexpr int kDhLength = 32;
constexpr int kHashLength = 64;
constexpr int kBlockLength = 128;

const unsigned char *bytes(const QByteArray &data) {
    return reinterpret_cast<const unsigned char *>(data.constData());
}
unsigned char *bytes(QByteArray &data) { return reinterpret_cast<unsigned char *>(data.data()); }

QByteArray blake2b(const QByteArray &data) {
    QByteArray out(kHashLength, Qt::Uninitialized);
    crypto_generichash(bytes(out), out.size(), bytes(data), data.size(), nullptr, 0);
    return out;
}

// The 96-bit ChaChaPoly nonce: 32 zero bits, then the counter little-endian.
std::array<unsigned char, 12> nonceBytes(quint64 n) {
    std::array<unsigned char, 12> nonce{};
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<unsigned char>(n >> (8 * i));
    return nonce;
}

} // namespace

bool initialize() {
    static int result = -1;
    static std::once_flag once;
    std::call_once(once, [] { result = sodium_init(); });
    return result >= 0;
}

KeyPair generateKeyPair() {
    initialize();
    KeyPair pair;
    pair.privateKey = QByteArray(kDhLength, Qt::Uninitialized);
    randombytes_buf(bytes(pair.privateKey), kDhLength);
    pair.publicKey = publicKeyFor(pair.privateKey);
    return pair;
}

QByteArray publicKeyFor(const QByteArray &privateKey) {
    initialize();
    QByteArray publicKey(kDhLength, Qt::Uninitialized);
    crypto_scalarmult_curve25519_base(bytes(publicKey), bytes(privateKey));
    return publicKey;
}

QString deviceIdFor(const QByteArray &publicKey) {
    initialize();
    QByteArray digest(16, Qt::Uninitialized);
    crypto_generichash(bytes(digest), digest.size(), bytes(publicKey), publicKey.size(), nullptr, 0);
    return QString::fromLatin1(digest.toHex());
}

QByteArray randomBytes(int length) {
    initialize();
    QByteArray out(length, Qt::Uninitialized);
    randombytes_buf(bytes(out), out.size());
    return out;
}

QByteArray hmacBlake2b(const QByteArray &key, const QByteArray &data) {
    QByteArray block = key.size() > kBlockLength ? blake2b(key) : key;
    block.append(QByteArray(kBlockLength - block.size(), '\0')); // Qt 5 has no fill-resize
    QByteArray inner(kBlockLength, Qt::Uninitialized);
    QByteArray outer(kBlockLength, Qt::Uninitialized);
    for (int i = 0; i < kBlockLength; ++i) {
        inner[i] = char(block[i] ^ 0x36);
        outer[i] = char(block[i] ^ 0x5c);
    }
    return blake2b(outer + blake2b(inner + data));
}

void hkdf(const QByteArray &chainingKey, const QByteArray &inputKeyMaterial, QByteArray *out1,
          QByteArray *out2, QByteArray *out3) {
    const QByteArray tempKey = hmacBlake2b(chainingKey, inputKeyMaterial);
    const QByteArray first = hmacBlake2b(tempKey, QByteArray(1, '\x01'));
    const QByteArray second = hmacBlake2b(tempKey, first + QByteArray(1, '\x02'));
    if (out1) *out1 = first;
    if (out2) *out2 = second;
    if (out3) *out3 = hmacBlake2b(tempKey, second + QByteArray(1, '\x03'));
}

// MARK: - CipherState

void CipherState::initializeKey(const QByteArray &key) {
    hasKey_ = key.size() == 32;
    if (hasKey_) std::copy(key.cbegin(), key.cend(), key_.begin());
    else sodium_memzero(key_.data(), key_.size());
    nonce_ = 0;
}

std::optional<QByteArray> CipherState::encryptWithAd(const QByteArray &ad, const QByteArray &plaintext) {
    if (!hasKey_) return plaintext;
    if (nonce_ == kMaxNonce) return std::nullopt; // reserved for rekey
    QByteArray out(plaintext.size() + kTagLength, Qt::Uninitialized);
    unsigned long long length = 0;
    const auto nonce = nonceBytes(nonce_);
    crypto_aead_chacha20poly1305_ietf_encrypt(bytes(out), &length, bytes(plaintext), plaintext.size(),
                                              bytes(ad), ad.size(), nullptr, nonce.data(), key_.data());
    ++nonce_;
    out.resize(int(length));
    return out;
}

std::optional<QByteArray> CipherState::decryptWithAd(const QByteArray &ad, const QByteArray &ciphertext) {
    if (!hasKey_) return ciphertext;
    if (nonce_ == kMaxNonce || ciphertext.size() < kTagLength) return std::nullopt;
    QByteArray out(ciphertext.size() - kTagLength, Qt::Uninitialized);
    unsigned long long length = 0;
    const auto nonce = nonceBytes(nonce_);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(bytes(out), &length, nullptr, bytes(ciphertext),
                                                  ciphertext.size(), bytes(ad), ad.size(), nonce.data(),
                                                  key_.data()) != 0) {
        return std::nullopt; // the nonce does not advance on failure
    }
    ++nonce_;
    out.resize(int(length));
    return out;
}

void CipherState::rekey() {
    if (!hasKey_) return;
    // k = first 32 bytes of ENCRYPT(k, maxnonce, zerolen, zeros)
    unsigned char zeros[32] = {};
    unsigned char out[32 + kTagLength];
    unsigned long long length = 0;
    const auto nonce = nonceBytes(kMaxNonce);
    crypto_aead_chacha20poly1305_ietf_encrypt(out, &length, zeros, sizeof zeros, nullptr, 0, nullptr,
                                              nonce.data(), key_.data());
    std::copy(out, out + 32, key_.begin());
    sodium_memzero(out, sizeof out);
}

// MARK: - HandshakeState

HandshakeState::HandshakeState(Pattern pattern, bool initiator, const QByteArray &prologue,
                               const KeyPair &localStatic, const QByteArray &remoteStaticPublic,
                               const QByteArray &fixedEphemeralPrivate)
    : pattern_(pattern), initiator_(initiator), s_(localStatic), rs_(remoteStaticPublic),
      fixedEphemeral_(fixedEphemeralPrivate) {
    initialize();
    const QByteArray name = pattern == Pattern::NN ? QByteArrayLiteral("Noise_NN_25519_ChaChaPoly_BLAKE2b")
                                                   : QByteArrayLiteral("Noise_KK_25519_ChaChaPoly_BLAKE2b");
    h_ = name + QByteArray(kHashLength - name.size(), '\0'); // shorter than HASHLEN: zero-padded
    ck_ = h_;
    mixHash(prologue);
    if (pattern == Pattern::KK) {
        if (s_.publicKey.size() != kDhLength || rs_.size() != kDhLength) {
            failed_ = true;
            return;
        }
        // Pre-messages: -> s, <- s
        mixHash(initiator ? s_.publicKey : rs_);
        mixHash(initiator ? rs_ : s_.publicKey);
    }
}

void HandshakeState::mixHash(const QByteArray &data) { h_ = blake2b(h_ + data); }

void HandshakeState::mixKey(const QByteArray &inputKeyMaterial) {
    QByteArray tempKey;
    hkdf(ck_, inputKeyMaterial, &ck_, &tempKey);
    cipher_.initializeKey(tempKey.left(32));
}

QByteArray HandshakeState::encryptAndHash(const QByteArray &plaintext) {
    const QByteArray ciphertext = cipher_.encryptWithAd(h_, plaintext).value_or(QByteArray());
    mixHash(ciphertext);
    return ciphertext;
}

std::optional<QByteArray> HandshakeState::decryptAndHash(const QByteArray &ciphertext) {
    const auto plaintext = cipher_.decryptWithAd(h_, ciphertext);
    if (!plaintext) return std::nullopt;
    mixHash(ciphertext);
    return plaintext;
}

std::optional<QByteArray> HandshakeState::dh(const QByteArray &privateKey, const QByteArray &publicKey) const {
    if (privateKey.size() != kDhLength || publicKey.size() != kDhLength) return std::nullopt;
    QByteArray shared(kDhLength, Qt::Uninitialized);
    // libsodium refuses low-order points (an all-zero result).
    if (crypto_scalarmult_curve25519(bytes(shared), bytes(privateKey), bytes(publicKey)) != 0) return std::nullopt;
    return shared;
}

bool HandshakeState::mixDh(char localKey, char remoteKey) {
    const QByteArray &local = localKey == 'e' ? e_.privateKey : s_.privateKey;
    const QByteArray &remote = remoteKey == 'e' ? re_ : rs_;
    const auto shared = dh(local, remote);
    if (!shared) return false;
    mixKey(*shared);
    return true;
}

void HandshakeState::split() {
    QByteArray first, second;
    hkdf(ck_, QByteArray(), &first, &second);
    c1_.initializeKey(first.left(32));
    c2_.initializeKey(second.left(32));
}

// Message 1: -> e (NN) / -> e, es, ss (KK)
// Message 2: <- e, ee (NN) / <- e, ee, se (KK)
std::optional<QByteArray> HandshakeState::writeMessage(const QByteArray &payload) {
    const bool myTurn = (messageIndex_ == 0 && initiator_) || (messageIndex_ == 1 && !initiator_);
    if (failed_ || !myTurn) return std::nullopt;
    e_ = fixedEphemeral_.isEmpty() ? generateKeyPair() : KeyPair{publicKeyFor(fixedEphemeral_), fixedEphemeral_};
    QByteArray message = e_.publicKey;
    mixHash(e_.publicKey);
    bool ok = true;
    if (messageIndex_ == 0) {
        if (pattern_ == Pattern::KK) ok = mixDh('e', 's') && mixDh('s', 's'); // es, ss
    } else {
        ok = mixDh('e', 'e');                                            // ee
        if (ok && pattern_ == Pattern::KK) ok = mixDh('e', 's');         // se (responder: e with rs)
    }
    if (!ok) {
        failed_ = true;
        return std::nullopt;
    }
    message += encryptAndHash(payload);
    if (++messageIndex_ == 2) split();
    return message;
}

std::optional<QByteArray> HandshakeState::readMessage(const QByteArray &message) {
    const bool theirTurn = (messageIndex_ == 0 && !initiator_) || (messageIndex_ == 1 && initiator_);
    if (failed_ || !theirTurn || message.size() < kDhLength) {
        failed_ = true;
        return std::nullopt;
    }
    re_ = message.left(kDhLength);
    mixHash(re_);
    bool ok = true;
    if (messageIndex_ == 0) {
        if (pattern_ == Pattern::KK) ok = mixDh('s', 'e') && mixDh('s', 's'); // es (responder side), ss
    } else {
        ok = mixDh('e', 'e');                                                // ee
        if (ok && pattern_ == Pattern::KK) ok = mixDh('s', 'e');             // se (initiator: s with re)
    }
    const auto payload = ok ? decryptAndHash(message.mid(kDhLength)) : std::nullopt;
    if (!payload) {
        failed_ = true;
        return std::nullopt;
    }
    if (++messageIndex_ == 2) split();
    return payload;
}

} // namespace SyncCrypto
