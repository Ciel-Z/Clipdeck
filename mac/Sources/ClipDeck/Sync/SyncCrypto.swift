import Clibsodium
import Foundation

/// LAN sync crypto (docs/lan-sync-design.md §9), byte-compatible with the
/// Linux/Windows implementation in linux/src/sync/noise.cpp and cpace.cpp:
/// Noise NN/KK (25519, ChaChaPoly, BLAKE2b) and CPace (ristretto255, SHA-512).
enum SyncCrypto {
    static let isAvailable: Bool = sodium_init() >= 0

    struct KeyPair: Equatable, Sendable {
        var publicKey: Data
        var privateKey: Data
    }

    static func randomBytes(_ count: Int) -> Data {
        _ = isAvailable
        var out = [UInt8](repeating: 0, count: count)
        randombytes_buf(&out, count)
        return Data(out)
    }

    static func generateKeyPair() -> KeyPair {
        let privateKey = randomBytes(32)
        return KeyPair(publicKey: publicKey(for: privateKey), privateKey: privateKey)
    }

    static func publicKey(for privateKey: Data) -> Data {
        _ = isAvailable
        var out = [UInt8](repeating: 0, count: 32)
        let secret = [UInt8](privateKey)
        _ = crypto_scalarmult_curve25519_base(&out, secret)
        return Data(out)
    }

    /// The first 16 bytes of BLAKE2b(public key), lowercase hex.
    static func deviceID(for publicKey: Data) -> String {
        hex(blake2b(publicKey, length: 16))
    }

    static func blake2b(_ data: Data, length: Int = 64) -> Data {
        _ = isAvailable
        var out = [UInt8](repeating: 0, count: length)
        let input = [UInt8](data)
        _ = crypto_generichash(&out, length, input, UInt64(input.count), nil, 0)
        return Data(out)
    }

    static func sha512(_ data: Data) -> Data {
        _ = isAvailable
        var out = [UInt8](repeating: 0, count: 64)
        let input = [UInt8](data)
        _ = crypto_hash_sha512(&out, input, UInt64(input.count))
        return Data(out)
    }

    static func hmacSHA512(key: Data, data: Data) -> Data {
        _ = isAvailable
        var state = crypto_auth_hmacsha512_state()
        var out = [UInt8](repeating: 0, count: 64)
        let keyBytes = [UInt8](key)
        let input = [UInt8](data)
        _ = crypto_auth_hmacsha512_init(&state, keyBytes, keyBytes.count)
        _ = crypto_auth_hmacsha512_update(&state, input, UInt64(input.count))
        _ = crypto_auth_hmacsha512_final(&state, &out)
        return Data(out)
    }

    /// HMAC built on BLAKE2b (block 128, digest 64), as Noise specifies.
    static func hmacBLAKE2b(key: Data, data: Data) -> Data {
        var block = key.count > 128 ? blake2b(key) : key
        block.append(Data(repeating: 0, count: 128 - block.count))
        let inner = Data(block.map { $0 ^ 0x36 })
        let outer = Data(block.map { $0 ^ 0x5c })
        return blake2b(outer + blake2b(inner + data))
    }

    /// The Noise HKDF: two or three 64-byte outputs.
    static func hkdf(chainingKey: Data, inputKeyMaterial: Data) -> (Data, Data, Data) {
        let tempKey = hmacBLAKE2b(key: chainingKey, data: inputKeyMaterial)
        let first = hmacBLAKE2b(key: tempKey, data: Data([1]))
        let second = hmacBLAKE2b(key: tempKey, data: first + Data([2]))
        let third = hmacBLAKE2b(key: tempKey, data: second + Data([3]))
        return (first, second, third)
    }

    static func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }
    static func fromHex(_ text: String) -> Data? {
        var out = Data()
        var index = text.startIndex
        while index < text.endIndex {
            let next = text.index(index, offsetBy: 2, limitedBy: text.endIndex) ?? text.endIndex
            guard let byte = UInt8(text[index ..< next], radix: 16) else { return nil }
            out.append(byte)
            index = next
        }
        return out
    }
}

// MARK: - Noise

