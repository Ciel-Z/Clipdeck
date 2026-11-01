import AppKit
import CryptoKit
import Foundation
import GRDB

final class HistoryRepository: @unchecked Sendable {
    fileprivate let database: ClipDeckDatabase
    private let duplicateWindowMilliseconds: Int64 = 24 * 60 * 60 * 1_000

    /// How long a local deletion is remembered, so a peer cannot sync the
    /// item straight back (the sync window is 24 hours).
    static let deletedRecordRetentionMilliseconds: Int64 = 48 * 60 * 60 * 1_000

    init(database: ClipDeckDatabase) {
        self.database = database
        // Later clock values must order after everything already stored.
        if let latest = try? database.writer.read({ db in
            try Int64.fetchOne(db, sql: "SELECT MAX(updated_hlc) FROM content")
        }) {
            HybridLogicalClock.shared.observe(UInt64(bitPattern: latest))
        }
    }

    func ensureDefaultCategories() throws {
        try database.writer.write { db in
            guard try Int.fetchOne(db, sql: "SELECT COUNT(*) FROM category") == 0 else { return }
            let now = Self.milliseconds(Date())
            for (order, preset) in DefaultCategoryScripts.presets.enumerated() {
                try db.execute(
                    sql: "INSERT INTO category (name, icon, script, preview_language, sort_order, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                    arguments: [preset.name, preset.icon, preset.script, preset.previewLanguage, order, now, now]
                )
            }
        }
    }

    func restoreDefaultCategories() throws {
        try database.writer.write { db in
            let now = Self.milliseconds(Date())
            for preset in DefaultCategoryScripts.presets {
                if let id = try Int64.fetchOne(db, sql: "SELECT id FROM category WHERE name = ? LIMIT 1", arguments: [preset.name]) {
                    try db.execute(
                        sql: "UPDATE category SET icon = ?, script = ?, preview_language = ?, updated_at = ? WHERE id = ?",
                        arguments: [preset.icon, preset.script, preset.previewLanguage, now, id]
                    )
                } else {
                    try db.execute(
                        sql: "INSERT INTO category (name, icon, script, preview_language, sort_order, created_at, updated_at) VALUES (?, ?, ?, ?, 0, ?, ?)",
                        arguments: [preset.name, preset.icon, preset.script, preset.previewLanguage, now, now]
                    )
                }
            }

            let presetNames = Set(DefaultCategoryScripts.presets.map(\.name))
            let rows = try Row.fetchAll(db, sql: "SELECT id, name FROM category ORDER BY sort_order, id")
            let IDsByName = Dictionary(uniqueKeysWithValues: rows.compactMap { row -> (String, Int64)? in
                guard let name: String = row["name"], let id: Int64 = row["id"] else { return nil }
                return (name, id)
            })
            var orderedIDs = DefaultCategoryScripts.presets.dropLast().compactMap { IDsByName[$0.name] }
            orderedIDs.append(contentsOf: rows.compactMap { row -> Int64? in
                guard let name: String = row["name"], !presetNames.contains(name) else { return nil }
                return row["id"]
            })
            if let colorID = IDsByName[DefaultCategoryScripts.presets.last!.name] {
                orderedIDs.append(colorID)
            }
            for (index, id) in orderedIDs.enumerated() {
                try db.execute(
                    sql: "UPDATE category SET sort_order = ?, updated_at = ? WHERE id = ?",
                    arguments: [index, now, id]
                )
            }
        }
    }

    func fetchCategories() throws -> [ClipboardCategory] {
        try database.writer.read { db in
            try Row.fetchAll(db, sql: "SELECT * FROM category ORDER BY sort_order, id").map(Self.category)
        }
    }

    func createCategory(name: String, icon: String?, script: String, previewLanguage: String?, sortOrder: Int) throws -> ClipboardCategory {
        let now = Self.milliseconds(Date())
        return try database.writer.write { db in
            try db.execute(
                sql: "INSERT INTO category (name, icon, script, preview_language, sort_order, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                arguments: [name, icon, script, previewLanguage, sortOrder, now, now]
            )
            return ClipboardCategory(id: db.lastInsertedRowID, name: name, icon: icon, script: script, previewLanguage: previewLanguage, sortOrder: sortOrder)
        }
    }

