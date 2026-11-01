import Darwin
import Foundation
import Network

// MARK: - Interfaces

/// Which network adapters LAN sync uses (docs/lan-sync-design.md §5): up,
/// multicast-capable, private IPv4, and not loopback, VPN or virtual.
enum SyncInterfaces {
    struct Interface: Equatable {
        var name: String
        var address: in_addr_t
        var netmask: in_addr_t
        var addressText: String
    }

    static func isPrivate(_ hostOrder: UInt32) -> Bool {
        hostOrder >> 24 == 10 || hostOrder >> 20 == (172 << 4 | 1) || hostOrder >> 16 == (192 << 8 | 168)
    }

    static func looksVirtualOrVPN(_ name: String) -> Bool {
        let prefixes = ["utun", "ipsec", "ppp", "awdl", "llw", "bridge", "vmnet", "vboxnet", "gif", "stf", "anpi",
                        "tap", "tun", "wg", "docker", "veth", "lo"]
        return prefixes.contains { name.hasPrefix($0) }
    }

    static func eligible() -> [Interface] {
        var result: [Interface] = []
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return result }
        defer { freeifaddrs(list) }
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor?.pointee {
            defer { cursor = entry.ifa_next }
            let flags = Int32(entry.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_RUNNING != 0, flags & IFF_MULTICAST != 0, flags & IFF_LOOPBACK == 0,
                  let address = entry.ifa_addr, address.pointee.sa_family == sa_family_t(AF_INET),
                  let mask = entry.ifa_netmask else { continue }
            let name = String(cString: entry.ifa_name)
            guard !looksVirtualOrVPN(name) else { continue }
            let ip = address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr.s_addr }
            let netmask = mask.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr.s_addr }
            guard isPrivate(UInt32(bigEndian: ip)) else { continue }
            var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            var copy = in_addr(s_addr: ip)
            inet_ntop(AF_INET, &copy, &text, socklen_t(INET_ADDRSTRLEN))
            result.append(Interface(name: name, address: ip, netmask: netmask, addressText: String(cString: text)))
        }
        return result
    }

    /// Same subnet as this device first.
    static func rank(_ addresses: [String], interfaces: [Interface]) -> [String] {
        func score(_ entry: String) -> Int {
            let host = entry.split(separator: ":").first.map(String.init) ?? entry
            var addr = in_addr()
            guard inet_pton(AF_INET, host, &addr) == 1 else { return 0 }
            return interfaces.contains { ($0.address & $0.netmask) == (addr.s_addr & $0.netmask) } ? 2 : 0
        }
        return addresses.enumerated().sorted { lhs, rhs in
            let (a, b) = (score(lhs.element), score(rhs.element))
            return a != b ? a > b : lhs.offset < rhs.offset
        }.map(\.element)
    }
}

// MARK: - UDP discovery

/// UDP discovery on demand, as in linux/src/sync/discovery.cpp: queries go
/// out from their own socket so replies reach only us; the shared port
/// answers queries (while visible) and carries locate packets.
@MainActor
final class SyncDiscovery {
    var onReply: ((String, UInt16, Data) -> Void)?
    var onLocate: ((String, UInt16, Data) -> Void)?

    private var sharedSocket: Int32 = -1
    private var querySocket: Int32 = -1
    private var sources: [DispatchSourceRead] = []
    private var visible = false
    private var tag = Data()
    private var tcpPort: UInt16 = 0
    private var ownNonces: Set<Data> = []
    private var lastReplyTo: [String: Date] = [:]
    private var lastLocateFrom: [String: Date] = [:]

    var isListening: Bool { sharedSocket >= 0 }

    @discardableResult
    func start() -> Bool {
        if sharedSocket >= 0 { return true }
        sharedSocket = makeSocket(port: SyncProtocol.discoveryPort)
        querySocket = makeSocket(port: 0)
        guard sharedSocket >= 0, querySocket >= 0 else {
            stop()
            return false
        }
        rejoin()
        listen(on: sharedSocket, isQuerySocket: false)
        listen(on: querySocket, isQuerySocket: true)
        return true
    }

