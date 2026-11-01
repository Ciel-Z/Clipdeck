#include "sync_protocol.h"
#include "noise.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QtEndian>

namespace SyncProtocol {
namespace {

const QByteArray kMagic = QByteArrayLiteral("CDK1");

} // namespace

// MARK: - UDP discovery

QByteArray encodePacket(const DiscoveryPacket &packet) {
    QByteArray out = kMagic;
    out.append(char(packet.type));
    auto appendPort = [&out](quint16 port) {
        out.append(char(port >> 8));
        out.append(char(port & 0xff));
    };
    switch (packet.type) {
    case PacketType::Query:
        out.append(packet.nonce.leftJustified(8, '\0', true));
        break;
    case PacketType::Reply:
        out.append(packet.nonce.leftJustified(8, '\0', true));
        out.append(packet.tag.leftJustified(4, ' ', true));
        appendPort(packet.tcpPort);
        break;
    case PacketType::Locate:
        out.append(packet.locate.leftJustified(16, '\0', true));
        appendPort(packet.tcpPort);
        break;
    }
    return out;
}

std::optional<DiscoveryPacket> decodePacket(const QByteArray &data) {
    if (data.size() < 5 || !data.startsWith(kMagic)) return std::nullopt;
    DiscoveryPacket packet;
    packet.type = PacketType(quint8(data[4]));
    const QByteArray body = data.mid(5);
    auto port = [](const QByteArray &bytes, int at) {
        return quint16((quint8(bytes[at]) << 8) | quint8(bytes[at + 1]));
    };
    switch (packet.type) {
    case PacketType::Query:
        if (body.size() != 8) return std::nullopt;
        packet.nonce = body;
        return packet;
    case PacketType::Reply:
        if (body.size() != 14) return std::nullopt;
        packet.nonce = body.left(8);
        packet.tag = body.mid(8, 4);
        packet.tcpPort = port(body, 12);
        return packet.tcpPort ? std::optional<DiscoveryPacket>(packet) : std::nullopt;
    case PacketType::Locate:
        if (body.size() != 18) return std::nullopt;
        packet.locate = body.left(16);
        packet.tcpPort = port(body, 16);
        return packet;
    }
    return std::nullopt;
}

QByteArray locateToken(const QByteArray &locateKey, qint64 unixSeconds) {
    QByteArray slot(8, '\0');
    qToBigEndian<quint64>(quint64(unixSeconds / 30), reinterpret_cast<uchar *>(slot.data()));
    return SyncCrypto::hmacBlake2b(locateKey, QByteArrayLiteral("ClipDeck locate") + slot).left(16);
}

QByteArray randomTag() {
    static const char alphabet[] = "ACDEFGHJKMNPQRTUVWXY3479"; // no 0/O, 1/I/L, 2/Z, 5/S, 6/B, 8
    const QByteArray random = SyncCrypto::randomBytes(4);
    QByteArray tag;
    for (char byte : random) tag.append(alphabet[quint8(byte) % (sizeof alphabet - 1)]);
    return tag;
}

// MARK: - Framing

QByteArray frameBytes(const QByteArray &payload) {
    QByteArray out(2, '\0');
    out[0] = char((payload.size() >> 8) & 0xff);
    out[1] = char(payload.size() & 0xff);
    return out + payload;
}

bool takeFrames(QByteArray &buffer, QList<QByteArray> &frames) {
    while (buffer.size() >= 2) {
        const int length = (quint8(buffer[0]) << 8) | quint8(buffer[1]);
        if (length == 0) return false;
        if (buffer.size() < 2 + length) break;
        frames.append(buffer.mid(2, length));
        buffer.remove(0, 2 + length);
    }
    return true;
}

QByteArray jsonFrame(const QJsonObject &object) {
    return QByteArray(1, char(FrameKind::Json)) + QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray blobFrame(const QByteArray &blobId, quint32 offset, const QByteArray &chunk) {
    QByteArray out(1, char(FrameKind::Blob));
    out.append(blobId.leftJustified(16, '\0', true));
    QByteArray position(4, '\0');
    qToBigEndian<quint32>(offset, reinterpret_cast<uchar *>(position.data()));
    return out + position + chunk;
}

std::optional<QJsonObject> parseJsonFrame(const QByteArray &plaintext) {
    if (plaintext.isEmpty() || plaintext[0] != char(FrameKind::Json)) return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(plaintext.mid(1), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    return document.object();
}

std::optional<BlobChunk> parseBlobFrame(const QByteArray &plaintext) {
    if (plaintext.size() < 21 || plaintext[0] != char(FrameKind::Blob)) return std::nullopt;
    BlobChunk chunk;
    chunk.blobId = plaintext.mid(1, 16);
    chunk.offset = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(plaintext.constData() + 17));
    chunk.data = plaintext.mid(21);
    return chunk;
}

// MARK: - Items

WireItem wireItemFor(const ClipboardItem &item) {
    WireItem wire;
    wire.item = item;
    wire.body = item.isImage() ? item.imageData : item.text.toUtf8();
    wire.bodySize = wire.body.size();
    if (wire.bodySize > kInlineBodyLimit) wire.blobId = SyncCrypto::randomBytes(16);
    return wire;
}

QJsonObject encodeItem(const WireItem &wire) {
    const ClipboardItem &item = wire.item;
    QJsonObject object{
        {QStringLiteral("type"), QStringLiteral("item")},
        {QStringLiteral("syncId"), item.syncID},
        {QStringLiteral("origin"), item.originDevice},
        {QStringLiteral("hlc"), QString::number(item.updatedHlc)},
        {QStringLiteral("kind"), item.isImage() ? QStringLiteral("image") : QStringLiteral("text")},
        {QStringLiteral("title"), item.title},
        {QStringLiteral("copiedAt"), QString::number(item.copiedAt.toMSecsSinceEpoch())},
        {QStringLiteral("angle"), item.imageAngle},
        {QStringLiteral("sourceName"), item.sourceName},
        {QStringLiteral("bodySize"), QString::number(wire.bodySize)},
    };
    // Rich text rides along only when small; the plain text is always sent.
    if (item.isText() && !item.richTextHtml.isEmpty() && item.richTextHtml.size() + wire.bodySize < kInlineBodyLimit)
        object.insert(QStringLiteral("html"), item.richTextHtml);
    if (wire.blobId.isEmpty()) object.insert(QStringLiteral("body"), QString::fromLatin1(wire.body.toBase64()));
    else object.insert(QStringLiteral("blob"), QString::fromLatin1(wire.blobId.toHex()));
    return object;
}

std::optional<WireItem> decodeItem(const QJsonObject &object) {
    if (object.value(QStringLiteral("type")).toString() != QStringLiteral("item")) return std::nullopt;
    WireItem wire;
    ClipboardItem &item = wire.item;
    item.syncID = object.value(QStringLiteral("syncId")).toString();
    bool ok = false;
    item.updatedHlc = object.value(QStringLiteral("hlc")).toString().toULongLong(&ok);
    if (item.syncID.isEmpty() || !ok || QUuid(item.syncID).isNull()) return std::nullopt;
    item.originDevice = object.value(QStringLiteral("origin")).toString();
    const QString kind = object.value(QStringLiteral("kind")).toString();
    if (kind != QStringLiteral("text") && kind != QStringLiteral("image")) return std::nullopt;
    item.type = kind == QStringLiteral("image") ? ClipboardPayloadType::Image : ClipboardPayloadType::Text;
    item.title = object.value(QStringLiteral("title")).toString();
    item.copiedAt = QDateTime::fromMSecsSinceEpoch(object.value(QStringLiteral("copiedAt")).toString().toLongLong());
    item.imageAngle = object.value(QStringLiteral("angle")).toInt();
    item.sourceName = object.value(QStringLiteral("sourceName")).toString();
    item.richTextHtml = object.value(QStringLiteral("html")).toString();
    wire.bodySize = object.value(QStringLiteral("bodySize")).toString().toLongLong();
    if (object.contains(QStringLiteral("blob"))) {
        wire.blobId = QByteArray::fromHex(object.value(QStringLiteral("blob")).toString().toLatin1());
        if (wire.blobId.size() != 16 || wire.bodySize <= 0) return std::nullopt;
        return wire;
    }
    if (!applyBody(wire, QByteArray::fromBase64(object.value(QStringLiteral("body")).toString().toLatin1())))
        return std::nullopt;
    return wire;
}

bool applyBody(WireItem &wire, const QByteArray &body) {
    if (body.size() != wire.bodySize) return false;
    wire.body = body;
    if (wire.item.isImage()) {
        QImage image;
        if (!image.loadFromData(body, "PNG")) return false;
        wire.item.imageData = body;
        wire.item.image = image;
        wire.item.imagePixelSize = image.size();
    } else {
        wire.item.text = QString::fromUtf8(body);
        wire.item.textLength = wire.item.text.size();
    }
    return true;
}

} // namespace SyncProtocol
