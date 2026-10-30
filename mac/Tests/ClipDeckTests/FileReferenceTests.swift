import AppKit
import SwiftUI
import XCTest
@testable import ClipDeck

@MainActor
final class FileReferenceTests: XCTestCase {
    private let source = SourceApplication(bundleIdentifier: "com.apple.finder", name: "Finder", iconData: nil)

    private func reference(_ path: String, directory: Bool = false) -> ClipboardFileReference {
        ClipboardFileReference(path: path, name: URL(fileURLWithPath: path).lastPathComponent,
                               typeIdentifier: directory ? "public.folder" : "public.data",
                               isDirectory: directory, iconData: nil)
    }

    func testNativeFileCopyCapturesAllReferencesWithoutReadingContents() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let photo = directory.appendingPathComponent("图片 with spaces.png")
        // This isn't valid image data: a Finder file copy records its URL, not its bytes.
        try Data("not an image".utf8).write(to: photo)
        let folder = directory.appendingPathComponent("Project", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let board = NSPasteboard(name: .init("ClipDeckTests.files.\(UUID())"))
        board.clearContents()
        XCTAssertTrue(board.writeObjects([photo as NSURL, folder as NSURL]))
        guard case let .files(files) = ClipboardReader().read(from: board) else {
            return XCTFail("Expected references for Finder's native file URLs")
        }
        XCTAssertEqual(files.map(\.path), [photo.path, folder.path])
        XCTAssertEqual(files.map(\.isDirectory), [false, true])
        XCTAssertFalse(ClipboardFileReference.encoded(files).isEmpty)
        XCTAssertNotNil(files.first?.iconData)
    }

    func testFilesPersistSearchDeduplicateAndRemainVisibleWhenSourcesDisappear() throws {
        let repository = HistoryRepository(database: try ClipDeckDatabase(path: ":memory:"))
        let a = reference("/not-mounted/共享/季度报告.pdf")
        let b = reference("/not-mounted/Project", directory: true)
        let first = try repository.save(ClipboardItem(payload: .files([a, b]), sourceApplication: source))
        let second = try repository.save(ClipboardItem(payload: .files([b, a]), sourceApplication: source))
        XCTAssertEqual(first.databaseID, second.databaseID)
        let restored = try repository.fetchRecent()
        XCTAssertEqual(restored.count, 1)
        XCTAssertEqual(restored.first?.payload, .files([a, b]))
        XCTAssertEqual(try repository.searchText("季度报告").count, 1)
        XCTAssertEqual(try repository.searchText("not-mounted").count, 1)

        let store = InMemoryHistoryStore()
        store.replaceItems(restored)
        store.selectCategory(.files)
        XCTAssertEqual(store.displayedItems.count, 1)
        store.selectCategory(.text)
        XCTAssertTrue(store.displayedItems.isEmpty)
        store.selectCategory(.all)
        store.beginSearch()
        store.searchQuery = "季度报告"
        XCTAssertEqual(store.applyLocalSearch(store.searchQuery).count, 1)
        store.selectCategory(.files)
        XCTAssertEqual(store.displayedItems.count, 1)
        store.beginEditingSelection()
        XCTAssertNil(store.editingItemID)
        try repository.deleteExpired(retentionDays: 30)
        XCTAssertEqual(try repository.fetchRecent().count, 1)
    }

