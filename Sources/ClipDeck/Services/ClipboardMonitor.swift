import AppKit

struct LinkMetadata: Equatable, Sendable {
    let title: String?
    let iconData: Data?
}

enum LinkMetadataService {
    private static let maximumHTMLBytes = 512 * 1_024
    private static let maximumIconBytes = 512 * 1_024

    static func fetch(for url: URL) async -> LinkMetadata {
        guard let (data, response) = await download(
            url: url,
            accept: "text/html,application/xhtml+xml",
            maximumBytes: maximumHTMLBytes,
            timeout: 8
        ),
              let html = decodeHTML(data) else {
            return LinkMetadata(title: nil, iconData: await fetchFirstIcon(from: fallbackIconURLs(for: url)))
        }

        let baseURL = response.url ?? url
        let title = title(fromHTML: html)
        var candidates = iconURLs(fromHTML: html, baseURL: baseURL)
        if let manifestURL = manifestURL(fromHTML: html, baseURL: baseURL),
           let (manifestData, _) = await download(
               url: manifestURL,
               accept: "application/manifest+json,application/json",
               maximumBytes: maximumHTMLBytes,
               timeout: 6
           ) {
            candidates.append(contentsOf: iconURLs(fromManifest: manifestData, baseURL: manifestURL))
        }
        candidates.append(contentsOf: fallbackIconURLs(for: baseURL))
        return LinkMetadata(title: title, iconData: await fetchFirstIcon(from: candidates))
    }