    func updateCategory(id: Int64, name: String, icon: String?, script: String, previewLanguage: String?) throws {
        try database.writer.write { db in
            try db.execute(
                sql: "UPDATE category SET name = ?, icon = ?, script = ?, preview_language = ?, updated_at = ? WHERE id = ?",
                arguments: [name, icon, script, previewLanguage, Self.milliseconds(Date()), id]
            )
        }
    }

    func reorderCategories(_ categoryIDs: [Int64]) throws {
        try database.writer.write { db in
            for (index, id) in categoryIDs.enumerated() {
                try db.execute(
                    sql: "UPDATE category SET sort_order = ?, updated_at = ? WHERE id = ?",
                    arguments: [index, Self.milliseconds(Date()), id]
                )
            }
        }
    }

    func deleteCategory(id: Int64) throws {
        try database.writer.write { db in
            try db.execute(sql: "DELETE FROM category WHERE id = ?", arguments: [id])
        }
    }

    func categoryError(id: Int64) throws -> String? {
        try database.writer.read { db in
            try String.fetchOne(db, sql: "SELECT last_error_message FROM category WHERE id = ?", arguments: [id])
        }
    }

    func assignCategory(_ category: ClipboardCategory?, to item: ClipboardItem) throws -> ClipboardItem {
        guard let databaseID = item.databaseID else { return item }
        try database.writer.write { db in
            try db.execute(sql: "UPDATE content SET category_id = ?, updated_at = ? WHERE id = ?", arguments: [category?.id, Self.milliseconds(Date()), databaseID])
        }
        return ClipboardItem(
            id: item.id,
            databaseID: databaseID,
            title: item.title,
            linkIconData: item.linkIconData,
            linkPreviewData: item.linkPreviewData,
            hasFetchedLinkPreview: item.hasFetchedLinkPreview,
            imageAngle: item.imageAngle,
            payload: item.payload,
            sourceApplication: item.sourceApplication,
            category: category,
            copiedAt: item.copiedAt
        )
    }

    func recordCategoryErrors(_ errors: [(categoryID: Int64, message: String?)]) throws {
        guard !errors.isEmpty else { return }
        try database.writer.write { db in
            let now = Self.milliseconds(Date())
            for error in errors {
                try db.execute(
                    sql: """
                        UPDATE category
                        SET last_error_message = ?, last_error_at = ?, updated_at = ?
                        WHERE id = ? AND last_error_message IS NOT ?
                        """,
                    arguments: [
                        error.message,
                        error.message == nil ? nil : now,
                        now,
                        error.categoryID,
                        error.message,
                    ]
                )
            }
        }
    }

    func save(_ item: ClipboardItem) throws -> ClipboardItem {
        let now = Self.milliseconds(item.copiedAt)
        let hash = Self.hash(item.payload)
        let duplicateCutoff = now - duplicateWindowMilliseconds
        let persisted = try database.writer.write { db -> (id: Int64, title: String, iconData: Data?, previewData: Data?, previewFetched: Bool, imageAngle: Int) in
            let applicationID = try upsertApplication(item.sourceApplication, at: now, in: db)
            if let existingID = try Int64.fetchOne(
                db,
                sql: "SELECT id FROM content WHERE content_hash = ? AND last_copied_at >= ? ORDER BY last_copied_at DESC LIMIT 1",
                arguments: [hash, duplicateCutoff]
            ) {
                // Copying it again is a change that syncs (the item re-enters the window).
                try db.execute(
                    sql: "UPDATE content SET application_id = ?, last_copied_at = ?, copy_count = copy_count + 1, updated_at = ?, updated_hlc = ? WHERE id = ?",
                    arguments: [applicationID, now, now, Self.nextClockValue(), existingID]
                )
                let existingTitle = try String.fetchOne(
                    db,
                    sql: "SELECT title FROM content WHERE id = ?",
                    arguments: [existingID]
                ) ?? item.title
                let existingIcon = try Data.fetchOne(
                    db,
                    sql: "SELECT link_icon_data FROM content WHERE id = ?",
                    arguments: [existingID]
                )
                let existingPreview = try Data.fetchOne(
                    db,
                    sql: "SELECT link_preview_data FROM content WHERE id = ?",
                    arguments: [existingID]
                )
                let previewFetched = try Bool.fetchOne(
                    db,
                    sql: "SELECT link_preview_fetched FROM content WHERE id = ?",
                    arguments: [existingID]
                ) ?? false
                let imageAngle = try Int.fetchOne(
                    db,
                    sql: "SELECT image_angle FROM content WHERE id = ?",
                    arguments: [existingID]
                ) ?? 0
                return (existingID, existingTitle, existingIcon, existingPreview, previewFetched, imageAngle)
            }

            let values = Self.databaseValues(item.payload)
            try db.execute(
                sql: """
                    INSERT INTO content (
                        title, link_icon_data, link_preview_data, link_preview_fetched, content_type, plain_text, rich_text, rich_text_type,
                        image_data, image_width, image_height, image_format, image_angle, file_references,
                        content_hash, content_size, application_id,
                        copied_at, last_copied_at, copy_count, created_at, updated_at, sync_id, updated_hlc
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?)
                    """,
                arguments: [
                    item.title, item.linkIconData, item.linkPreviewData, item.hasFetchedLinkPreview,
                    values.type, values.plainText, values.richText, values.richTextType,
                    values.imageData, values.imageWidth, values.imageHeight, values.imageFormat, item.imageAngle, values.files,
                    hash, values.size, applicationID, now, now, now, now,
                    UUID().uuidString.lowercased(), Self.nextClockValue(),
                ]
            )
            return (db.lastInsertedRowID, item.title, item.linkIconData, item.linkPreviewData, item.hasFetchedLinkPreview, item.imageAngle)
        }
        return ClipboardItem(
            id: item.id,
            databaseID: persisted.id,
            title: persisted.title,
            linkIconData: persisted.iconData,
            linkPreviewData: persisted.previewData,
            hasFetchedLinkPreview: persisted.previewFetched,
            imageAngle: persisted.imageAngle,
            payload: item.payload,
            sourceApplication: item.sourceApplication,
            copiedAt: item.copiedAt
        )
    }

