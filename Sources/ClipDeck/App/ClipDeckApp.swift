import AppKit

@main
@MainActor
struct ClipDeckApp {
    static func main() {
        let application = NSApplication.shared
        let coordinator = AppCoordinator()
        application.delegate = coordinator
        application.run()
        withExtendedLifetime(coordinator) {
            // Keep the application delegate alive for the full AppKit run loop.
        }
    }
}
