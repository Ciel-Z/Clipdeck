import Foundation

/// Wire formats for LAN sync (docs/lan-sync-design.md §4 and §7), identical
/// to linux/src/sync/sync_protocol.cpp.
enum SyncProtocol {
    static let discoveryPort: UInt16 = 47820
    static let discoveryGroup = "239.255.77.77"
    static let protocolVersion = 1
    static let maxPeers = 8
    static let syncWindowMilliseconds: Int64 = 24 * 60 * 60 * 1_000
    static let visibilitySeconds = 180
    static let maxFrame = 65_535
    static let maxPlaintext = maxFrame - 16 - 1
    static let inlineBodyLimit = 32 * 1_024
    static let magic = Data("CDK1".utf8)

    // MARK: UDP discovery

    enum PacketType: UInt8 { case query = 1, reply = 2, locate = 3 }

    struct Packet: Equatable {
        var type: PacketType
        var nonce = Data()
        var tag = Data()
        var tcpPort: UInt16 = 0
        var locate = Data()
    }

    private static func padded(_ data: Data, to length: Int, with byte: UInt8) -> Data {
        data.count >= length ? Data(data.prefix(length)) : data + Data(repeating: byte, count: length - data.count)
    }

    private static func portBytes(_ port: UInt16) -> Data { Data([UInt8(port >> 8), UInt8(port & 0xff)]) }

    static func encode(_ packet: Packet) -> Data {
        var out = magic
        out.append(packet.type.rawValue)
        switch packet.type {
        case .query:
            out += padded(packet.nonce, to: 8, with: 0)
        case .reply:
            out += padded(packet.nonce, to: 8, with: 0)
            out += padded(packet.tag, to: 4, with: 0x20)
            out += portBytes(packet.tcpPort)
        case .locate:
            out += padded(packet.locate, to: 16, with: 0)
            out += portBytes(packet.tcpPort)
        }
        return out
    }

    static func decode(_ data: Data) -> Packet? {
        let bytes = [UInt8](data)
        guard bytes.count >= 5, Data(bytes.prefix(4)) == magic, let type = PacketType(rawValue: bytes[4]) else { return nil }
        let body = Array(bytes.dropFirst(5))
        func port(_ at: Int) -> UInt16 { UInt16(body[at]) << 8 | UInt16(body[at + 1]) }
        switch type {
        case .query:
            guard body.count == 8 else { return nil }
            return Packet(type: .query, nonce: Data(body))
        case .reply:
            guard body.count == 14, port(12) != 0 else { return nil }
            return Packet(type: .reply, nonce: Data(body[0 ..< 8]), tag: Data(body[8 ..< 12]), tcpPort: port(12))
        case .locate:
            guard body.count == 18 else { return nil }
            return Packet(type: .locate, tcpPort: port(16), locate: Data(body[0 ..< 16]))
        }
    }

    /// HMAC(locate key, 30-second time slot), first 16 bytes.
    static func locateToken(key: Data, unixSeconds: Int64) -> Data {
        var slot = UInt64(unixSeconds / 30).bigEndian
        let slotBytes = withUnsafeBytes(of: &slot) { Data($0) }
        return SyncCrypto.hmacBLAKE2b(key: key, data: Data("ClipDeck locate".utf8) + slotBytes).prefix(16)
    }

    static func randomTag() -> Data {
        let alphabet = Array("ACDEFGHJKMNPQRTUVWXY3479".utf8)
        return Data(SyncCrypto.randomBytes(4).map { alphabet[Int($0) % alphabet.count] })
    }

    // MARK: Framing

    static func frame(_ payload: Data) -> Data {
        Data([UInt8((payload.count >> 8) & 0xff), UInt8(payload.count & 0xff)]) + payload
    }

    /// Pops complete frames from `buffer`; nil when a frame is malformed.
    static func takeFrames(_ buffer: inout Data) -> [Data]? {
        var frames: [Data] = []
        while buffer.count >= 2 {
            let start = buffer.startIndex
            let length = Int(buffer[start]) << 8 | Int(buffer[start + 1])
            guard length > 0 else { return nil }
            guard buffer.count >= 2 + length else { break }
            frames.append(Data(buffer[(start + 2) ..< (start + 2 + length)]))
            buffer = Data(buffer.dropFirst(2 + length))
        }
        return frames
    }

