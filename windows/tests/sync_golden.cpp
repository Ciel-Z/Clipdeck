// Prints the golden values the macOS sync tests compare against
// (mac/Tests/ClipDeckTests/Sync/SyncGoldenTests.swift), so both
// implementations stay byte-compatible. Run: sync-golden.exe
#include "sync/cpace.h"
#include "sync/noise.h"
#include "sync/sync_protocol.h"

#include <QCoreApplication>
#include <cstdio>

using namespace SyncCrypto;

namespace {
void print(const char *name, const QByteArray &value) { std::printf("%s=%s\n", name, value.toHex().constData()); }
QByteArray sequence(int length, int start = 0) {
    QByteArray out;
    for (int i = 0; i < length; ++i) out.append(char(start + i));
    return out;
}
} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    initialize();
    print("hmacBlake2b", hmacBlake2b(QByteArrayLiteral("key"), QByteArrayLiteral("data")));
    QByteArray first, second, third;
    hkdf(sequence(64), sequence(32, 100), &first, &second, &third);
    print("hkdf1", first);
    print("hkdf2", second);
    print("hkdf3", third);
    const QByteArray privateKey = sequence(32, 1);
    print("publicKey", publicKeyFor(privateKey));
    std::printf("deviceId=%s\n", qPrintable(deviceIdFor(publicKeyFor(privateKey))));
    print("locateToken", SyncProtocol::locateToken(sequence(32), 1700000000));
    SyncProtocol::DiscoveryPacket reply;
    reply.type = SyncProtocol::PacketType::Reply;
    reply.nonce = QByteArrayLiteral("12345678");
    reply.tag = QByteArrayLiteral("K7Q2");
    reply.tcpPort = 47821;
    print("replyPacket", SyncProtocol::encodePacket(reply));
    SyncProtocol::DiscoveryPacket locate;
    locate.type = SyncProtocol::PacketType::Locate;
    locate.locate = sequence(16);
    locate.tcpPort = 47821;
    print("locatePacket", SyncProtocol::encodePacket(locate));
    print("blobFrame", SyncProtocol::blobFrame(sequence(16), 70000, QByteArrayLiteral("abc")));
    print("frameBytes", SyncProtocol::frameBytes(QByteArrayLiteral("hello")));
    const QByteArray isk = sequence(64, 7);
    print("confirmInitiator", CPace::confirmation(isk, true));
    print("confirmResponder", CPace::confirmation(isk, false));
    print("publicKeyMac", hmacBlake2b(isk, QByteArrayLiteral("ClipDeck public key") + sequence(32)));
    print("locateKey", hmacBlake2b(isk, QByteArrayLiteral("ClipDeck locate key")).left(32));
    print("cpaceGenerator", CPace::generator(QByteArrayLiteral("482193"), QByteArrayLiteral("ClipDeck pairing v1"),
                                             sequence(64)).value_or(QByteArray()));
    return 0;
}
