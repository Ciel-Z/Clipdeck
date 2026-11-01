import Foundation
import XCTest
@testable import ClipDeck

/// Keeps the Swift LAN sync byte-compatible with linux/src/sync: the published
/// Noise (cacophony) and CPace (draft-13) vectors, and golden values printed
/// by windows/tests/sync_golden.cpp.
final class SyncGoldenTests: XCTestCase {
    private func hex(_ text: String) -> Data { SyncCrypto.fromHex(text)! }
    private func sequence(_ length: Int, from start: Int = 0) -> Data { Data((0 ..< length).map { UInt8(start + $0) }) }

    override func setUp() {
        super.setUp()
        XCTAssertTrue(SyncCrypto.isAvailable)
    }

    // MARK: Golden values from the C++ implementation

    func testPrimitivesMatchTheSharedImplementation() {
        XCTAssertEqual(
            SyncCrypto.hex(SyncCrypto.hmacBLAKE2b(key: Data("key".utf8), data: Data("data".utf8))),
            "3cc28e2f7d79744693dd39171cf9cb0f42aaa0052a85f2b0bd5389e0e39cc97c6140d1a1aebcb255f9fa367414bf1aef02fa256d27cd973ff523dacade695b70")
        let (first, second, third) = SyncCrypto.hkdf(chainingKey: sequence(64), inputKeyMaterial: sequence(32, from: 100))
        XCTAssertEqual(SyncCrypto.hex(first),
                       "2262ed5f40c3324574d00a1e0849a7cc67fb5b6a80211c3c59404bb8e47512b1cb95e73840718c5d68ca345f934f39a52f1f31e8f6d3ed521f0664f46feddae4")
        XCTAssertEqual(SyncCrypto.hex(second),
                       "f6a6559fa2aaab64d3d2666b83fe3f4fed6fae1959537fedd6a39b6f8d411e6132593234cc2a750244335da5021c27bf7eacbb65cfa608457de6ecc144b90077")
        XCTAssertEqual(SyncCrypto.hex(third),
                       "07c82c1301c70e885f4662f1f3f7b7bb9e04d81c94a6e1882f812c02ffb69580c93095bb599ad2fced3d3cf38c9c17df4fb3c0029f231247fca69b040e16449e")
        let publicKey = SyncCrypto.publicKey(for: sequence(32, from: 1))
        XCTAssertEqual(SyncCrypto.hex(publicKey), "07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c")
        XCTAssertEqual(SyncCrypto.deviceID(for: publicKey), "38f866e2b504c1c92f3438ee464c916d")
    }

    func testPacketsAndFramesMatchTheSharedImplementation() {
        XCTAssertEqual(SyncCrypto.hex(SyncProtocol.locateToken(key: sequence(32), unixSeconds: 1_700_000_000)),
                       "7f318afa4d4de057d39b0f5a51c98605")
        let reply = SyncProtocol.Packet(type: .reply, nonce: Data("12345678".utf8), tag: Data("K7Q2".utf8), tcpPort: 47821)
        XCTAssertEqual(SyncCrypto.hex(SyncProtocol.encode(reply)), "43444b310231323334353637384b375132bacd")
        XCTAssertEqual(SyncProtocol.decode(SyncProtocol.encode(reply)), reply)
        let locate = SyncProtocol.Packet(type: .locate, tcpPort: 47821, locate: sequence(16))
        XCTAssertEqual(SyncCrypto.hex(SyncProtocol.encode(locate)), "43444b3103000102030405060708090a0b0c0d0e0fbacd")
        XCTAssertEqual(SyncProtocol.decode(SyncProtocol.encode(locate)), locate)
        XCTAssertNil(SyncProtocol.decode(Data("CDK1".utf8) + Data([2, 0])))

        let blob = SyncProtocol.blobFrame(blobID: sequence(16), offset: 70_000, chunk: Data("abc".utf8))
        XCTAssertEqual(SyncCrypto.hex(blob), "42000102030405060708090a0b0c0d0e0f00011170616263")
        let chunk = SyncProtocol.parseBlob(blob)
        XCTAssertEqual(chunk?.blobID, sequence(16))
        XCTAssertEqual(chunk?.offset, 70_000)
        XCTAssertEqual(chunk?.data, Data("abc".utf8))

        let framed = SyncProtocol.frame(Data("hello".utf8))
        XCTAssertEqual(SyncCrypto.hex(framed), "000568656c6c6f")
        var buffer = framed + SyncProtocol.frame(Data("x".utf8)) + Data([0, 9, 1])
        XCTAssertEqual(SyncProtocol.takeFrames(&buffer), [Data("hello".utf8), Data("x".utf8)])
        XCTAssertEqual(buffer, Data([0, 9, 1]))
        var malformed = Data([0, 0])
        XCTAssertNil(SyncProtocol.takeFrames(&malformed))
    }