    /// Joins the group again on every eligible interface; the OS can drop a
    /// membership without telling us. Fails harmlessly where we are still in.
    func rejoin() {
        guard sharedSocket >= 0 else { return }
        var group = in_addr()
        inet_pton(AF_INET, SyncProtocol.discoveryGroup, &group)
        for interface in SyncInterfaces.eligible() {
            var request = ip_mreq(imr_multiaddr: group, imr_interface: in_addr(s_addr: interface.address))
            setsockopt(sharedSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, socklen_t(MemoryLayout<ip_mreq>.size))
        }
    }

    func stop() {
        sources.forEach { $0.cancel() }
        sources.removeAll()
        if sharedSocket >= 0 { close(sharedSocket) }
        if querySocket >= 0 { close(querySocket) }
        sharedSocket = -1
        querySocket = -1
        visible = false
    }

    func setVisible(_ visible: Bool, tag: Data, tcpPort: UInt16) {
        self.visible = visible
        self.tag = tag
        self.tcpPort = tcpPort
    }

    func query() {
        guard start() else { return }
        rejoin()
        sendQueryRound()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { MainActor.assumeIsolated { self.sendQueryRound() } }
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { MainActor.assumeIsolated { self.sendQueryRound() } }
    }

    func locate(token: Data, tcpPort: UInt16, target: String? = nil) {
        guard start() else { return }
        let packet = SyncProtocol.encode(SyncProtocol.Packet(type: .locate, tcpPort: tcpPort, locate: token))
        if let target { send(packet, on: sharedSocket, to: target) } else { sendToGroup(packet, on: sharedSocket) }
    }

    private func sendQueryRound() {
        let nonce = SyncCrypto.randomBytes(8)
        ownNonces.insert(nonce)
        if ownNonces.count > 64 { ownNonces.removeAll() }
        sendToGroup(SyncProtocol.encode(SyncProtocol.Packet(type: .query, nonce: nonce)), on: querySocket)
    }

    private func makeSocket(port: UInt16) -> Int32 {
        let descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard descriptor >= 0 else { return -1 }
        var yes: Int32 = 1
        setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(descriptor, SOL_SOCKET, SO_REUSEPORT, &yes, socklen_t(MemoryLayout<Int32>.size))
        var ttl: UInt8 = 1
        setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, socklen_t(MemoryLayout<UInt8>.size))
        var loop: UInt8 = 0
        setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, socklen_t(MemoryLayout<UInt8>.size))
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr.s_addr = INADDR_ANY
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0 else {
            close(descriptor)
            return -1
        }
        return descriptor
    }

    private func listen(on descriptor: Int32, isQuerySocket: Bool) {
        let source = DispatchSource.makeReadSource(fileDescriptor: descriptor, queue: .main)
        source.setEventHandler { [weak self] in
            MainActor.assumeIsolated { self?.receive(on: descriptor, isQuerySocket: isQuerySocket) }
        }
        source.resume()
        sources.append(source)
    }

    private func receive(on descriptor: Int32, isQuerySocket: Bool) {
        var buffer = [UInt8](repeating: 0, count: 256)
        var from = sockaddr_in()
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let count = withUnsafeMutablePointer(to: &from) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(descriptor, &buffer, buffer.count, 0, $0, &length) }
        }
        guard count > 0, let packet = SyncProtocol.decode(Data(buffer.prefix(count))) else { return }
        var addressText = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &from.sin_addr, &addressText, socklen_t(INET_ADDRSTRLEN))
        let sender = String(cString: addressText)
        let senderPort = UInt16(bigEndian: from.sin_port)
        let now = Date()
        if isQuerySocket {
            if packet.type == .reply, ownNonces.contains(packet.nonce) { onReply?(sender, packet.tcpPort, packet.tag) }
            return
        }
        switch packet.type {
        case .query:
            guard visible, !ownNonces.contains(packet.nonce),
                  now.timeIntervalSince(lastReplyTo[sender] ?? .distantPast) >= 2 else { return }
            lastReplyTo[sender] = now
            let reply = SyncProtocol.Packet(type: .reply, nonce: packet.nonce, tag: tag, tcpPort: tcpPort)
            send(SyncProtocol.encode(reply), on: sharedSocket, to: sender, port: senderPort)
        case .locate:
            guard now.timeIntervalSince(lastLocateFrom[sender] ?? .distantPast) >= 2 else { return }
            lastLocateFrom[sender] = now
            onLocate?(sender, packet.tcpPort, packet.locate)
        case .reply:
            break
        }
    }

    private func sendToGroup(_ datagram: Data, on descriptor: Int32) {
        for interface in SyncInterfaces.eligible() {
            var outgoing = in_addr(s_addr: interface.address)
            setsockopt(descriptor, IPPROTO_IP, IP_MULTICAST_IF, &outgoing, socklen_t(MemoryLayout<in_addr>.size))
            send(datagram, on: descriptor, to: SyncProtocol.discoveryGroup)
        }
    }

    private func send(_ datagram: Data, on descriptor: Int32, to host: String, port: UInt16 = SyncProtocol.discoveryPort) {
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        inet_pton(AF_INET, host, &address.sin_addr)
        let bytes = [UInt8](datagram)
        _ = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(descriptor, bytes, bytes.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }
}

