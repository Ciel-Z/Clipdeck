import AppKit
@preconcurrency import WebKit

@main
@MainActor
final class ScriptEditorSmoke: NSObject, NSApplicationDelegate, WKScriptMessageHandler, WKNavigationDelegate {
    private var window: NSWindow!
    private var webView: WKWebView!
    private var timeout: Timer?
    private var validationPassed = false
    private var observedChange = false

    static func main() {
        let app = NSApplication.shared
        let delegate = ScriptEditorSmoke()
        app.delegate = delegate
        app.run()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        webView = WKWebView(frame: NSRect(x: 0, y: 0, width: 800, height: 480), configuration: configuration)
        webView.navigationDelegate = self
        window = NSWindow(contentRect: webView.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = webView

        let indexURL = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("script-editor/dist/index.html")
        webView.loadFileURL(indexURL, allowingReadAccessTo: indexURL.deletingLastPathComponent())

        timeout = Timer.scheduledTimer(withTimeInterval: 8, repeats: false) { _ in
            fputs("SCRIPT_EDITOR_SMOKE_FAILED: prewarmed bridge or editor command timeout\n", stderr)
            NSApp.terminate(nil)
        }
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        // Match the production prewarm path: attach the host only after the
        // editor page has loaded and its original ready message has passed.
        webView.configuration.userContentController.add(self, name: "clipDeckEditor")
        validateEditor()
    }

    func userContentController(
        _ userContentController: WKUserContentController,
        didReceive message: WKScriptMessage
    ) {
        guard let payload = message.body as? [String: Any] else { return }
        if payload["type"] as? String == "change" {
            observedChange = true
            finishIfReady()
        }
    }

    private func validateEditor() {
        let text = "function classify(item) {"
        let data = try! JSONSerialization.data(withJSONObject: [
            "type": "initialize", "revision": 1, "text": text, "theme": "dark",
        ])
        let json = String(data: data, encoding: .utf8)!
        let validation = """
        window.clipDeckEditor.receive(\(json));
        (() => {
          const scroller = document.querySelector('.cm-scroller');
          const content = document.querySelector('.cm-content');
          const gutters = document.querySelector('.cm-gutters');
          window.clipDeckEditor.receive({ type: 'setSelection', anchor: \(text.utf16.count) });
          content.focus();
          content.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', bubbles: true }));
          const enterIndented = window.clipDeckEditor.getDocument() === \(String(reflecting: text + "\n    "));
          content.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', code: 'Tab', bubbles: true }));
          const tabIndented = window.clipDeckEditor.getDocument() === \(String(reflecting: text + "\n        "));
          content.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', code: 'Tab', shiftKey: true, bubbles: true }));
          const shiftTabOutdented = window.clipDeckEditor.getDocument() === \(String(reflecting: text + "\n    "));
          const cursorIsHighContrast = getComputedStyle(document.documentElement)
            .getPropertyValue('--editor-caret').trim() === '#ffd60a';
          const unifiedBackground = getComputedStyle(document.querySelector('.cm-editor')).backgroundColor ===
            getComputedStyle(gutters).backgroundColor;
          return window.clipDeckEditor !== undefined &&
            enterIndented && tabIndented && shiftTabOutdented && cursorIsHighContrast && unifiedBackground &&
            scroller && content && gutters &&
            getComputedStyle(scroller).display === 'flex' &&
            content.getBoundingClientRect().left >= gutters.getBoundingClientRect().right;
        })()
        """
        webView.evaluateJavaScript(validation) { result, error in
            if error == nil, result as? Bool == true {
                self.validationPassed = true
                self.finishIfReady()
            } else {
                fputs("SCRIPT_EDITOR_SMOKE_FAILED: \(error?.localizedDescription ?? "bridge unavailable")\n", stderr)
                NSApp.terminate(nil)
            }
        }
    }

    private func finishIfReady() {
        guard validationPassed, observedChange else { return }
        timeout?.invalidate()
        print("SCRIPT_EDITOR_SMOKE_OK")
        NSApp.terminate(nil)
    }
}
