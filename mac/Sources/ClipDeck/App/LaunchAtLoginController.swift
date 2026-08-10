import ServiceManagement

@MainActor
protocol LaunchAtLoginService: AnyObject {
    var status: SMAppService.Status { get }

    func register() throws
    func unregister() throws
}

extension SMAppService: LaunchAtLoginService {}

@MainActor
final class LaunchAtLoginController: ObservableObject {
    @Published private(set) var status: SMAppService.Status
    @Published private(set) var errorMessage: String?
    @Published private(set) var isUpdating = false

    private let service: any LaunchAtLoginService

    init(service: any LaunchAtLoginService = SMAppService.mainApp) {
        self.service = service
        status = service.status
    }

    var isOn: Bool {
        status == .enabled || status == .requiresApproval
    }

    var requiresApproval: Bool {
        status == .requiresApproval
    }

    var statusMessage: String? {
        if let errorMessage {
            return "设置失败：\(errorMessage)"
        }
        switch status {
        case .enabled:
            return "登录 Mac 后自动启动 ClipDeck。"
        case .requiresApproval:
            return "已添加到登录项，请在系统设置中允许 ClipDeck 后生效。"
        case .notFound:
            return "当前应用无法注册为登录项，请从有效的 ClipDeck.app 中运行。"
        case .notRegistered:
            return nil
        @unknown default:
            return "无法读取当前的开机自启状态。"
        }
    }

    func refresh() {
        status = service.status
    }

    func setEnabled(_ enabled: Bool) {
        guard !isUpdating else { return }
        errorMessage = nil
        isUpdating = true
        defer {
            status = service.status
            isUpdating = false
        }

        do {
            if enabled {
                guard status != .enabled, status != .requiresApproval else { return }
                try service.register()
            } else {
                guard status != .notRegistered else { return }
                try service.unregister()
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func openSystemSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }
}