    func fetchRecent(limit: Int = 100) throws -> [ClipboardItem] {
        try database.writer.read { db in
            let rows = try Row.fetchAll(
                db,
                sql: """
                    SELECT content.*, application.bundle_id, application.name AS application_name, application.icon_data,
                           category.name AS category_name, category.icon AS category_icon, category.script AS category_script,
                           category.preview_language, category.sort_order
                    FROM content LEFT JOIN application ON application.id = content.application_id
                    LEFT JOIN category ON category.id = content.category_id
                    ORDER BY content.last_copied_at DESC LIMIT ?
                    """,
                arguments: [limit]
            )
            return rows.compactMap(Self.item)
        }
    }

    func searchText(_ query: String, limit: Int = 100) throws -> [ClipboardItem] {
        let matchQuery = Self.ftsQuery(query)
        guard !matchQuery.isEmpty else { return [] }
        return try database.writer.read { db in
            try Self.searchText(matchQuery: matchQuery, limit: limit, in: db)
        }
    }

    func searchTextAsync(_ query: String, limit: Int = 100) async throws -> [ClipboardItem] {
        let matchQuery = Self.ftsQuery(query)
        guard !matchQuery.isEmpty else { return [] }
        return try await database.writer.read { db in
            try Self.searchText(matchQuery: matchQuery, limit: limit, in: db)
        }
    }

    /// Deleting is local only; the sync ID is remembered for a while so a
    /// peer cannot sync the item straight back.
    func delete(_ item: ClipboardItem) throws {
        guard let databaseID = item.databaseID else { return }
        try database.writer.write { db in
            try db.execute(
                sql: """
                    INSERT OR REPLACE INTO sync_deleted (sync_id, deleted_at)
                    SELECT sync_id, ? FROM content WHERE id = ? AND sync_id IS NOT NULL
                    """,
                arguments: [Self.milliseconds(Date()), databaseID]
            )
            try db.execute(sql: "DELETE FROM content WHERE id = ?", arguments: [databaseID])
        }
    }

    /// A paste or copy from the Deck counts as copying the item again: it
    /// moves to the front and, with LAN sync, into the window that is synced.
    func touch(_ item: ClipboardItem) throws {
        guard let databaseID = item.databaseID else { return }
        let now = Self.milliseconds(Date())
        try database.writer.write { db in
            try db.execute(
                sql: """
                    UPDATE content SET last_copied_at = MAX(last_copied_at, ?), copy_count = copy_count + 1,
                        updated_at = ?, updated_hlc = ?
                    WHERE id = ?
                    """,
                arguments: [now, now, Self.nextClockValue(), databaseID]
            )
        }
    }

