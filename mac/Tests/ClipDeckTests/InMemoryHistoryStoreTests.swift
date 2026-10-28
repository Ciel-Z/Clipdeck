import AppKit
import UniformTypeIdentifiers
import XCTest
@testable import ClipDeck

@MainActor
final class InMemoryHistoryStoreTests: XCTestCase {
    private let source = SourceApplication(
        bundleIdentifier: "com.example.editor",
        name: "Editor",
        iconData: nil
    )

    func testNewestItemIsSelectedAndCapacityIsEnforced() {
        let store = InMemoryHistoryStore(capacity: 2)
        let first = item("first")
        let second = item("second")
        let third = item("third")

        store.insert(first)
        store.insert(second)
        store.insert(third)

        XCTAssertEqual(store.items.map(\.id), [third.id, second.id])
        XCTAssertEqual(store.selectedItemID, third.id)
    }

    func testExpandingPersistedHistoryPreservesPriorityItemIdentityAndSelection() {
        let store = InMemoryHistoryStore()
        let priority = ClipboardItem(
            databaseID: 1,
            payload: .text(plainText: "priority", rtfData: nil),
            sourceApplication: source
        )
        store.replaceItems([priority])
        let reloadedPriority = ClipboardItem(
            databaseID: 1,
            payload: priority.payload,
            sourceApplication: source,
            copiedAt: priority.copiedAt
        )
        let older = ClipboardItem(
            databaseID: 2,
            payload: .text(plainText: "older", rtfData: nil),
            sourceApplication: source,
            copiedAt: priority.copiedAt.addingTimeInterval(-60)
        )

        store.expandPersistedItems([reloadedPriority, older])

        XCTAssertEqual(store.items.map(\.databaseID), [1, 2])
        XCTAssertEqual(store.items.first?.id, priority.id)
        XCTAssertEqual(store.selectedItemID, priority.id)
    }

    func testDuplicatePayloadMovesNewestCopyToFront() {
        let store = InMemoryHistoryStore()
        let original = item("same")
        let other = item("other")
        let duplicate = item("same")

        store.insert(original)
        store.insert(other)
        store.insert(duplicate)

        XCTAssertEqual(store.items.count, 2)
        XCTAssertEqual(store.items.first?.id, duplicate.id)
    }

    func testReconcilingCapturedItemPreservesIdentityPositionAndSelection() {
        let store = InMemoryHistoryStore()
        let captured = item("captured")
        store.insert(captured)
        let persisted = ClipboardItem(
            id: captured.id,
            databaseID: 42,
            payload: captured.payload,
            sourceApplication: captured.sourceApplication,
            copiedAt: captured.copiedAt
        )

        XCTAssertEqual(store.reconcileCapturedItem(persisted), .reconciled)
        XCTAssertEqual(store.items.first?.databaseID, 42)
        XCTAssertEqual(store.items.first?.id, captured.id)
        XCTAssertEqual(store.selectedItemID, captured.id)
    }

    func testReconcilingDeletedTransientCaptureDoesNotRestoreIt() {
        let store = InMemoryHistoryStore()
        let captured = item("captured")
        store.insert(captured)
        store.deleteSelection()

        XCTAssertEqual(store.reconcileCapturedItem(captured), .removed)
        XCTAssertTrue(store.items.isEmpty)
    }

    func testOlderCaptureIsSupersededByNewerDuplicate() {
        let store = InMemoryHistoryStore()
        let older = item("duplicate")
        let newer = item("duplicate")
        store.insert(older)
        store.insert(newer)

        XCTAssertEqual(store.reconcileCapturedItem(older), .superseded)
        XCTAssertEqual(store.items.map(\.id), [newer.id])
    }

    func testSelectionNavigationAndDeletion() {
        let store = InMemoryHistoryStore()
        let older = item("older")
        let newer = item("newer")
        store.insert(older)
        store.insert(newer)

        store.selectNext()
        XCTAssertEqual(store.selectedItemID, older.id)
        store.deleteSelection()
        XCTAssertEqual(store.items, [newer])
        XCTAssertEqual(store.selectedItemID, newer.id)
    }

