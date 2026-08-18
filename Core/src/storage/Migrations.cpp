#include "Migrations.hpp"

#include <sqlite3.h>

#include <string>

namespace localscribe {
namespace {

Expected<void> execute(sqlite3 *database, const char *sql)
{
    char *message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) {
        return success();
    }
    std::string detail =
        message == nullptr ? sqlite3_errmsg(database) : message;
    sqlite3_free(message);
    return Error{LS_SQLITE_ERROR, "SQLite migration failed: " + detail};
}

Expected<int> userVersion(sqlite3 *database)
{
    sqlite3_stmt *rawStatement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "PRAGMA user_version",
            -1,
            &rawStatement,
            nullptr)
        != SQLITE_OK) {
        return Error{
            LS_SQLITE_ERROR,
            "cannot read SQLite schema version"};
    }
    const int step = sqlite3_step(rawStatement);
    const int version =
        step == SQLITE_ROW ? sqlite3_column_int(rawStatement, 0) : -1;
    sqlite3_finalize(rawStatement);
    if (step != SQLITE_ROW) {
        return Error{
            LS_SQLITE_ERROR,
            "cannot read SQLite schema version"};
    }
    return version;
}

} // namespace

Expected<void> applyMigrations(sqlite3 *database)
{
    if (database == nullptr) {
        return Error{LS_INVALID_ARGUMENT, "database is null"};
    }

    if (auto result = execute(database, "PRAGMA foreign_keys = ON");
        !result) {
        return result;
    }
    if (auto result = execute(database, "PRAGMA journal_mode = WAL");
        !result) {
        return result;
    }
    if (auto result = execute(database, "PRAGMA synchronous = FULL");
        !result) {
        return result;
    }
    if (auto result = execute(database, "PRAGMA busy_timeout = 5000");
        !result) {
        return result;
    }

    auto version = userVersion(database);
    if (!version) {
        return version.error();
    }
    if (version.value() > kJournalSchemaVersion) {
        return Error{
            LS_SCHEMA_TOO_NEW,
            "journal schema is newer than this core"};
    }
    if (version.value() == kJournalSchemaVersion) {
        return success();
    }

    static constexpr const char *kMigrationV1 = R"SQL(
BEGIN IMMEDIATE;

CREATE TABLE sessions (
    session_id TEXT PRIMARY KEY NOT NULL,
    phase INTEGER NOT NULL,
    created_at TEXT NOT NULL DEFAULT '',
    ended_at TEXT NOT NULL DEFAULT '',
    source_app TEXT NOT NULL DEFAULT '',
    local_speaker_name TEXT NOT NULL,
    asr_backend_id TEXT NOT NULL,
    asr_backend_version TEXT NOT NULL,
    diarization_backend_id TEXT NOT NULL,
    diarization_backend_version TEXT NOT NULL,
    language_mode INTEGER NOT NULL,
    microphone_source_id INTEGER NOT NULL,
    system_audio_source_id INTEGER NOT NULL,
    required_source_mask INTEGER NOT NULL,
    completeness_threshold_ns INTEGER NOT NULL,
    timeline_origin_ns INTEGER NOT NULL DEFAULT 0,
    journal_checkpoint INTEGER NOT NULL DEFAULT 0,
    highest_segment_revision INTEGER NOT NULL DEFAULT 0,
    finalize_reason INTEGER NOT NULL DEFAULT 0,
    recovery_marked INTEGER NOT NULL DEFAULT 0 CHECK (recovery_marked IN (0, 1))
);

CREATE TABLE state_events (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    event_sequence INTEGER NOT NULL,
    phase INTEGER NOT NULL,
    reason INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (session_id, event_sequence)
);

CREATE TABLE sources (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    source_id INTEGER NOT NULL,
    source_kind INTEGER NOT NULL,
    required INTEGER NOT NULL CHECK (required IN (0, 1)),
    health INTEGER NOT NULL,
    accepted_frames INTEGER NOT NULL DEFAULT 0,
    rejected_frames INTEGER NOT NULL DEFAULT 0,
    discontinuities INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (session_id, source_id)
);

CREATE TABLE source_events (
    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    source_id INTEGER NOT NULL,
    source_kind INTEGER NOT NULL,
    event_kind INTEGER NOT NULL,
    health INTEGER NOT NULL,
    start_time_ns INTEGER NOT NULL,
    end_time_ns INTEGER NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    test_injected INTEGER NOT NULL DEFAULT 0 CHECK (test_injected IN (0, 1))
);

CREATE INDEX source_events_session_time
    ON source_events(session_id, start_time_ns, event_id);

CREATE TABLE segments (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    stable_id BLOB NOT NULL CHECK (length(stable_id) = 16),
    revision INTEGER NOT NULL CHECK (revision > 0),
    source_id INTEGER NOT NULL,
    start_time_ns INTEGER NOT NULL,
    end_time_ns INTEGER NOT NULL,
    speaker_id INTEGER NOT NULL,
    speaker_label TEXT NOT NULL,
    text TEXT NOT NULL,
    language TEXT NOT NULL,
    confidence REAL NOT NULL,
    flags INTEGER NOT NULL,
    journal_checkpoint INTEGER NOT NULL,
    PRIMARY KEY (session_id, stable_id, revision)
);

CREATE INDEX segments_render_order
    ON segments(session_id, start_time_ns, end_time_ns, source_id, stable_id);

CREATE TABLE publication_receipts (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    journal_checkpoint INTEGER NOT NULL,
    highest_segment_revision INTEGER NOT NULL,
    destination INTEGER NOT NULL,
    published_at_unix_ns INTEGER NOT NULL,
    sha256_hex TEXT NOT NULL,
    file_identity TEXT NOT NULL,
    PRIMARY KEY (session_id, journal_checkpoint)
);

PRAGMA user_version = 1;
COMMIT;
)SQL";

    static constexpr const char *kMigrationV2 = R"SQL(
