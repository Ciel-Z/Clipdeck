import AppKit
import SwiftUI

/// The "同步设备…" panel, the same sections as windows/qml/SyncPanel.qml:
/// an incoming pairing code, this Mac, paired devices, nearby devices and
/// the pause switch.
struct SyncPanelView: View {
    @ObservedObject var sync: SyncService
    @State private var editingName = false
    @State private var nameDraft = ""
    @State private var codeDraft = ""
    @State private var hoveredDevice: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                if sync.pairing.role == .incoming { incomingPairing }
                thisDevice
                section("我的设备") { pairedDevices }
                section("附近的设备") { nearbyDevices }
                Divider()
                Toggle("暂停同步", isOn: Binding(get: { sync.isPaused }, set: { sync.setPaused($0) }))
                    .toggleStyle(.switch)
                    .controlSize(.small)
                if !sync.networkHint.isEmpty {
                    Text(sync.networkHint).font(.caption).foregroundStyle(.secondary)
                }
            }
            .padding(16)
        }
        .frame(width: 340)
        .frame(minHeight: 260, maxHeight: 560)
    }

    // MARK: Sections

    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.system(size: 11, weight: .semibold)).foregroundStyle(.secondary)
            content()
        }
    }

    private var incomingPairing: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("\(sync.pairing.hostname.isEmpty ? "一台设备" : sync.pairing.hostname) 请求配对")
                .font(.system(size: 13, weight: .semibold))
            Text(sync.pairing.address).font(.caption).foregroundStyle(.secondary)
            switch sync.pairing.stage {
            case .done:
                Label("已配对", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
            case .failed:
                Text(sync.pairing.error).foregroundStyle(.red).font(.callout)
                Button("关闭") { sync.cancelPairing() }
            default:
                Text("在对方设备上输入配对码").font(.callout)
                Text(sync.pairing.code.map(String.init).joined(separator: " "))
                    .font(.system(size: 28, weight: .semibold, design: .monospaced))
                    .textSelection(.enabled)
                Text("剩余 \(sync.pairing.attemptsLeft) 次尝试").font(.caption).foregroundStyle(.secondary)
                Button("拒绝") { sync.rejectPairing() }
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.accentColor.opacity(0.12)))
    }

    private var thisDevice: some View {
        section("本机") {
            HStack(spacing: 8) {
                Image(systemName: "laptopcomputer").frame(width: 20)
                if editingName {
                    TextField("设备名称", text: $nameDraft)
                        .textFieldStyle(.roundedBorder)
                        .onSubmit(commitName)
                    Button("完成", action: commitName)
                } else {
                    Text(sync.deviceName).lineLimit(1)
                    Button {
                        nameDraft = sync.deviceName
                        editingName = true
                    } label: {
                        Image(systemName: "pencil")
                    }
                    .buttonStyle(.borderless)
                    .help("重命名本机")
                }
                Spacer()
            }
            HStack {
                Toggle(isOn: Binding(get: { sync.isVisible }, set: { sync.setVisible($0) })) {
                    VStack(alignment: .leading, spacing: 1) {
                        Text("局域网可见")
                        Text(sync.isVisible
                             ? "识别码 \(sync.visibilityTag) · \(Self.countdown(sync.visibleSecondsLeft)) 后关闭"
                             : "开启后附近的设备可以发现并请求配对")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                .toggleStyle(.switch)
                .controlSize(.small)
            }
            Text(sync.statusText).font(.caption).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private var pairedDevices: some View {
        if sync.devices.isEmpty {
            Text("还没有配对的设备").font(.callout).foregroundStyle(.secondary)
        }
        ForEach(sync.devices) { device in
            HStack(spacing: 8) {
                Circle()
                    .fill(device.state == .offline ? Color.secondary.opacity(0.5) : Color.green)
                    .frame(width: 7, height: 7)
                VStack(alignment: .leading, spacing: 1) {
                    Text(device.address.isEmpty ? device.hostname : "\(device.hostname) · \(device.address)").lineLimit(1)
                    Text(device.state == .syncing ? "正在同步" : device.lastSeen)
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if hoveredDevice == device.id {
                    Button("立即同步") { sync.syncNow() }.buttonStyle(.borderless).disabled(sync.isPaused)
                    Button("取消配对") { sync.unpair(device.id) }.buttonStyle(.borderless).foregroundStyle(.red)
                }
            }
            .padding(.vertical, 4)
            .contentShape(Rectangle())
            .onHover { hoveredDevice = $0 ? device.id : (hoveredDevice == device.id ? nil : hoveredDevice) }
        }
    }

    @ViewBuilder
    private var nearbyDevices: some View {
        HStack {
            Button(sync.isSearching ? "正在搜索…" : "搜索附近的设备") { sync.searchNearby() }
                .disabled(sync.isSearching || !sync.isAvailable)
            if sync.isSearching { ProgressView().controlSize(.small) }
        }
        if sync.nearby.isEmpty, !sync.isSearching {
            Text("对方需在 ClipDeck 中开启“局域网可见”").font(.caption).foregroundStyle(.secondary)
        }
        ForEach(sync.nearby) { device in
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text("\(device.hostname) · \(device.address)").lineLimit(1)
                        Text("识别码 \(device.tag)").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    if !sync.pairing.isBusy, !(sync.pairing.role == .outgoing && sync.pairing.key == device.id) {
                        Button("配对") {
                            codeDraft = ""
                            sync.pair(device.id)
                        }
                        .disabled(!device.pairable)
                    }
                }
                if sync.pairing.role == .outgoing, sync.pairing.key == device.id { outgoingPairing }
            }
            .padding(.vertical, 4)
        }
    }

    @ViewBuilder
    private var outgoingPairing: some View {
        switch sync.pairing.stage {
        case .connecting:
            HStack {
                ProgressView().controlSize(.small)
                Text("正在连接…").font(.callout)
                Button("取消") { sync.cancelPairing() }.buttonStyle(.borderless)
            }
        case .code, .verifying:
            HStack {
                TextField("6 位配对码", text: $codeDraft)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.body, design: .monospaced))
                    .frame(width: 120)
                    .onChange(of: codeDraft) { _, value in
                        let digits = String(value.filter(\.isNumber).prefix(6))
                        if digits != value { codeDraft = digits }
                    }
                    .onSubmit { sync.submitPairingCode(codeDraft) }
                    .disabled(sync.pairing.stage == .verifying)
                Button("确定") { sync.submitPairingCode(codeDraft) }
                    .disabled(codeDraft.count != 6 || sync.pairing.stage == .verifying)
                Button("取消") { sync.cancelPairing() }.buttonStyle(.borderless)
            }
            .onChange(of: sync.pairing.attemptsLeft) { _, _ in codeDraft = "" }
            if !sync.pairing.error.isEmpty {
                Text("\(sync.pairing.error)，剩余 \(sync.pairing.attemptsLeft) 次").font(.caption).foregroundStyle(.red)
            }
        case .finishing:
            HStack { ProgressView().controlSize(.small); Text("正在完成配对…").font(.callout) }
        case .done:
            Label("已配对", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
        case .failed:
            HStack {
                Text(sync.pairing.error).font(.caption).foregroundStyle(.red)
                Button("关闭") { sync.cancelPairing() }.buttonStyle(.borderless)
            }
        }
    }

    private func commitName() {
        sync.setDeviceName(nameDraft)
        editingName = false
    }

    static func countdown(_ seconds: Int) -> String {
        String(format: "%d:%02d", seconds / 60, seconds % 60)
    }
}

/// A floating panel below the status item. It hides when it loses focus,
/// except while it shows a pairing code for another device.
@MainActor
final class SyncPanelController: NSObject, NSWindowDelegate {
    private let sync: SyncService
    private var panel: NSPanel?
    private var shownAt = Date.distantPast

    init(sync: SyncService) {
        self.sync = sync
    }

    var isVisible: Bool { panel?.isVisible == true }

    func show(near button: NSStatusBarButton?) {
        let panel = self.panel ?? makePanel()
        self.panel = panel
        if let button, let window = button.window {
            let frame = window.convertToScreen(button.convert(button.bounds, to: nil))
            let size = panel.frame.size
            let visible = (window.screen ?? NSScreen.main)?.visibleFrame ?? .zero
            var origin = NSPoint(x: frame.midX - size.width / 2, y: frame.minY - size.height - 6)
            origin.x = min(max(origin.x, visible.minX + 8), visible.maxX - size.width - 8)
            origin.y = max(origin.y, visible.minY + 8)
            panel.setFrameOrigin(origin)
        } else {
            panel.center()
        }
        shownAt = Date()
        NSApp.activate()
        panel.makeKeyAndOrderFront(nil)
    }

    func hide() {
        panel?.orderOut(nil)
    }

    private func makePanel() -> NSPanel {
        let host = NSHostingController(rootView: SyncPanelView(sync: sync))
        let panel = NSPanel(contentViewController: host)
        panel.styleMask = [.titled, .closable, .fullSizeContentView, .nonactivatingPanel]
        panel.title = "局域网同步"
        panel.titleVisibility = .hidden
        panel.titlebarAppearsTransparent = true
        panel.isMovableByWindowBackground = true
        panel.level = .floating
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary]
        panel.delegate = self
        panel.setContentSize(host.view.fittingSize)
        return panel
    }

    func windowDidResignKey(_ notification: Notification) {
        // A grace period for the click that opened it, and never while a code
        // is on screen for the other device.
        guard Date().timeIntervalSince(shownAt) > 0.6, sync.pairing.role != .incoming else { return }
        hide()
    }
}
