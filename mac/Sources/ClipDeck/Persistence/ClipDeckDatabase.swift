import Foundation
import GRDB

final class ClipDeckDatabase: @unchecked Sendable {
    let writer: any DatabaseWriter
    let path: String

    init(path: String) throws {
        self.path = path
        var configuration = Configuration()
        if path != ":memory:" {
            configuration.journalMode = .wal
        }
        configuration.prepareDatabase { database in
            try database.execute(sql: "PRAGMA foreign_keys = ON")
        }
        if path == ":memory:" {
            writer = try DatabaseQueue(path: path, configuration: configuration)
        } else {
            writer = try DatabasePool(path: path, configuration: configuration)
        }
        try Self.migrator.migrate(writer)
    }

    static func applicationDatabase() throws -> ClipDeckDatabase {
        let fileManager = FileManager.default
        let supportDirectory = try fileManager.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let directory = supportDirectory.appending(path: "ClipDeck", directoryHint: .isDirectory)
        try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
        return try ClipDeckDatabase(path: directory.appending(path: "history.sqlite").path)
    }

    func releaseMemory() {
        if let pool = writer as? DatabasePool {
            pool.releaseMemory()
        } else if let queue = writer as? DatabaseQueue {
            queue.releaseMemory()
        }
    }

    private static var migrator: DatabaseMigrator {
        var migrator = DatabaseMigrator()
        migrator.registerMigration("createInitialSchema") { database in
            try database.create(table: "application") { table in
                table.autoIncrementedPrimaryKey("id")
                table.column("bundle_id", .text).notNull().unique()
                table.column("name", .text).notNull()
                table.column("icon_data", .blob)
                table.column("created_at", .integer).notNull()
                table.column("updated_at", .integer).notNull()
            }

            try database.create(table: "category") { table in
                table.autoIncrementedPrimaryKey("id")
                table.column("name", .text).notNull()
                table.column("icon", .text)
                table.column("script", .text).notNull()
                table.column("preview_language", .text)
                table.column("sort_order", .integer).notNull()
                table.column("last_error_message", .text)
                table.column("last_error_at", .integer)
                table.column("created_at", .integer).notNull()
                table.column("updated_at", .integer).notNull()
            }

            try database.create(table: "content") { table in
                table.autoIncrementedPrimaryKey("id")
                table.column("content_type", .text).notNull()
                table.column("plain_text", .text)
                table.column("rich_text", .blob)
                table.column("rich_text_type", .text)
                table.column("image_data", .blob)
                table.column("image_width", .integer)
                table.column("image_height", .integer)
                table.column("image_format", .text)
                table.column("content_hash", .text).notNull()
                table.column("content_size", .integer).notNull()
                table.column("application_id", .integer)
                    .references("application", onDelete: .setNull)
                table.column("category_id", .integer)
                    .references("category", onDelete: .setNull)
                table.column("copied_at", .integer).notNull()
                table.column("last_copied_at", .integer).notNull()
                table.column("copy_count", .integer).notNull().defaults(to: 1)
                table.column("created_at", .integer).notNull()
                table.column("updated_at", .integer).notNull()
            }

            try database.create(index: "idx_content_last_copied_at", on: "content", columns: ["last_copied_at"])
            try database.create(index: "idx_content_category_id", on: "content", columns: ["category_id"])
            try database.create(index: "idx_content_application_id", on: "content", columns: ["application_id"])
            try database.create(index: "idx_content_hash_last_copied", on: "content", columns: ["content_hash", "last_copied_at"])
            try database.create(index: "idx_category_sort_order", on: "category", columns: ["sort_order"])
        }
        migrator.registerMigration("createContentFTS") { database in
            try database.execute(sql: """
                CREATE VIRTUAL TABLE content_fts USING fts5(
                    plain_text,
                    content = 'content',
                    content_rowid = 'id',
                    tokenize = 'unicode61'
                );

                INSERT INTO content_fts(rowid, plain_text)
                SELECT id, plain_text FROM content
                WHERE content_type = 'text' AND plain_text IS NOT NULL;

                CREATE TRIGGER content_fts_after_insert AFTER INSERT ON content
                WHEN new.content_type = 'text' AND new.plain_text IS NOT NULL
                BEGIN
                    INSERT INTO content_fts(rowid, plain_text) VALUES (new.id, new.plain_text);
                END;

                CREATE TRIGGER content_fts_after_delete AFTER DELETE ON content
                WHEN old.content_type = 'text' AND old.plain_text IS NOT NULL
                BEGIN
                    INSERT INTO content_fts(content_fts, rowid, plain_text)
                    VALUES ('delete', old.id, old.plain_text);
                END;

                CREATE TRIGGER content_fts_after_update AFTER UPDATE OF plain_text, content_type ON content
                BEGIN
                    INSERT INTO content_fts(content_fts, rowid, plain_text)
                    SELECT 'delete', old.id, old.plain_text
                    WHERE old.content_type = 'text' AND old.plain_text IS NOT NULL;
                    INSERT INTO content_fts(rowid, plain_text)
                    SELECT new.id, new.plain_text
                    WHERE new.content_type = 'text' AND new.plain_text IS NOT NULL;
                END;
                """)
        }
        migrator.registerMigration("tightenDefaultJSONContainerRule") { database in
            try database.execute(
                sql: """
                    UPDATE category
                    SET script = ?, updated_at = ?
                    WHERE name = 'JSON' AND script = ?
                    """,
                arguments: [
                    DefaultCategoryScripts.json,
                    Int64(Date().timeIntervalSince1970 * 1_000),
                    DefaultCategoryScripts.legacyJSON,
                ]
            )
        }
        migrator.registerMigration("tightenFormattedDefaultJSONContainerRule") { database in
            try database.execute(
                sql: """
                    UPDATE category
                    SET script = ?, updated_at = ?
                    WHERE name = 'JSON' AND script = ?
                    """,
                arguments: [
                    DefaultCategoryScripts.json,
                    Int64(Date().timeIntervalSince1970 * 1_000),
                    DefaultCategoryScripts.formattedLegacyJSON,
                ]
            )
        }
        migrator.registerMigration("addContentTitle") { database in
            try database.alter(table: "content") { table in
                table.add(column: "title", .text).notNull().defaults(to: "文本")
            }
            try database.execute(
                sql: "UPDATE content SET title = '图片' WHERE content_type = 'image'"
            )
        }
        migrator.registerMigration("indexContentTitle") { database in
            try database.execute(sql: """
                DROP TRIGGER IF EXISTS content_fts_after_insert;
                DROP TRIGGER IF EXISTS content_fts_after_delete;
                DROP TRIGGER IF EXISTS content_fts_after_update;
                DROP TABLE IF EXISTS content_fts;

                CREATE VIRTUAL TABLE content_fts USING fts5(
                    title,
                    plain_text,
                    content = 'content',
                    content_rowid = 'id',
                    tokenize = 'unicode61'
                );

                INSERT INTO content_fts(rowid, title, plain_text)
                SELECT id, title, plain_text FROM content;

                CREATE TRIGGER content_fts_after_insert AFTER INSERT ON content
                BEGIN
                    INSERT INTO content_fts(rowid, title, plain_text)
                    VALUES (new.id, new.title, new.plain_text);
                END;

                CREATE TRIGGER content_fts_after_delete AFTER DELETE ON content
                BEGIN
                    INSERT INTO content_fts(content_fts, rowid, title, plain_text)
                    VALUES ('delete', old.id, old.title, old.plain_text);
                END;

                CREATE TRIGGER content_fts_after_update AFTER UPDATE OF title, plain_text, content_type ON content
                BEGIN
                    INSERT INTO content_fts(content_fts, rowid, title, plain_text)
                    VALUES ('delete', old.id, old.title, old.plain_text);
                    INSERT INTO content_fts(rowid, title, plain_text)
                    VALUES (new.id, new.title, new.plain_text);
                END;
                """)
        }
        migrator.registerMigration("addLinkIconData") { database in
            try database.alter(table: "content") { table in
                table.add(column: "link_icon_data", .blob)
            }
        }
        migrator.registerMigration("refreshDefaultCategoriesV2") { database in
            let hadCategories = (try Int.fetchOne(database, sql: "SELECT COUNT(*) FROM category") ?? 0) > 0
            let now = Int64(Date().timeIntervalSince1970 * 1_000)
            let replacements: [(String, String, String)] = [
                ("URL", DefaultCategoryScripts.legacyURL, DefaultCategoryScripts.url),
                ("SQL", DefaultCategoryScripts.legacySQL, DefaultCategoryScripts.sql),
                ("Markdown", DefaultCategoryScripts.legacyMarkdown, DefaultCategoryScripts.markdown),
            ]
            for (name, legacyScript, formattedScript) in replacements {
                try database.execute(
                    sql: "UPDATE category SET script = ?, updated_at = ? WHERE name = ? AND script = ?",
                    arguments: [formattedScript, now, name, legacyScript]
                )
            }

            try database.execute(
                sql: "DELETE FROM category WHERE name = 'Shell' AND script = ?",
                arguments: [DefaultCategoryScripts.legacyShell]
            )

            let additions = hadCategories ? DefaultCategoryScripts.presets.filter {
                ["Java", "JavaScript", "色彩"].contains($0.name)
            } : []
            var nextOrder = (try Int.fetchOne(database, sql: "SELECT COALESCE(MAX(sort_order), -1) + 1 FROM category")) ?? 0
            for preset in additions {
                guard try Int.fetchOne(
                    database,
                    sql: "SELECT COUNT(*) FROM category WHERE name = ?",
                    arguments: [preset.name]
                ) == 0 else { continue }
                try database.execute(
                    sql: "INSERT INTO category (name, icon, script, preview_language, sort_order, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                    arguments: [preset.name, preset.icon, preset.script, preset.previewLanguage, nextOrder, now, now]
                )
                nextOrder += 1
            }
        }
        migrator.registerMigration("tightenDefaultColorRule") { database in
            try database.execute(
                sql: "UPDATE category SET script = ?, updated_at = ? WHERE name = '色彩' AND script = ?",
                arguments: [
                    DefaultCategoryScripts.color,
                    Int64(Date().timeIntervalSince1970 * 1_000),
                    DefaultCategoryScripts.permissiveColor,
                ]
            )
        }
        migrator.registerMigration("mergeDefaultDevelopmentCategories") { database in
            guard (try Int.fetchOne(database, sql: "SELECT COUNT(*) FROM category") ?? 0) > 0 else {
                return
            }
            let now = Int64(Date().timeIntervalSince1970 * 1_000)
            let legacyRows = try Row.fetchAll(
                database,
                sql: """
                    SELECT id, sort_order FROM category
                    WHERE (name = 'Java' AND script = ?)
                       OR (name = 'JavaScript' AND script = ?)
                    ORDER BY sort_order, id
                    """,
                arguments: [DefaultCategoryScripts.java, DefaultCategoryScripts.javaScript]
            )
            let appendedOrder = try Int.fetchOne(
                database,
                sql: "SELECT COALESCE(MAX(sort_order), -1) + 1 FROM category"
            ) ?? 0
            let suggestedOrder = legacyRows.compactMap { $0["sort_order"] as Int? }.min()
                ?? appendedOrder
            let codePreset = DefaultCategoryScripts.presets.first { $0.name == "代码" }!
            let codeID: Int64
            if let existingID = try Int64.fetchOne(
                database,
                sql: "SELECT id FROM category WHERE name = '代码' LIMIT 1"
            ) {
                codeID = existingID
            } else {
                try database.execute(
                    sql: "INSERT INTO category (name, icon, script, preview_language, sort_order, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                    arguments: [codePreset.name, codePreset.icon, codePreset.script, codePreset.previewLanguage, suggestedOrder, now, now]
                )
                codeID = database.lastInsertedRowID
            }

            for row in legacyRows {
                let legacyID: Int64 = row["id"]
                try database.execute(
                    sql: "UPDATE content SET category_id = ?, updated_at = ? WHERE category_id = ?",
                    arguments: [codeID, now, legacyID]
                )
                try database.execute(sql: "DELETE FROM category WHERE id = ?", arguments: [legacyID])
            }

            let orderedIDs = try Int64.fetchAll(
                database,
                sql: "SELECT id FROM category ORDER BY CASE WHEN name = '色彩' THEN 1 ELSE 0 END, sort_order, id"
            )
            for (index, id) in orderedIDs.enumerated() {
                try database.execute(
                    sql: "UPDATE category SET sort_order = ?, updated_at = ? WHERE id = ?",
                    arguments: [index, now, id]
                )
            }
        }
        migrator.registerMigration("mergeFormattedDefaultShellIntoCode") { database in
            guard let codeID = try Int64.fetchOne(
                database,
                sql: "SELECT id FROM category WHERE name = '代码' LIMIT 1"
            ) else { return }
            let shellIDs = try Int64.fetchAll(
                database,
                sql: "SELECT id FROM category WHERE name = 'Shell' AND script IN (?, ?)",
                arguments: [DefaultCategoryScripts.legacyShell, DefaultCategoryScripts.formattedLegacyShell]
            )
            let now = Int64(Date().timeIntervalSince1970 * 1_000)
            for shellID in shellIDs {
                try database.execute(
                    sql: "UPDATE content SET category_id = ?, updated_at = ? WHERE category_id = ?",
                    arguments: [codeID, now, shellID]
                )
                try database.execute(sql: "DELETE FROM category WHERE id = ?", arguments: [shellID])
            }
        }
        migrator.registerMigration("useWeightedCodeDetectionEngine") { database in
            try database.execute(
                sql: "UPDATE category SET script = ?, updated_at = ? WHERE name = '代码' AND script = ?",
                arguments: [
                    DefaultCategoryScripts.code,
                    Int64(Date().timeIntervalSince1970 * 1_000),
                    DefaultCategoryScripts.relevanceBasedCode,
                ]
            )
        }
        migrator.registerMigration("supportBareHexColors") { database in
            try database.execute(
                sql: "UPDATE category SET script = ?, updated_at = ? WHERE name = '色彩' AND script = ?",
                arguments: [
                    DefaultCategoryScripts.color,
                    Int64(Date().timeIntervalSince1970 * 1_000),
                    DefaultCategoryScripts.hashPrefixedColor,
                ]
            )
        }
        migrator.registerMigration("addLinkPreviewData") { database in
            try database.alter(table: "content") { table in
                table.add(column: "link_preview_data", .blob)
                table.add(column: "link_preview_fetched", .boolean).notNull().defaults(to: false)
            }
        }
        return migrator
    }
}