    func testPairingDerivationsMatchTheSharedImplementation() {
        let isk = sequence(64, from: 7)
        XCTAssertEqual(SyncCrypto.hex(CPaceSession.confirmation(isk: isk, fromInitiator: true)),
                       "887415da0d5afdcd67955fabdd9f05576e7507e35cafab9ced5e701501ca6f5c73c74a3d5a44fde73e416d12e605eb89ff2fdbc505940e077ba372b9d7fdce75")
        XCTAssertEqual(SyncCrypto.hex(CPaceSession.confirmation(isk: isk, fromInitiator: false)),
                       "d8bde7551dda77419b1e7ee3693872de99e0685d085bd4313ed79cfe68d1bcdf02cbb85feff6ff5719f9ddb90b38c289d488b1324f567091cbf6b07745e34dbf")
        XCTAssertEqual(SyncCrypto.hex(SyncCrypto.hmacBLAKE2b(key: isk, data: Data("ClipDeck public key".utf8) + sequence(32))),
                       "7d4a018b44ca5c347b36ffe9f51f29c7fbfdf8dec083106150b169ca98a77f7d523af79cbdfbff5c48bb6db7c94e1dd5f3933641bdffcfb4ac374739b23630d7")
        XCTAssertEqual(SyncCrypto.hex(SyncCrypto.hmacBLAKE2b(key: isk, data: Data("ClipDeck locate key".utf8)).prefix(32)),
                       "7c672c0d58d0887668fcf9715985ac83be428e46a6532282b0a5a68e8a63ef93")
        XCTAssertEqual(
            SyncCrypto.hex(CPaceSession.generator(password: Data("482193".utf8), channelID: SyncService.pairingChannelID,
                                                  sid: sequence(64))),
            "66cdf631c9cec954e0c1c7eaca9033e0ec17458594dd1ecf0a02c256f6eee229")
    }

    // MARK: Noise vectors (cacophony, Noise_NN/KK_25519_ChaChaPoly_BLAKE2b)

    private let prologue = Data("John Galt".utf8)
    private lazy var initEphemeral = hex("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a")
    private lazy var respEphemeral = hex("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b")
    private lazy var payloads = [
        hex("4c756477696720766f6e204d69736573"), hex("4d757272617920526f746862617264"),
        hex("462e20412e20486179656b"), hex("4361726c204d656e676572"),
    ]

    private func runVector(_ pattern: NoiseHandshake.Pattern, initStatic: SyncCrypto.KeyPair?,
                           respStatic: SyncCrypto.KeyPair?, expected: [String], handshakeHash: String) {
        var initiator = NoiseHandshake(pattern: pattern, initiator: true, prologue: prologue, localStatic: initStatic,
                                       remoteStatic: respStatic?.publicKey, fixedEphemeralPrivateKey: initEphemeral)
        var responder = NoiseHandshake(pattern: pattern, initiator: false, prologue: prologue, localStatic: respStatic,
                                       remoteStatic: initStatic?.publicKey, fixedEphemeralPrivateKey: respEphemeral)
        let first = initiator.writeMessage(payload: payloads[0])
        XCTAssertEqual(first.map(SyncCrypto.hex), expected[0])
        XCTAssertEqual(first.flatMap { responder.readMessage($0) }, payloads[0])
        let second = responder.writeMessage(payload: payloads[1])
        XCTAssertEqual(second.map(SyncCrypto.hex), expected[1])
        XCTAssertEqual(second.flatMap { initiator.readMessage($0) }, payloads[1])
        XCTAssertTrue(initiator.isComplete && responder.isComplete)
        XCTAssertEqual(SyncCrypto.hex(initiator.handshakeHash), handshakeHash)
        XCTAssertEqual(SyncCrypto.hex(responder.handshakeHash), handshakeHash)

        var initiatorSend = initiator.sendCipher
        var responderSend = responder.sendCipher
        var initiatorReceive = initiator.receiveCipher
        var responderReceive = responder.receiveCipher
        let third = initiatorSend.encrypt(ad: Data(), plaintext: payloads[2])
        let fourth = responderSend.encrypt(ad: Data(), plaintext: payloads[3])
        XCTAssertEqual(third.map(SyncCrypto.hex), expected[2])
        XCTAssertEqual(fourth.map(SyncCrypto.hex), expected[3])
        XCTAssertEqual(third.flatMap { responderReceive.decrypt(ad: Data(), ciphertext: $0) }, payloads[2])
        XCTAssertEqual(fourth.flatMap { initiatorReceive.decrypt(ad: Data(), ciphertext: $0) }, payloads[3])
    }