    static func jsonFrame(_ object: [String: Any]) -> Data {
        let json = (try? JSONSerialization.data(withJSONObject: object, options: [])) ?? Data("{}".utf8)
        return Data([UInt8(ascii: "J")]) + json
    }

    static func blobFrame(blobID: Data, offset: UInt32, chunk: Data) -> Data {
        var position = offset.bigEndian
        return Data([UInt8(ascii: "B")]) + padded(blobID, to: 16, with: 0) + withUnsafeBytes(of: &position) { Data($0) } + chunk
    }

    static func parseJSON(_ plaintext: Data) -> [String: Any]? {
        guard plaintext.first == UInt8(ascii: "J") else { return nil }
        return (try? JSONSerialization.jsonObject(with: plaintext.dropFirst())) as? [String: Any]
    }

    struct BlobChunk {
        var blobID: Data
        var offset: UInt32
        var data: Data
    }

    static func parseBlob(_ plaintext: Data) -> BlobChunk? {
        let bytes = [UInt8](plaintext)
        guard bytes.count >= 21, bytes[0] == UInt8(ascii: "B") else { return nil }
        let offset = UInt32(bytes[17]) << 24 | UInt32(bytes[18]) << 16 | UInt32(bytes[19]) << 8 | UInt32(bytes[20])
        return BlobChunk(blobID: Data(bytes[1 ..< 17]), offset: offset, data: Data(bytes[21...]))
    }

    // MARK: Items

    /// One history item as it travels between devices. Images are PNG.
    struct WireItem {
        var syncID: String
        var origin: String
        var hlc: UInt64
        var isImage: Bool
        var title: String
        var copiedAtMilliseconds: Int64
        var angle: Int
        var sourceName: String
        var html: String?
        var body = Data()
        var bodySize: Int64 = 0
        var blobID = Data()

        var text: String { String(decoding: body, as: UTF8.self) }
    }

    static func makeWireItem(_ item: WireItem) -> WireItem {
        var wire = item
        wire.bodySize = Int64(wire.body.count)
        wire.blobID = wire.body.count > inlineBodyLimit ? SyncCrypto.randomBytes(16) : Data()
        return wire
    }

    static func encode(_ wire: WireItem) -> [String: Any] {
        var object: [String: Any] = [
            "type": "item",
            "syncId": wire.syncID,
            "origin": wire.origin,
            "hlc": String(wire.hlc),
            "kind": wire.isImage ? "image" : "text",
            "title": wire.title,
            "copiedAt": String(wire.copiedAtMilliseconds),
            "angle": wire.angle,
            "sourceName": wire.sourceName,
            "bodySize": String(wire.bodySize),
        ]
        if !wire.isImage, let html = wire.html, html.utf8.count + wire.body.count < inlineBodyLimit {
            object["html"] = html
        }
        if wire.blobID.isEmpty {
            object["body"] = wire.body.base64EncodedString()
        } else {
            object["blob"] = SyncCrypto.hex(wire.blobID)
        }
        return object
    }

    static func decodeItem(_ object: [String: Any]) -> WireItem? {
        guard object["type"] as? String == "item",
              let syncID = object["syncId"] as? String, UUID(uuidString: syncID) != nil,
              let hlcText = object["hlc"] as? String, let hlc = UInt64(hlcText),
              let kind = object["kind"] as? String, kind == "text" || kind == "image" else { return nil }
        var wire = WireItem(
            syncID: syncID,
            origin: object["origin"] as? String ?? "",
            hlc: hlc,
            isImage: kind == "image",
            title: object["title"] as? String ?? "",
            copiedAtMilliseconds: Int64(object["copiedAt"] as? String ?? "") ?? 0,
            angle: object["angle"] as? Int ?? 0,
            sourceName: object["sourceName"] as? String ?? "",
            html: object["html"] as? String
        )
        wire.bodySize = Int64(object["bodySize"] as? String ?? "") ?? 0
        if let blob = object["blob"] as? String {
            guard let blobID = SyncCrypto.fromHex(blob), blobID.count == 16, wire.bodySize > 0 else { return nil }
            wire.blobID = blobID
            return wire
        }
        guard let body = Data(base64Encoded: object["body"] as? String ?? ""), Int64(body.count) == wire.bodySize
        else { return nil }
        wire.body = body
        return wire
    }
}
