#pragma once

#include "history_repository.h"

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QString>

#include <optional>

// Wire formats for LAN sync (docs/lan-sync-design.md §4 and §7). Everything
// here is pure data conversion so the three clients can share golden tests.
namespace SyncProtocol {

constexpr quint16 kDiscoveryPort = 47820;
inline QHostAddress discoveryGroup() { return QHostAddress(QStringLiteral("239.255.77.77")); }
constexpr int kProtocolVersion = 1;
constexpr int kMaxPeers = 8;
constexpr qint64 kSyncWindowMs = 24LL * 60 * 60 * 1000;
constexpr int kVisibilitySeconds = 180;
constexpr int kMaxFrame = 65535;          // ciphertext bytes per frame
constexpr int kMaxPlaintext = kMaxFrame - 16 - 1; // minus tag and frame-type byte
constexpr int kInlineBodyLimit = 32 * 1024;

// MARK: - UDP discovery

enum class PacketType : quint8 { Query = 1, Reply = 2, Locate = 3 };

struct DiscoveryPacket {
    PacketType type = PacketType::Query;
    QByteArray nonce;     // query/reply: 8 bytes, echoed in the reply
    QByteArray tag;       // reply: 4-character visibility tag (e.g. "K7Q2")
    quint16 tcpPort = 0;  // reply/locate
    QByteArray locate;    // locate: 16-byte HMAC token
};

QByteArray encodePacket(const DiscoveryPacket &packet);
std::optional<DiscoveryPacket> decodePacket(const QByteArray &data);
// HMAC(locate key, 30-second time slot), first 16 bytes.
QByteArray locateToken(const QByteArray &locateKey, qint64 unixSeconds);
QByteArray randomTag(); // 4 characters from an unambiguous alphabet

// MARK: - Encrypted stream framing

// A frame on the wire: 2-byte big-endian length, then that many bytes.
QByteArray frameBytes(const QByteArray &payload);
// Pops complete frames from `buffer`; false when a frame is malformed.
bool takeFrames(QByteArray &buffer, QList<QByteArray> &frames);

// The decrypted body of a frame: one type byte, then the content.
enum class FrameKind : char { Json = 'J', Blob = 'B' };
QByteArray jsonFrame(const QJsonObject &object);
QByteArray blobFrame(const QByteArray &blobId, quint32 offset, const QByteArray &chunk);
struct BlobChunk {
    QByteArray blobId; // 16 bytes
    quint32 offset = 0;
    QByteArray data;
};
std::optional<QJsonObject> parseJsonFrame(const QByteArray &plaintext);
std::optional<BlobChunk> parseBlobFrame(const QByteArray &plaintext);

// MARK: - Items

// An item as sent: identity, clock, timestamps, title, and the body (inline
// when small, otherwise announced as a blob that follows).
struct WireItem {
    ClipboardItem item;
    QByteArray body;        // UTF-8 text or PNG bytes
    QByteArray blobId;      // non-empty when the body follows as blob frames
    qint64 bodySize = 0;
};

WireItem wireItemFor(const ClipboardItem &item);
QJsonObject encodeItem(const WireItem &wire);
std::optional<WireItem> decodeItem(const QJsonObject &object);
// Applies a received body (inline or reassembled) to the decoded item.
bool applyBody(WireItem &wire, const QByteArray &body);

} // namespace SyncProtocol
