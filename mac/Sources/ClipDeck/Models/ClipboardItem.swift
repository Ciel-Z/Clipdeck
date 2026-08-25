import Foundation

struct SourceApplication: Equatable, Sendable {
    let bundleIdentifier: String?
    let name: String
    let iconData: Data?
}

struct ClipboardCategory: Identifiable, Equatable, Sendable {
    let id: Int64
    let name: String
    let icon: String?
    let script: String
    let previewLanguage: String?
    let sortOrder: Int
    let lastErrorMessage: String?
    let lastErrorAt: Date?

    init(
        id: Int64,
        name: String,
        icon: String?,
        script: String,
        previewLanguage: String?,
        sortOrder: Int,
        lastErrorMessage: String? = nil,
        lastErrorAt: Date? = nil
    ) {
        self.id = id
        self.name = name
        self.icon = icon
        self.script = script
        self.previewLanguage = previewLanguage
        self.sortOrder = sortOrder
        self.lastErrorMessage = lastErrorMessage
        self.lastErrorAt = lastErrorAt
    }
}

enum ClipboardPayload: Equatable, Sendable {
    case text(plainText: String, rtfData: Data?)
    case image(data: Data, width: Int, height: Int, format: ClipboardImageFormat)
}

enum ClipboardImageFormat: String, Equatable, Sendable {
    case png
    case tiff
}

struct ClipboardItem: Identifiable, Equatable, Sendable {
    let id: UUID
    let databaseID: Int64?
    let title: String
    let linkIconData: Data?
    let linkPreviewData: Data?
    let hasFetchedLinkPreview: Bool
    let imageAngle: Int
    let payload: ClipboardPayload
    let sourceApplication: SourceApplication
    let category: ClipboardCategory?
    let copiedAt: Date

    init(
        id: UUID = UUID(),
        databaseID: Int64? = nil,
        title: String? = nil,
        linkIconData: Data? = nil,
        linkPreviewData: Data? = nil,
        hasFetchedLinkPreview: Bool = false,
        imageAngle: Int = 0,
        payload: ClipboardPayload,
        sourceApplication: SourceApplication,
        category: ClipboardCategory? = nil,
        copiedAt: Date = Date()
    ) {
        self.id = id
        self.databaseID = databaseID
        self.title = title ?? payload.defaultTitle
        self.linkIconData = linkIconData
        self.linkPreviewData = linkPreviewData
        self.hasFetchedLinkPreview = hasFetchedLinkPreview
        self.imageAngle = Self.normalizedImageAngle(imageAngle)
        self.payload = payload
        self.sourceApplication = sourceApplication
        self.category = category
        self.copiedAt = copiedAt
    }

    private static func normalizedImageAngle(_ angle: Int) -> Int {
        let normalized = angle % 360
        return normalized >= 0 ? normalized : normalized + 360
    }
}

extension ClipboardItem {
    var webURL: URL? {
        guard case let .text(plainText, _) = payload else { return nil }
        let candidate = plainText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !candidate.contains(where: \.isWhitespace),
              let url = URL(string: candidate),
              let scheme = url.scheme?.lowercased(),
              scheme == "http" || scheme == "https",
              url.host != nil else { return nil }
        return url
    }

    var linkHost: String? {
        guard let host = webURL?.host(percentEncoded: false) else { return nil }
        return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
    }
}

private extension ClipboardPayload {
    var defaultTitle: String {
        switch self {
        case .text: "文本"
        case .image: "图片"
        }
    }
}
