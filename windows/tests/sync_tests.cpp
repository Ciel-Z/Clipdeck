// LAN sync crypto and protocol checks (docs/lan-sync-design.md §9). The
// Noise vectors come from the cacophony suite and the CPace vectors from
// draft-irtf-cfrg-cpace-13 Appendix B.3 (ristretto255, SHA-512).
#include "sync/cpace.h"
#include "sync/discovery.h"
#include "sync/key_store.h"
#include "sync/noise.h"
#include "sync/sync_protocol.h"
#include "sync/sync_service.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>
#include <memory>

using namespace SyncCrypto;

namespace {
int failures = 0;
void check(bool condition, const char *name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
QByteArray hex(const char *text) { return QByteArray::fromHex(QByteArray(text)); }

const QByteArray kPrologue = hex("4a6f686e2047616c74");
const QByteArray kInitEphemeral = hex("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a");
const QByteArray kRespEphemeral = hex("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b");
const QByteArray kPayloads[4] = {hex("4c756477696720766f6e204d69736573"), hex("4d757272617920526f746862617264"),
                                 hex("462e20412e20486179656b"), hex("4361726c204d656e676572")};

// Runs a two-message handshake plus two transport messages with the vector's
// fixed keys and compares every ciphertext and the handshake hash.
void runNoiseVector(const char *label, HandshakeState::Pattern pattern, const KeyPair &initStatic,
                    const KeyPair &respStatic, const QByteArray (&expected)[4], const QByteArray &handshakeHash) {
    HandshakeState initiator(pattern, true, kPrologue, initStatic, respStatic.publicKey, kInitEphemeral);
    HandshakeState responder(pattern, false, kPrologue, respStatic, initStatic.publicKey, kRespEphemeral);
    bool ok = true;
    const auto m1 = initiator.writeMessage(kPayloads[0]);
    ok = ok && m1 && *m1 == expected[0];
    const auto r1 = m1 ? responder.readMessage(*m1) : std::nullopt;
    ok = ok && r1 && *r1 == kPayloads[0];
    const auto m2 = responder.writeMessage(kPayloads[1]);
    ok = ok && m2 && *m2 == expected[1];
    const auto r2 = m2 ? initiator.readMessage(*m2) : std::nullopt;
    ok = ok && r2 && *r2 == kPayloads[1];
    check(ok && initiator.isComplete() && responder.isComplete(), (std::string(label) + "-handshake-matches-vector").c_str());
    check(initiator.handshakeHash() == handshakeHash && responder.handshakeHash() == handshakeHash,
          (std::string(label) + "-handshake-hash-matches-vector").c_str());
    const auto m3 = initiator.sendCipher().encryptWithAd({}, kPayloads[2]);
    const auto m4 = responder.sendCipher().encryptWithAd({}, kPayloads[3]);
    check(m3 && *m3 == expected[2] && m4 && *m4 == expected[3], (std::string(label) + "-transport-matches-vector").c_str());
    const auto d3 = m3 ? responder.receiveCipher().decryptWithAd({}, *m3) : std::nullopt;
    const auto d4 = m4 ? initiator.receiveCipher().decryptWithAd({}, *m4) : std::nullopt;
    check(d3 && *d3 == kPayloads[2] && d4 && *d4 == kPayloads[3], (std::string(label) + "-transport-decrypts").c_str());
}

void noiseVectorTests() {
    const QByteArray nn[4] = {
        hex("ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444c756477696720766f6e204d69736573"),
        hex("95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843d10cf8ef4ab895bed3e4673211f0c9337039d63a450c7b28196b8a0ebade00"),
        hex("e50ec882703a1f34bf4957d8cafd036d34e02930f672f424c676e1"),
        hex("35bb2a728d3e8e5f47781d486089e4a37c5c2e4261256f44569a9f"),
    };
    runNoiseVector("noise-nn", HandshakeState::Pattern::NN, {}, {}, nn,
                   hex("25b3d1154146a2e058e4db548e0841992cf33a972d5b85a908e4fb8f14b6d94f4987e17baa330c93dd842d6eda030cd47190c60d7c862574078779aba1302a2e"));

    const QByteArray initPrivate = hex("e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1");
    const QByteArray respPrivate = hex("4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893");
    const KeyPair initStatic{publicKeyFor(initPrivate), initPrivate};
    const KeyPair respStatic{publicKeyFor(respPrivate), respPrivate};
    check(respStatic.publicKey == hex("31e0303fd6418d2f8c0e78b91f22e8caed0fbe48656dcf4767e4834f701b8f62")
              && initStatic.publicKey == hex("6bc3822a2aa7f4e6981d6538692b3cdf3e6df9eea6ed269eb41d93c22757b75a"),
          "x25519-public-keys-match-vector");
    const QByteArray kk[4] = {
        hex("ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944f79a1d4b21fc3ea4a0c87213b8b4f0599d758682c26a3ae5e09195a3e742bc74"),
        hex("95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843e20b1bf85731f75d7e21b5d54baaa66341de4292c3d42571c1bd7e7f1abe38"),
        hex("25bfaa58833b07cdd6af7c07f2c51daac681a8ac0a02dd373259bd"),
        hex("ef586cff556dec8ef0053871ff0d4bf3f2c72e842487ec6d1da69f"),
    };
    runNoiseVector("noise-kk", HandshakeState::Pattern::KK, initStatic, respStatic, kk,
                   hex("76dbc866183c8ee7363dbf0ebab8d6355010245f9817aa78359818a03a052586d7e8b4bb2ae5622a1a61212df90af04bb2b2cc189ce0e819ba0c4970c9f71805"));
}

void noiseSecurityTests() {
    const KeyPair alice = generateKeyPair();
    const KeyPair bob = generateKeyPair();
    const KeyPair mallory = generateKeyPair();

    // An impostor who does not hold Bob's private key cannot answer as Bob.
    HandshakeState initiator(HandshakeState::Pattern::KK, true, QByteArrayLiteral("ClipDeck"), alice, bob.publicKey);
    HandshakeState impostor(HandshakeState::Pattern::KK, false, QByteArrayLiteral("ClipDeck"), mallory, alice.publicKey);
    const auto first = initiator.writeMessage({});
    check(first && !impostor.readMessage(*first), "kk-impostor-cannot-read-the-first-message");

    // A tampered frame is rejected and does not advance the receiver.
    HandshakeState a(HandshakeState::Pattern::KK, true, QByteArrayLiteral("ClipDeck"), alice, bob.publicKey);
    HandshakeState b(HandshakeState::Pattern::KK, false, QByteArrayLiteral("ClipDeck"), bob, alice.publicKey);
    b.readMessage(*a.writeMessage({}));
    a.readMessage(*b.writeMessage({}));
    QByteArray frame = *a.sendCipher().encryptWithAd({}, QByteArrayLiteral("hello"));
    QByteArray tampered = frame;
    tampered[3] = char(tampered[3] ^ 1);
    check(!b.receiveCipher().decryptWithAd({}, tampered), "tampered-frame-is-rejected");
    check(b.receiveCipher().decryptWithAd({}, frame).value_or(QByteArray()) == "hello", "intact-frame-still-decrypts");
    check(!b.receiveCipher().decryptWithAd({}, frame), "replayed-frame-is-rejected");

    // Rekeying on both sides keeps the channel working.
    a.sendCipher().rekey();
    b.receiveCipher().rekey();
    const auto afterRekey = a.sendCipher().encryptWithAd({}, QByteArrayLiteral("after"));
    check(afterRekey && b.receiveCipher().decryptWithAd({}, *afterRekey).value_or(QByteArray()) == "after",
          "rekeyed-channel-works");
    check(deviceIdFor(alice.publicKey).size() == 32 && deviceIdFor(alice.publicKey) != deviceIdFor(bob.publicKey),
          "device-ids-come-from-public-keys");
}

void cpaceVectorTests() {
    const QByteArray prs = QByteArrayLiteral("Password");
    const QByteArray ci = hex("6f630b425f726573706f6e6465720b415f696e69746961746f72");
    const QByteArray sid = hex("7e4b4791d6a8ef019b936c79fb7f2c57");
    check(CPace::generatorString(prs, ci, sid).size() == 172, "cpace-generator-string-length");
    check(CPace::generator(prs, ci, sid).value_or(QByteArray())
              == hex("a6fc82c3b8968fbb2e06fee81ca858586dea50d248f0c7ca6a18b0902a30b36b"),
          "cpace-generator-matches-vector");

    const QByteArray ya = hex("da3d23700a9e5699258aef94dc060dfda5ebb61f02a5ea77fad53f4ff0976d08");
    const QByteArray yb = hex("d2316b454718c35362d83d69df6320f38578ed5984651435e2949762d900b80d");
    const CPace a(true, prs, ci, sid, QByteArrayLiteral("ADa"), ya);
    const CPace b(false, prs, ci, sid, QByteArrayLiteral("ADb"), yb);
    check(a.message() == hex("d40fb265a7abeaee7939d91a585fe59f7053f982c296ec413c624c669308f87a"), "cpace-ya-matches-vector");
    check(b.message() == hex("08bcf6e9777a9c313a3db6daa510f2d398403319c2341bd506a92e672eb7e307"), "cpace-yb-matches-vector");
    const QByteArray expectedIsk = hex("4c5469a16b2364c4b944ebc1a79e51d1674ad47db26e8718154f59faebfaa52d"
                                       "8346f30aa58377117eb20d527f2cbc5c76381f7fd372e89df8239f87f2e02ed1");
    check(a.sharedKey(b.message(), QByteArrayLiteral("ADb")).value_or(QByteArray()) == expectedIsk
              && b.sharedKey(a.message(), QByteArrayLiteral("ADa")).value_or(QByteArray()) == expectedIsk,
          "cpace-isk-matches-vector");
}

void cpacePairingTests() {
    const QByteArray sid = randomBytes(64);
    const QByteArray ci = QByteArrayLiteral("ClipDeck pairing");
    const CPace a(true, QByteArrayLiteral("482193"), ci, sid, QByteArrayLiteral("device-a"));
    const CPace b(false, QByteArrayLiteral("482193"), ci, sid, QByteArrayLiteral("device-b"));
    const auto ka = a.sharedKey(b.message(), QByteArrayLiteral("device-b"));
    const auto kb = b.sharedKey(a.message(), QByteArrayLiteral("device-a"));
    check(ka && kb && *ka == *kb, "same-code-gives-the-same-key");
    check(ka && kb && CPace::verifyConfirmation(*kb, true, CPace::confirmation(*ka, true))
              && CPace::verifyConfirmation(*ka, false, CPace::confirmation(*kb, false)),
          "confirmations-verify-with-the-same-code");
    check(ka && !CPace::verifyConfirmation(*ka, true, CPace::confirmation(*ka, false)),
          "confirmation-is-bound-to-the-role");

    const CPace wrong(false, QByteArrayLiteral("482194"), ci, sid, QByteArrayLiteral("device-b"));
    const auto kw = wrong.sharedKey(a.message(), QByteArrayLiteral("device-a"));
    check(ka && kw && *ka != *kw && !CPace::verifyConfirmation(*kw, true, CPace::confirmation(*ka, true)),
          "a-wrong-code-fails-confirmation");
    const CPace otherSession(false, QByteArrayLiteral("482193"), ci, randomBytes(64), QByteArrayLiteral("device-b"));
    const auto ko = otherSession.sharedKey(a.message(), QByteArrayLiteral("device-a"));
    check(ka && ko && *ka != *ko, "a-relayed-session-gets-a-different-key");
    check(!a.sharedKey(QByteArray(32, '\0'), QByteArrayLiteral("device-b")), "identity-point-is-rejected");
    check(!a.sharedKey(QByteArray(32, '\xff'), QByteArrayLiteral("device-b")), "invalid-point-is-rejected");
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

void settle(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

void protocolTests() {
    SyncProtocol::DiscoveryPacket reply;
    reply.type = SyncProtocol::PacketType::Reply;
    reply.nonce = QByteArray("12345678");
    reply.tag = QByteArray("K7Q2");
    reply.tcpPort = 47821;
    const QByteArray encoded = SyncProtocol::encodePacket(reply);
    const auto decoded = SyncProtocol::decodePacket(encoded);
    check(encoded.size() == 19 && decoded && decoded->tag == "K7Q2" && decoded->tcpPort == 47821,
          "reply-packet-round-trips");
    check(encoded.size() < 40, "udp-packets-stay-small");
    check(!SyncProtocol::decodePacket(QByteArray("CDK1\x02short")), "truncated-packet-is-rejected");
    const QByteArray key = randomBytes(32);
    check(SyncProtocol::locateToken(key, 1000) == SyncProtocol::locateToken(key, 1010)
              && SyncProtocol::locateToken(key, 1000) != SyncProtocol::locateToken(key, 1030)
              && SyncProtocol::locateToken(key, 1000) != SyncProtocol::locateToken(randomBytes(32), 1000),
          "locate-token-changes-every-30-seconds-and-per-key");

    QByteArray stream = SyncProtocol::frameBytes("abc") + SyncProtocol::frameBytes("defg");
    stream.chop(1);
    QList<QByteArray> frames;
    check(SyncProtocol::takeFrames(stream, frames) && frames.size() == 1 && frames[0] == "abc" && stream.size() == 5,
          "frames-split-and-wait-for-the-rest");

    ClipboardItem item;
    item.syncID = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.updatedHlc = 123456789;
    item.text = QStringLiteral("hello 你好");
    item.title = QStringLiteral("文本");
    item.copiedAt = QDateTime::currentDateTime();
    const auto wire = SyncProtocol::decodeItem(SyncProtocol::encodeItem(SyncProtocol::wireItemFor(item)));
    check(wire && wire->item.text == item.text && wire->item.syncID == item.syncID && wire->item.updatedHlc == 123456789,
          "item-round-trips-inline");
    item.text = QString(40000, QLatin1Char('x'));
    const SyncProtocol::WireItem large = SyncProtocol::wireItemFor(item);
    auto announced = SyncProtocol::decodeItem(SyncProtocol::encodeItem(large));
    check(announced && !announced->blobId.isEmpty() && announced->item.text.isEmpty()
              && SyncProtocol::applyBody(*announced, large.body) && announced->item.text == item.text,
          "large-item-travels-as-a-blob");

    QTemporaryDir directory;
    SyncKeyStore store(directory.filePath(QStringLiteral("keys.bin")));
    store.setPrivateKey(randomBytes(32));
    store.setLocateKey(QStringLiteral("peer"), randomBytes(32));
    check(store.save(), "key-store-saves");
    QFile raw(store.path());
    raw.open(QIODevice::ReadOnly);
    check(!raw.readAll().contains("privateKey"), "key-store-is-encrypted-on-disk");
    SyncKeyStore reloaded(store.path());
    check(reloaded.load() && reloaded.privateKey() == store.privateKey()
              && reloaded.locateKey(QStringLiteral("peer")) == store.locateKey(QStringLiteral("peer")),
          "key-store-reloads");
}

void discoveryTests() {
    // Two discovery sockets on one machine share the port; multicast loops
    // back so the query reaches the other one.
    SyncDiscovery visible;
    SyncDiscovery searcher;
    visible.setIncludeLoopback(true);
    searcher.setIncludeLoopback(true);
    const bool started = visible.start() && searcher.start();
    check(started, "discovery-sockets-start");
    if (!started) return;
    QList<QByteArray> tags;
    QObject::connect(&searcher, &SyncDiscovery::replied,
                     [&tags](const QHostAddress &, quint16 port, const QByteArray &tag) {
                         if (port == 47999) tags.append(tag);
                     });
    visible.setVisible(false, QByteArrayLiteral("K7Q2"), 47999);
    searcher.query();
    settle(1500);
    check(tags.isEmpty(), "invisible-device-does-not-answer");
    visible.setVisible(true, QByteArrayLiteral("K7Q2"), 47999);
    searcher.query();
    check(waitUntil([&tags] { return !tags.isEmpty(); }, 5000) && tags.first() == "K7Q2",
          "visible-device-answers-with-its-tag-and-port");

    QList<QByteArray> tokens;
    QObject::connect(&visible, &SyncDiscovery::located,
                     [&tokens](const QHostAddress &, quint16, const QByteArray &token) { tokens.append(token); });
    const QByteArray token = SyncProtocol::locateToken(randomBytes(32), QDateTime::currentSecsSinceEpoch());
    searcher.locate(token, 47998);
    check(waitUntil([&tokens, &token] { return tokens.contains(token); }, 5000), "locate-packet-arrives");
}

struct Device {
    Device(const QString &name, QTemporaryDir &directory)
        : repository(directory.filePath(name + QStringLiteral(".sqlite"))) {
        SyncService::Options options;
        options.keyStorePath = directory.filePath(name + QStringLiteral("-keys.bin"));
        options.preferredPort = 0;
        options.allowLoopback = true;
        options.enableDiscovery = false;
        options.deviceNameOverride = name;
        service = std::make_unique<SyncService>(&repository, options);
    }
    HistoryRepository repository;
    std::unique_ptr<SyncService> service;
    bool has(const QString &text) const {
        for (const ClipboardItem &item : repository.fetch()) {
            if (item.text == text) return true;
        }
        return false;
    }
    int count(const QString &text) const {
        int total = 0;
        for (const ClipboardItem &item : repository.fetch()) total += item.text == text;
        return total;
    }
};

ClipboardItem textItem(const QString &text, QDateTime copiedAt = QDateTime::currentDateTime()) {
    ClipboardItem item;
    item.text = text;
    item.sourceName = QStringLiteral("Notepad");
    item.copiedAt = copiedAt;
    return item;
}

void endToEndTests() {
    QTemporaryDir directory;
    Device alpha(QStringLiteral("alpha"), directory);
    Device beta(QStringLiteral("beta"), directory);
    check(alpha.service->start() && beta.service->start(), "services-start");

    // Copied before pairing: the last 24 hours sync, older items do not.
    alpha.repository.save(textItem(QStringLiteral("copied before pairing")));
    alpha.repository.save(textItem(QStringLiteral("copied two days ago"), QDateTime::currentDateTime().addDays(-2)));

    // A wrong code first: the responder offers another attempt.
    beta.service->setVisible(true);
    alpha.service->pairWithAddress(QStringLiteral("127.0.0.1"), beta.service->port());
    check(waitUntil([&] {
              return !beta.service->incomingPairingCode().isEmpty()
                  && alpha.service->pairing().value(QStringLiteral("stage")).toString() == QStringLiteral("code");
          }),
          "pairing-request-shows-a-code");
    const QString code = beta.service->incomingPairingCode();
    alpha.service->submitPairingCode(code == QStringLiteral("000000") ? QStringLiteral("111111") : QStringLiteral("000000"));
    check(waitUntil([&] {
              return alpha.service->pairing().value(QStringLiteral("error")).toString() == QStringLiteral("配对码不正确")
                  && alpha.service->pairing().value(QStringLiteral("stage")).toString() == QStringLiteral("code");
          }),
          "wrong-code-asks-again");
    check(beta.service->pairing().value(QStringLiteral("attemptsLeft")).toInt() == 4, "wrong-code-uses-an-attempt");
    alpha.service->submitPairingCode(code);
    check(waitUntil([&] { return alpha.service->devices().size() == 1 && beta.service->devices().size() == 1; }),
          "right-code-pairs-both-devices");
    check(alpha.service->devices().value(0).toMap().value(QStringLiteral("hostname")).toString() == QStringLiteral("beta")
              && beta.service->devices().value(0).toMap().value(QStringLiteral("hostname")).toString() == QStringLiteral("alpha"),
          "paired-devices-know-each-others-names");
    check(waitUntil([&] {
              return alpha.service->isConnectedTo(beta.service->deviceID())
                  && beta.service->isConnectedTo(alpha.service->deviceID());
          }),
          "paired-devices-connect");
    check(waitUntil([&] { return beta.has(QStringLiteral("copied before pairing")); }), "recent-history-syncs-after-pairing");
    check(!beta.has(QStringLiteral("copied two days ago")), "history-older-than-24-hours-does-not-sync");

    // Live sync both ways.
    int historyEvents = 0;
    QObject::connect(beta.service.get(), &SyncService::historyChanged, [&historyEvents] { ++historyEvents; });
    alpha.repository.save(textItem(QStringLiteral("live from alpha")));
    alpha.service->notifyLocalChange();
    check(waitUntil([&] { return beta.has(QStringLiteral("live from alpha")); }), "new-copy-syncs-alpha-to-beta");
    check(waitUntil([&] { return historyEvents > 0; }), "receiver-is-told-its-history-changed");
    beta.repository.save(textItem(QStringLiteral("reply from beta")));
    beta.service->notifyLocalChange();
    check(waitUntil([&] { return alpha.has(QStringLiteral("reply from beta")); }), "new-copy-syncs-beta-to-alpha");

    // An edit wins on the other side (newer clock) and replaces the item.
    for (const ClipboardItem &item : beta.repository.fetch()) {
        if (item.text == QStringLiteral("live from alpha"))
            beta.repository.updateText(item.databaseID, QStringLiteral("edited on beta"), QString());
    }
    beta.service->notifyLocalChange();
    check(waitUntil([&] { return alpha.has(QStringLiteral("edited on beta")) && !alpha.has(QStringLiteral("live from alpha")); }),
          "edit-replaces-the-same-item-on-the-peer");

    // A large image travels in blob frames.
    ClipboardItem picture;
    picture.type = ClipboardPayloadType::Image;
    QImage image(600, 400, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) image.setPixel(x, y, qRgb(x % 256, y % 256, (x * y) % 256));
    }
    QBuffer buffer(&picture.imageData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    picture.image = image;
    picture.sourceName = QStringLiteral("Paint");
    picture.copiedAt = QDateTime::currentDateTime();
    check(picture.imageData.size() > SyncProtocol::kInlineBodyLimit, "test-image-is-large-enough-for-blobs");
    alpha.repository.save(picture);
    alpha.service->notifyLocalChange();
    check(waitUntil([&] {
              for (const ClipboardItem &item : beta.repository.fetch()) {
                  if (item.isImage() && item.imageData == picture.imageData) return true;
              }
              return false;
          }),
          "large-image-syncs-intact");

    // The same content copied on both sides stays one card on each.
    alpha.repository.save(textItem(QStringLiteral("copied on both")));
    beta.repository.save(textItem(QStringLiteral("copied on both")));
    alpha.service->notifyLocalChange();
    beta.service->notifyLocalChange();
    settle(1500);
    check(alpha.count(QStringLiteral("copied on both")) == 1 && beta.count(QStringLiteral("copied on both")) == 1,
          "same-content-on-both-devices-stays-one-card");

    // A local delete stays local and is not synced back.
    for (const ClipboardItem &item : beta.repository.fetch()) {
        if (item.text == QStringLiteral("reply from beta")) beta.repository.remove(item.databaseID);
    }
    alpha.repository.save(textItem(QStringLiteral("reply from beta"))); // copied again on alpha
    alpha.service->notifyLocalChange();
    settle(1500);
    check(!beta.has(QStringLiteral("reply from beta")) && alpha.has(QStringLiteral("reply from beta")),
          "local-delete-stays-local-and-is-not-synced-back");

    // Pausing disconnects; resuming reconnects and catches up.
    beta.service->setPaused(true);
    check(waitUntil([&] { return !alpha.service->isConnectedTo(beta.service->deviceID()); }), "pause-disconnects");
    alpha.repository.save(textItem(QStringLiteral("while beta was paused")));
    beta.service->setPaused(false);
    alpha.service->connectToPeerNow(beta.service->deviceID());
    check(waitUntil([&] { return beta.has(QStringLiteral("while beta was paused")); }, 15000),
          "resume-reconnects-and-catches-up");

    // Unpairing removes the pairing on both sides.
    alpha.service->unpair(beta.service->deviceID());
    check(waitUntil([&] { return alpha.service->devices().isEmpty() && beta.service->devices().isEmpty(); }),
          "unpair-removes-both-sides");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ClipDeckTests"));
    QCoreApplication::setApplicationName(QStringLiteral("sync-tests"));
    QSettings().remove(QStringLiteral("sync"));
    check(initialize(), "libsodium-initializes");
    noiseVectorTests();
    noiseSecurityTests();
    cpaceVectorTests();
    cpacePairingTests();
    protocolTests();
    discoveryTests();
    endToEndTests();
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