    func testNoiseNNMatchesTheVector() {
        runVector(.nn, initStatic: nil, respStatic: nil, expected: [
            "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444c756477696720766f6e204d69736573",
            "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843d10cf8ef4ab895bed3e4673211f0c9337039d63a450c7b28196b8a0ebade00",
            "e50ec882703a1f34bf4957d8cafd036d34e02930f672f424c676e1",
            "35bb2a728d3e8e5f47781d486089e4a37c5c2e4261256f44569a9f",
        ], handshakeHash: "25b3d1154146a2e058e4db548e0841992cf33a972d5b85a908e4fb8f14b6d94f4987e17baa330c93dd842d6eda030cd47190c60d7c862574078779aba1302a2e")
    }

    func testNoiseKKMatchesTheVector() {
        let initPrivate = hex("e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1")
        let respPrivate = hex("4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893")
        let initStatic = SyncCrypto.KeyPair(publicKey: SyncCrypto.publicKey(for: initPrivate), privateKey: initPrivate)
        let respStatic = SyncCrypto.KeyPair(publicKey: SyncCrypto.publicKey(for: respPrivate), privateKey: respPrivate)
        XCTAssertEqual(SyncCrypto.hex(initStatic.publicKey), "6bc3822a2aa7f4e6981d6538692b3cdf3e6df9eea6ed269eb41d93c22757b75a")
        XCTAssertEqual(SyncCrypto.hex(respStatic.publicKey), "31e0303fd6418d2f8c0e78b91f22e8caed0fbe48656dcf4767e4834f701b8f62")
        runVector(.kk, initStatic: initStatic, respStatic: respStatic, expected: [
            "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944f79a1d4b21fc3ea4a0c87213b8b4f0599d758682c26a3ae5e09195a3e742bc74",
            "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843e20b1bf85731f75d7e21b5d54baaa66341de4292c3d42571c1bd7e7f1abe38",
            "25bfaa58833b07cdd6af7c07f2c51daac681a8ac0a02dd373259bd",
            "ef586cff556dec8ef0053871ff0d4bf3f2c72e842487ec6d1da69f",
        ], handshakeHash: "76dbc866183c8ee7363dbf0ebab8d6355010245f9817aa78359818a03a052586d7e8b4bb2ae5622a1a61212df90af04bb2b2cc189ce0e819ba0c4970c9f71805")
    }

    func testKKRejectsAnUnknownStaticKey() {
        let alice = SyncCrypto.generateKeyPair()
        let bob = SyncCrypto.generateKeyPair()
        let mallory = SyncCrypto.generateKeyPair()
        var initiator = NoiseHandshake(pattern: .kk, initiator: true, prologue: SyncChannel.prologue, localStatic: mallory,
                                       remoteStatic: bob.publicKey)
        var responder = NoiseHandshake(pattern: .kk, initiator: false, prologue: SyncChannel.prologue, localStatic: bob,
                                       remoteStatic: alice.publicKey)
        let first = initiator.writeMessage(payload: Data())
        XCTAssertNotNil(first)
        XCTAssertNil(first.flatMap { responder.readMessage($0) })
    }

    // MARK: CPace (draft-irtf-cfrg-cpace-13, ristretto255 vector)

    func testCPaceMatchesTheVector() {
        let password = Data("Password".utf8)
        let channelID = hex("6f630b425f726573706f6e6465720b415f696e69746961746f72")
        let sid = hex("7e4b4791d6a8ef019b936c79fb7f2c57")
        XCTAssertEqual(CPaceSession.generatorString(password: password, channelID: channelID, sid: sid).count, 172)
        XCTAssertEqual(SyncCrypto.hex(CPaceSession.generator(password: password, channelID: channelID, sid: sid)),
                       "a6fc82c3b8968fbb2e06fee81ca858586dea50d248f0c7ca6a18b0902a30b36b")
        let a = CPaceSession(initiator: true, password: password, channelID: channelID, sid: sid,
                             associatedData: Data("ADa".utf8),
                             fixedScalar: hex("da3d23700a9e5699258aef94dc060dfda5ebb61f02a5ea77fad53f4ff0976d08"))
        let b = CPaceSession(initiator: false, password: password, channelID: channelID, sid: sid,
                             associatedData: Data("ADb".utf8),
                             fixedScalar: hex("d2316b454718c35362d83d69df6320f38578ed5984651435e2949762d900b80d"))
        XCTAssertEqual(SyncCrypto.hex(a.message), "d40fb265a7abeaee7939d91a585fe59f7053f982c296ec413c624c669308f87a")
        XCTAssertEqual(SyncCrypto.hex(b.message), "08bcf6e9777a9c313a3db6daa510f2d398403319c2341bd506a92e672eb7e307")
        let expected = "4c5469a16b2364c4b944ebc1a79e51d1674ad47db26e8718154f59faebfaa52d"
            + "8346f30aa58377117eb20d527f2cbc5c76381f7fd372e89df8239f87f2e02ed1"
        XCTAssertEqual(a.sharedKey(peerMessage: b.message, peerAssociatedData: Data("ADb".utf8)).map(SyncCrypto.hex), expected)
        XCTAssertEqual(b.sharedKey(peerMessage: a.message, peerAssociatedData: Data("ADa".utf8)).map(SyncCrypto.hex), expected)
    }