    func testImagePixelLimitAvoidsOverflow() {
        XCTAssertTrue(ClipboardReader.isSafeImageSize(width: 10_000, height: 6_000))
        XCTAssertFalse(ClipboardReader.isSafeImageSize(width: 10_000, height: 6_001))
        XCTAssertFalse(ClipboardReader.isSafeImageSize(width: Int.max, height: Int.max))
    }

    func testTIFFIsCompressedBeforeApplyingStorageLimit() throws {
        let bitmap = try XCTUnwrap(NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: 100,
            pixelsHigh: 100,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0
        ))
        let tiff = try XCTUnwrap(bitmap.tiffRepresentation)
        let png = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
        XCTAssertGreaterThan(tiff.count, png.count)

        let payload = ClipboardReader.normalizedImagePayload(
            data: tiff,
            type: .tiff,
            maximumByteCount: tiff.count - 1
        )

        guard case let .image(data, width, height, format) = payload else {
            return XCTFail("Expected a normalized image payload")
        }
        XCTAssertEqual(format, .png)
        XCTAssertEqual(width, 100)
        XCTAssertEqual(height, 100)
        XCTAssertLessThan(data.count, tiff.count)
    }

    func testClipboardReaderPrefersImageWhenFileURLIsAlsoPresent() throws {
        let pasteboard = NSPasteboard(name: .init("ClipDeckTests.wechat.\(UUID())"))
        let tiff = try testImageTIFF()
        pasteboard.clearContents()
        XCTAssertTrue(pasteboard.setString("file:///tmp/wechat-photo.jpg", forType: .fileURL))
        XCTAssertTrue(pasteboard.setData(tiff, forType: .tiff))

        let payload = try withImageRecordingEnabled {
            try XCTUnwrap(ClipboardReader().read(from: pasteboard))
        }

        guard case let .image(_, width, height, format) = payload else {
            return XCTFail("Expected the image representation to win over the file URL")
        }
        XCTAssertEqual(width, 8)
        XCTAssertEqual(height, 4)
        XCTAssertEqual(format, .png)
    }

    func testClipboardReaderImportsImageFromTemporaryFileURL() throws {
        let fileURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("clipdeck-wechat-\(UUID().uuidString).png")
        let png = try testImagePNG()
        try png.write(to: fileURL, options: .atomic)
        defer { try? FileManager.default.removeItem(at: fileURL) }

        let pasteboard = NSPasteboard(name: .init("ClipDeckTests.wechat-file.\(UUID())"))
        pasteboard.clearContents()
        XCTAssertTrue(pasteboard.writeObjects([fileURL as NSURL]))

        let payload = try withImageRecordingEnabled {
            try XCTUnwrap(ClipboardReader().read(from: pasteboard))
        }

        guard case let .image(_, width, height, format) = payload else {
            return XCTFail("Expected an image loaded from the temporary file URL")
        }
        XCTAssertEqual(width, 8)
        XCTAssertEqual(height, 4)
        XCTAssertEqual(format, .png)
    }

    func testClipboardReaderImportsJPEGRepresentation() throws {
        let jpeg = try XCTUnwrap(testBitmap().representation(
            using: .jpeg,
            properties: [.compressionFactor: 0.8]
        ))
        let pasteboard = NSPasteboard(name: .init("ClipDeckTests.jpeg.\(UUID())"))
        pasteboard.clearContents()
        XCTAssertTrue(pasteboard.setData(
            jpeg,
            forType: NSPasteboard.PasteboardType(UTType.jpeg.identifier)
        ))

        let payload = try withImageRecordingEnabled {
            try XCTUnwrap(ClipboardReader().read(from: pasteboard))
        }

        guard case let .image(_, width, height, format) = payload else {
            return XCTFail("Expected JPEG pasteboard data to be normalized")
        }
        XCTAssertEqual(width, 8)
        XCTAssertEqual(height, 4)
        XCTAssertEqual(format, .png)
    }

    func testCategorySwitchingFiltersItemsAndKeepsAVisibleSelection() {
        let store = InMemoryHistoryStore()
        let text = item("text")
        let image = ClipboardItem(
            payload: .image(data: Data([0x00]), width: 1, height: 1, format: .png),
            sourceApplication: source
        )
        store.insert(text)
        store.insert(image)

        store.selectNextCategory()
        XCTAssertEqual(store.selectedCategory, .text)
        XCTAssertEqual(store.displayedItems.map(\.id), [text.id])
        XCTAssertEqual(store.selectedItemID, text.id)

        store.selectNextCategory()
        XCTAssertEqual(store.selectedCategory, .images)
        XCTAssertEqual(store.displayedItems.map(\.id), [image.id])
        XCTAssertEqual(store.selectedItemID, image.id)

        store.selectNextCategory()
        XCTAssertEqual(store.selectedCategory, .all)
    }

    func testRevealSelectionDoesNotChangeTheRememberedItem() {
        let store = InMemoryHistoryStore()
        let older = item("older")
        let newer = item("newer")
        store.insert(older)
        store.insert(newer)
        store.selectNext()
        let selectedID = store.selectedItemID
        let token = store.selectionRevealToken

        store.revealSelection()

        XCTAssertEqual(store.selectedItemID, selectedID)
        XCTAssertEqual(store.selectionRevealToken, token + 1)
    }

    func testSearchResultsDriveSelectionAndRestoreHistoryWhenClosed() {
        let store = InMemoryHistoryStore()
        let first = item("first")
        let result = item("matching")
        store.insert(first)

        store.beginSearch()
        store.searchQuery = "match"
        store.replaceSearchResults([result])
        XCTAssertEqual(store.displayedItems, [result])
        XCTAssertEqual(store.selectedItem, result)

        store.endSearch()
        XCTAssertEqual(store.displayedItems, [first])
        XCTAssertEqual(store.selectedItem, first)
    }

    func testLocalSearchUpdatesImmediatelyAndExcludesImages() {
        let store = InMemoryHistoryStore()
        let matching = item("Hello Search World")
        let other = item("unrelated")
        let image = ClipboardItem(
            payload: .image(data: Data([0x00]), width: 1, height: 1, format: .png),
            sourceApplication: source
        )
        store.insert(other)
        store.insert(image)
        store.insert(matching)

        store.beginSearch()
        store.searchQuery = "search"
        let results = store.applyLocalSearch(store.searchQuery)

        XCTAssertEqual(results.map(\.id), [matching.id])
        XCTAssertEqual(store.displayedItems.map(\.id), [matching.id])
        XCTAssertEqual(store.selectedItemID, matching.id)
    }

    func testLocalSearchPreservesAVisibleSelectionWhileQueryChanges() {
        let store = InMemoryHistoryStore()
        let first = item("search first")
        let second = item("search second")
        store.insert(first)
        store.insert(second)
        store.beginSearch()
        store.searchQuery = "search"
        store.applyLocalSearch(store.searchQuery)
        store.selectNext()
        XCTAssertEqual(store.selectedItemID, first.id)

        store.searchQuery = "search f"
        store.applyLocalSearch(store.searchQuery)

        XCTAssertEqual(store.selectedItemID, first.id)
    }

    func testFirstSearchCharacterSelectsHistoryButLaterTypingPreservesCategory() {
        let store = InMemoryHistoryStore()
        let image = ClipboardItem(
            payload: .image(data: Data([0x00]), width: 1, height: 1, format: .png),
            sourceApplication: source
        )
        store.insert(item("matching text"))
        store.insert(image)
        store.selectCategory(.images)
        store.beginSearch()

        XCTAssertEqual(store.selectedCategory, .images)
        store.searchQuery = "m"
        XCTAssertEqual(store.selectedCategory, .all)

        store.selectCategory(.text)
        store.searchQuery = "ma"
        XCTAssertEqual(store.selectedCategory, .text)
    }

    func testFinishingSearchEditingKeepsResultsActive() {
        let store = InMemoryHistoryStore()
        let result = item("matching")
        store.insert(result)
        store.beginSearch()
        store.searchQuery = "match"
        store.applyLocalSearch(store.searchQuery)

        store.finishSearchEditing()

        XCTAssertTrue(store.isSearchActive)
        XCTAssertFalse(store.isSearchEditing)
        XCTAssertEqual(store.displayedItems, [result])
        XCTAssertEqual(store.selectedItemID, result.id)
    }

    func testBackspaceResumesSearchEditingAndRemovesTheLastCharacter() {
        let store = InMemoryHistoryStore()
        store.insert(item("matching"))
        store.beginSearch()
        store.searchQuery = "match"
        store.finishSearchEditing()

        store.resumeSearchEditingAndDeleteBackward()

        XCTAssertTrue(store.isSearchEditing)
        XCTAssertEqual(store.searchQuery, "matc")
    }

    func testBackspaceCollapsesSearchAfterDeletingTheLastCharacter() {
        let store = InMemoryHistoryStore()
        let original = item("matching")
        store.insert(original)
        store.beginSearch()
        store.searchQuery = "m"
        store.finishSearchEditing()

        store.resumeSearchEditingAndDeleteBackward()

        XCTAssertFalse(store.isSearchActive)
        XCTAssertFalse(store.isSearchEditing)
        XCTAssertEqual(store.searchQuery, "")
        XCTAssertEqual(store.displayedItems, [original])
    }

    func testPreviewClosesWhenSelectionChanges() {
        let store = InMemoryHistoryStore()
        let older = item("older")
        let newer = item("newer")
        store.insert(older)
        store.insert(newer)
        store.togglePreviewForSelection()
        XCTAssertEqual(store.previewedItemID, newer.id)

        store.selectNext()

        XCTAssertEqual(store.selectedItemID, older.id)
        XCTAssertNil(store.previewedItemID)
    }

    func testEditingReplacesItemAndNotifiesPersistence() {
        let store = InMemoryHistoryStore()
        let original = item("before")
        var persisted: ClipboardItem?
        store.onUpdate = { persisted = $0 }
        store.insert(original)
        store.beginEditing(original)

        store.updateEditingItem(payload: .text(plainText: "after", rtfData: nil))

        XCTAssertEqual(store.items.count, 1)
        XCTAssertEqual(store.items.first?.payload, .text(plainText: "after", rtfData: nil))
        XCTAssertEqual(persisted?.payload, .text(plainText: "after", rtfData: nil))
        XCTAssertNil(store.editingItemID)
        XCTAssertEqual(store.selectedItemID, store.items.first?.id)
    }

    func testImageEditingStoresAngleWithoutChangingImagePayload() {
        let store = InMemoryHistoryStore()
        let original = ClipboardItem(
            payload: .image(data: Data([1, 2, 3]), width: 3, height: 1, format: .png),
            sourceApplication: source
        )
        store.insert(original)
        store.beginEditing(original)

        store.updateEditingItem(payload: original.payload, imageAngle: 90)

        XCTAssertEqual(store.items.first?.payload, original.payload)
        XCTAssertEqual(store.items.first?.imageAngle, 90)
    }

    func testImagePreviewSizeUsesAspectRatioInsteadOfPixelCount() {
        let small = ContentPreviewLayout.imageSize(pixelWidth: 400, pixelHeight: 200, angle: 0)
        let large = ContentPreviewLayout.imageSize(pixelWidth: 8_000, pixelHeight: 4_000, angle: 0)
        let portrait = ContentPreviewLayout.imageSize(pixelWidth: 2_000, pixelHeight: 4_000, angle: 0)

        XCTAssertEqual(small, large)
        XCTAssertGreaterThan(small.width, small.height)
        XCTAssertLessThan(portrait.width, portrait.height)
        XCTAssertLessThanOrEqual(large.width, 560)
        XCTAssertLessThanOrEqual(large.height, 405)
    }

    func testImagePreviewSizeAccountsForRenderedRotation() {
        let landscape = ContentPreviewLayout.imageSize(
            pixelWidth: 4_000,
            pixelHeight: 2_000,
            angle: 0
        )
        let rotated = ContentPreviewLayout.imageSize(
            pixelWidth: 4_000,
            pixelHeight: 2_000,
            angle: 90
        )

        XCTAssertGreaterThan(landscape.width, landscape.height)
        XCTAssertLessThan(rotated.width, rotated.height)
    }

    func testJSONTextFormatterPrettyPrintsValidJSONAndRejectsInvalidInput() {
        XCTAssertEqual(
            JSONTextFormatter.format(#"{"b":2,"a":1}"#),
            "{\n  \"a\" : 1,\n  \"b\" : 2\n}"
        )
        XCTAssertNil(JSONTextFormatter.format("{broken}"))
    }

    func testRenamingPersistsAndTitleParticipatesInLocalSearch() {
        let store = InMemoryHistoryStore()
        let original = item("正文没有关键词")
        var persisted: ClipboardItem?
        store.onUpdate = { persisted = $0 }
        store.insert(original)
        store.beginRenaming(original)

        store.updateRenamingItem(title: "项目代号 Aurora")

        XCTAssertEqual(store.items.first?.title, "项目代号 Aurora")
        XCTAssertEqual(persisted?.title, "项目代号 Aurora")
        XCTAssertNil(store.renamingItemID)
        XCTAssertEqual(store.applyLocalSearch("Aurora").first?.title, "项目代号 Aurora")
    }

    func testQuickPasteOverlayTracksCommandModifierState() {
        let store = InMemoryHistoryStore()

        store.setQuickPasteOverlayVisible(true)
        XCTAssertTrue(store.isQuickPasteOverlayVisible)

        store.setQuickPasteOverlayVisible(false)
        XCTAssertFalse(store.isQuickPasteOverlayVisible)
    }

    private func item(_ text: String) -> ClipboardItem {
        ClipboardItem(payload: .text(plainText: text, rtfData: nil), sourceApplication: source)
    }

    private func testImageTIFF() throws -> Data {
        try XCTUnwrap(testBitmap().tiffRepresentation)
    }

    private func testImagePNG() throws -> Data {
        try XCTUnwrap(testBitmap().representation(using: .png, properties: [:]))
    }

    private func testBitmap() throws -> NSBitmapImageRep {
        try XCTUnwrap(NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: 8,
            pixelsHigh: 4,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0
        ))
    }

    private func withImageRecordingEnabled<T>(_ body: () throws -> T) rethrows -> T {
        let defaults = UserDefaults.standard
        let previousRecordsImages = defaults.object(forKey: AppPreferences.recordsImagesKey)
        let previousMaximumMegabytes = defaults.object(forKey: AppPreferences.maximumImageMegabytesKey)
        defaults.set(true, forKey: AppPreferences.recordsImagesKey)
        defaults.set(10, forKey: AppPreferences.maximumImageMegabytesKey)
        defer {
            if let previousRecordsImages {
                defaults.set(previousRecordsImages, forKey: AppPreferences.recordsImagesKey)
            } else {
                defaults.removeObject(forKey: AppPreferences.recordsImagesKey)
            }
            if let previousMaximumMegabytes {
                defaults.set(previousMaximumMegabytes, forKey: AppPreferences.maximumImageMegabytesKey)
            } else {
                defaults.removeObject(forKey: AppPreferences.maximumImageMegabytesKey)
            }
        }
        return try body()
    }
}
