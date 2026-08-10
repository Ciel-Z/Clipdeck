import Foundation
import XCTest
@testable import ClipDeck

final class HistoryRepositoryTests: XCTestCase {
    private let source = SourceApplication(
        bundleIdentifier: "com.example.editor",
        name: "Editor",
        iconData: Data([1, 2, 3])
    )

    func testTextAndImageRoundTripAndDelete() throws {
        let repository = try makeRepository()
        let text = try repository.save(ClipboardItem(
            payload: .text(plainText: "hello", rtfData: Data([4, 5])),
            sourceApplication: source
        ))
        _ = try repository.save(ClipboardItem(
            payload: .image(data: Data([6, 7]), width: 2, height: 1, format: .png),
            sourceApplication: source
        ))

        let items = try repository.fetchRecent()
        XCTAssertEqual(items.count, 2)
        XCTAssertTrue(items.contains { $0.payload == text.payload })

        try repository.delete(text)
        XCTAssertEqual(try repository.fetchRecent().count, 1)
    }

    func testRecentDuplicateUpdatesExistingRow() throws {
        let repository = try makeRepository()
        _ = try repository.save(ClipboardItem(payload: .text(plainText: "same", rtfData: nil), sourceApplication: source))
        _ = try repository.save(ClipboardItem(payload: .text(plainText: "same", rtfData: nil), sourceApplication: source))

        XCTAssertEqual(try repository.fetchRecent().count, 1)
    }

    func testSameDayLinkCopyReusesFetchedMetadata() throws {
        let repository = try makeRepository()
        let startOfToday = Calendar.current.startOfDay(for: Date())
        let firstCopyDate = startOfToday.addingTimeInterval(3_600)
        let secondCopyDate = startOfToday.addingTimeInterval(7_200)
        let first = try repository.save(ClipboardItem(
            title: "Example",
            linkIconData: Data([1, 2, 3]),
            linkPreviewData: Data([4, 5, 6]),
            hasFetchedLinkPreview: true,
            payload: .text(plainText: "https://example.com/page", rtfData: nil),
            sourceApplication: source,
            copiedAt: firstCopyDate
        ))

        let repeated = try repository.save(ClipboardItem(
            payload: .text(plainText: "https://example.com/page", rtfData: nil),
            sourceApplication: source,
            copiedAt: secondCopyDate
        ))

        XCTAssertEqual(repeated.databaseID, first.databaseID)
        XCTAssertEqual(repeated.title, "Example")
        XCTAssertEqual(repeated.linkIconData, Data([1, 2, 3]))
        XCTAssertEqual(repeated.linkPreviewData, Data([4, 5, 6]))
        XCTAssertTrue(repeated.hasFetchedLinkPreview)
        XCTAssertEqual(try repository.fetchRecent().count, 1)
    }

    func testLinkCopyOnAnotherDayCreatesANewRecord() throws {
        let repository = try makeRepository()
        let startOfToday = Calendar.current.startOfDay(for: Date())
        _ = try repository.save(ClipboardItem(
            title: "Yesterday",
            linkIconData: Data([1]),
            hasFetchedLinkPreview: true,
            payload: .text(plainText: "https://example.com/page", rtfData: nil),
            sourceApplication: source,
            copiedAt: startOfToday.addingTimeInterval(-3_600)
        ))

        _ = try repository.save(ClipboardItem(
            payload: .text(plainText: "https://example.com/page", rtfData: nil),
            sourceApplication: source,
            copiedAt: startOfToday.addingTimeInterval(3_600)
        ))

        XCTAssertEqual(try repository.fetchRecent().count, 2)
    }

    func testExpiredHistoryIsDeleted() throws {
        let repository = try makeRepository()
        _ = try repository.save(ClipboardItem(
            payload: .text(plainText: "old", rtfData: nil),
            sourceApplication: source,
            copiedAt: Date().addingTimeInterval(-31 * 86_400)
        ))

        XCTAssertEqual(try repository.deleteExpired(retentionDays: 30), 1)
        XCTAssertTrue(try repository.fetchRecent().isEmpty)
    }

    func testForeverRetentionDoesNotDeleteHistory() throws {
        let repository = try makeRepository()
        _ = try repository.save(ClipboardItem(
            payload: .text(plainText: "old", rtfData: nil),
            sourceApplication: source,
            copiedAt: Date().addingTimeInterval(-800 * 86_400)
        ))

        XCTAssertEqual(try repository.deleteExpired(retentionDays: 0), 0)
        XCTAssertEqual(try repository.fetchRecent().count, 1)
    }

    func testFullTextSearchIndexesTextButNotImages() throws {
        let repository = try makeRepository()
        _ = try repository.save(ClipboardItem(
            payload: .text(plainText: "SELECT user_name FROM accounts", rtfData: nil),
            sourceApplication: source
        ))
        _ = try repository.save(ClipboardItem(
            payload: .text(plainText: "unrelated markdown", rtfData: nil),
            sourceApplication: source
        ))
        _ = try repository.save(ClipboardItem(
            payload: .image(data: Data("SELECT".utf8), width: 1, height: 1, format: .png),
            sourceApplication: source
        ))

        let matches = try repository.searchText("sel user")
        XCTAssertEqual(matches.count, 1)
        XCTAssertEqual(matches.first?.payload, .text(plainText: "SELECT user_name FROM accounts", rtfData: nil))
        XCTAssertTrue(try repository.searchText("missing").isEmpty)
    }

