import AppKit
import SwiftUI
@preconcurrency import WebKit

/// Native host for the repository-level `script-editor` CodeMirror component.
/// Editing lives in the shared web bundle; script execution remains native.
struct ScriptEditorWebView: NSViewRepresentable {
    @Binding var text: String
    var autofocus = false
    var onSave: () -> Void = {}
    var onRun: () -> Void = {}
    var onFormat: () -> Void = {}

    @Environment(\.colorScheme) private var colorScheme

    func makeCoordinator() -> Coordinator {
        Coordinator(parent: self)
    }

    func makeNSView(context: Context) -> WKWebView {
        let runtime = ScriptEditorRuntime.shared
        let prewarmed = runtime.takePrewarmedWebView(messageHandler: context.coordinator)
        let webView: WKWebView
        if let prewarmed {
            webView = prewarmed.webView
        } else {
            let configuration = runtime.makeConfiguration()
            configuration.userContentController.add(context.coordinator, name: Coordinator.handlerName)
            webView = WKWebView(frame: .zero, configuration: configuration)
        }
        webView.navigationDelegate = context.coordinator
        webView.setValue(false, forKey: "drawsBackground")
        context.coordinator.webView = webView

        if let prewarmed {
            if prewarmed.isReady {
                context.coordinator.attachPreloadedPage()
            }
        } else {
            Self.loadEditor(in: webView)
        }
        return webView
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        context.coordinator.update(parent: self)
        context.coordinator.sendDocumentIfNeeded(text)
        context.coordinator.sendTheme(colorScheme == .dark ? "dark" : "light")
    }

    static func dismantleNSView(_ webView: WKWebView, coordinator: Coordinator) {
        webView.configuration.userContentController.removeScriptMessageHandler(forName: Coordinator.handlerName)
        webView.navigationDelegate = nil
    }

    private static func loadEditor(in webView: WKWebView) {
        if let indexURL = ScriptEditorResources.indexURL() {
            webView.loadFileURL(indexURL, allowingReadAccessTo: indexURL.deletingLastPathComponent())
        } else {
            webView.loadHTMLString(Self.missingResourcesHTML, baseURL: nil)
        }
    }

    @MainActor
    final class Coordinator: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
        static let handlerName = "clipDeckEditor"

        private var parent: ScriptEditorWebView
        private var isReady = false
        private var revision = 0
        private var editorText: String?
        private var currentTheme: String?
        weak var webView: WKWebView?

        init(parent: ScriptEditorWebView) {
            self.parent = parent
        }

        func update(parent: ScriptEditorWebView) {
            self.parent = parent
        }

        func attachPreloadedPage() {
            isReady = true
            sendInitialization()
        }

        func userContentController(
            _ userContentController: WKUserContentController,
            didReceive message: WKScriptMessage
        ) {
            guard message.name == Self.handlerName,
                  message.webView?.url?.isFileURL == true,
                  let payload = message.body as? [String: Any],
                  let type = payload["type"] as? String else { return }

            switch type {
            case "ready":
                isReady = true
                sendInitialization()
            case "change":
                guard let changedText = payload["text"] as? String else { return }
                if let nextRevision = payload["revision"] as? Int {
                    revision = max(revision, nextRevision)
                }
                editorText = changedText
                if parent.text != changedText {
                    parent.text = changedText
                }
            case "shortcut":
                switch payload["command"] as? String {
                case "save": parent.onSave()
                case "run": parent.onRun()
                case "format": parent.onFormat()
                default: break
                }
            default:
                break
            }
        }

        func webView(
            _ webView: WKWebView,
            decidePolicyFor navigationAction: WKNavigationAction,
            decisionHandler: @escaping @MainActor @Sendable (WKNavigationActionPolicy) -> Void
        ) {
            guard let url = navigationAction.request.url else {
                decisionHandler(.cancel)
                return
            }
            decisionHandler(url.isFileURL ? .allow : .cancel)
        }

        func sendDocumentIfNeeded(_ text: String) {
            guard isReady, editorText != text else { return }
            revision += 1
            editorText = text
            send(["type": "setDocument", "revision": revision, "text": text])
        }

        func sendTheme(_ theme: String) {
            guard currentTheme != theme else { return }
            currentTheme = theme
            guard isReady else { return }
            send(["type": "setTheme", "theme": theme])
        }

