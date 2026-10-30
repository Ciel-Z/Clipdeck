import AppKit
import ImageIO
import UniformTypeIdentifiers

struct ClipboardReader: Sendable {
    static let maximumPixelCount = 60_000_000

    func read(
        from pasteboard: NSPasteboard = .general,
        preferImageFileURLs: Bool = false
    ) -> ClipboardPayload? {
        // File managers also publish PNG/TIFF representations of the file's
        // application icon. A native file URL takes precedence over that image.
        if pasteboard.availableType(from: [.fileURL]) != nil {
            let urls = Self.fileURLs(from: pasteboard)
            // WeChat's Copy Photo can publish only a temporary image URL.
            // Restrict this exception to a single image, never a document,
            // folder or mixed file selection (which may also carry icon data).
            if preferImageFileURLs, urls.count == 1,
               UTType(filenameExtension: urls[0].pathExtension)?.conforms(to: .image) == true {
                guard AppPreferences.recordsImages else { return nil }
                if let image = Self.readImageFileURL(
                    from: pasteboard,
                    maximumByteCount: AppPreferences.maximumImageByteCount
                ) ?? readImage(from: pasteboard) {
                    return image
                }
            }
            let files = Self.readFileReferences(from: pasteboard)
            return files.isEmpty ? nil : .files(files)
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
        let maximumByteCount = AppPreferences.maximumImageByteCount

        for type in Self.imageTypes(in: pasteboard) {
            guard let data = pasteboard.data(forType: type) else { continue }
            if let payload = Self.normalizedImagePayload(
                data: data,
                type: type,
                maximumByteCount: maximumByteCount
            ) {
                return payload
            }
        }

        // File copies must stay references rather than AppKit-rendered icons.
        guard pasteboard.availableType(from: [.fileURL]) == nil else { return nil }

        // Some applications expose only a custom pasteboard representation
        // understood by AppKit rather than a registered public image UTI.
        if let image = NSImage(pasteboard: pasteboard),
           let tiff = image.tiffRepresentation,
           let payload = Self.normalizedImagePayload(
               data: tiff,
               type: .tiff,
               maximumByteCount: maximumByteCount
           ) {
            return payload
        }

        return nil
    }

    private static func fileURLs(from pasteboard: NSPasteboard) -> [URL] {
        (pasteboard.readObjects(
            forClasses: [NSURL.self],
            options: [.urlReadingFileURLsOnly: true]
        ) as? [URL] ?? []).filter(\.isFileURL)
    }

    private static func readFileReferences(from pasteboard: NSPasteboard) -> [ClipboardFileReference] {
        let urls = fileURLs(from: pasteboard)
        var seen = Set<String>()
        return urls.filter { $0.isFileURL && seen.insert($0.standardizedFileURL.path).inserted }.map { url in
            let url = url.standardizedFileURL
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .contentTypeKey, .nameKey])
            let directory = values?.isDirectory ?? url.hasDirectoryPath
            let type = values?.contentType ?? (directory ? .folder : UTType(filenameExtension: url.pathExtension))
            // Generic type icons avoid reading file contents or generating thumbnails.
            let icon = NSWorkspace.shared.icon(for: type ?? .data)
            icon.size = NSSize(width: 32, height: 32)
            return ClipboardFileReference(
                path: url.path,
                name: values?.name ?? url.lastPathComponent,
                typeIdentifier: type?.identifier ?? UTType.data.identifier,
                isDirectory: directory,
                iconData: icon.tiffRepresentation
            )
        }
    }

    private static func imageTypes(in pasteboard: NSPasteboard) -> [NSPasteboard.PasteboardType] {
        let preferred = [
            NSPasteboard.PasteboardType.png,
            .tiff,
            NSPasteboard.PasteboardType(UTType.jpeg.identifier),
            NSPasteboard.PasteboardType(UTType.heic.identifier),
            NSPasteboard.PasteboardType(UTType.webP.identifier),
            NSPasteboard.PasteboardType(UTType.gif.identifier),
        ]
        let available = pasteboard.types ?? []
        return (preferred + available.filter { type in
            UTType(type.rawValue)?.conforms(to: .image) == true
        }).reduce(into: []) { result, type in
            if available.contains(type), !result.contains(type) {
                result.append(type)
            }
        }
    }

    private static func readImageFileURL(
        from pasteboard: NSPasteboard,
        maximumByteCount: Int
    ) -> ClipboardPayload? {
        let options: [NSPasteboard.ReadingOptionKey: Any] = [
            .urlReadingFileURLsOnly: true,
        ]
        guard let objects = pasteboard.readObjects(
            forClasses: [NSURL.self],
            options: options
        ) as? [NSURL] else { return nil }

        let maximumSourceByteCount = max(
            64 * 1_024 * 1_024,
            min(maximumByteCount, 32 * 1_024 * 1_024) * 8
        )
        for object in objects {
            let url = object as URL
            guard url.isFileURL,
                  let type = UTType(filenameExtension: url.pathExtension),
                  type.conforms(to: .image),
                  let values = try? url.resourceValues(forKeys: [.fileSizeKey]),
                  let fileSize = values.fileSize,
                  fileSize > 0,
                  fileSize <= maximumSourceByteCount,
                  let data = try? Data(contentsOf: url, options: .mappedIfSafe),
                  let payload = normalizedImagePayload(
                      data: data,
                      type: NSPasteboard.PasteboardType(type.identifier),
                      maximumByteCount: maximumByteCount
                  ) else { continue }
            return payload
        }
        return nil
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

        if type != .png {
            let maximumDimension = max(width, height)
            guard let image = CGImageSourceCreateThumbnailAtIndex(
                      source,
                      0,
                      [
                          kCGImageSourceCreateThumbnailFromImageAlways: true,
                          kCGImageSourceCreateThumbnailWithTransform: true,
                          kCGImageSourceThumbnailMaxPixelSize: maximumDimension,
                      ] as CFDictionary
                  ),
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
                width: image.width,
                height: image.height,
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