BEGIN IMMEDIATE;

ALTER TABLE segments
    ADD COLUMN speaker_embedding_model TEXT NOT NULL DEFAULT '';
ALTER TABLE segments
    ADD COLUMN speaker_embedding_dimension INTEGER NOT NULL DEFAULT 0
        CHECK (speaker_embedding_dimension >= 0);
ALTER TABLE segments
    ADD COLUMN speaker_embedding BLOB NOT NULL DEFAULT X''
        CHECK (length(speaker_embedding) = speaker_embedding_dimension * 4);

CREATE INDEX segments_session_speaker
    ON segments(session_id, speaker_id, stable_id, revision);

CREATE TABLE voice_profiles (
    profile_id INTEGER PRIMARY KEY AUTOINCREMENT,
    display_name TEXT NOT NULL COLLATE NOCASE UNIQUE,
    embedding_model_id TEXT NOT NULL,
    embedding_dimension INTEGER NOT NULL CHECK (embedding_dimension > 0),
    centroid BLOB NOT NULL CHECK (
        length(centroid) = embedding_dimension * 4
    ),
    observation_count INTEGER NOT NULL CHECK (observation_count > 0),
    created_at_unix_ns INTEGER NOT NULL CHECK (created_at_unix_ns >= 0),
    updated_at_unix_ns INTEGER NOT NULL CHECK (updated_at_unix_ns >= created_at_unix_ns),
    CHECK (profile_id > 0 AND profile_id <= 4611686018427387903)
);

CREATE TABLE voice_profile_prototypes (
    profile_id INTEGER NOT NULL
        REFERENCES voice_profiles(profile_id) ON DELETE CASCADE,
    prototype_index INTEGER NOT NULL CHECK (prototype_index >= 0),
    embedding_dimension INTEGER NOT NULL CHECK (embedding_dimension > 0),
    embedding BLOB NOT NULL CHECK (
        length(embedding) = embedding_dimension * 4
    ),
    PRIMARY KEY (profile_id, prototype_index)
);

CREATE TABLE voice_profile_observations (
    profile_id INTEGER NOT NULL
        REFERENCES voice_profiles(profile_id) ON DELETE CASCADE,
    session_id TEXT NOT NULL
        REFERENCES sessions(session_id) ON DELETE CASCADE,
    stable_id BLOB NOT NULL CHECK (length(stable_id) = 16),
    PRIMARY KEY (profile_id, session_id, stable_id)
);