    func testAsyncFullTextSearchMatchesSynchronousSearch() async throws {
        let repository = try makeRepository()
        let saved = try repository.save(ClipboardItem(
            payload: .text(plainText: "cancelable database search", rtfData: nil),
            sourceApplication: source
        ))

        let matches = try await repository.searchTextAsync("cancelable search")

        XCTAssertEqual(matches.map(\.databaseID), [saved.databaseID])
    }

    func testUpdatingTextRefreshesStoredPayloadAndFullTextIndex() throws {
        let repository = try makeRepository()
        let saved = try repository.save(ClipboardItem(
            payload: .text(plainText: "old searchable value", rtfData: Data([1, 2])),
            sourceApplication: source
        ))
        let updated = ClipboardItem(
            databaseID: saved.databaseID,
            payload: .text(plainText: "new replacement value", rtfData: nil),
            sourceApplication: saved.sourceApplication,
            copiedAt: saved.copiedAt
        )

        try repository.update(updated)

        XCTAssertEqual(try repository.fetchRecent().first?.payload, updated.payload)
        XCTAssertTrue(try repository.searchText("old searchable").isEmpty)
        XCTAssertEqual(try repository.searchText("new replacement").first?.payload, updated.payload)
    }

    func testTitlesDefaultPersistRenameAndAreSearchableForImages() throws {
        let repository = try makeRepository()
        let text = try repository.save(ClipboardItem(
            payload: .text(plainText: "body", rtfData: nil),
            sourceApplication: source
        ))
        let image = try repository.save(ClipboardItem(
            payload: .image(data: Data([9, 8, 7]), width: 3, height: 1, format: .png),
            sourceApplication: source
        ))

        XCTAssertEqual(text.title, "文本")
        XCTAssertEqual(image.title, "图片")

        let renamed = ClipboardItem(
            id: image.id,
            databaseID: image.databaseID,
            title: "发布页设计稿",
            payload: image.payload,
            sourceApplication: image.sourceApplication,
            copiedAt: image.copiedAt
        )
        try repository.update(renamed)

        XCTAssertEqual(try repository.fetchRecent().first?.title, "发布页设计稿")
        XCTAssertEqual(try repository.searchText("发布页").first?.title, "发布页设计稿")
    }

    func testLinkMetadataPersistsAndTitleRemainsSearchable() throws {
        let repository = try makeRepository()
        let saved = try repository.save(ClipboardItem(
            title: "Example Documentation",
            linkIconData: Data([3, 1, 4, 1, 5]),
            linkPreviewData: Data([9, 2, 6, 5]),
            hasFetchedLinkPreview: true,
            payload: .text(plainText: "https://example.com/docs", rtfData: nil),
            sourceApplication: source
        ))

        let restored = try XCTUnwrap(try repository.fetchRecent().first)
        XCTAssertEqual(restored.databaseID, saved.databaseID)
        XCTAssertEqual(restored.title, "Example Documentation")
        XCTAssertEqual(restored.linkIconData, Data([3, 1, 4, 1, 5]))
        XCTAssertEqual(restored.linkPreviewData, Data([9, 2, 6, 5]))
        XCTAssertTrue(restored.hasFetchedLinkPreview)
        XCTAssertEqual(try repository.searchText("Documentation").first?.databaseID, saved.databaseID)
    }

    func testCacheMaintenanceDeletesOnlyExpiredThenCanClearAllHistory() throws {
        let repository = try makeRepository()
        _ = try repository.save(ClipboardItem(
            payload: .text(plainText: "expired", rtfData: nil),
            sourceApplication: source,
            copiedAt: Date().addingTimeInterval(-31 * 86_400)
        ))
        _ = try repository.save(ClipboardItem(
            payload: .text(plainText: "active", rtfData: nil),
            sourceApplication: source
        ))

        XCTAssertEqual(try repository.clearExpiredAndReclaim(retentionDays: 30), 1)
        XCTAssertEqual(try repository.fetchRecent().map(\.title), ["文本"])
        XCTAssertEqual(try repository.clearAllHistoryAndReclaim(), 1)
        XCTAssertTrue(try repository.fetchRecent().isEmpty)
    }

    func testClearingAllHistoryShrinksFileBackedDatabase() throws {
        let directory = FileManager.default.temporaryDirectory
            .appending(path: "ClipDeckTests-\(UUID())", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let repository = try HistoryRepository(
            database: ClipDeckDatabase(path: directory.appending(path: "history.sqlite").path)
        )
        for index in 0 ..< 24 {
            _ = try repository.save(ClipboardItem(
                payload: .image(
                    data: Data(repeating: UInt8(index), count: 256 * 1_024),
                    width: 512,
                    height: 512,
                    format: .png
                ),
                sourceApplication: source
            ))
        }
        let byteCountBeforeClearing = repository.databaseByteCount()

        XCTAssertEqual(try repository.clearAllHistoryAndReclaim(), 24)

        let byteCountAfterClearing = repository.databaseByteCount()
        XCTAssertLessThan(byteCountAfterClearing, byteCountBeforeClearing)
    }

    private func makeRepository() throws -> HistoryRepository {
        try HistoryRepository(database: ClipDeckDatabase(path: ":memory:"))
    }
}
