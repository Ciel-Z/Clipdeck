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
    let payload: ClipboardPayload
    let sourceApplication: SourceApplication
    let category: ClipboardCategory?
    let copiedAt: Date

    init(
        id: UUID = UUID(),
        databaseID: Int64? = nil,
        title: String? = nil,
        payload: ClipboardPayload,
        sourceApplication: SourceApplication,
        category: ClipboardCategory? = nil,
        copiedAt: Date = Date()
    ) {
        self.id = id
        self.databaseID = databaseID
        self.title = title ?? payload.defaultTitle
        self.payload = payload
        self.sourceApplication = sourceApplication
        self.category = category
        self.copiedAt = copiedAt
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