CREATE TABLE session_voice_profile_enrollments (
    session_id TEXT NOT NULL
        REFERENCES sessions(session_id) ON DELETE CASCADE,
    original_speaker_id INTEGER NOT NULL,
    profile_id INTEGER NOT NULL CHECK (
        profile_id > 0 AND profile_id <= 4611686018427387903
    ),
    display_name TEXT NOT NULL CHECK (
        length(CAST(display_name AS BLOB)) BETWEEN 1 AND 256
    ),
    PRIMARY KEY (session_id, original_speaker_id)
);

PRAGMA user_version = 2;
COMMIT;
)SQL";

    static constexpr const char *kMigrationV3 = R"SQL(
BEGIN IMMEDIATE;

CREATE TABLE pending_speaker_groups (
    session_id TEXT NOT NULL
        REFERENCES sessions(session_id) ON DELETE CASCADE,
    group_id INTEGER NOT NULL CHECK (group_id > 0),
    deadline_monotonic_ns INTEGER NOT NULL CHECK (deadline_monotonic_ns >= 0),
    created_checkpoint INTEGER NOT NULL CHECK (created_checkpoint > 0),
    resolved_checkpoint INTEGER DEFAULT NULL CHECK (
        resolved_checkpoint IS NULL
        OR resolved_checkpoint >= created_checkpoint
    ),
    PRIMARY KEY (session_id, group_id)
);

CREATE INDEX pending_speaker_groups_unresolved_deadline
    ON pending_speaker_groups(
        session_id, resolved_checkpoint, deadline_monotonic_ns, group_id
    );

CREATE TABLE pending_speaker_segments (
    session_id TEXT NOT NULL,
    group_id INTEGER NOT NULL,
    stable_id BLOB NOT NULL CHECK (length(stable_id) = 16),
    revision INTEGER NOT NULL CHECK (revision > 0),
    source_id INTEGER NOT NULL,
    start_time_ns INTEGER NOT NULL,
    end_time_ns INTEGER NOT NULL,
    speaker_id INTEGER NOT NULL,
    speaker_label TEXT NOT NULL,
    text TEXT NOT NULL,
    language TEXT NOT NULL,
    confidence REAL NOT NULL,
    flags INTEGER NOT NULL,
    staged_checkpoint INTEGER NOT NULL CHECK (staged_checkpoint > 0),
    speaker_embedding_model TEXT NOT NULL DEFAULT '',
    speaker_embedding_dimension INTEGER NOT NULL DEFAULT 0
        CHECK (speaker_embedding_dimension >= 0),
    speaker_embedding BLOB NOT NULL DEFAULT X'' CHECK (
        length(speaker_embedding) = speaker_embedding_dimension * 4
    ),
    PRIMARY KEY (session_id, group_id, stable_id, revision),
    FOREIGN KEY (session_id, group_id)
        REFERENCES pending_speaker_groups(session_id, group_id)
        ON DELETE CASCADE
);

CREATE INDEX pending_speaker_segments_group_order
    ON pending_speaker_segments(
        session_id, group_id, start_time_ns, end_time_ns, source_id,
        stable_id, revision
    );

PRAGMA user_version = 3;
COMMIT;
)SQL";

    int currentVersion = version.value();
    if (currentVersion < 1) {
        auto migration = execute(database, kMigrationV1);
        if (!migration) {
            (void)execute(database, "ROLLBACK");
            return migration;
        }
        currentVersion = 1;
    }
    if (currentVersion < 2) {
        auto migration = execute(database, kMigrationV2);
        if (!migration) {
            (void)execute(database, "ROLLBACK");
            return migration;
        }
        currentVersion = 2;
    }
    if (currentVersion < 3) {
        auto migration = execute(database, kMigrationV3);
        if (!migration) {
            (void)execute(database, "ROLLBACK");
            return migration;
        }
    }
    return success();
}

} // namespace localscribe