    func wasDeleted(syncID: String) throws -> Bool {
        try database.writer.read { db in
            try Bool.fetchOne(db, sql: "SELECT 1 FROM sync_deleted WHERE sync_id = ?", arguments: [syncID]) ?? false
        }
    }

    /// The row's sync identity and clock value (tests and, later, sync).
    func syncState(databaseID: Int64) throws -> (syncID: String, updatedHLC: UInt64)? {
        try database.writer.read { db in
            guard let row = try Row.fetchOne(
                db,
                sql: "SELECT sync_id, updated_hlc FROM content WHERE id = ?",
                arguments: [databaseID]
            ), let syncID: String = row["sync_id"] else { return nil }
            return (syncID, UInt64(bitPattern: row["updated_hlc"] as Int64))
        }
    }

    func update(_ item: ClipboardItem) throws {
        guard let databaseID = item.databaseID else { return }
        let values = Self.databaseValues(item.payload)
        let hash = Self.hash(item.payload)
        try database.writer.write { db in
            // Only changes that sync move the clock on: the title, content and
            // rotation. Link metadata is fetched by each device itself.
            let stored = try Row.fetchOne(
                db,
                sql: "SELECT title, content_hash, image_angle, updated_hlc FROM content WHERE id = ?",
                arguments: [databaseID]
            )
            let syncedFieldsChanged = stored.map {
                ($0["title"] as String) != item.title
                    || ($0["content_hash"] as String) != hash
                    || ($0["image_angle"] as Int) != item.imageAngle
            } ?? false
            let updatedHLC = syncedFieldsChanged
                ? Self.nextClockValue()
                : (stored?["updated_hlc"] as Int64?) ?? 0
            try db.execute(
                sql: """
                    UPDATE content SET
                        title = ?, link_icon_data = ?, link_preview_data = ?, link_preview_fetched = ?, content_type = ?, plain_text = ?, rich_text = ?, rich_text_type = ?,
                        image_data = ?, image_width = ?, image_height = ?, image_format = ?, image_angle = ?, file_references = ?,
                        content_hash = ?, content_size = ?, updated_at = ?, updated_hlc = ?
                    WHERE id = ?
                    """,
                arguments: [
                    item.title, item.linkIconData, item.linkPreviewData, item.hasFetchedLinkPreview,
                    values.type, values.plainText, values.richText, values.richTextType,
                    values.imageData, values.imageWidth, values.imageHeight, values.imageFormat, item.imageAngle, values.files,
                    hash, values.size, Self.milliseconds(Date()), updatedHLC, databaseID,
                ]
            )
        }
    }

    private static func searchText(matchQuery: String, limit: Int, in db: Database) throws -> [ClipboardItem] {
        let rows = try Row.fetchAll(
            db,
            sql: """
                SELECT content.*, application.bundle_id, application.name AS application_name, application.icon_data,
                       category.name AS category_name, category.icon AS category_icon, category.script AS category_script,
                       category.preview_language, category.sort_order
                FROM content_fts
                JOIN content ON content.id = content_fts.rowid
                LEFT JOIN application ON application.id = content.application_id
                LEFT JOIN category ON category.id = content.category_id
                WHERE content_fts MATCH ?
                ORDER BY bm25(content_fts), content.last_copied_at DESC
                LIMIT ?
                """,
            arguments: [matchQuery, limit]
        )
        return rows.compactMap(Self.item)
    }

    @discardableResult
    func deleteExpired(retentionDays: Int = 30) throws -> Int {
        guard retentionDays > 0 else { return 0 }
        let cutoff = Self.milliseconds(Date().addingTimeInterval(-Double(retentionDays) * 86_400))
        return try database.writer.write { db in
            // Expired items are older than the 24-hour sync window and need no
            // deletion record; this is just a good moment to drop old records.
            try db.execute(
                sql: "DELETE FROM sync_deleted WHERE deleted_at < ?",
                arguments: [Self.milliseconds(Date()) - Self.deletedRecordRetentionMilliseconds]
            )
            var total = 0
            while true {
                try db.execute(
                    sql: "DELETE FROM content WHERE id IN (SELECT id FROM content WHERE last_copied_at < ? LIMIT 500)",
                    arguments: [cutoff]
                )
                let deleted = db.changesCount
                total += deleted
                if deleted < 500 { return total }
            }
        }
    }

