import AppKit

// Read-only by default. --write-reference explicitly replaces the general
// pasteboard with the same files in reference form for manual receiver testing.
// Never prints file names, paths or file contents.
func referenceEntry(_ url: URL) -> NSPasteboardItem {
    let entry = NSPasteboardItem()
    let raw = CFURLCreateFileReferenceURL(nil, url as CFURL, nil).map {
        CFURLGetString($0.takeRetainedValue()) as String
    } ?? url.absoluteString
    entry.setString(raw, forType: .fileURL)
    return entry
}
let board = NSPasteboard.general
print("changeCount=\(board.changeCount)")
print("types=\(board.types?.map(\.rawValue).joined(separator: ",") ?? "none")")
let entries = board.pasteboardItems ?? []
print("itemCount=\(entries.count)")
for (index, entry) in entries.enumerated() {
    print("item[\(index)].types=\(entry.types.map(\.rawValue).joined(separator: ","))")
    if let raw = entry.string(forType: .fileURL) {
        let url = URL(string: raw)
        print("item[\(index)].fileURL: valid=\(url?.isFileURL == true), referenceURL=\(raw.contains("/.file/id=")), readable=\(url.map { FileManager.default.isReadableFile(atPath: $0.path) } ?? false), bytes=\(entry.data(forType: .fileURL)?.count ?? 0)")
    }
}
let urls = board.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
print("nativeURLCount=\(urls.count), readableURLCount=\(urls.filter { FileManager.default.isReadableFile(atPath: $0.path) }.count)")
if !urls.isEmpty {
    let referenceBoard = NSPasteboard.withUniqueName()
    referenceBoard.writeObjects(urls.map(referenceEntry))
    print("writer=reference, fileURLBytesMatchFinder=\(referenceBoard.data(forType: .fileURL) == board.data(forType: .fileURL)), referenceURL=\(referenceBoard.string(forType: .fileURL)?.contains("/.file/id=") == true)")
    referenceBoard.releaseGlobally()
    let local = NSPasteboard.withUniqueName()
    local.writeObjects(urls.map { $0 as NSURL })
    print("writer=native, fileURLBytesMatchFinder=\(local.data(forType: .fileURL) == board.data(forType: .fileURL)), nativeReadable=\(local.canReadObject(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]))")
    local.releaseGlobally()
}
if let app = Bundle(path: "/Applications/ClipDeck.app") {
    print("installedVersion=\(app.infoDictionary?["CFBundleShortVersionString"] ?? "unknown"), build=\(app.infoDictionary?["CFBundleVersion"] ?? "unknown")")
}
if CommandLine.arguments.contains("--write-reference"), !urls.isEmpty {
    let restored = urls.map(referenceEntry)
    board.clearContents()
    print("referenceFilesWritten=\(board.writeObjects(restored)), count=\(restored.count)")
}
