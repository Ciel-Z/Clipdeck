import AppKit
import ImageIO

struct ClipboardReader: Sendable {
    static let maximumPixelCount = 60_000_000
    static let maximumImageByteCount = 10 * 1_024 * 1_024

    func read(from pasteboard: NSPasteboard = .general) -> ClipboardPayload? {
        // File drags often also publish a string representation. They are outside v1 scope.
        if pasteboard.availableType(from: [.fileURL]) != nil {
            return nil
        }

        if let imagePayload = readImage(from: pasteboard) {
            return imagePayload
        }

        guard let plainText = pasteboard.string(forType: .string), !plainText.isEmpty else {
            return nil
        }
        return .text(
            plainText: plainText,
            rtfData: pasteboard.data(forType: .rtf)
        )
    }

    private func readImage(from pasteboard: NSPasteboard) -> ClipboardPayload? {
        let candidates: [NSPasteboard.PasteboardType] = [.png, .tiff]
        guard let type = pasteboard.availableType(from: candidates),
              let data = pasteboard.data(forType: type),
              data.count <= Self.maximumImageByteCount,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? Int,
              let height = properties[kCGImagePropertyPixelHeight] as? Int,
              Self.isSafeImageSize(width: width, height: height) else {
            return nil
        }
        let format: ClipboardImageFormat = type == .png ? .png : .tiff
        return .image(data: data, width: width, height: height, format: format)
    }

    static func isSafeImageSize(width: Int, height: Int) -> Bool {
        guard width > 0, height > 0, width <= maximumPixelCount else { return false }
        return height <= maximumPixelCount / width
    }
}