    func databaseByteCount() -> Int64 {
        guard database.path != ":memory:" else { return 0 }
        let fileManager = FileManager.default
        return [database.path, database.path + "-wal", database.path + "-shm"].reduce(0) {
            partial, path in
            let size = (try? fileManager.attributesOfItem(atPath: path)[.size] as? NSNumber)?
                .int64Value ?? 0
            return partial + size
        }
    }

    @discardableResult
    func clearExpiredAndReclaim(retentionDays: Int) throws -> Int {
        let deleted = try deleteExpired(retentionDays: retentionDays)
        try reclaimDatabaseSpace()
        return deleted
    }

    @discardableResult
    func clearAllHistoryAndReclaim() throws -> Int {
        let deleted = try database.writer.write { db in
            // Clearing is a local deletion too: remember what was there, so
            // peers do not refill the history from the last 24 hours.
            try db.execute(
                sql: """
                    INSERT OR REPLACE INTO sync_deleted (sync_id, deleted_at)
                    SELECT sync_id, ? FROM content WHERE sync_id IS NOT NULL
                    """,
                arguments: [Self.milliseconds(Date())]
            )
            try db.execute(sql: "DELETE FROM content")
            return db.changesCount
        }
        try reclaimDatabaseSpace()
        return deleted
    }

    private func reclaimDatabaseSpace() throws {
        try database.writer.writeWithoutTransaction { db in
            try db.execute(sql: "VACUUM")
            try db.execute(sql: "PRAGMA wal_checkpoint(TRUNCATE)")
        }
        database.releaseMemory()
    }

    fileprivate func upsertApplication(_ application: SourceApplication, at now: Int64, in db: Database) throws -> Int64 {
        let bundleID = application.bundleIdentifier ?? "unknown:\(application.name)"
        try db.execute(
            sql: """
                INSERT INTO application (bundle_id, name, icon_data, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?)
                ON CONFLICT(bundle_id) DO UPDATE SET name = excluded.name, icon_data = excluded.icon_data, updated_at = excluded.updated_at
                """,
            arguments: [bundleID, application.name, application.iconData, now, now]
        )
        return try Int64.fetchOne(db, sql: "SELECT id FROM application WHERE bundle_id = ?", arguments: [bundleID])!
    }

    fileprivate static func item(_ row: Row) -> ClipboardItem? {
        let payload: ClipboardPayload
        if (row["content_type"] as String) == "text" {
            guard let text: String = row["plain_text"] else { return nil }
            payload = .text(plainText: text, rtfData: row["rich_text"])
        } else if (row["content_type"] as String) == "files" {
            guard let data: Data = row["file_references"],
                  let files = ClipboardFileReference.decoded(data) else { return nil }
            payload = .files(files)
        } else {
            guard let data: Data = row["image_data"],
                  let width: Int = row["image_width"],
                  let height: Int = row["image_height"],
                  let rawFormat: String = row["image_format"],
                  let format = ClipboardImageFormat(rawValue: rawFormat) else { return nil }
            payload = .image(data: data, width: width, height: height, format: format)
        }
        let timestamp: Int64 = row["last_copied_at"]
        return ClipboardItem(
            databaseID: row["id"],
            title: row["title"],
            linkIconData: row["link_icon_data"],
            linkPreviewData: row["link_preview_data"],
            hasFetchedLinkPreview: row["link_preview_fetched"],
            imageAngle: row["image_angle"] ?? 0,
            payload: payload,
            sourceApplication: SourceApplication(
                bundleIdentifier: row["bundle_id"],
                name: row["application_name"] ?? "Unknown",
                iconData: row["icon_data"]
            ),
            category: category(from: row),
            copiedAt: Date(timeIntervalSince1970: Double(timestamp) / 1_000)
        )
    }

    private static func category(from row: Row) -> ClipboardCategory? {
        guard let id: Int64 = row["category_id"], let name: String = row["category_name"] else { return nil }
        return ClipboardCategory(id: id, name: name, icon: row["category_icon"], script: row["category_script"] ?? "", previewLanguage: row["preview_language"], sortOrder: row["sort_order"] ?? 0)
    }

    private static func category(_ row: Row) -> ClipboardCategory {
        let errorTimestamp: Int64? = row["last_error_at"]
        return ClipboardCategory(
            id: row["id"],
            name: row["name"],
            icon: row["icon"],
            script: row["script"],
            previewLanguage: row["preview_language"],
            sortOrder: row["sort_order"],
            lastErrorMessage: row["last_error_message"],
            lastErrorAt: errorTimestamp.map { Date(timeIntervalSince1970: Double($0) / 1_000) }
        )
    }

