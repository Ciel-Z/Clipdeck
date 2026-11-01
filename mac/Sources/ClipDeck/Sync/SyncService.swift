import Foundation
import Network
import Security

/// Secrets for LAN sync (docs/lan-sync-design.md §9.7) in the Keychain, this
/// device only: the private key, each paired device's locate key, and the
/// public keys of devices unpaired while offline. Same contents as
/// linux/src/sync/key_store.cpp.
struct SyncKeyStore {
    static let service = "app.clipdeck.sync"
    var privateKey = Data()
    var locateKeys: [String: Data] = [:]
    var unpairedKeys: [String: Data] = [:]

    static func load() -> SyncKeyStore {
        var store = SyncKeyStore()
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: "keys",
            kSecReturnData as String: true,
        ]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data,
              let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return store }
        func keys(_ value: Any?) -> [String: Data] {
            ((value as? [String: String]) ?? [:]).compactMapValues { Data(base64Encoded: $0) }
        }
        store.privateKey = Data(base64Encoded: root["privateKey"] as? String ?? "") ?? Data()
        store.locateKeys = keys(root["locateKeys"])
        store.unpairedKeys = keys(root["unpaired"])
        return store
    }

    @discardableResult
    func save() -> Bool {
        let root: [String: Any] = [
            "privateKey": privateKey.base64EncodedString(),
            "locateKeys": locateKeys.mapValues { $0.base64EncodedString() },
            "unpaired": unpairedKeys.mapValues { $0.base64EncodedString() },
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: root) else { return false }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: "keys",
        ]
        let attributes: [String: Any] = [
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
            kSecAttrSynchronizable as String: false,
        ]
        if SecItemUpdate(query as CFDictionary, attributes as CFDictionary) == errSecSuccess { return true }
        return SecItemAdd(query.merging(attributes) { $1 } as CFDictionary, nil) == errSecSuccess
    }

    mutating func markUnpaired(_ deviceID: String, publicKey: Data?) {
        locateKeys[deviceID] = nil
        if let publicKey, !publicKey.isEmpty { unpairedKeys[deviceID] = publicKey } else { unpairedKeys[deviceID] = nil }
    }
}

/// LAN sync on macOS (docs/lan-sync-design.md), the same protocol and rules
/// as linux/src/sync/sync_service.cpp: on-demand discovery, CPace pairing over
/// Noise NN, and one Noise KK connection per paired device exchanging the
/// last 24 hours of non-file history.
@MainActor
final class SyncService: ObservableObject {
    struct DeviceRow: Identifiable, Equatable {
        enum State: Equatable { case connected, syncing, offline }
        var id: String
        var hostname: String
        var address: String
        var state: State
        var lastSeen: String
    }

    struct NearbyRow: Identifiable, Equatable {
        var id: String // "ip:port"
        var hostname: String
        var address: String
        var tag: String
        var pairable: Bool
    }

    struct PairingState: Equatable {
        enum Role: Equatable { case none, outgoing, incoming }
        enum Stage: Equatable { case connecting, code, verifying, finishing, failed, done }
        var role: Role = .none
        var stage: Stage = .connecting
        var key = ""
        var hostname = ""
        var address = ""
        var code = ""
        var attemptsLeft = 5
        var error = ""
        var isActive: Bool { role != .none }
    }

    @Published private(set) var isAvailable = false
    @Published private(set) var isVisible = false
    @Published private(set) var visibleSecondsLeft = 0
    @Published private(set) var isPaused = UserDefaults.standard.bool(forKey: "sync.paused")
    @Published private(set) var isSyncing = false
    @Published private(set) var deviceName = ""
    @Published private(set) var visibilityTag = ""
    @Published private(set) var devices: [DeviceRow] = []
    @Published private(set) var nearby: [NearbyRow] = []
    @Published private(set) var isSearching = false
    @Published private(set) var pairing = PairingState()
    @Published private(set) var networkHint = ""

    /// Items from another device were written to the history database.
    var onHistoryChanged: (() -> Void)?
    /// A device asked to pair: show the sync panel.
    var onPairingRequested: (() -> Void)?

    nonisolated static let pairingChannelID = Data("ClipDeck pairing v1".utf8)
    private static let pingInterval: TimeInterval = 15
    private static let idleTimeout: TimeInterval = 45
    private static let batchSize = 100

    private let repository: HistoryRepository
    private let classifier: ClassificationService?
    private var keyStore = SyncKeyStore()
    private var identity = SyncCrypto.KeyPair(publicKey: Data(), privateKey: Data())
    private(set) var deviceID = ""
    private var listener: NWListener?
    private let discovery = SyncDiscovery()

    private final class Peer {
        var stored: SyncPeerRecord
        var channel: SyncChannel?
        var attempts: [SyncChannel] = []
        var failures = 0
        var reconnect: DispatchWorkItem?
        var pushedCursor: UInt64 = 0
        var pushInFlight = false
        var receiving = false
        var receivedChanges = false
        var lastActivity = Date()
        var lastLocate = Date.distantPast
        var pendingBlobs: [Data: SyncProtocol.WireItem] = [:]
        var blobData: [Data: Data] = [:]
        init(_ stored: SyncPeerRecord) { self.stored = stored }
        var isConnected: Bool { channel?.isEstablished == true }
    }

