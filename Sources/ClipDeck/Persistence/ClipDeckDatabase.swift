import Foundation
import GRDB

final class ClipDeckDatabase: @unchecked Sendable {
    let writer: any DatabaseWriter

    init(path: String) throws {
        var configuration = Configuration()
        if path != ":memory:" {
            configuration.journalMode = .wal
        }
        configuration.prepareDatabase { database in
            try database.execute(sql: "PRAGMA foreign_keys = ON")
        }
        writer = try DatabaseQueue(path: path, configuration: configuration)
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
        return migrator
    }
}