    fileprivate static func hash(_ payload: ClipboardPayload) -> String {
        let data: Data
        switch payload {
        case let .text(text, _): data = Data("text:\(text)".utf8)
        case let .image(imageData, _, _, _): data = Data("image:".utf8) + imageData
        case let .files(files): data = Data("files:".utf8) + ClipboardFileReference.identityData(files)
        }
        return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    private static func ftsQuery(_ query: String) -> String {
        query
            .split(whereSeparator: \.isWhitespace)
            .map { token in
                let escaped = token.replacingOccurrences(of: "\"", with: "\"\"")
                return "\"\(escaped)\"*"
            }
            .joined(separator: " AND ")
    }

    private static func nextClockValue() -> Int64 {
        Int64(bitPattern: HybridLogicalClock.shared.now())
    }

    private static func milliseconds(_ date: Date) -> Int64 {
        Int64((date.timeIntervalSince1970 * 1_000).rounded())
    }

    private static func databaseValues(_ payload: ClipboardPayload) -> (
        type: String, plainText: String?, richText: Data?, richTextType: String?,
        imageData: Data?, imageWidth: Int?, imageHeight: Int?, imageFormat: String?, size: Int, files: Data?
    ) {
        switch payload {
        case let .text(text, rtf):
            return ("text", text, rtf, rtf == nil ? nil : "rtf", nil, nil, nil, nil, text.utf8.count + (rtf?.count ?? 0), nil)
        case let .image(data, width, height, format):
            return ("image", nil, nil, nil, data, width, height, format.rawValue, data.count, nil)
        case let .files(files):
            let data = ClipboardFileReference.encoded(files)
            let searchable = files.map { "\($0.name)\n\($0.path)" }.joined(separator: "\n")
            return ("files", searchable, nil, nil, nil, nil, nil, nil, data.count, data)
        }
    }
}

/// Hybrid logical clock (docs/lan-sync-design.md §3): orders history changes
/// across devices whose system clocks disagree. A value packs milliseconds
/// since the epoch into the high 48 bits and a counter into the low 16, so
/// values compare as plain integers and still read as wall-clock time. Same
/// encoding and rules as the Linux/Windows `HybridLogicalClock`.
final class HybridLogicalClock: @unchecked Sendable {
    static let counterBits: UInt64 = 16
    static let counterMask: UInt64 = (1 << counterBits) - 1
    static let shared = HybridLogicalClock()

    private let wallClockMilliseconds: () -> Int64
    private let lock = NSLock()
    private var lastValue: UInt64 = 0

    init(wallClockMilliseconds: @escaping () -> Int64 = { Int64(Date().timeIntervalSince1970 * 1_000) }) {
        self.wallClockMilliseconds = wallClockMilliseconds
    }

    static func make(milliseconds: Int64, counter: UInt64 = 0) -> UInt64 {
        (UInt64(max(0, milliseconds)) << counterBits) | (counter & counterMask)
    }

    static func milliseconds(of value: UInt64) -> Int64 { Int64(value >> counterBits) }
    static func counter(of value: UInt64) -> UInt64 { value & counterMask }

    /// A local change.
    func now() -> UInt64 { advance(past: 0) }

    /// A value received from another device: later values order after it.
    func receive(_ remote: UInt64) -> UInt64 { advance(past: remote) }

    /// Makes later values greater than one already stored.
    func observe(_ value: UInt64) {
        lock.lock()
        defer { lock.unlock() }
        lastValue = max(lastValue, value)
    }

    var last: UInt64 {
        lock.lock()
        defer { lock.unlock() }
        return lastValue
    }

    // The wall clock if it is ahead of everything seen so far, otherwise one
    // tick past the larger of the last value and `floor` (a full counter
    // carries into the milliseconds).
    private func advance(past floor: UInt64) -> UInt64 {
        let fromWall = Self.make(milliseconds: wallClockMilliseconds())
        lock.lock()
        defer { lock.unlock() }
        let seen = max(lastValue, floor)
        lastValue = fromWall > seen ? fromWall : seen + 1
        return lastValue
    }
}

// MARK: - LAN sync (docs/lan-sync-design.md), as linux/src/history_repository.cpp

/// A paired device. Its locate key lives in the Keychain, not here.
struct SyncPeerRecord: Equatable, Sendable {
    var deviceID: String
    var hostname: String
    var publicKey: Data
    var addresses: [String] = []
    var lastSeenMilliseconds: Int64 = 0
    var sentCursor: UInt64 = 0
}

extension HistoryRepository {
    enum SyncImportResult: Equatable {
        case inserted(Int64), updated(Int64), merged(Int64), skipped, failed
    }

