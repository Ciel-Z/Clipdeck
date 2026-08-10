import ServiceManagement
import XCTest
@testable import ClipDeck

@MainActor
final class LaunchAtLoginControllerTests: XCTestCase {
    func testRegisterEnablesLaunchAtLogin() {
        let service = ServiceStub(status: .notRegistered)
        let controller = LaunchAtLoginController(service: service)

        controller.setEnabled(true)

        XCTAssertEqual(service.registerCallCount, 1)
        XCTAssertTrue(controller.isOn)
        XCTAssertNil(controller.errorMessage)
    }

    func testRequiresApprovalKeepsSwitchOnAndShowsGuidance() {
        let service = ServiceStub(status: .requiresApproval)
        let controller = LaunchAtLoginController(service: service)

        XCTAssertTrue(controller.isOn)
        XCTAssertTrue(controller.requiresApproval)
        XCTAssertEqual(
            controller.statusMessage,
            "已添加到登录项，请在系统设置中允许 ClipDeck 后生效。"
        )
    }

    func testUnregisterDisablesLaunchAtLogin() {
        let service = ServiceStub(status: .enabled)
        let controller = LaunchAtLoginController(service: service)

        controller.setEnabled(false)

        XCTAssertEqual(service.unregisterCallCount, 1)
        XCTAssertFalse(controller.isOn)
    }

    func testRegistrationFailureIsExposed() {
        let service = ServiceStub(status: .notRegistered)
        service.registerError = TestError.registrationFailed
        let controller = LaunchAtLoginController(service: service)

        controller.setEnabled(true)

        XCTAssertFalse(controller.isOn)
        XCTAssertNotNil(controller.errorMessage)
        XCTAssertTrue(controller.statusMessage?.hasPrefix("设置失败：") == true)
    }
}

@MainActor
private final class ServiceStub: LaunchAtLoginService {
    var status: SMAppService.Status
    var registerError: Error?
    var unregisterError: Error?
    private(set) var registerCallCount = 0
    private(set) var unregisterCallCount = 0

    init(status: SMAppService.Status) {
        self.status = status
    }

    func register() throws {
        registerCallCount += 1
        if let registerError { throw registerError }
        status = .enabled
    }

    func unregister() throws {
        unregisterCallCount += 1
        if let unregisterError { throw unregisterError }
        status = .notRegistered
    }
}

private enum TestError: Error {
    case registrationFailed
}
