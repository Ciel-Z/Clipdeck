import Foundation
import XCTest
@testable import ClipDeck

/// LAN sync bookkeeping (docs/lan-sync-design.md §3); mirrors the shared
/// Linux/Windows checks in windows/tests/core_tests.cpp.
final class SyncBookkeepingTests: XCTestCase {
    private let source = SourceApplication(bundleIdentifier: "com.example.editor", name: "Editor", iconData: nil)

    func testHybridClockOrdersChangesEvenWhenTheWallClockStepsBack() {
        var wall: Int64 = 1_000_000
        let clock = HybridLogicalClock(wallClockMilliseconds: { wall })
        let first = clock.now()
        XCTAssertEqual(HybridLogicalClock.milliseconds(of: first), wall)
        XCTAssertEqual(HybridLogicalClock.counter(of: first), 0)
        let sameMillisecond = clock.now()
        XCTAssertEqual(sameMillisecond, first + 1)
        wall -= 5_000
        XCTAssertGreaterThan(clock.now(), sameMillisecond)
        let remote = HybridLogicalClock.make(milliseconds: wall + 60_000, counter: 7)
        let received = clock.receive(remote)
        XCTAssertGreaterThan(received, remote)
        XCTAssertGreaterThan(clock.now(), received)
        wall += 120_000
        XCTAssertEqual(HybridLogicalClock.counter(of: clock.now()), 0)
    }

    func testHybridClockMatchesTheSharedEncoding() {
        // Same packing as the C++ clock: milliseconds << 16 | counter.
        XCTAssertEqual(HybridLogicalClock.make(milliseconds: 2_000, counter: 3), (2_000 << 16) | 3)
        let full = HybridLogicalClock(wallClockMilliseconds: { 5 })
        full.observe(HybridLogicalClock.make(milliseconds: 5, counter: 0xFFFF))
        let carried = full.now()
        XCTAssertEqual(HybridLogicalClock.milliseconds(of: carried), 6)
        XCTAssertEqual(HybridLogicalClock.counter(of: carried), 0)
    }

    func testNewItemsGetASyncIdentityAndCopyingAgainKeepsIt() throws {
        let repository = try makeRepository()
        let item = ClipboardItem(payload: .text(plainText: "sync me", rtfData: nil), sourceApplication: source)
        let saved = try repository.save(item)
        let databaseID = try XCTUnwrap(saved.databaseID)
        let first = try XCTUnwrap(try repository.syncState(databaseID: databaseID))
        XCTAssertFalse(first.syncID.isEmpty)
        XCTAssertEqual(first.syncID, first.syncID.lowercased())

        let again = try repository.save(item)
        XCTAssertEqual(again.databaseID, databaseID)
        let second = try XCTUnwrap(try repository.syncState(databaseID: databaseID))
        XCTAssertEqual(second.syncID, first.syncID)
        XCTAssertGreaterThan(second.updatedHLC, first.updatedHLC)
    }

    func testOnlySyncedFieldsMoveTheClock() throws {
        let repository = try makeRepository()
        let saved = try repository.save(ClipboardItem(
            payload: .text(plainText: "https://example.com", rtfData: nil),
            sourceApplication: source
        ))
        let databaseID = try XCTUnwrap(saved.databaseID)
        let before = try XCTUnwrap(try repository.syncState(databaseID: databaseID)).updatedHLC

        try repository.update(ClipboardItem(
            id: saved.id, databaseID: databaseID, title: saved.title,
            linkIconData: Data([1]), hasFetchedLinkPreview: true,
            payload: saved.payload, sourceApplication: source
        ))
        XCTAssertEqual(try repository.syncState(databaseID: databaseID)?.updatedHLC, before)

        try repository.update(ClipboardItem(
            id: saved.id, databaseID: databaseID, title: "Renamed",
            payload: saved.payload, sourceApplication: source
        ))
        XCTAssertGreaterThan(try XCTUnwrap(try repository.syncState(databaseID: databaseID)).updatedHLC, before)
    }

    func testTouchCountsAsCopyingAgain() throws {
        let repository = try makeRepository()
        let older = try repository.save(ClipboardItem(
            payload: .text(plainText: "older", rtfData: nil),
            sourceApplication: source,
            copiedAt: Date().addingTimeInterval(-3_600)
        ))
        _ = try repository.save(ClipboardItem(payload: .text(plainText: "newer", rtfData: nil), sourceApplication: source))
        let databaseID = try XCTUnwrap(older.databaseID)
        let before = try XCTUnwrap(try repository.syncState(databaseID: databaseID)).updatedHLC

        try repository.touch(older)

        XCTAssertGreaterThan(try XCTUnwrap(try repository.syncState(databaseID: databaseID)).updatedHLC, before)
        XCTAssertEqual(try repository.fetchRecent().first?.databaseID, databaseID)
    }

    func testRecentHistoryOrdersSameMillisecondCopiesByClock() throws {
        let database = try ClipDeckDatabase(path: ":memory:")
        let repository = HistoryRepository(database: database)
        let first = try repository.save(ClipboardItem(payload: .text(plainText: "first", rtfData: nil), sourceApplication: source))
        let second = try repository.save(ClipboardItem(payload: .text(plainText: "second", rtfData: nil), sourceApplication: source))
        // Force the timestamp tie instead of relying on execution speed.
        try database.writer.write { db in
            try db.execute(sql: "UPDATE content SET last_copied_at = 1000")
        }
        XCTAssertEqual(try repository.fetchRecent().first?.databaseID, second.databaseID)

        try repository.touch(first)
        try database.writer.write { db in
            try db.execute(sql: "UPDATE content SET last_copied_at = 1000")
        }
        XCTAssertEqual(try repository.fetchRecent().first?.databaseID, first.databaseID)
    }

    func testDeletingRemembersTheSyncIdentity() throws {
        let repository = try makeRepository()
        let deleted = try repository.save(ClipboardItem(payload: .text(plainText: "a", rtfData: nil), sourceApplication: source))
        let cleared = try repository.save(ClipboardItem(payload: .text(plainText: "b", rtfData: nil), sourceApplication: source))
        let deletedID = try XCTUnwrap(try repository.syncState(databaseID: XCTUnwrap(deleted.databaseID))).syncID
        let clearedID = try XCTUnwrap(try repository.syncState(databaseID: XCTUnwrap(cleared.databaseID))).syncID
        XCTAssertFalse(try repository.wasDeleted(syncID: deletedID))

        try repository.delete(deleted)
        XCTAssertTrue(try repository.wasDeleted(syncID: deletedID))
        _ = try repository.clearAllHistoryAndReclaim()
        XCTAssertTrue(try repository.wasDeleted(syncID: clearedID))
    }

    @MainActor
    func testMarkCopiedMovesTheItemToTheFrontAndKeepsTheSelection() throws {
        let store = InMemoryHistoryStore()
        let older = ClipboardItem(
            payload: .text(plainText: "older", rtfData: nil),
            sourceApplication: source,
            copiedAt: Date().addingTimeInterval(-3_600)
        )
        let newer = ClipboardItem(payload: .text(plainText: "newer", rtfData: nil), sourceApplication: source)
        store.insert(older)
        store.insert(newer)

        store.markCopied(older.id)

        XCTAssertEqual(store.items.first?.id, older.id)
        XCTAssertGreaterThan(try XCTUnwrap(store.items.first?.copiedAt), older.copiedAt)
        XCTAssertEqual(store.selectedItemID, newer.id)
    }

    private func makeRepository() throws -> HistoryRepository {
        try HistoryRepository(database: ClipDeckDatabase(path: ":memory:"))
    }
}
