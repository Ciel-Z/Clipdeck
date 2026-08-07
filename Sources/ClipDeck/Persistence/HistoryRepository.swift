import CryptoKit
import Foundation
import GRDB

enum DefaultCategoryScripts {
    static let legacyJSON = "function classify(item) { try { JSON.parse(item.text); return true; } catch (_) { return false; } }"
    static let formattedLegacyJSON = """
    function classify(item) {
        try {
            JSON.parse(item.text);
            return true;
        }
        catch (_) {
            return false;
        }
    }
    """
    static let json = """
    function classify(item) {
        const text = item.text.trim();
        const isContainer =
            (text.startsWith("{") && text.endsWith("}")) ||
            (text.startsWith("[") && text.endsWith("]"));
        if (!isContainer) return false;
        try {
            const value = JSON.parse(text);
            return value !== null && typeof value === "object";
        } catch (_) {
            return false;
        }
    }
    """
}

final class HistoryRepository: @unchecked Sendable {
    private let database: ClipDeckDatabase
    private let duplicateWindowMilliseconds: Int64 = 10_000

    init(database: ClipDeckDatabase) {
        self.database = database
    }

    func ensureDefaultCategories() throws {
        try database.writer.write { db in
            guard try Int.fetchOne(db, sql: "SELECT COUNT(*) FROM category") == 0 else { return }
            let defaults: [(String, String, String, Int)] = [
                ("JSON", "curlybraces", DefaultCategoryScripts.json, 0),
                ("URL", "link", "function classify(item) { return /^https?:\\/\\/\\S+$/i.test(item.text.trim()); }", 1),
                ("SQL", "cylinder", "function classify(item) { return /^(select|insert|update|delete|create|alter|drop)\\b/i.test(item.text.trim()); }", 2),
                ("Shell", "terminal", "function classify(item) { return /^(sudo |git |cd |ls |npm |yarn |pnpm |brew |curl |ssh )/.test(item.text.trim()); }", 3),
                ("Markdown", "text.badge.checkmark", "function classify(item) { return /(^|\\n)(#{1,6} |[-*] |```)/.test(item.text); }", 4),
            ]
            let now = Self.milliseconds(Date())
            for (name, icon, script, order) in defaults {
                try db.execute(
                    sql: "INSERT INTO category (name, icon, script, preview_language, sort_order, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                    arguments: [name, icon, script, name.lowercased(), order, now, now]
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
            payload: item.payload,
            sourceApplication: item.sourceApplication,
            category: category,
            copiedAt: item.copiedAt
        )
    }

    func recordCategoryError(_ message: String?, for categoryID: Int64) throws {
        try database.writer.write { db in
            try db.execute(
                sql: "UPDATE category SET last_error_message = ?, last_error_at = ?, updated_at = ? WHERE id = ?",
                arguments: [message, message == nil ? nil : Self.milliseconds(Date()), Self.milliseconds(Date()), categoryID]
            )
        }
    }

    func save(_ item: ClipboardItem) throws -> ClipboardItem {
        let now = Self.milliseconds(item.copiedAt)
        let hash = Self.hash(item.payload)
        let persisted = try database.writer.write { db -> (id: Int64, title: String) in
            let applicationID = try upsertApplication(item.sourceApplication, at: now, in: db)
            if let existingID = try Int64.fetchOne(
                db,
                sql: "SELECT id FROM content WHERE content_hash = ? AND last_copied_at >= ? ORDER BY last_copied_at DESC LIMIT 1",
                arguments: [hash, now - duplicateWindowMilliseconds]
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
                return (existingID, existingTitle)
            }

            let values = Self.databaseValues(item.payload)
            try db.execute(
                sql: """
                    INSERT INTO content (
                        title, content_type, plain_text, rich_text, rich_text_type,
                        image_data, image_width, image_height, image_format,
                        content_hash, content_size, application_id,
                        copied_at, last_copied_at, copy_count, created_at, updated_at
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?)
                    """,
                arguments: [
                    item.title, values.type, values.plainText, values.richText, values.richTextType,
                    values.imageData, values.imageWidth, values.imageHeight, values.imageFormat,
                    hash, values.size, applicationID, now, now, now, now,
                ]
            )
            return (db.lastInsertedRowID, item.title)
        }
        return ClipboardItem(
            databaseID: persisted.id,
            title: persisted.title,
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
                        title = ?, content_type = ?, plain_text = ?, rich_text = ?, rich_text_type = ?,
                        image_data = ?, image_width = ?, image_height = ?, image_format = ?,
                        content_hash = ?, content_size = ?, updated_at = ?
                    WHERE id = ?
                    """,
                arguments: [
                    item.title, values.type, values.plainText, values.richText, values.richTextType,
                    values.imageData, values.imageWidth, values.imageHeight, values.imageFormat,
                    Self.hash(item.payload), values.size, Self.milliseconds(Date()), databaseID,
                ]
            )
        }
    }

    @discardableResult
    func deleteExpired(retentionDays: Int = 30) throws -> Int {
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
        imageData: Data?, imageWidth: Int?, imageHeight: Int?, imageFormat: String?, size: Int
    ) {
        switch payload {
        case let .text(text, rtf):
            return ("text", text, rtf, rtf == nil ? nil : "rtf", nil, nil, nil, nil, text.utf8.count + (rtf?.count ?? 0))
        case let .image(data, width, height, format):
            return ("image", nil, nil, nil, data, width, height, format.rawValue, data.count)
        }
    }
}