/// One direction of an established connection (Noise CipherState).
struct NoiseCipherState: Sendable {
    static let tagLength = 16
    private(set) var key: [UInt8]?
    var nonce: UInt64 = 0

    mutating func initializeKey(_ key: Data?) {
        self.key = key.map { [UInt8]($0.prefix(32)) }
        nonce = 0
    }

    var hasKey: Bool { key != nil }

    private static func nonceBytes(_ n: UInt64) -> [UInt8] {
        var nonce = [UInt8](repeating: 0, count: 12)
        for i in 0 ..< 8 { nonce[4 + i] = UInt8(truncatingIfNeeded: n >> (8 * UInt64(i))) }
        return nonce
    }

    mutating func encrypt(ad: Data, plaintext: Data) -> Data? {
        guard let key else { return plaintext }
        guard nonce != UInt64.max else { return nil }
        let message = [UInt8](plaintext)
        let associated = [UInt8](ad)
        var out = [UInt8](repeating: 0, count: message.count + Self.tagLength)
        var length: UInt64 = 0
        let npub = Self.nonceBytes(nonce)
        _ = crypto_aead_chacha20poly1305_ietf_encrypt(
            &out, &length, message, UInt64(message.count), associated, UInt64(associated.count), nil, npub, key)
        nonce += 1
        return Data(out.prefix(Int(length)))
    }

    mutating func decrypt(ad: Data, ciphertext: Data) -> Data? {
        guard let key else { return ciphertext }
        guard nonce != UInt64.max, ciphertext.count >= Self.tagLength else { return nil }
        let input = [UInt8](ciphertext)
        let associated = [UInt8](ad)
        var out = [UInt8](repeating: 0, count: input.count - Self.tagLength)
        var length: UInt64 = 0
        let npub = Self.nonceBytes(nonce)
        guard crypto_aead_chacha20poly1305_ietf_decrypt(
            &out, &length, nil, input, UInt64(input.count), associated, UInt64(associated.count), npub, key
        ) == 0 else { return nil }
        nonce += 1
        return Data(out.prefix(Int(length)))
    }

    mutating func rekey() {
        guard let key else { return }
        let zeros = [UInt8](repeating: 0, count: 32)
        var out = [UInt8](repeating: 0, count: 32 + Self.tagLength)
        var length: UInt64 = 0
        let npub = Self.nonceBytes(UInt64.max)
        _ = crypto_aead_chacha20poly1305_ietf_encrypt(&out, &length, zeros, 32, nil, 0, nil, npub, key)
        self.key = Array(out.prefix(32))
    }
}

/// Noise_NN / Noise_KK _25519_ChaChaPoly_BLAKE2b, two handshake messages.
struct NoiseHandshake {
    enum Pattern: Sendable { case nn, kk }

    let pattern: Pattern
    let initiator: Bool
    private(set) var handshakeHash: Data
    private var chainingKey: Data
    private var cipher = NoiseCipherState()
    private var localStatic: SyncCrypto.KeyPair?
    private var ephemeral: SyncCrypto.KeyPair?
    private var remoteStatic: Data?
    private var remoteEphemeral: Data?
    private let fixedEphemeral: Data?
    private(set) var messageIndex = 0
    private(set) var failed = false
    private(set) var initiatorToResponder = NoiseCipherState()
    private(set) var responderToInitiator = NoiseCipherState()

    init(pattern: Pattern, initiator: Bool, prologue: Data, localStatic: SyncCrypto.KeyPair? = nil,
         remoteStatic: Data? = nil, fixedEphemeralPrivateKey: Data? = nil) {
        _ = SyncCrypto.isAvailable
        self.pattern = pattern
        self.initiator = initiator
        self.localStatic = localStatic
        self.remoteStatic = remoteStatic
        fixedEphemeral = fixedEphemeralPrivateKey
        let name = Data((pattern == .nn ? "Noise_NN_25519_ChaChaPoly_BLAKE2b" : "Noise_KK_25519_ChaChaPoly_BLAKE2b").utf8)
        handshakeHash = name + Data(repeating: 0, count: 64 - name.count)
        chainingKey = handshakeHash
        mixHash(prologue)
        if pattern == .kk {
            guard let localPublic = localStatic?.publicKey, let remoteStatic, localPublic.count == 32, remoteStatic.count == 32
            else {
                failed = true
                return
            }
            mixHash(initiator ? localPublic : remoteStatic)
            mixHash(initiator ? remoteStatic : localPublic)
        }
    }

