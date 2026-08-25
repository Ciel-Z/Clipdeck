import AppKit
import ImageIO
import UniformTypeIdentifiers

struct LinkMetadata: Equatable, Sendable {
    let title: String?
    let iconData: Data?
    let previewData: Data?
}

enum LinkMetadataService {
    private static let maximumHTMLBytes = 512 * 1_024
    private static let maximumIconBytes = 512 * 1_024
    private static let maximumPreviewDownloadBytes = 8 * 1_024 * 1_024
    private static let maximumPreviewPixelDimension = 1_200

    static func fetch(for url: URL) async -> LinkMetadata {
        guard let (data, response) = await download(
            url: url,
            accept: "text/html,application/xhtml+xml",
            maximumBytes: maximumHTMLBytes,
            timeout: 8
        ),
              let html = decodeHTML(data) else {
            return LinkMetadata(
                title: nil,
                iconData: await fetchFirstIcon(from: fallbackIconURLs(for: url)),
                previewData: nil
            )
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
        async let iconData = fetchFirstIcon(from: candidates)
        async let previewData = fetchPreview(from: previewImageURL(fromHTML: html, baseURL: baseURL))
        return await LinkMetadata(title: title, iconData: iconData, previewData: previewData)
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

    static func previewImageURL(fromHTML html: String, baseURL: URL) -> URL? {
        guard let value = metadataContent(
            in: html,
            keys: ["og:image", "og:image:url", "twitter:image", "twitter:image:src"]
        ), let url = URL(string: decodeEntities(value), relativeTo: baseURL)?.absoluteURL,
           ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return nil }
        return url
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

    private static func fetchPreview(from url: URL?) async -> Data? {
        guard let url,
              let (data, _) = await download(
                  url: url,
                  accept: "image/*",
                  maximumBytes: maximumPreviewDownloadBytes,
                  timeout: 8
              ) else { return nil }
        return normalizedPreviewData(data)
    }

    private static func normalizedPreviewData(_ data: Data) -> Data? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let image = CGImageSourceCreateThumbnailAtIndex(
                  source,
                  0,
                  [
                      kCGImageSourceCreateThumbnailFromImageAlways: true,
                      kCGImageSourceCreateThumbnailWithTransform: true,
                      kCGImageSourceThumbnailMaxPixelSize: maximumPreviewPixelDimension,
                  ] as CFDictionary
              ),
              let output = CFDataCreateMutable(nil, 0),
              let destination = CGImageDestinationCreateWithData(
                  output,
                  UTType.jpeg.identifier as CFString,
                  1,
                  nil
              ) else { return nil }
        CGImageDestinationAddImage(
            destination,
            image,
            [kCGImageDestinationLossyCompressionQuality: 0.82] as CFDictionary
        )
        guard CGImageDestinationFinalize(destination) else { return nil }
        return output as Data
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
    private let onCapture: @MainActor () -> Void
    private let reader = ClipboardReader()
    private let readerQueue = DispatchQueue(label: "app.clipdeck.clipboard-reader", qos: .userInitiated)
    private let persistenceQueue = DispatchQueue(
        label: "app.clipdeck.clipboard-persistence",
        qos: .utility
    )
    private var timer: Timer?
    private var lastChangeCount = NSPasteboard.general.changeCount
    private var ignoredChangeCount: Int?

    init(
        store: InMemoryHistoryStore,
        repository: HistoryRepository? = nil,
        classifier: ClassificationService? = nil,
        onCapture: @escaping @MainActor () -> Void = {}
    ) {
        self.store = store
        self.repository = repository
        self.classifier = classifier
        self.onCapture = onCapture
    }

    func start() {
        guard timer == nil else { return }
        let timer = Timer(timeInterval: 0.2, repeats: true) { [weak self] _ in
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
        readerQueue.async { [reader] in
            autoreleasepool {
                guard let payload = reader.read() else { return }
                let capturedItem = ClipboardItem(payload: payload, sourceApplication: sourceApplication)
                // Make capture feedback independent from disk and JavaScript
                // classification latency. The persisted form keeps this UUID and
                // replaces the transient item in place when background work ends.
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    self.store.insert(capturedItem)
                    self.onCapture()
                    self.persistAndClassify(capturedItem)
                }
            }
        }
    }

    private func persistAndClassify(_ capturedItem: ClipboardItem) {
        persistenceQueue.async { [weak self, repository, classifier] in
            autoreleasepool {
                let item: ClipboardItem
                do {
                    let savedItem = try repository?.save(capturedItem) ?? capturedItem
                    item = try classifier?.classify(savedItem) ?? savedItem
                } catch {
                    NSLog("ClipDeck failed to persist clipboard item: %@", String(describing: error))
                    item = capturedItem
                }
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    let reconciliation = self.store.reconcileCapturedItem(item)
                    if reconciliation == .removed {
                        if item.databaseID != nil, let repository {
                            Task.detached(priority: .utility) {
                                try? repository.delete(item)
                            }
                        }
                        return
                    }
                    guard reconciliation == .reconciled else { return }
                    if let url = item.webURL,
                       !item.hasFetchedLinkPreview || item.linkIconData == nil || item.title == "文本" {
                        let itemID = item.id
                        Task.detached(priority: .utility) { [weak self] in
                            let metadata = await LinkMetadataService.fetch(for: url)
                            await MainActor.run {
                                self?.store.updateLinkMetadata(
                                    itemID: itemID,
                                    title: metadata.title,
                                    iconData: metadata.iconData,
                                    previewData: metadata.previewData
                                )
                            }
                        }
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