        private func send(_ payload: [String: Any]) {
            guard let webView,
                  JSONSerialization.isValidJSONObject(payload),
                  let data = try? JSONSerialization.data(withJSONObject: payload),
                  let json = String(data: data, encoding: .utf8) else { return }
            webView.evaluateJavaScript("window.clipDeckEditor?.receive(\(json))")
        }

        private func sendInitialization() {
            send([
                "type": "initialize",
                "revision": revision,
                "text": parent.text,
                "theme": currentTheme ?? "dark",
                "autofocus": parent.autofocus,
            ])
            editorText = parent.text
        }
    }

    private static let missingResourcesHTML = """
    <!doctype html><meta charset="utf-8">
    <body style="font:13px -apple-system;color:#ff6961;background:transparent;padding:12px">
    无法加载公共脚本编辑器资源，请先在 script-editor 目录执行 npm run build。
    </body>
    """
}

@MainActor
final class ScriptEditorRuntime: NSObject, WKNavigationDelegate {
    static let shared = ScriptEditorRuntime()

    private let dataStore = WKWebsiteDataStore.nonPersistent()
    private var prewarmedWebView: WKWebView?
    private var prewarmedPageIsReady = false
    private var prewarmScheduled = false

    func makeConfiguration() -> WKWebViewConfiguration {
        let configuration = WKWebViewConfiguration()
        configuration.defaultWebpagePreferences.allowsContentJavaScript = true
        configuration.websiteDataStore = dataStore
        return configuration
    }

    func schedulePrewarm() {
        guard !prewarmScheduled, prewarmedWebView == nil else { return }
        prewarmScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            self?.prewarmScheduled = false
            self?.prewarm()
        }
    }

    func takePrewarmedWebView(
        messageHandler: WKScriptMessageHandler
    ) -> (webView: WKWebView, isReady: Bool)? {
        guard let webView = prewarmedWebView else { return nil }
        let result = (webView, prewarmedPageIsReady)
        prewarmedWebView = nil
        prewarmedPageIsReady = false
        webView.navigationDelegate = nil
        webView.configuration.userContentController.add(
            messageHandler,
            name: ScriptEditorWebView.Coordinator.handlerName
        )
        schedulePrewarm()
        return result
    }

    private func prewarm() {
        guard prewarmedWebView == nil, let indexURL = ScriptEditorResources.indexURL() else { return }
        let webView = WKWebView(frame: NSRect(x: 0, y: 0, width: 1, height: 1), configuration: makeConfiguration())
        webView.navigationDelegate = self
        prewarmedWebView = webView
        prewarmedPageIsReady = false
        webView.loadFileURL(indexURL, allowingReadAccessTo: indexURL.deletingLastPathComponent())
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        guard webView === prewarmedWebView else { return }
        prewarmedPageIsReady = true
    }

    func webView(
        _ webView: WKWebView,
        didFail navigation: WKNavigation!,
        withError error: any Error
    ) {
        discardFailedPrewarm(webView)
    }

    func webView(
        _ webView: WKWebView,
        didFailProvisionalNavigation navigation: WKNavigation!,
        withError error: any Error
    ) {
        discardFailedPrewarm(webView)
    }

    private func discardFailedPrewarm(_ webView: WKWebView) {
        guard webView === prewarmedWebView else { return }
        prewarmedWebView = nil
        prewarmedPageIsReady = false
    }
}

private enum ScriptEditorResources {
    static func indexURL() -> URL? {
        let candidates: [URL?] = [
            Bundle.main.resourceURL?.appendingPathComponent("script-editor/index.html"),
            Bundle.main.resourceURL?.appendingPathComponent("dist/index.html"),
            packageResourceURL(),
            developmentIndexURL(),
        ]
        return candidates.compactMap { $0 }.first { FileManager.default.fileExists(atPath: $0.path) }
    }

    private static func packageResourceURL() -> URL? {
        #if SWIFT_PACKAGE
        Bundle.module.resourceURL?.appendingPathComponent("script-editor/index.html")
        #else
        nil
        #endif
    }

    private static func developmentIndexURL() -> URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // UI
            .deletingLastPathComponent() // ClipDeck
            .deletingLastPathComponent() // Sources
            .deletingLastPathComponent() // mac
            .deletingLastPathComponent() // repository root
            .appendingPathComponent("script-editor/dist/index.html")
    }
}
