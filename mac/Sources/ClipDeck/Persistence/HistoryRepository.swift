import CryptoKit
import Foundation
import GRDB

final class HistoryRepository: @unchecked Sendable {
    private let database: ClipDeckDatabase
    private let duplicateWindowMilliseconds: Int64 = 24 * 60 * 60 * 1_000

    init(database: ClipDeckDatabase) {
        self.database = database
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
                try db.execute(
                    sql: "UPDATE content SET application_id = ?, last_copied_at = ?, copy_count = copy_count + 1, updated_at = ? WHERE id = ?",
                    arguments: [applicationID, now, now, existingID]
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
                        copied_at, last_copied_at, copy_count, created_at, updated_at
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?)
                    """,
                arguments: [
                    item.title, item.linkIconData, item.linkPreviewData, item.hasFetchedLinkPreview,
                    values.type, values.plainText, values.richText, values.richTextType,
                    values.imageData, values.imageWidth, values.imageHeight, values.imageFormat, item.imageAngle, values.files,
                    hash, values.size, applicationID, now, now, now, now,
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

    func delete(_ item: ClipboardItem) throws {
        guard let databaseID = item.databaseID else { return }
        try database.writer.write { db in
            try db.execute(sql: "DELETE FROM content WHERE id = ?", arguments: [databaseID])
        }
    }

    func update(_ item: ClipboardItem) throws {
        guard let databaseID = item.databaseID else { return }
        let values = Self.databaseValues(item.payload)
        try database.writer.write { db in
            try db.execute(
                sql: """
                    UPDATE content SET
                        title = ?, link_icon_data = ?, link_preview_data = ?, link_preview_fetched = ?, content_type = ?, plain_text = ?, rich_text = ?, rich_text_type = ?,
                        image_data = ?, image_width = ?, image_height = ?, image_format = ?, image_angle = ?, file_references = ?,
                        content_hash = ?, content_size = ?, updated_at = ?
                    WHERE id = ?
                    """,
                arguments: [
                    item.title, item.linkIconData, item.linkPreviewData, item.hasFetchedLinkPreview,
                    values.type, values.plainText, values.richText, values.richTextType,
                    values.imageData, values.imageWidth, values.imageHeight, values.imageFormat, item.imageAngle, values.files,
                    Self.hash(item.payload), values.size, Self.milliseconds(Date()), databaseID,
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

    private func upsertApplication(_ application: SourceApplication, at now: Int64, in db: Database) throws -> Int64 {
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

    private static func item(_ row: Row) -> ClipboardItem? {
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

    private static func hash(_ payload: ClipboardPayload) -> String {
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