    func testFileURLsTakePrecedenceOverApplicationIconImages() throws {
        let defaults = UserDefaults.standard
        let previous = defaults.object(forKey: AppPreferences.recordsImagesKey)
        defaults.set(true, forKey: AppPreferences.recordsImagesKey)
        defer {
            if let previous { defaults.set(previous, forKey: AppPreferences.recordsImagesKey) }
            else { defaults.removeObject(forKey: AppPreferences.recordsImagesKey) }
        }
        let bitmap = try XCTUnwrap(NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: 16, pixelsHigh: 16,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
        ))
        let png = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
        let tiff = try XCTUnwrap(bitmap.tiffRepresentation)
        let selections = [
            [URL(fileURLWithPath: "/tmp/document.pdf")],
            [URL(fileURLWithPath: "/tmp/Folder", isDirectory: true)],
            [URL(fileURLWithPath: "/tmp/photo.png")],
            [URL(fileURLWithPath: "/tmp/photo.png"), URL(fileURLWithPath: "/tmp/document.pdf")],
        ]
        for urls in selections {
            let board = NSPasteboard(name: .init("ClipDeckTests.file-icon.\(UUID())"))
            board.clearContents()
            XCTAssertTrue(board.writeObjects(urls.map { $0 as NSURL }))
            XCTAssertTrue(board.setData(png, forType: .png))
            XCTAssertTrue(board.setData(tiff, forType: .tiff))
            guard case let .files(files) = ClipboardReader().read(from: board) else {
                XCTFail("File URLs must win over icon PNG/TIFF: \(urls)")
                continue
            }
            XCTAssertEqual(files.map(\.path), urls.map(\.path))
            // Even a messaging app's document or mixed selection is a file copy.
            if urls.count > 1 || urls[0].pathExtension != "png" {
                guard case .files = ClipboardReader().read(from: board, preferImageFileURLs: true) else {
                    XCTFail("The photo exception must not consume documents or folders")
                    continue
                }
            }
        }
    }

    func testPreflightIdentifiesMissingSubsetWithoutChangingPasteboard() throws {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try Data("original".utf8).write(to: file)
        defer { try? FileManager.default.removeItem(at: file) }
        let valid = reference(file.path)
        let missing = reference(file.path + ".missing")
        let board = NSPasteboard(name: .init("ClipDeckTests.preflight.\(UUID())"))
        board.clearContents()
        board.setString("keep this", forType: .string)
        let count = board.changeCount
        let result = FileReferenceService.check([valid, missing])
        XCTAssertEqual(result.available, [valid])
        XCTAssertEqual(result.unavailable, [missing])
        XCTAssertEqual(board.changeCount, count)
        XCTAssertEqual(board.string(forType: .string), "keep this")
        let empty = ClipboardItem(payload: .files([]), sourceApplication: source)
        XCTAssertFalse(PasteService().write(empty, to: board, preserveFormatting: false))
        XCTAssertEqual(board.changeCount, count)
    }

    func testWritingFileHistoryRestoresNativeURLsAndMetadataDoesNotAffectIdentity() throws {
        let files = [reference("/tmp/甲 with spaces.pdf"), reference("/tmp/Project", directory: true)]
        let board = NSPasteboard(name: .init("ClipDeckTests.write-files.\(UUID())"))
        let item = ClipboardItem(payload: .files(files), sourceApplication: source)
        XCTAssertTrue(PasteService().write(item, to: board, preserveFormatting: false))
        let urls = board.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]
        XCTAssertEqual(urls?.map(\.path), files.map(\.path))
        XCTAssertFalse(board.pasteboardItems?.contains {
            $0.types.contains(.init("NSFilenamesPboardType"))
        } ?? true)
        XCTAssertEqual(board.pasteboardItems?.count, files.count)
        XCTAssertNil(board.string(forType: .string))
        XCTAssertNil(board.data(forType: .png))
        XCTAssertNil(board.data(forType: .tiff))
        XCTAssertEqual(ClipboardFileReference.identityData(files), ClipboardFileReference.identityData(files.reversed()))
    }

    func testEscapeDismissesFileFeedbackBeforeHidingDeck() {
        let store = InMemoryHistoryStore()
        let item = ClipboardItem(payload: .files([reference("/tmp/missing")]), sourceApplication: source)
        store.insert(item)
        let controller = PanelController(store: store, onPaste: { _, _ in },
                                         onManageCategories: {}, onSearch: { _ in })
        controller.show(targetsClipDeck: true)
        store.filePasteFeedback = FilePasteFeedback(itemID: item.id, requestID: UUID(),
                                                    phase: .unavailable("无法粘贴，源文件不可访问"))
        let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [],
                                    timestamp: 0, windowNumber: controller.window!.windowNumber,
                                    context: nil, characters: "\u{1b}", charactersIgnoringModifiers: "\u{1b}",
                                    isARepeat: false, keyCode: 53)!
        controller.window?.sendEvent(event)
        XCTAssertNil(store.filePasteFeedback)
        XCTAssertTrue(controller.isVisible)
        controller.hide()
    }

    func testPartialPasteRequiresConfirmationAndOnlyWritesConfirmedFiles() async throws {
        let store = InMemoryHistoryStore()
        let valid = reference("/tmp/valid.pdf")
        let missing = reference("/tmp/missing.pdf")
        let item = ClipboardItem(payload: .files([valid, missing]), sourceApplication: source)
        store.insert(item)
        let board = NSPasteboard(name: .init("ClipDeckTests.confirm.\(UUID())"))
        board.clearContents()
        board.setString("original copy", forType: .string)
        let controller = FilePasteController(store: store, pasteboard: board, checker: { files in
            .init(available: files.filter { $0 == valid }, unavailable: files.filter { $0 == missing })
        }, isPresented: { true })
        var writes = 0
        let onReady: @MainActor (ClipboardItem) -> Void = { selected in
            writes += 1
            _ = PasteService().write(selected, to: board, preserveFormatting: false)
        }
        controller.prepare(item, onReady: onReady)
        try await waitUntil { store.filePasteFeedback?.phase != .checking }
        guard case .partial = store.filePasteFeedback?.phase else { return XCTFail("Expected confirmation") }
        XCTAssertEqual(writes, 0)
        XCTAssertEqual(board.string(forType: .string), "original copy")
        controller.prepare(item, onReady: onReady)
        try await waitUntil { writes == 1 }
        let urls = board.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]
        XCTAssertEqual(urls?.map(\.path), [valid.path])
    }

    func testTimeoutIgnoresLateResultAndNewClipboardContentIsProtected() async throws {
        let store = InMemoryHistoryStore()
        let file = reference("/tmp/slow-network-file")
        let item = ClipboardItem(payload: .files([file]), sourceApplication: source)
        store.insert(item)
        let board = NSPasteboard(name: .init("ClipDeckTests.timeout.\(UUID())"))
        board.clearContents()
        board.setString("original copy", forType: .string)
        let controller = FilePasteController(store: store, pasteboard: board, timeout: 0.01, checker: { files in
            Thread.sleep(forTimeInterval: 0.08)
            return .init(available: files, unavailable: [])
        }, isPresented: { true })
        var ready = false
        controller.prepare(item) { _ in ready = true }
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertFalse(ready)
        guard case let .unavailable(message) = store.filePasteFeedback?.phase else { return XCTFail("Expected timeout") }
        XCTAssertTrue(message.contains("超时"))
        XCTAssertEqual(board.string(forType: .string), "original copy")

        store.filePasteFeedback = nil
        let changed = FilePasteController(store: store, pasteboard: board, checker: { files in
            Thread.sleep(forTimeInterval: 0.05)
            return .init(available: files, unavailable: [])
        }, isPresented: { true })
        changed.prepare(item) { _ in ready = true }
        board.clearContents()
        board.setString("new copy", forType: .string)
        try await waitUntil { store.filePasteFeedback?.phase != .checking }
        XCTAssertFalse(ready)
        XCTAssertEqual(board.string(forType: .string), "new copy")
    }

    func testSwitchingCardsCancelsPendingFilePaste() async throws {
        let store = InMemoryHistoryStore()
        let item = ClipboardItem(payload: .files([reference("/tmp/slow-file")]), sourceApplication: source)
        store.insert(item)
        let controller = FilePasteController(store: store, checker: { files in
            Thread.sleep(forTimeInterval: 0.05)
            return .init(available: files, unavailable: [])
        }, isPresented: { true })
        var ready = false
        controller.prepare(item) { _ in ready = true }
        store.insert(ClipboardItem(payload: .text(plainText: "new text", rtfData: nil), sourceApplication: source))
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertFalse(ready)
        XCTAssertNil(store.filePasteFeedback)
    }

    private func waitUntil(_ done: () -> Bool) async throws {
        for _ in 0..<100 {
            if done() { return }
            try await Task.sleep(for: .milliseconds(10))
        }
        XCTFail("Timed out waiting for file paste completion")
    }

    // Opt-in captures render the production SwiftUI view for visual review.
    func testRenderFileCardReview() throws {
        guard let directory = ProcessInfo.processInfo.environment["CLIPDECK_RENDER_DIRECTORY"] else { return }
        let output = URL(fileURLWithPath: directory)
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        let files = [reference("/Volumes/Share/A.pdf"), reference("/Volumes/Share/B.png"),
                     reference("/Volumes/Share/Project", directory: true)]
        let store = InMemoryHistoryStore()
        let item = ClipboardItem(payload: .files(files), sourceApplication: source,
                                 copiedAt: Date().addingTimeInterval(-720))
        store.insert(item)
        store.replaceCategories([
            ClipboardCategory(id: 1, name: "JSON", icon: nil, script: "", previewLanguage: "json", sortOrder: 0),
            ClipboardCategory(id: 2, name: "SQL", icon: nil, script: "", previewLanguage: "sql", sortOrder: 1),
            ClipboardCategory(id: 3, name: "URL", icon: nil, script: "", previewLanguage: "url", sortOrder: 2),
        ])
        store.selectCategory(.files)
        let view = NSHostingView(rootView: HistoryPanelView(store: store, onPaste: { _, _ in }, onClose: {},
                                                           onManageCategories: {}, onSearch: { _ in }))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1120, height: 420),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: .darkAqua)
        window.contentView = view
        window.orderFront(nil)
        defer { window.orderOut(nil) }
        let states: [(String, FilePasteFeedback.Phase?)] = [
            ("files-normal", nil),
            ("files-unavailable", .unavailable("无法粘贴，源文件不可访问")),
            ("files-partial", .partial(available: Array(files.prefix(2)), unavailable: [files[2]])),
        ]
        for (name, phase) in states {
            store.filePasteFeedback = phase.map { FilePasteFeedback(itemID: item.id, requestID: UUID(), phase: $0) }
            RunLoop.current.run(until: Date().addingTimeInterval(0.25))
            view.layoutSubtreeIfNeeded()
            let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
            view.cacheDisplay(in: view.bounds, to: bitmap)
            try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(
                to: output.appendingPathComponent("\(name).png")
            )
        }
        func capture(_ target: NSView, name: String) throws {
            RunLoop.current.run(until: Date().addingTimeInterval(0.25))
            target.layoutSubtreeIfNeeded()
            let bitmap = try XCTUnwrap(target.bitmapImageRepForCachingDisplay(in: target.bounds))
            target.cacheDisplay(in: target.bounds, to: bitmap)
            try XCTUnwrap(bitmap.representation(using: .png, properties: [:])).write(
                to: output.appendingPathComponent("\(name).png")
            )
        }
        let examples = [
            ("single-file", [reference("/Users/example/Projects/openVPN-handle.py")]),
            ("six-files", [
                reference("/Users/example/Downloads/ClipDeck_0.9.2_aarch64.dmg"),
                reference("/Users/example/Downloads/show_table_info.xlsx"),
                reference("/Users/example/Downloads/sm3_otp.user.js"),
                reference("/Users/example/Downloads/DKEY+Token_4.5.3_APKPure", directory: true),
                reference("/Users/example/Downloads/数据密集型应用系统设计 (Martin Kleppmann) (z-library.sk, 1lib.sk, z-lib.sk).epub"),
                reference("/Users/example/Downloads/说明文档.pdf"),
            ]),
            ("twelve-files", (1...12).map { reference("/Volumes/Share/报告/季度报告-\($0).pdf") }),
        ]
        for (name, references) in examples {
            let example = ClipboardItem(payload: .files(references), sourceApplication: source)
            store.replaceItems([example])
            store.filePasteFeedback = nil
            window.contentView = view
            window.setContentSize(NSSize(width: 1120, height: name == "six-files" ? 351 : 420))
            try capture(view, name: "\(name)-card")
            if name == "twelve-files" {
                for height in [250, 351, 420] {
                    window.setContentSize(NSSize(width: 1120, height: height))
                    try capture(view, name: "twelve-files-height-\(height)")
                }
            }
            if references.count == 1 {
                store.filePasteFeedback = FilePasteFeedback(itemID: example.id, requestID: UUID(),
                                                            phase: .unavailable("无法粘贴，源文件不可访问"))
                try capture(view, name: "\(name)-unavailable")
            }
            let preview = NSHostingView(rootView: ContentPreviewView(item: example))
            window.contentView = preview
            window.setContentSize(NSSize(width: 560, height: 360))
            try capture(preview, name: "\(name)-preview")
            if references.count > 1 {
                for _ in 1..<references.count {
                    NotificationCenter.default.post(name: .clipDeckPreviewScroll, object: nil,
                                                    userInfo: ["direction": 1])
                }
                try capture(preview, name: "\(name)-preview-end")
            }
        }
    }
}