    func testCPaceNeedsTheSameCodeAndSession() {
        let sid = SyncCrypto.randomBytes(64)
        let channelID = SyncService.pairingChannelID
        let a = CPaceSession(initiator: true, password: Data("482193".utf8), channelID: channelID, sid: sid,
                             associatedData: Data("device-a".utf8))
        let b = CPaceSession(initiator: false, password: Data("482193".utf8), channelID: channelID, sid: sid,
                             associatedData: Data("device-b".utf8))
        let ka = a.sharedKey(peerMessage: b.message, peerAssociatedData: Data("device-b".utf8))
        let kb = b.sharedKey(peerMessage: a.message, peerAssociatedData: Data("device-a".utf8))
        XCTAssertNotNil(ka)
        XCTAssertEqual(ka, kb)
        XCTAssertTrue(CPaceSession.verifyConfirmation(isk: kb!, fromInitiator: true,
                                                      tag: CPaceSession.confirmation(isk: ka!, fromInitiator: true)))
        XCTAssertFalse(CPaceSession.verifyConfirmation(isk: ka!, fromInitiator: true,
                                                       tag: CPaceSession.confirmation(isk: ka!, fromInitiator: false)))

        let wrong = CPaceSession(initiator: false, password: Data("482194".utf8), channelID: channelID, sid: sid,
                                 associatedData: Data("device-b".utf8))
        XCTAssertNotEqual(wrong.sharedKey(peerMessage: a.message, peerAssociatedData: Data("device-a".utf8)), ka)
        let relayed = CPaceSession(initiator: false, password: Data("482193".utf8), channelID: channelID,
                                   sid: SyncCrypto.randomBytes(64), associatedData: Data("device-b".utf8))
        XCTAssertNotEqual(relayed.sharedKey(peerMessage: a.message, peerAssociatedData: Data("device-a".utf8)), ka)
        XCTAssertNil(a.sharedKey(peerMessage: Data(count: 32), peerAssociatedData: Data("device-b".utf8)))
        XCTAssertNil(a.sharedKey(peerMessage: Data(repeating: 0xff, count: 32), peerAssociatedData: Data("device-b".utf8)))
    }

    // MARK: Items

    func testItemsRoundTripInlineAndAsBlobs() throws {
        var small = SyncProtocol.WireItem(syncID: UUID().uuidString.lowercased(), origin: "aa", hlc: 1 << 40, isImage: false,
                                          title: "标题", copiedAtMilliseconds: 1_700_000_000_123, angle: 0,
                                          sourceName: "Notes", html: nil)
        small.body = Data("hello sync".utf8)
        small = SyncProtocol.makeWireItem(small)
        XCTAssertTrue(small.blobID.isEmpty)
        let encoded = SyncProtocol.encode(small)
        XCTAssertEqual(encoded["hlc"] as? String, String(1 << 40))
        let frame = SyncProtocol.jsonFrame(encoded)
        let decoded = try XCTUnwrap(SyncProtocol.parseJSON(frame).flatMap(SyncProtocol.decodeItem))
        XCTAssertEqual(decoded.syncID, small.syncID)
        XCTAssertEqual(decoded.hlc, small.hlc)
        XCTAssertEqual(decoded.text, "hello sync")
        XCTAssertEqual(decoded.copiedAtMilliseconds, 1_700_000_000_123)

        var large = small
        large.isImage = true
        large.body = SyncCrypto.randomBytes(SyncProtocol.inlineBodyLimit + 1)
        large = SyncProtocol.makeWireItem(large)
        XCTAssertEqual(large.blobID.count, 16)
        let header = try XCTUnwrap(SyncProtocol.decodeItem(SyncProtocol.encode(large)))
        XCTAssertEqual(header.blobID, large.blobID)
        XCTAssertEqual(header.bodySize, Int64(large.body.count))
        XCTAssertTrue(header.body.isEmpty)

        var tampered = encoded
        tampered["bodySize"] = "3"
        XCTAssertNil(SyncProtocol.decodeItem(tampered))
        tampered = encoded
        tampered["syncId"] = "not-a-uuid"
        XCTAssertNil(SyncProtocol.decodeItem(tampered))
    }
}