    /// Changes to send: non-file items copied within `windowMilliseconds`
    /// whose clock is past `after`, oldest change first. Images go as PNG.
    func syncChanges(after: UInt64, windowMilliseconds: Int64, limit: Int) throws -> [SyncProtocol.WireItem] {
        let cutoff = Self.syncMilliseconds(Date()) - windowMilliseconds
        return try database.writer.read { db in
            let rows = try Row.fetchAll(
                db,
                sql: """
                    SELECT content.*, application.name AS application_name
                    FROM content LEFT JOIN application ON application.id = content.application_id
                    WHERE content.content_type IN ('text', 'image') AND content.updated_hlc > ? AND content.last_copied_at >= ?
                    ORDER BY content.updated_hlc ASC LIMIT ?
                    """,
                arguments: [Int64(bitPattern: after), cutoff, max(1, limit)]
            )
            return rows.compactMap { row -> SyncProtocol.WireItem? in
                guard let syncID: String = row["sync_id"] else { return nil }
                let isImage = (row["content_type"] as String) == "image"
                var body = Data()
                if isImage {
                    guard let data: Data = row["image_data"] else { return nil }
                    let format: String = row["image_format"] ?? "png"
                    guard let png = format == "png" ? data : Self.pngData(from: data) else { return nil }
                    body = png
                } else {
                    guard let text: String = row["plain_text"] else { return nil }
                    body = Data(text.utf8)
                }
                var wire = SyncProtocol.WireItem(
                    syncID: syncID,
                    origin: row["origin_device"] ?? "",
                    hlc: UInt64(bitPattern: row["updated_hlc"] as Int64),
                    isImage: isImage,
                    title: row["title"] ?? "",
                    copiedAtMilliseconds: row["last_copied_at"],
                    angle: row["image_angle"] ?? 0,
                    sourceName: row["application_name"] ?? "ClipDeck",
                    html: nil
                )
                wire.body = body
                return SyncProtocol.makeWireItem(wire)
            }
        }
    }

    /// Merges an item from a peer: a newer clock value wins for the same sync
    /// ID; the same content merges into the local row; locally deleted items
    /// are skipped. Classification is left to the caller.
    func importRemote(_ wire: SyncProtocol.WireItem) throws -> SyncImportResult {
        if try wasDeleted(syncID: wire.syncID) { return .skipped }
        _ = HybridLogicalClock.shared.receive(wire.hlc)
        let payload: ClipboardPayload
        if wire.isImage {
            guard let representation = NSBitmapImageRep(data: wire.body) else { return .failed }
            payload = .image(data: wire.body, width: representation.pixelsWide, height: representation.pixelsHigh, format: .png)
        } else {
            payload = .text(plainText: wire.text, rtfData: nil)
        }
        let values = Self.databaseValues(payload)
        let hash = Self.hash(payload)
        let copiedAt = wire.copiedAtMilliseconds > 0 ? wire.copiedAtMilliseconds : Self.syncMilliseconds(Date())
        let now = Self.syncMilliseconds(Date())
        let hlc = Int64(bitPattern: wire.hlc)
        let title = wire.title.isEmpty ? payload.defaultTitle : wire.title
        return try database.writer.write { db -> SyncImportResult in
            if let existing = try Row.fetchOne(db, sql: "SELECT id, updated_hlc FROM content WHERE sync_id = ?",
                                                 arguments: [wire.syncID]) {
                let id: Int64 = existing["id"]
                guard UInt64(bitPattern: existing["updated_hlc"] as Int64) < wire.hlc else { return .skipped }
                try db.execute(
                    sql: """
                        UPDATE content SET title = ?, content_type = ?, plain_text = ?, rich_text = NULL, rich_text_type = NULL,
                            image_data = ?, image_width = ?, image_height = ?, image_format = ?, image_angle = ?,
                            content_hash = ?, content_size = ?, last_copied_at = MAX(last_copied_at, ?),
                            updated_hlc = ?, updated_at = ?
                        WHERE id = ?
                        """,
                    arguments: [title, values.type, values.plainText, values.imageData, values.imageWidth, values.imageHeight,
                                values.imageFormat, wire.angle, hash, values.size, copiedAt, hlc, now, id]
                )
                return .updated(id)
            }
            if let same = try Row.fetchOne(
                db, sql: "SELECT id, updated_hlc FROM content WHERE content_hash = ? ORDER BY last_copied_at DESC LIMIT 1",
                arguments: [hash]) {
                let id: Int64 = same["id"]
                try db.execute(
                    sql: "UPDATE content SET last_copied_at = MAX(last_copied_at, ?), updated_hlc = MAX(updated_hlc, ?), updated_at = ? WHERE id = ?",
                    arguments: [copiedAt, hlc, now, id]
                )
                return .merged(id)
            }
            let application = SourceApplication(
                bundleIdentifier: "clipdeck-sync:\(wire.origin)",
                name: wire.sourceName.isEmpty ? "ClipDeck" : wire.sourceName,
                iconData: nil
            )
            let applicationID = try self.upsertApplication(application, at: now, in: db)
            try db.execute(
                sql: """
                    INSERT INTO content (
                        title, link_preview_fetched, content_type, plain_text, image_data, image_width, image_height, image_format,
                        image_angle, content_hash, content_size, application_id, copied_at, last_copied_at, copy_count,
                        created_at, updated_at, sync_id, origin_device, updated_hlc
                    ) VALUES (?, 0, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?, ?)
                    """,
                arguments: [title, values.type, values.plainText, values.imageData, values.imageWidth, values.imageHeight,
                            values.imageFormat, wire.angle, hash, values.size, applicationID, copiedAt, copiedAt, now, now,
                            wire.syncID, wire.origin.isEmpty ? nil : wire.origin, hlc]
            )
            return .inserted(db.lastInsertedRowID)
        }
    }