    var isComplete: Bool { messageIndex >= 2 && !failed }
    var sendCipher: NoiseCipherState { initiator ? initiatorToResponder : responderToInitiator }
    var receiveCipher: NoiseCipherState { initiator ? responderToInitiator : initiatorToResponder }

    private mutating func mixHash(_ data: Data) { handshakeHash = SyncCrypto.blake2b(handshakeHash + data) }

    private mutating func mixKey(_ material: Data) {
        let (first, second, _) = SyncCrypto.hkdf(chainingKey: chainingKey, inputKeyMaterial: material)
        chainingKey = first
        cipher.initializeKey(second.prefix(32))
    }

    private func dh(_ privateKey: Data?, _ publicKey: Data?) -> Data? {
        guard let privateKey, let publicKey, privateKey.count == 32, publicKey.count == 32 else { return nil }
        var out = [UInt8](repeating: 0, count: 32)
        guard crypto_scalarmult_curve25519(&out, [UInt8](privateKey), [UInt8](publicKey)) == 0 else { return nil }
        return Data(out)
    }

    /// local: "e" or "s"; remote: "e" or "s".
    private mutating func mixDH(local: Character, remote: Character) -> Bool {
        let privateKey = local == "e" ? ephemeral?.privateKey : localStatic?.privateKey
        let publicKey = remote == "e" ? remoteEphemeral : remoteStatic
        guard let shared = dh(privateKey, publicKey) else { return false }
        mixKey(shared)
        return true
    }

    private mutating func split() {
        let (first, second, _) = SyncCrypto.hkdf(chainingKey: chainingKey, inputKeyMaterial: Data())
        initiatorToResponder.initializeKey(first.prefix(32))
        responderToInitiator.initializeKey(second.prefix(32))
    }

    /// Message 1: -> e (NN) / -> e, es, ss (KK). Message 2: <- e, ee (NN) / <- e, ee, se (KK).
    mutating func writeMessage(payload: Data) -> Data? {
        let myTurn = (messageIndex == 0 && initiator) || (messageIndex == 1 && !initiator)
        guard !failed, myTurn else { return nil }
        if let fixedEphemeral {
            ephemeral = SyncCrypto.KeyPair(publicKey: SyncCrypto.publicKey(for: fixedEphemeral), privateKey: fixedEphemeral)
        } else {
            ephemeral = SyncCrypto.generateKeyPair()
        }
        let ephemeralPublic = ephemeral!.publicKey
        var message = ephemeralPublic
        mixHash(ephemeralPublic)
        var ok = true
        if messageIndex == 0 {
            if pattern == .kk { ok = mixDH(local: "e", remote: "s") && mixDH(local: "s", remote: "s") }
        } else {
            ok = mixDH(local: "e", remote: "e")
            if ok, pattern == .kk { ok = mixDH(local: "e", remote: "s") }
        }
        guard ok, let ciphertext = cipher.encrypt(ad: handshakeHash, plaintext: payload) else {
            failed = true
            return nil
        }
        mixHash(ciphertext)
        message += ciphertext
        messageIndex += 1
        if messageIndex == 2 { split() }
        return message
    }

    mutating func readMessage(_ message: Data) -> Data? {
        let theirTurn = (messageIndex == 0 && !initiator) || (messageIndex == 1 && initiator)
        guard !failed, theirTurn, message.count >= 32 else {
            failed = true
            return nil
        }
        remoteEphemeral = Data(message.prefix(32))
        mixHash(remoteEphemeral!)
        var ok = true
        if messageIndex == 0 {
            if pattern == .kk { ok = mixDH(local: "s", remote: "e") && mixDH(local: "s", remote: "s") }
        } else {
            ok = mixDH(local: "e", remote: "e")
            if ok, pattern == .kk { ok = mixDH(local: "s", remote: "e") }
        }
        let ciphertext = Data(message.dropFirst(32))
        guard ok, let payload = cipher.decrypt(ad: handshakeHash, ciphertext: ciphertext) else {
            failed = true
            return nil
        }
        mixHash(ciphertext)
        messageIndex += 1
        if messageIndex == 2 { split() }
        return payload
    }
}

