#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <optional>

// Noise Protocol Framework (revision 34), the two patterns ClipDeck needs
// (docs/lan-sync-design.md §9): Noise_NN_25519_ChaChaPoly_BLAKE2b for the
// unauthenticated channel before pairing, and Noise_KK_25519_ChaChaPoly_BLAKE2b
// between paired devices. Built on libsodium.

namespace SyncCrypto {

// sodium_init() once per process; false if libsodium cannot run.
bool initialize();

struct KeyPair {
    QByteArray publicKey;  // 32 bytes
    QByteArray privateKey; // 32 bytes
};
KeyPair generateKeyPair();
// The X25519 public key for a private key (32 bytes each).
QByteArray publicKeyFor(const QByteArray &privateKey);
// A device ID: the first 16 bytes of BLAKE2b(public key), as lowercase hex.
QString deviceIdFor(const QByteArray &publicKey);
QByteArray randomBytes(int length);

// One direction of an established connection (Noise CipherState).
class CipherState {
public:
    static constexpr quint64 kMaxNonce = ~quint64(0);
    static constexpr int kTagLength = 16;

    void initializeKey(const QByteArray &key); // 32 bytes; empty clears it
    bool hasKey() const { return hasKey_; }
    void setNonce(quint64 nonce) { nonce_ = nonce; }
    quint64 nonce() const { return nonce_; }

    // Encrypts with the current nonce and advances it. Without a key the
    // plaintext is returned unchanged (as the spec requires during handshakes).
    std::optional<QByteArray> encryptWithAd(const QByteArray &ad, const QByteArray &plaintext);
    std::optional<QByteArray> decryptWithAd(const QByteArray &ad, const QByteArray &ciphertext);
    void rekey();

private:
    std::array<unsigned char, 32> key_{};
    bool hasKey_ = false;
    quint64 nonce_ = 0;
};

class HandshakeState {
public:
    enum class Pattern { NN, KK };

    // For KK, `localStatic` and `remoteStaticPublic` are required.
    // `fixedEphemeral` is for test vectors only.
    HandshakeState(Pattern pattern, bool initiator, const QByteArray &prologue,
                   const KeyPair &localStatic = {}, const QByteArray &remoteStaticPublic = {},
                   const QByteArray &fixedEphemeralPrivate = {});

    // Both patterns are two messages: initiator writes 1, responder writes 2.
    std::optional<QByteArray> writeMessage(const QByteArray &payload);
    std::optional<QByteArray> readMessage(const QByteArray &message);

    bool isComplete() const { return messageIndex_ >= 2 && !failed_; }
    bool failed() const { return failed_; }
    // After completion: the transport ciphers and the handshake hash.
    CipherState &sendCipher() { return initiator_ ? c1_ : c2_; }
    CipherState &receiveCipher() { return initiator_ ? c2_ : c1_; }
    QByteArray handshakeHash() const { return h_; }

private:
    void mixHash(const QByteArray &data);
    void mixKey(const QByteArray &inputKeyMaterial);
    QByteArray encryptAndHash(const QByteArray &plaintext);
    std::optional<QByteArray> decryptAndHash(const QByteArray &ciphertext);
    std::optional<QByteArray> dh(const QByteArray &privateKey, const QByteArray &publicKey) const;
    bool mixDh(char localKey, char remoteKey); // 'e' / 's'
    void split();

    Pattern pattern_;
    bool initiator_;
    QByteArray h_;
    QByteArray ck_;
    CipherState cipher_;
    KeyPair s_;
    KeyPair e_;
    QByteArray rs_;
    QByteArray re_;
    QByteArray fixedEphemeral_;
    CipherState c1_;
    CipherState c2_;
    int messageIndex_ = 0;
    bool failed_ = false;
};

// HMAC-BLAKE2b and the Noise HKDF, exposed for tests and key derivation.
QByteArray hmacBlake2b(const QByteArray &key, const QByteArray &data);
void hkdf(const QByteArray &chainingKey, const QByteArray &inputKeyMaterial, QByteArray *out1,
          QByteArray *out2, QByteArray *out3 = nullptr);

} // namespace SyncCrypto