// MARK: - Encrypted channel

/// One encrypted TCP connection, as linux/src/sync/secure_channel.cpp: a
/// plaintext "CDK1" + kind frame, a Noise handshake (NN for pairing, KK for
/// sync), then encrypted frames. A KK responder tries each known device's key.
@MainActor
final class SyncChannel {
    enum Kind: UInt8 { case pairing = 0x50, sync = 0x53 } // "P", "S"

    struct Candidate: Equatable {
        var deviceID: String
        var publicKey: Data
    }

    nonisolated static let prologue = Data("ClipDeck LAN sync v1".utf8)
    static let rekeyInterval = 10_000

    let initiator: Bool
    private(set) var kind: Kind = .pairing
    private(set) var peer = Candidate(deviceID: "", publicKey: Data())
    private(set) var isEstablished = false
    private(set) var handshakeHash = Data()
    private(set) var peerAddress = ""
    private(set) var localAddress = ""

    var onEstablished: ((SyncChannel) -> Void)?
    var onReceive: ((SyncChannel, Data) -> Void)?
    var onClose: ((SyncChannel, String) -> Void)?

    private let connection: NWConnection
    private let localStatic: SyncCrypto.KeyPair
    private var candidates: [Candidate] = []
    private var handshake: NoiseHandshake?
    private var sendCipher = NoiseCipherState()
    private var receiveCipher = NoiseCipherState()
    private var buffer = Data()
    private var headerSeen = false
    private var closed = false
    private var sentSinceRekey = 0
    private var receivedSinceRekey = 0