    private struct Nearby {
        var key: String
        var address: String
        var port: UInt16
        var tag: String
        var hostname = ""
        var deviceID = ""
        var infoChannel: SyncChannel?
    }

    private var peers: [String: Peer] = [:]
    private var nearbyDevices: [Nearby] = []
    private var pairingChannel: SyncChannel?
    private var pairingPeerID = ""
    private var pairingPeerMessage = Data()
    private var pairingCPace: CPaceSession?
    private var pairingISK = Data()
    private var pairingExpiry: DispatchWorkItem?
    private var visibleUntil = Date.distantPast
    private var tagData = Data()
    private var tickTimer: Timer?
    private var pushWork: DispatchWorkItem?
    private var searchWork: DispatchWorkItem?
    private var syncingOff: DispatchWorkItem?
    private var lastTick = Date()
    private var tickCount = 0
    private var interfaceSignature: [String]?

    init(repository: HistoryRepository, classifier: ClassificationService?) {
        self.repository = repository
        self.classifier = classifier
        discovery.onReply = { [weak self] host, port, tag in self?.replied(host: host, port: port, tag: tag) }
        discovery.onLocate = { [weak self] host, port, token in self?.located(host: host, port: port, token: token) }
    }

    var listeningPort: UInt16 { listener?.port?.rawValue ?? 0 }

    var statusText: String {
        if !isAvailable { return "同步不可用" }
        if isPaused { return "已暂停同步" }
        let connected = peers.values.filter(\.isConnected).count
        if connected > 0 { return "已连接 \(connected) 台设备" }
        return peers.isEmpty ? "未配对设备" : "设备均离线"
    }

    var hasPairedDevices: Bool { !peers.isEmpty }

    // MARK: Start

    func start() {
        guard SyncCrypto.isAvailable, !isAvailable else { return }
        keyStore = SyncKeyStore.load()
        if keyStore.privateKey.count != 32 {
            keyStore.privateKey = SyncCrypto.generateKeyPair().privateKey
            keyStore.save()
        }
        identity = SyncCrypto.KeyPair(publicKey: SyncCrypto.publicKey(for: keyStore.privateKey), privateKey: keyStore.privateKey)
        deviceID = SyncCrypto.deviceID(for: identity.publicKey)
        deviceName = UserDefaults.standard.string(forKey: "sync.deviceName")
            ?? Host.current().localizedName ?? ProcessInfo.processInfo.hostName
        for stored in (try? repository.syncPeers()) ?? [] { peers[stored.deviceID] = Peer(stored) }
        guard startListener() else { return }
        discovery.start()
        isAvailable = true
        lastTick = Date()
        tickTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        checkNetwork()
        if !isPaused { peers.values.forEach(connect) }
        publish()
    }

    private func startListener() -> Bool {
        func make(_ port: NWEndpoint.Port) -> NWListener? {
            let parameters = NWParameters.tcp
            (parameters.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options)?.version = .v4
            parameters.allowLocalEndpointReuse = true
            return try? NWListener(using: parameters, on: port)
        }
        guard let listener = make(47821) ?? make(.any) else { return false }
        listener.newConnectionHandler = { [weak self] connection in
            MainActor.assumeIsolated { self?.accept(connection) }
        }
        listener.stateUpdateHandler = { [weak self] state in
            MainActor.assumeIsolated {
                if case .ready = state { self?.publish() }
            }
        }
        listener.start(queue: .main)
        self.listener = listener
        return true
    }

    // MARK: Settings

    func setVisible(_ visible: Bool) {
        guard isAvailable, visible != isVisible else { return }
        isVisible = visible
        if visible {
            tagData = SyncProtocol.randomTag()
            visibleUntil = Date().addingTimeInterval(TimeInterval(SyncProtocol.visibilitySeconds))
            discovery.start()
        }
        discovery.setVisible(visible, tag: tagData, tcpPort: listeningPort)
        publish()
    }

    func setPaused(_ paused: Bool) {
        guard paused != isPaused else { return }
        isPaused = paused
        UserDefaults.standard.set(paused, forKey: "sync.paused")
        for peer in peers.values {
            if paused {
                peer.channel?.close()
                peer.reconnect?.cancel()
            } else {
                peer.failures = 0
                connect(peer)
            }
        }
        publish()
    }

    func setDeviceName(_ name: String) {
        let trimmed = String(name.trimmingCharacters(in: .whitespacesAndNewlines).prefix(64))
        guard !trimmed.isEmpty, trimmed != deviceName else { return }
        deviceName = trimmed
        UserDefaults.standard.set(trimmed, forKey: "sync.deviceName")
        for peer in peers.values where peer.isConnected {
            send(["type": "hello", "deviceId": deviceID, "hostname": deviceName, "port": Int(listeningPort)], on: peer.channel)
        }
        publish()
    }

