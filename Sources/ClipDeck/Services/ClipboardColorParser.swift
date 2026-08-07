import AppKit

enum ClipboardColorParser {
    static func color(from source: String) -> NSColor? {
        let value = source.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if value.hasPrefix("#") {
            return hexColor(String(value.dropFirst()))
        }
        if [6, 8].contains(value.count), value.allSatisfy(\.isHexDigit) {
            return hexColor(value)
        }
        guard value.hasPrefix("rgb"),
              let opening = value.firstIndex(of: "("), value.last == ")" else { return nil }
        let components = value[value.index(after: opening)..<value.index(before: value.endIndex)]
            .split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        guard components.count == 3 || components.count == 4,
              let red = channel(components[0]),
              let green = channel(components[1]),
              let blue = channel(components[2]) else { return nil }
        let alpha: CGFloat
        if components.count == 4 {
            guard let parsedAlpha = alphaChannel(components[3]) else { return nil }
            alpha = parsedAlpha
        } else {
            alpha = 1
        }
        return NSColor(calibratedRed: red, green: green, blue: blue, alpha: alpha)
    }

    private static func hexColor(_ value: String) -> NSColor? {
        guard [3, 4, 6, 8].contains(value.count),
              value.allSatisfy(\.isHexDigit) else { return nil }
        let expanded = value.count <= 4
            ? value.map { "\($0)\($0)" }.joined()
            : value
        guard let number = UInt64(expanded, radix: 16) else { return nil }
        let hasAlpha = expanded.count == 8
        let red = CGFloat((number >> (hasAlpha ? 24 : 16)) & 0xff) / 255
        let green = CGFloat((number >> (hasAlpha ? 16 : 8)) & 0xff) / 255
        let blue = CGFloat((number >> (hasAlpha ? 8 : 0)) & 0xff) / 255
        let alpha = hasAlpha ? CGFloat(number & 0xff) / 255 : 1
        return NSColor(calibratedRed: red, green: green, blue: blue, alpha: alpha)
    }

    private static func channel(_ value: String) -> CGFloat? {
        if value.hasSuffix("%"), let percent = Double(value.dropLast()), (0 ... 100).contains(percent) {
            return CGFloat(percent / 100)
        }
        guard let number = Double(value), (0 ... 255).contains(number) else { return nil }
        return CGFloat(number / 255)
    }

    private static func alphaChannel(_ value: String) -> CGFloat? {
        if value.hasSuffix("%"), let percent = Double(value.dropLast()), (0 ... 100).contains(percent) {
            return CGFloat(percent / 100)
        }
        guard let number = Double(value), (0 ... 1).contains(number) else { return nil }
        return CGFloat(number)
    }
}
