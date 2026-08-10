import AppKit
import ImageIO
import UniformTypeIdentifiers

struct ClipboardReader: Sendable {
    static let maximumPixelCount = 60_000_000

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
        guard AppPreferences.recordsImages else { return nil }
        let candidates: [NSPasteboard.PasteboardType] = [.png, .tiff]
        guard let type = pasteboard.availableType(from: candidates),
              let data = pasteboard.data(forType: type) else { return nil }
        return Self.normalizedImagePayload(
            data: data,
            type: type,
            maximumByteCount: AppPreferences.maximumImageByteCount
        )
    }

    static func normalizedImagePayload(
        data: Data,
        type: NSPasteboard.PasteboardType,
        maximumByteCount: Int
    ) -> ClipboardPayload? {
        guard maximumByteCount > 0,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue,
              let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue,
              Self.isSafeImageSize(width: width, height: height) else {
            return nil
        }

        if type == .tiff {
            guard let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
                  let destinationData = CFDataCreateMutable(nil, 0),
                  let destination = CGImageDestinationCreateWithData(
                      destinationData,
                      UTType.png.identifier as CFString,
                      1,
                      nil
                  ) else { return nil }
            CGImageDestinationAddImage(destination, image, nil)
            guard CGImageDestinationFinalize(destination),
                  CFDataGetLength(destinationData) <= maximumByteCount else { return nil }
            return .image(
                data: destinationData as Data,
                width: width,
                height: height,
                format: .png
            )
        }

        guard type == .png, data.count <= maximumByteCount else { return nil }
        return .image(data: data, width: width, height: height, format: .png)
    }

    static func isSafeImageSize(width: Int, height: Int) -> Bool {
        guard width > 0, height > 0, width <= maximumPixelCount else { return false }
        return height <= maximumPixelCount / width
    }
}