    func syncPeers() throws -> [SyncPeerRecord] {
        try database.writer.read { db in
            try Row.fetchAll(db, sql: "SELECT * FROM sync_peer ORDER BY hostname").map { row in
                SyncPeerRecord(
                    deviceID: row["device_id"],
                    hostname: row["hostname"],
                    publicKey: row["public_key"],
                    addresses: (row["addresses"] as String? ?? "").split(separator: ",").map(String.init),
                    lastSeenMilliseconds: row["last_seen"],
                    sentCursor: UInt64(bitPattern: row["sent_cursor"] as Int64)
                )
            }
        }
    }

    func saveSyncPeer(_ peer: SyncPeerRecord) throws {
        try database.writer.write { db in
            try db.execute(
                sql: """
                    INSERT INTO sync_peer (device_id, hostname, public_key, addresses, last_seen, sent_cursor)
                    VALUES (?, ?, ?, ?, ?, ?)
                    ON CONFLICT(device_id) DO UPDATE SET hostname = excluded.hostname, public_key = excluded.public_key,
                        addresses = excluded.addresses, last_seen = excluded.last_seen, sent_cursor = excluded.sent_cursor
                    """,
                arguments: [peer.deviceID, peer.hostname, peer.publicKey, peer.addresses.joined(separator: ","),
                            peer.lastSeenMilliseconds, Int64(bitPattern: peer.sentCursor)]
            )
        }
    }

    func removeSyncPeer(deviceID: String) throws {
        try database.writer.write { db in
            try db.execute(sql: "DELETE FROM sync_peer WHERE device_id = ?", arguments: [deviceID])
        }
    }

    /// The item for a row (after an import, to classify it).
    func item(databaseID: Int64) throws -> ClipboardItem? {
        try database.writer.read { db in
            try Row.fetchOne(
                db,
                sql: """
                    SELECT content.*, application.bundle_id, application.name AS application_name, application.icon_data,
                           category.name AS category_name, category.icon AS category_icon, category.script AS category_script,
                           category.preview_language, category.sort_order
                    FROM content LEFT JOIN application ON application.id = content.application_id
                    LEFT JOIN category ON category.id = content.category_id
                    WHERE content.id = ?
                    """,
                arguments: [databaseID]
            ).flatMap { Self.item($0) }
        }
    }

    static func pngData(from data: Data) -> Data? {
        guard let representation = NSBitmapImageRep(data: data) else { return nil }
        return representation.representation(using: .png, properties: [:])
    }

    fileprivate static func syncMilliseconds(_ date: Date) -> Int64 { Int64(date.timeIntervalSince1970 * 1_000) }
}