    static func connect(host: String, port: UInt16, kind: Kind, localStatic: SyncCrypto.KeyPair,
                        peer: Candidate) -> SyncChannel {
        // A direct LAN connection, never through a system proxy.
        let parameters = NWParameters.tcp
        parameters.preferNoProxies = true
        let connection = NWConnection(
            host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port) ?? 47821, using: parameters)
        let channel = SyncChannel(connection: connection, initiator: true, localStatic: localStatic)
        channel.kind = kind
        channel.peer = peer
        channel.peerAddress = host
        channel.start()
        return channel
    }

    static func accept(_ connection: NWConnection, localStatic: SyncCrypto.KeyPair,
                       candidates: [Candidate]) -> SyncChannel {
        let channel = SyncChannel(connection: connection, initiator: false, localStatic: localStatic)
        channel.candidates = candidates
        if case let .hostPort(host, _) = connection.endpoint { channel.peerAddress = "\(host)" }
        channel.start()
        return channel
    }

    private init(connection: NWConnection, initiator: Bool, localStatic: SyncCrypto.KeyPair) {
        self.connection = connection
        self.initiator = initiator
        self.localStatic = localStatic
    }

    private func start() {
        connection.stateUpdateHandler = { [weak self] state in
            MainActor.assumeIsolated { self?.stateChanged(state) }
        }
        connection.start(queue: .main)
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in
            MainActor.assumeIsolated {
                guard let self, !self.isEstablished else { return }
                self.fail("handshake timed out")
            }
        }
    }

    private func stateChanged(_ state: NWConnection.State) {
        switch state {
        case .ready:
            if let local = connection.currentPath?.localEndpoint, case let .hostPort(host, _) = local {
                localAddress = "\(host)".components(separatedBy: "%").first ?? ""
            }
            if initiator {
                writeFrame(SyncProtocol.magic + Data([kind.rawValue]))
                startHandshake()
            }
            receiveNext()
        case let .failed(error):
            fail(error.localizedDescription)
        case .cancelled:
            fail("closed")
        default:
            break
        }
    }

    private func startHandshake() {
        handshake = kind == .pairing
            ? NoiseHandshake(pattern: .nn, initiator: true, prologue: Self.prologue)
            : NoiseHandshake(pattern: .kk, initiator: true, prologue: Self.prologue, localStatic: localStatic,
                             remoteStatic: peer.publicKey)
        guard let message = handshake?.writeMessage(payload: Data()) else { return fail("handshake could not start") }
        writeFrame(message)
    }

    private func receiveNext() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, isComplete, error in
            MainActor.assumeIsolated {
                guard let self, !self.closed else { return }
                if let data, !data.isEmpty { self.received(data) }
                if isComplete || error != nil {
                    self.fail(error?.localizedDescription ?? "disconnected")
                } else if !self.closed {
                    self.receiveNext()
                }
            }
        }
    }

    private func received(_ data: Data) {
        buffer += data
        guard let frames = SyncProtocol.takeFrames(&buffer) else { return fail("malformed frame") }
        for frame in frames where !closed { handle(frame) }
    }

    private func handle(_ frame: Data) {
        if isEstablished {
            guard let plaintext = receiveCipher.decrypt(ad: Data(), ciphertext: frame) else {
                return fail("frame failed authentication")
            }
            receivedSinceRekey += 1
            if receivedSinceRekey >= Self.rekeyInterval {
                receiveCipher.rekey()
                receivedSinceRekey = 0
            }
            onReceive?(self, plaintext)
            return
        }
        if initiator {
            guard handshake != nil, handshake!.readMessage(frame) != nil else { return fail("handshake rejected") }
            return finishHandshake()
        }
        if !headerSeen {
            guard frame.count == 5, frame.prefix(4) == SyncProtocol.magic, let kind = Kind(rawValue: frame[frame.startIndex + 4])
            else { return fail("not a ClipDeck connection") }
            self.kind = kind
            headerSeen = true
            return
        }
        if kind == .pairing {
            var state = NoiseHandshake(pattern: .nn, initiator: false, prologue: Self.prologue)
            guard state.readMessage(frame) != nil else { return fail("handshake rejected") }
            handshake = state
        } else {
            for candidate in candidates {
                var state = NoiseHandshake(pattern: .kk, initiator: false, prologue: Self.prologue,
                                           localStatic: localStatic, remoteStatic: candidate.publicKey)
                if state.readMessage(frame) != nil {
                    handshake = state
                    peer = candidate
                    break
                }
            }
            guard handshake != nil else { return fail("unknown device") }
        }
        guard let reply = handshake!.writeMessage(payload: Data()) else { return fail("handshake failed") }
        writeFrame(reply)
        finishHandshake()
    }

    private func finishHandshake() {
        guard let state = handshake, state.isComplete else { return fail("handshake incomplete") }
        sendCipher = state.sendCipher
        receiveCipher = state.receiveCipher
        handshakeHash = state.handshakeHash
        handshake = nil
        isEstablished = true
        onEstablished?(self)
    }

    private func writeFrame(_ payload: Data) {
        connection.send(content: SyncProtocol.frame(payload), completion: .contentProcessed { _ in })
    }

    @discardableResult
    func send(_ plaintext: Data) -> Bool {
        guard isEstablished, !closed, plaintext.count <= SyncProtocol.maxPlaintext + 1,
              let ciphertext = sendCipher.encrypt(ad: Data(), plaintext: plaintext) else { return false }
        writeFrame(ciphertext)
        sentSinceRekey += 1
        if sentSinceRekey >= Self.rekeyInterval {
            sendCipher.rekey()
            sentSinceRekey = 0
        }
        return true
    }

    func close() {
        guard !closed else { return }
        closed = true
        connection.cancel()
        onClose?(self, "closed")
    }

    private func fail(_ reason: String) {
        guard !closed else { return }
        closed = true
        connection.cancel()
        onClose?(self, reason)
    }
}