    func syncNow() {
        guard !isPaused else { return }
        for peer in peers.values {
            if peer.isConnected {
                push(peer)
            } else {
                peer.failures = 0
                if weInitiate(peer.stored.deviceID) { connect(peer) } else { sendLocate(peer) }
            }
        }
    }

    /// Call after any local history change; changes go out in about 500 ms.
    func notifyLocalChange() {
        guard !isPaused else { return }
        pushWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated { self?.peers.values.forEach { self?.push($0) } }
        }
        pushWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: work)
    }

    // MARK: Published state

    private func publish() {
        let now = Date()
        visibleSecondsLeft = isVisible ? max(0, Int(visibleUntil.timeIntervalSince(now).rounded(.up))) : 0
        visibilityTag = isVisible ? String(decoding: tagData, as: UTF8.self) : ""
        devices = peers.values.map { peer in
            let state: DeviceRow.State = !peer.isConnected ? .offline
                : (peer.pushInFlight || peer.receiving) ? .syncing : .connected
            let address = peer.stored.addresses.first.flatMap { $0.split(separator: ":").first.map(String.init) } ?? ""
            return DeviceRow(id: peer.stored.deviceID, hostname: peer.stored.hostname, address: address, state: state,
                             lastSeen: peer.isConnected ? "已连接" : Self.relativeTime(peer.stored.lastSeenMilliseconds))
        }.sorted { $0.hostname < $1.hostname }
        nearby = nearbyDevices.filter { peers[$0.deviceID] == nil }.map {
            NearbyRow(id: $0.key, hostname: $0.hostname.isEmpty ? "ClipDeck" : $0.hostname, address: $0.address,
                      tag: $0.tag, pairable: peers.count < SyncProtocol.maxPeers)
        }
        objectWillChange.send()
    }

    private static func relativeTime(_ milliseconds: Int64) -> String {
        guard milliseconds > 0 else { return "从未连接" }
        let seconds = Int64(Date().timeIntervalSince1970) - milliseconds / 1_000
        if seconds < 60 { return "刚刚" }
        if seconds < 3_600 { return "\(seconds / 60) 分钟前" }
        if seconds < 86_400 { return "\(seconds / 3_600) 小时前" }
        return "\(seconds / 86_400) 天前"
    }

    private static func nowMilliseconds() -> Int64 { Int64(Date().timeIntervalSince1970 * 1_000) }

    // MARK: Timers and network

    private func tick() {
        let now = Date()
        let elapsed = now.timeIntervalSince(lastTick)
        lastTick = now
        tickCount += 1
        if isVisible {
            if now >= visibleUntil, pairing.role != .incoming { setVisible(false) } else { publish() }
        }
        if tickCount % 10 == 0 || elapsed > 5 {
            if elapsed > 5 { peers.values.forEach { $0.failures = 0 } } // woke from sleep
            checkNetwork()
        }
        for peer in peers.values {
            guard peer.isConnected else {
                if !isPaused, !weInitiate(peer.stored.deviceID), now.timeIntervalSince(peer.lastLocate) > 60 { sendLocate(peer) }
                continue
            }
            let idle = now.timeIntervalSince(peer.lastActivity)
            if idle > Self.idleTimeout {
                peer.channel?.close()
            } else if idle > Self.pingInterval, tickCount % 5 == 0 {
                send(["type": "ping"], on: peer.channel)
            }
        }
    }

    private func checkNetwork() {
        let interfaces = SyncInterfaces.eligible()
        let signature = interfaces.map(\.addressText).sorted()
        let hint = interfaces.isEmpty ? "未连接到局域网" : ""
        if hint != networkHint { networkHint = hint }
        defer { interfaceSignature = signature }
        guard let previous = interfaceSignature, previous != signature else { return }
        // A new or lost network: rejoin the multicast group and reconnect now.
        discovery.stop()
        discovery.start()
        discovery.setVisible(isVisible, tag: tagData, tcpPort: listeningPort)
        guard !isPaused else { return }
        for peer in peers.values where !peer.isConnected {
            peer.failures = 0
            connect(peer)
        }
    }

    // MARK: Connections

    private func weInitiate(_ peerID: String) -> Bool { deviceID < peerID }

    private var syncCandidates: [SyncChannel.Candidate] {
        peers.values.map { SyncChannel.Candidate(deviceID: $0.stored.deviceID, publicKey: $0.stored.publicKey) }
            + keyStore.unpairedKeys.map { SyncChannel.Candidate(deviceID: $0.key, publicKey: $0.value) }
    }

    private func accept(_ connection: NWConnection) {
        let channel = SyncChannel.accept(connection, localStatic: identity, candidates: syncCandidates)
        channel.onEstablished = { [weak self] channel in
            guard let self, channel.kind == .sync else { return }
            // Only the LAN interfaces sync uses (no VPN or public address).
            let allowed = SyncInterfaces.eligible().contains { $0.addressText == channel.localAddress }
            guard allowed else { return channel.close() }
            let id = channel.peer.deviceID
            if self.keyStore.unpairedKeys[id] != nil {
                self.send(["type": "unpair"], on: channel)
                self.keyStore.unpairedKeys[id] = nil
                self.keyStore.save()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { MainActor.assumeIsolated { channel.close() } }
                return
            }
            guard let peer = self.peers[id], !self.isPaused else { return channel.close() }
            self.attach(peer, channel)
        }
        wire(channel)
    }

    private func wire(_ channel: SyncChannel) {
        channel.onReceive = { [weak self] channel, plaintext in self?.received(plaintext, on: channel) }
        channel.onClose = { [weak self] channel, _ in self?.closed(channel) }
    }

    private func connect(_ peer: Peer) {
        guard !isPaused, isAvailable, weInitiate(peer.stored.deviceID), !peer.isConnected, peer.attempts.isEmpty else { return }
        let addresses = SyncInterfaces.rank(peer.stored.addresses, interfaces: SyncInterfaces.eligible())
        guard !addresses.isEmpty else {
            sendLocate(peer)
            scheduleReconnect(peer)
            return
        }
        let id = peer.stored.deviceID
        // Staggered by 250 ms; the first address that answers wins.
        for (index, entry) in addresses.enumerated() {
            let parts = entry.split(separator: ":")
            guard parts.count == 2, let port = UInt16(parts[1]) else { continue }
            let host = String(parts[0])
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(index * 250)) { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, let peer = self.peers[id], !self.isPaused, !peer.isConnected else { return }
                    let channel = SyncChannel.connect(
                        host: host, port: port, kind: .sync, localStatic: self.identity,
                        peer: SyncChannel.Candidate(deviceID: id, publicKey: peer.stored.publicKey))
                    peer.attempts.append(channel)
                    channel.onEstablished = { [weak self] channel in
                        guard let self, let peer = self.peers[id] else { return channel.close() }
                        peer.attempts.filter { $0 !== channel }.forEach { $0.close() }
                        peer.attempts.removeAll()
                        self.attach(peer, channel)
                    }
                    self.wire(channel)
                }
            }
        }
    }

    private func scheduleReconnect(_ peer: Peer) {
        guard !isPaused, weInitiate(peer.stored.deviceID) else { return }
        peer.reconnect?.cancel()
        // 1, 2, 4 … 60 s, ±20 %.
        let base = min(60.0, pow(2.0, Double(min(peer.failures, 6))))
        let delay = base * Double.random(in: 0.8 ... 1.2)
        let id = peer.stored.deviceID
        let work = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated {
                guard let self, let peer = self.peers[id] else { return }
                self.connect(peer)
            }
        }
        peer.reconnect = work
        peer.failures += 1
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    func connectToPeerNow(_ id: String) {
        guard let peer = peers[id] else { return }
        peer.failures = 0
        connect(peer)
    }

    private func attach(_ peer: Peer, _ channel: SyncChannel) {
        if let current = peer.channel, current !== channel, current.isEstablished {
            // Both sides connected at once: keep the one the smaller ID opened.
            guard channel.initiator == weInitiate(peer.stored.deviceID) else { return channel.close() }
            current.close()
        }
        peer.channel = channel
        peer.failures = 0
        peer.pushedCursor = peer.stored.sentCursor
        peer.pushInFlight = false
        peer.receiving = false
        peer.lastActivity = Date()
        peer.stored.lastSeenMilliseconds = Self.nowMilliseconds()
        peer.reconnect?.cancel()
        send(["type": "hello", "version": SyncProtocol.protocolVersion, "deviceId": deviceID,
              "hostname": deviceName, "port": Int(listeningPort)], on: channel)
        save(peer)
        publish()
        push(peer)
    }

    private func closed(_ channel: SyncChannel) {
        if pairingChannel === channel, pairing.stage != .done, pairing.stage != .failed {
            failPairing("连接已断开")
        }
        for index in nearbyDevices.indices where nearbyDevices[index].infoChannel === channel {
            nearbyDevices[index].infoChannel = nil
        }
        for peer in peers.values {
            let wasAttempt = peer.attempts.contains { $0 === channel }
            let wasChannel = peer.channel === channel
            guard wasAttempt || wasChannel else { continue }
            peer.attempts.removeAll { $0 === channel }
            if wasChannel {
                peer.channel = nil
                peer.pushInFlight = false
                peer.receiving = false
                peer.pendingBlobs.removeAll()
                peer.blobData.removeAll()
                peer.stored.lastSeenMilliseconds = Self.nowMilliseconds()
                save(peer)
                updateSyncing()
                publish()
            }
            if peer.attempts.isEmpty, peer.channel == nil, weInitiate(peer.stored.deviceID) {
                if peer.failures >= 2 { sendLocate(peer) }
                scheduleReconnect(peer)
            }
        }
    }

    private func recordAddress(_ peer: Peer, address: String, port: Int) {
        guard !address.isEmpty, port > 0 else { return }
        let entry = "\(address):\(port)"
        peer.stored.addresses.removeAll { $0 == entry }
        peer.stored.addresses.insert(entry, at: 0)
        peer.stored.addresses = Array(peer.stored.addresses.prefix(4))
    }

    private func save(_ peer: Peer) { try? repository.saveSyncPeer(peer.stored) }

    private func send(_ object: [String: Any], on channel: SyncChannel?) {
        channel?.send(SyncProtocol.jsonFrame(object))
    }

    private func sendLocate(_ peer: Peer, target: String? = nil) {
        guard let key = keyStore.locateKeys[peer.stored.deviceID] else { return }
        peer.lastLocate = Date()
        let token = SyncProtocol.locateToken(key: key, unixSeconds: Int64(Date().timeIntervalSince1970))
        discovery.locate(token: token, tcpPort: listeningPort, target: target)
    }

    private func located(host: String, port: UInt16, token: Data) {
        let seconds = Int64(Date().timeIntervalSince1970)
        for peer in peers.values {
            guard let key = keyStore.locateKeys[peer.stored.deviceID] else { continue }
            let matches = (-1 ... 1).contains { SyncProtocol.locateToken(key: key, unixSeconds: seconds + Int64($0) * 30) == token }
            guard matches else { continue }
            recordAddress(peer, address: host, port: Int(port))
            save(peer)
            guard !isPaused, !peer.isConnected else { return }
            if weInitiate(peer.stored.deviceID) {
                peer.failures = 0
                connect(peer)
            } else if Date().timeIntervalSince(peer.lastLocate) > 2 {
                sendLocate(peer, target: host) // so it learns where we are and connects
            }
            return
        }
    }

    // MARK: Sync protocol

    private func received(_ plaintext: Data, on channel: SyncChannel) {
        if channel.kind == .pairing {
            guard let message = SyncProtocol.parseJSON(plaintext) else { return channel.close() }
            handlePairing(message, on: channel)
            return
        }
        guard let peer = peers.values.first(where: { $0.channel === channel }) else { return }
        peer.lastActivity = Date()
        if let message = SyncProtocol.parseJSON(plaintext) {
            handleSync(message, from: peer)
        } else if let chunk = SyncProtocol.parseBlob(plaintext) {
            handleBlob(chunk, from: peer)
        } else {
            channel.close()
        }
    }

    private func handleSync(_ message: [String: Any], from peer: Peer) {
        switch message["type"] as? String {
        case "hello":
            if let hostname = message["hostname"] as? String, !hostname.isEmpty {
                peer.stored.hostname = String(hostname.prefix(64))
            }
            if let channel = peer.channel { recordAddress(peer, address: channel.peerAddress, port: message["port"] as? Int ?? 0) }
            save(peer)
            publish()
        case "item":
            if !peer.receiving {
                peer.receiving = true
                peer.receivedChanges = false
                updateSyncing()
                publish()
            }
            guard let wire = SyncProtocol.decodeItem(message) else { return }
            if wire.blobID.isEmpty {
                importWire(wire, from: peer)
            } else if wire.bodySize <= 64 * 1_024 * 1_024 {
                peer.pendingBlobs[wire.blobID] = wire
                peer.blobData[wire.blobID] = Data()
            }
        case "batch_end":
            if peer.receivedChanges { onHistoryChanged?() }
            peer.receiving = false
            peer.receivedChanges = false
            send(["type": "ack", "hlc": message["hlc"] as? String ?? "0"], on: peer.channel)
            updateSyncing()
            publish()
        case "ack":
            let hlc = UInt64(message["hlc"] as? String ?? "") ?? 0
            peer.stored.sentCursor = max(peer.stored.sentCursor, hlc)
            peer.pushInFlight = false
            save(peer)
            push(peer) // more, if the batch was full
            updateSyncing()
            publish()
        case "ping":
            send(["type": "pong"], on: peer.channel)
        case "unpair":
            let id = peer.stored.deviceID
            try? repository.removeSyncPeer(deviceID: id)
            keyStore.markUnpaired(id, publicKey: nil)
            keyStore.save()
            peer.reconnect?.cancel()
            peers[id] = nil
            peer.channel?.close()
            publish()
        default:
            break
        }
    }

    private func handleBlob(_ chunk: SyncProtocol.BlobChunk, from peer: Peer) {
        guard var wire = peer.pendingBlobs[chunk.blobID], var data = peer.blobData[chunk.blobID] else { return }
        guard Int(chunk.offset) == data.count, Int64(data.count + chunk.data.count) <= wire.bodySize else {
            peer.pendingBlobs[chunk.blobID] = nil
            peer.blobData[chunk.blobID] = nil
            return
        }
        data += chunk.data
        guard Int64(data.count) == wire.bodySize else {
            peer.blobData[chunk.blobID] = data
            return
        }
        peer.pendingBlobs[chunk.blobID] = nil
        peer.blobData[chunk.blobID] = nil
        wire.body = data
        importWire(wire, from: peer)
    }

    private func importWire(_ incoming: SyncProtocol.WireItem, from peer: Peer) {
        var wire = incoming
        if wire.origin.isEmpty { wire.origin = peer.stored.deviceID }
        // Images over this device's own limit are not kept (§7).
        if wire.isImage {
            guard AppPreferences.recordsImages, wire.body.count <= AppPreferences.maximumImageByteCount else { return }
        }
        guard let result = try? repository.importRemote(wire) else { return }
        switch result {
        case let .inserted(id), let .updated(id):
            peer.receivedChanges = true
            if !wire.isImage, let item = try? repository.item(databaseID: id) { _ = try? classifier?.classify(item) }
        case .merged:
            peer.receivedChanges = true
        case .skipped, .failed:
            break
        }
    }

    private func push(_ peer: Peer) {
        guard !isPaused, peer.isConnected, !peer.pushInFlight, let channel = peer.channel else { return }
        guard let changes = try? repository.syncChanges(
            after: peer.pushedCursor, windowMilliseconds: SyncProtocol.syncWindowMilliseconds, limit: Self.batchSize),
            !changes.isEmpty else { return }
        peer.pushInFlight = true
        updateSyncing()
        var last = peer.pushedCursor
        for var wire in changes {
            if wire.origin.isEmpty { wire.origin = deviceID }
            send(SyncProtocol.encode(wire), on: channel)
            if !wire.blobID.isEmpty {
                let chunkSize = SyncProtocol.maxPlaintext - 20
                var offset = 0
                while offset < wire.body.count {
                    let end = min(offset + chunkSize, wire.body.count)
                    channel.send(SyncProtocol.blobFrame(blobID: wire.blobID, offset: UInt32(offset),
                                                        chunk: wire.body.subdata(in: offset ..< end)))
                    offset = end
                }
            }
            last = max(last, wire.hlc)
        }
        peer.pushedCursor = last
        send(["type": "batch_end", "hlc": String(last)], on: channel)
        publish()
    }

    private func updateSyncing() {
        let busy = peers.values.contains { $0.pushInFlight || $0.receiving }
        if busy {
            syncingOff?.cancel()
            syncingOff = nil
            if !isSyncing { isSyncing = true }
        } else if isSyncing, syncingOff == nil {
            // The Deck animation shows for at least 0.6 s.
            let work = DispatchWorkItem { [weak self] in
                MainActor.assumeIsolated {
                    self?.isSyncing = false
                    self?.syncingOff = nil
                }
            }
            syncingOff = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6, execute: work)
        }
    }

    // MARK: Discovery

    func searchNearby() {
        guard isAvailable else { return }
        nearbyDevices.removeAll { $0.infoChannel == nil }
        discovery.query()
        isSearching = true
        searchWork?.cancel()
        let work = DispatchWorkItem { [weak self] in MainActor.assumeIsolated { self?.isSearching = false } }
        searchWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 5, execute: work)
        publish()
    }

    private func replied(host: String, port: UInt16, tag: Data) {
        let key = "\(host):\(port)"
        guard !nearbyDevices.contains(where: { $0.key == key }) else { return }
        // The host name travels over an encrypted channel, never in UDP (§4).
        let channel = SyncChannel.connect(host: host, port: port, kind: .pairing, localStatic: identity,
                                          peer: SyncChannel.Candidate(deviceID: "", publicKey: Data()))
        channel.onEstablished = { [weak self] channel in self?.send(["type": "info_request"], on: channel) }
        wire(channel)
        nearbyDevices.append(Nearby(key: key, address: host, port: port, tag: String(decoding: tag, as: UTF8.self),
                                    infoChannel: channel))
        publish()
    }

    // MARK: Pairing

    func pair(_ key: String) {
        guard let device = nearbyDevices.first(where: { $0.key == key }) else { return }
        pair(host: device.address, port: device.port)
        pairing.key = key
        pairing.hostname = device.hostname
    }

    func pair(host: String, port: UInt16) {
        guard isAvailable, !pairing.isActive, peers.count < SyncProtocol.maxPeers else { return }
        clearPairing()
        pairing.role = .outgoing
        pairing.stage = .connecting
        pairing.address = host
        let channel = SyncChannel.connect(host: host, port: port, kind: .pairing, localStatic: identity,
                                          peer: SyncChannel.Candidate(deviceID: "", publicKey: Data()))
        channel.onEstablished = { [weak self] channel in
            guard let self else { return }
            self.send(["type": "pair_request", "deviceId": self.deviceID, "hostname": self.deviceName], on: channel)
        }
        wire(channel)
        pairingChannel = channel
    }

    private func handlePairing(_ message: [String: Any], on channel: SyncChannel) {
        let type = message["type"] as? String ?? ""
        if type == "info_request" {
            if isVisible { send(["type": "info", "deviceId": deviceID, "hostname": deviceName], on: channel) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { MainActor.assumeIsolated { channel.close() } }
            return
        }
        if type == "info" {
            for index in nearbyDevices.indices where nearbyDevices[index].infoChannel === channel {
                nearbyDevices[index].hostname = String((message["hostname"] as? String ?? "").prefix(64))
                nearbyDevices[index].deviceID = message["deviceId"] as? String ?? ""
                if let peer = peers[nearbyDevices[index].deviceID] {
                    recordAddress(peer, address: nearbyDevices[index].address, port: Int(nearbyDevices[index].port))
                    save(peer)
                }
            }
            channel.close()
            publish()
            return
        }
        if type == "pair_request" { return startIncomingPairing(message, on: channel) }
        guard pairingChannel === channel else { return channel.close() }

        if pairing.role == .outgoing {
            switch type {
            case "pair_challenge", "pair_retry":
                if let id = message["deviceId"] as? String { pairingPeerID = id }
                if let hostname = message["hostname"] as? String, !hostname.isEmpty { pairing.hostname = String(hostname.prefix(64)) }
                pairingPeerMessage = Data(base64Encoded: message["cpace"] as? String ?? "") ?? Data()
                pairing.attemptsLeft = message["attemptsLeft"] as? Int ?? pairing.attemptsLeft
                pairing.error = type == "pair_retry" ? "配对码不正确" : ""
                pairing.stage = .code
            case "pair_accept":
                let publicKey = Data(base64Encoded: message["publicKey"] as? String ?? "") ?? Data()
                let confirm = Data(base64Encoded: message["confirm"] as? String ?? "") ?? Data()
                let mac = Data(base64Encoded: message["mac"] as? String ?? "") ?? Data()
                guard !pairingISK.isEmpty,
                      CPaceSession.verifyConfirmation(isk: pairingISK, fromInitiator: false, tag: confirm),
                      Self.publicKeyMAC(isk: pairingISK, publicKey: publicKey) == mac,
                      SyncCrypto.deviceID(for: publicKey) == pairingPeerID else {
                    return failPairing("对方的身份校验失败")
                }
                send(["type": "pair_finish", "publicKey": identity.publicKey.base64EncodedString(),
                      "mac": Self.publicKeyMAC(isk: pairingISK, publicKey: identity.publicKey).base64EncodedString(),
                      "port": Int(listeningPort)], on: channel)
                finishPairing(id: pairingPeerID, hostname: pairing.hostname, publicKey: publicKey,
                              address: pairing.address, port: message["port"] as? Int ?? 0)
            case "pair_reject":
                let reasons = ["attempts": "配对码错误次数过多", "full": "对方的已配对设备已满",
                               "busy": "对方正在与其他设备配对", "expired": "配对码已过期"]
                failPairing(reasons[message["reason"] as? String ?? ""] ?? "对方拒绝了配对")
            default:
                break
            }
            return
        }

        // Incoming: the initiator's answer, then its public key.
        if type == "pair_response", pairing.stage == .code {
            let peerMessage = Data(base64Encoded: message["cpace"] as? String ?? "") ?? Data()
            let confirm = Data(base64Encoded: message["confirm"] as? String ?? "") ?? Data()
            if let isk = pairingCPace?.sharedKey(peerMessage: peerMessage, peerAssociatedData: Data(pairingPeerID.utf8)),
               CPaceSession.verifyConfirmation(isk: isk, fromInitiator: true, tag: confirm) {
                pairingISK = isk
                pairing.stage = .finishing
                send(["type": "pair_accept",
                      "confirm": CPaceSession.confirmation(isk: isk, fromInitiator: false).base64EncodedString(),
                      "publicKey": identity.publicKey.base64EncodedString(),
                      "mac": Self.publicKeyMAC(isk: isk, publicKey: identity.publicKey).base64EncodedString(),
                      "port": Int(listeningPort)], on: channel)
                return
            }
            pairing.attemptsLeft -= 1
            if pairing.attemptsLeft <= 0 {
                send(["type": "pair_reject", "reason": "attempts"], on: channel)
                return failPairing("配对码错误次数过多")
            }
            sendChallenge() // a fresh CPace exchange for the next attempt
            return
        }
        if type == "pair_finish", pairing.stage == .finishing {
            let publicKey = Data(base64Encoded: message["publicKey"] as? String ?? "") ?? Data()
            let mac = Data(base64Encoded: message["mac"] as? String ?? "") ?? Data()
            guard Self.publicKeyMAC(isk: pairingISK, publicKey: publicKey) == mac,
                  SyncCrypto.deviceID(for: publicKey) == pairingPeerID else { return failPairing("对方的身份校验失败") }
            finishPairing(id: pairingPeerID, hostname: pairing.hostname, publicKey: publicKey,
                          address: channel.peerAddress, port: message["port"] as? Int ?? 0)
        }
    }

    private static func publicKeyMAC(isk: Data, publicKey: Data) -> Data {
        SyncCrypto.hmacBLAKE2b(key: isk, data: Data("ClipDeck public key".utf8) + publicKey)
    }

    private func startIncomingPairing(_ request: [String: Any], on channel: SyncChannel) {
        func reject(_ reason: String) {
            send(["type": "pair_reject", "reason": reason], on: channel)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { MainActor.assumeIsolated { channel.close() } }
        }
        guard isVisible else { return reject("rejected") }
        guard !pairing.isActive else { return reject("busy") }
        guard peers.count < SyncProtocol.maxPeers else { return reject("full") }
        let peerID = request["deviceId"] as? String ?? ""
        guard peerID.count == 32, peerID != deviceID else { return reject("rejected") }
        clearPairing()
        pairing.role = .incoming
        pairing.hostname = String((request["hostname"] as? String ?? "").prefix(64))
        pairing.address = channel.peerAddress
        pairing.code = String(format: "%06d", Int.random(in: 0 ..< 1_000_000))
        pairing.attemptsLeft = 5
        pairingChannel = channel
        pairingPeerID = peerID
        let work = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated {
                guard let self, self.pairing.role == .incoming, self.pairing.stage != .done else { return }
                self.send(["type": "pair_reject", "reason": "expired"], on: self.pairingChannel)
                self.failPairing("配对码已过期")
            }
        }
        pairingExpiry = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 120, execute: work)
        visibleUntil = max(visibleUntil, Date().addingTimeInterval(120))
        sendChallenge()
        onPairingRequested?()
    }

    private func sendChallenge() {
        guard let channel = pairingChannel else { return }
        let session = CPaceSession(initiator: false, password: Data(pairing.code.utf8), channelID: Self.pairingChannelID,
                                   sid: channel.handshakeHash, associatedData: Data(deviceID.utf8))
        pairingCPace = session
        pairing.stage = .code
        send(["type": pairing.attemptsLeft < 5 ? "pair_retry" : "pair_challenge", "deviceId": deviceID,
              "hostname": deviceName, "cpace": session.message.base64EncodedString(),
              "attemptsLeft": pairing.attemptsLeft], on: channel)
    }

    func submitPairingCode(_ code: String) {
        guard pairing.role == .outgoing, pairing.stage == .code, let channel = pairingChannel else { return }
        let digits = code.trimmingCharacters(in: .whitespaces)
        guard digits.count == 6 else { return }
        let session = CPaceSession(initiator: true, password: Data(digits.utf8), channelID: Self.pairingChannelID,
                                   sid: channel.handshakeHash, associatedData: Data(deviceID.utf8))
        guard let isk = session.sharedKey(peerMessage: pairingPeerMessage, peerAssociatedData: Data(pairingPeerID.utf8))
        else { return failPairing("配对数据无效") }
        pairingISK = isk
        pairing.stage = .verifying
        pairing.error = ""
        send(["type": "pair_response", "cpace": session.message.base64EncodedString(),
              "confirm": CPaceSession.confirmation(isk: isk, fromInitiator: true).base64EncodedString()], on: channel)
    }

    private func finishPairing(id: String, hostname: String, publicKey: Data, address: String, port: Int) {
        let peer = peers[id] ?? Peer(SyncPeerRecord(deviceID: id, hostname: hostname, publicKey: publicKey))
        peer.stored.hostname = hostname.isEmpty ? "ClipDeck" : hostname
        peer.stored.publicKey = publicKey
        peer.stored.sentCursor = 0
        peer.stored.lastSeenMilliseconds = Self.nowMilliseconds()
        recordAddress(peer, address: address, port: port)
        peers[id] = peer
        keyStore.unpairedKeys[id] = nil
        keyStore.locateKeys[id] = SyncCrypto.hmacBLAKE2b(key: pairingISK, data: Data("ClipDeck locate key".utf8)).prefix(32)
        keyStore.save()
        save(peer)
        pairing.stage = .done
        let channel = pairingChannel
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { MainActor.assumeIsolated { channel?.close() } }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
            MainActor.assumeIsolated {
                guard let self, self.pairing.stage == .done else { return }
                self.clearPairing()
            }
        }
        publish()
        // The smaller device ID opens the sync connection.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in
            MainActor.assumeIsolated { self?.connectToPeerNow(id) }
        }
    }

    private func failPairing(_ error: String) {
        guard pairing.isActive else { return }
        pairing.stage = .failed
        pairing.error = error
        pairingISK = Data()
        pairingExpiry?.cancel()
        let channel = pairingChannel
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { MainActor.assumeIsolated { channel?.close() } }
    }

    func cancelPairing() {
        pairingChannel?.close()
        clearPairing()
    }

    func rejectPairing() {
        guard pairing.role == .incoming else { return }
        send(["type": "pair_reject", "reason": "rejected"], on: pairingChannel)
        let channel = pairingChannel
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { MainActor.assumeIsolated { channel?.close() } }
        clearPairing()
    }

    private func clearPairing() {
        pairingExpiry?.cancel()
        pairingExpiry = nil
        pairing = PairingState()
        pairingChannel = nil
        pairingPeerID = ""
        pairingPeerMessage = Data()
        pairingCPace = nil
        pairingISK = Data()
    }

    func unpair(_ id: String) {
        guard let peer = peers[id] else { return }
        if let channel = peer.channel, channel.isEstablished {
            send(["type": "unpair"], on: channel)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { MainActor.assumeIsolated { channel.close() } }
            keyStore.markUnpaired(id, publicKey: nil)
        } else {
            // Offline: remember its key so it can be told when it reconnects.
            keyStore.markUnpaired(id, publicKey: peer.stored.publicKey)
        }
        keyStore.save()
        try? repository.removeSyncPeer(deviceID: id)
        peer.attempts.forEach { $0.close() }
        peer.reconnect?.cancel()
        peers[id] = nil
        publish()
    }
}