// MARK: - CPace

/// CPace (draft-irtf-cfrg-cpace-13), ristretto255 and SHA-512, initiator/responder ordering.
struct CPaceSession {
    static let dsi = Data("CPaceRistretto255".utf8)
    static let dsiISK = Data("CPaceRistretto255_ISK".utf8)

    let initiator: Bool
    let sid: Data
    let associatedData: Data
    private let scalar: [UInt8]
    let message: Data
    let isValid: Bool

    init(initiator: Bool, password: Data, channelID: Data, sid: Data, associatedData: Data, fixedScalar: Data? = nil) {
        _ = SyncCrypto.isAvailable
        self.initiator = initiator
        self.sid = sid
        self.associatedData = associatedData
        var scalar = [UInt8](repeating: 0, count: 32)
        if let fixedScalar, fixedScalar.count == 32 {
            scalar = [UInt8](fixedScalar)
        } else {
            crypto_core_ristretto255_scalar_random(&scalar)
        }
        self.scalar = scalar
        let generator = [UInt8](Self.generator(password: password, channelID: channelID, sid: sid))
        var point = [UInt8](repeating: 0, count: 32)
        isValid = crypto_scalarmult_ristretto255(&point, scalar, generator) == 0
        message = Data(point)
    }

    static func lengthValue(_ data: Data) -> Data {
        var out = Data()
        var length = UInt64(data.count)
        repeat {
            var byte = UInt8(length & 0x7f)
            length >>= 7
            if length != 0 { byte |= 0x80 }
            out.append(byte)
        } while length != 0
        return out + data
    }

    static func generatorString(password: Data, channelID: Data, sid: Data) -> Data {
        let used = 1 + lengthValue(password).count + lengthValue(dsi).count
        let zeroPad = max(0, 128 - used)
        return lengthValue(dsi) + lengthValue(password) + lengthValue(Data(repeating: 0, count: zeroPad))
            + lengthValue(channelID) + lengthValue(sid)
    }

    static func generator(password: Data, channelID: Data, sid: Data) -> Data {
        let hash = [UInt8](SyncCrypto.sha512(generatorString(password: password, channelID: channelID, sid: sid)))
        var point = [UInt8](repeating: 0, count: 32)
        crypto_core_ristretto255_from_hash(&point, hash)
        return Data(point)
    }

    func sharedKey(peerMessage: Data, peerAssociatedData: Data) -> Data? {
        guard isValid, peerMessage.count == 32 else { return nil }
        let peer = [UInt8](peerMessage)
        var k = [UInt8](repeating: 0, count: 32)
        guard crypto_core_ristretto255_is_valid_point(peer) == 1,
              crypto_scalarmult_ristretto255(&k, scalar, peer) == 0 else { return nil }
        let ya = initiator ? message : peerMessage
        let ada = initiator ? associatedData : peerAssociatedData
        let yb = initiator ? peerMessage : message
        let adb = initiator ? peerAssociatedData : associatedData
        let transcript = Self.lengthValue(ya) + Self.lengthValue(ada) + Self.lengthValue(yb) + Self.lengthValue(adb)
        return SyncCrypto.sha512(Self.lengthValue(Self.dsiISK) + Self.lengthValue(sid) + Self.lengthValue(Data(k)) + transcript)
    }

    static func confirmation(isk: Data, fromInitiator: Bool) -> Data {
        SyncCrypto.hmacSHA512(
            key: isk,
            data: Data((fromInitiator ? "ClipDeck pairing confirm initiator" : "ClipDeck pairing confirm responder").utf8))
    }

    static func verifyConfirmation(isk: Data, fromInitiator: Bool, tag: Data) -> Bool {
        let expected = [UInt8](confirmation(isk: isk, fromInitiator: fromInitiator))
        let received = [UInt8](tag)
        return received.count == 64 && crypto_verify_64(received, expected) == 0
    }
}