    static func title(fromHTML html: String) -> String? {
        let titleElement = firstCapture(
            pattern: #"<title\b[^>]*>(.*?)</title\s*>"#,
            in: html,
            options: [.caseInsensitive, .dotMatchesLineSeparators]
        )
        let rawTitle = titleElement ?? metadataContent(
            in: html,
            keys: ["og:title", "twitter:title"]
        )
        guard let rawTitle else { return nil }
        let withoutTags = rawTitle.replacingOccurrences(of: #"<[^>]+>"#, with: "", options: .regularExpression)
        let clean = decodeEntities(withoutTags)
            .replacingOccurrences(of: #"\s+"#, with: " ", options: .regularExpression)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { return nil }
        return String(clean.prefix(180))
    }

    static func iconURL(fromHTML html: String, baseURL: URL) -> URL? {
        iconURLs(fromHTML: html, baseURL: baseURL).first
    }

    static func iconURLs(fromHTML html: String, baseURL: URL) -> [URL] {
        guard let regex = try? NSRegularExpression(
            pattern: #"<link\b[^>]*>"#,
            options: [.caseInsensitive]
        ) else { return [] }
        let range = NSRange(html.startIndex..., in: html)
        var results: [URL] = []
        for match in regex.matches(in: html, range: range) {
            guard let tagRange = Range(match.range, in: html) else { continue }
            let tag = String(html[tagRange])
            guard let rel = attribute("rel", in: tag)?.lowercased(), rel.contains("icon"),
                  let href = attribute("href", in: tag),
                  let resolved = URL(string: decodeEntities(href), relativeTo: baseURL)?.absoluteURL,
                  ["http", "https"].contains(resolved.scheme?.lowercased() ?? "") else { continue }
            if !results.contains(resolved) { results.append(resolved) }
        }
        return results
    }

    static func manifestURL(fromHTML html: String, baseURL: URL) -> URL? {
        guard let regex = try? NSRegularExpression(pattern: #"<link\b[^>]*>"#, options: [.caseInsensitive]) else {
            return nil
        }
        for match in regex.matches(in: html, range: NSRange(html.startIndex..., in: html)) {
            guard let tagRange = Range(match.range, in: html) else { continue }
            let tag = String(html[tagRange])
            guard let rel = attribute("rel", in: tag)?.lowercased(), rel.split(whereSeparator: \.isWhitespace).contains("manifest"),
                  let href = attribute("href", in: tag),
                  let url = URL(string: decodeEntities(href), relativeTo: baseURL)?.absoluteURL,
                  ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { continue }
            return url
        }
        return nil
    }

    static func iconURLs(fromManifest data: Data, baseURL: URL) -> [URL] {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let icons = object["icons"] as? [[String: Any]] else { return [] }
        return icons.reversed().compactMap { icon in
            guard let source = icon["src"] as? String,
                  let url = URL(string: source, relativeTo: baseURL)?.absoluteURL,
                  ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return nil }
            return url
        }
    }

    private static func fetchFirstIcon(from urls: [URL]) async -> Data? {
        var attempted = Set<URL>()
        for url in urls where attempted.insert(url).inserted {
            guard let (data, _) = await download(
                url: url,
                accept: "image/*",
                maximumBytes: maximumIconBytes,
                timeout: 6
            ), NSImage(data: data) != nil else { continue }
            return data
        }
        return nil
    }

    private static func fallbackIconURLs(for url: URL) -> [URL] {
        guard let scheme = url.scheme, let host = url.host else { return [] }
        return ["/favicon.ico", "/apple-touch-icon.png"].compactMap { path in
            var components = URLComponents()
            components.scheme = scheme
            components.host = host
            components.port = url.port
            components.path = path
            return components.url
        }
    }

    private static func download(
        url: URL,
        accept: String,
        maximumBytes: Int,
        timeout: TimeInterval
    ) async -> (Data, URLResponse)? {
        for usesRange in [true, false] {
            var request = URLRequest(url: url, timeoutInterval: timeout)
            request.setValue(accept, forHTTPHeaderField: "Accept")
            request.setValue("Mozilla/5.0 (Macintosh; Intel Mac OS X) AppleWebKit/605.1.15", forHTTPHeaderField: "User-Agent")
            if usesRange {
                request.setValue("bytes=0-\(maximumBytes - 1)", forHTTPHeaderField: "Range")
            }
            guard let (data, response) = try? await URLSession.shared.data(for: request),
                  let httpResponse = response as? HTTPURLResponse,
                  (200 ... 399).contains(httpResponse.statusCode),
                  !data.isEmpty,
                  data.count <= maximumBytes else { continue }
            return (data, response)
        }
        return nil
    }

    private static func decodeHTML(_ data: Data) -> String? {
        String(data: data, encoding: .utf8)
            ?? String(data: data, encoding: .isoLatin1)
    }

    private static func attribute(_ name: String, in tag: String) -> String? {
        let escaped = NSRegularExpression.escapedPattern(for: name)
        return firstCapture(
            pattern: #"\b"# + escaped + #"\s*=\s*[\"']([^\"']+)[\"']"#,
            in: tag,
            options: [.caseInsensitive]
        ) ?? firstCapture(
            pattern: #"\b"# + escaped + #"\s*=\s*([^\s>]+)"#,
            in: tag,
            options: [.caseInsensitive]
        )
    }

    private static func metadataContent(in html: String, keys: [String]) -> String? {
        guard let regex = try? NSRegularExpression(pattern: #"<meta\b[^>]*>"#, options: [.caseInsensitive]) else {
            return nil
        }
        for match in regex.matches(in: html, range: NSRange(html.startIndex..., in: html)) {
            guard let tagRange = Range(match.range, in: html) else { continue }
            let tag = String(html[tagRange])
            let key = attribute("property", in: tag) ?? attribute("name", in: tag)
            guard let key, keys.contains(where: { $0.caseInsensitiveCompare(key) == .orderedSame }) else { continue }
            if let content = attribute("content", in: tag) { return content }
        }
        return nil
    }

    private static func firstCapture(
        pattern: String,
        in text: String,
        options: NSRegularExpression.Options
    ) -> String? {
        guard let regex = try? NSRegularExpression(pattern: pattern, options: options),
              let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              match.numberOfRanges > 1,
              let range = Range(match.range(at: 1), in: text) else { return nil }
        return String(text[range])
    }

    private static func decodeEntities(_ text: String) -> String {
        var decoded = text
        let named = ["&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": "\"", "&#39;": "'", "&apos;": "'", "&nbsp;": " "]
        for (entity, replacement) in named {
            decoded = decoded.replacingOccurrences(of: entity, with: replacement, options: .caseInsensitive)
        }
        guard let regex = try? NSRegularExpression(pattern: #"&#(x[0-9a-f]+|[0-9]+);"#, options: .caseInsensitive) else { return decoded }
        let matches = regex.matches(in: decoded, range: NSRange(decoded.startIndex..., in: decoded)).reversed()
        for match in matches {
            guard let fullRange = Range(match.range(at: 0), in: decoded),
                  let valueRange = Range(match.range(at: 1), in: decoded) else { continue }
            let value = String(decoded[valueRange])
            let scalarValue = value.lowercased().hasPrefix("x")
                ? UInt32(value.dropFirst(), radix: 16)
                : UInt32(value, radix: 10)
            if let scalarValue, let scalar = UnicodeScalar(scalarValue) {
                decoded.replaceSubrange(fullRange, with: String(scalar))
            }
        }
        return decoded
    }
}

@MainActor
final class ClipboardMonitor {
    private let store: InMemoryHistoryStore
    private let repository: HistoryRepository?
    private let classifier: ClassificationService?
    private let reader = ClipboardReader()
    private let processingQueue = DispatchQueue(label: "app.clipdeck.clipboard-reader", qos: .utility)
    private var timer: Timer?
    private var lastChangeCount = NSPasteboard.general.changeCount
    private var ignoredChangeCount: Int?

    init(store: InMemoryHistoryStore, repository: HistoryRepository? = nil, classifier: ClassificationService? = nil) {
        self.store = store
        self.repository = repository
        self.classifier = classifier
    }

    func start() {
        guard timer == nil else { return }
        let timer = Timer(timeInterval: 0.4, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.poll()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    func suppressCurrentPasteboardChange() {
        let count = NSPasteboard.general.changeCount
        ignoredChangeCount = count
        lastChangeCount = count
    }

    private func poll() {
        let pasteboard = NSPasteboard.general
        let changeCount = pasteboard.changeCount
        guard changeCount != lastChangeCount else { return }
        lastChangeCount = changeCount
        if ignoredChangeCount == changeCount {
            ignoredChangeCount = nil
            return
        }

        let sourceApplication = Self.frontmostApplication()
        processingQueue.async { [reader, repository, classifier] in
            guard let payload = reader.read() else { return }
            let capturedItem = ClipboardItem(payload: payload, sourceApplication: sourceApplication)
            let item: ClipboardItem
            do {
                let savedItem = try repository?.save(capturedItem) ?? capturedItem
                item = try classifier?.classify(savedItem) ?? savedItem
            } catch {
                NSLog("ClipDeck failed to persist clipboard item: %@", String(describing: error))
                item = capturedItem
            }
            Task { @MainActor [weak self] in
                self?.store.insert(item)
            }
            if let url = item.webURL, item.linkIconData == nil || item.title == "文本" {
                let itemID = item.id
                Task.detached(priority: .utility) {
                    let metadata = await LinkMetadataService.fetch(for: url)
                    await MainActor.run { [weak self] in
                        self?.store.updateLinkMetadata(
                            itemID: itemID,
                            title: metadata.title,
                            iconData: metadata.iconData
                        )
                    }
                }
            }
        }
    }

    private static func frontmostApplication() -> SourceApplication {
        guard let application = NSWorkspace.shared.frontmostApplication else {
            return SourceApplication(bundleIdentifier: nil, name: "Unknown", iconData: nil)
        }
        let iconData = application.bundleURL
            .map { NSWorkspace.shared.icon(forFile: $0.path) }
            .flatMap { $0.tiffRepresentation }
        return SourceApplication(
            bundleIdentifier: application.bundleIdentifier,
            name: application.localizedName ?? "Unknown",
            iconData: iconData
        )
    }
}
