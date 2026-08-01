#include "RecoveryJournal.hpp"

#include "Migrations.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace localscribe {
namespace {

class Statement {
public:
    Statement() = default;
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    ~Statement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    sqlite3_stmt **out() { return &statement_; }
    sqlite3_stmt *get() const { return statement_; }

private:
    sqlite3_stmt *statement_{};
};

Error sqliteError(sqlite3 *database, std::string context)
{
    return Error{
        LS_SQLITE_ERROR,
        std::move(context) + ": " + sqlite3_errmsg(database)};
}

Expected<void> execute(sqlite3 *database, const char *sql)
{
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return sqliteError(database, "SQLite statement failed");
    }
    return success();
}

Expected<void> prepare(
    sqlite3 *database,
    const char *sql,
    Statement &statement)
{
    if (sqlite3_prepare_v2(database, sql, -1, statement.out(), nullptr)
        != SQLITE_OK) {
        return sqliteError(database, "cannot prepare SQLite statement");
    }
    return success();
}

Expected<void> stepDone(sqlite3 *database, sqlite3_stmt *statement)
{
    if (sqlite3_step(statement) != SQLITE_DONE) {
        return sqliteError(database, "cannot execute SQLite statement");
    }
    return success();
}

void bindText(sqlite3_stmt *statement, int index, const std::string &value)
{
    sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
}

std::string columnText(sqlite3_stmt *statement, int index)
{
    const auto *bytes = sqlite3_column_text(statement, index);
    const int length = sqlite3_column_bytes(statement, index);
    if (bytes == nullptr || length <= 0) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char *>(bytes),
        static_cast<std::size_t>(length));
}

class Transaction {
public:
    explicit Transaction(sqlite3 *database) : database_(database)
    {
        active_ = sqlite3_exec(
                      database_,
                      "BEGIN IMMEDIATE",
                      nullptr,
                      nullptr,
                      nullptr)
            == SQLITE_OK;
    }
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    ~Transaction()
    {
        if (active_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] Expected<void> commit()
    {
        if (!active_) {
            return sqliteError(database_, "transaction was not started");
        }
        if (sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr)
            != SQLITE_OK) {
            return sqliteError(database_, "cannot commit transaction");
        }
        active_ = false;
        return success();
    }

private:
    sqlite3 *database_{};
    bool active_{false};
};

bool validSessionId(const std::string &value)
{
    if (value.empty() || value.size() > 128) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte == 0 || byte < 0x20 || byte == 0x7F;
    });
}

bool sameSegment(sqlite3_stmt *row, const TranscriptSegment &segment)
{
    const auto *stable = static_cast<const std::uint8_t *>(
        sqlite3_column_blob(row, 0));
    if (stable == nullptr || sqlite3_column_bytes(row, 0) != 16
        || !std::equal(
            segment.stableId.begin(),
            segment.stableId.end(),
            stable)) {
        return false;
    }
    return static_cast<std::uint64_t>(sqlite3_column_int64(row, 1))
            == segment.sourceId
        && sqlite3_column_int64(row, 2) == segment.startTimeNs
        && sqlite3_column_int64(row, 3) == segment.endTimeNs
        && static_cast<std::uint64_t>(sqlite3_column_int64(row, 4))
            == segment.speakerId
        && columnText(row, 5) == segment.speakerLabel
        && columnText(row, 6) == segment.text
        && columnText(row, 7) == segment.language
        && static_cast<float>(sqlite3_column_double(row, 8))
            == segment.confidence
        && static_cast<std::uint32_t>(sqlite3_column_int64(row, 9))
            == segment.flags;
}

bool validDigest(const std::string &digest)
{
    return digest.size() == 64
        && std::all_of(
            digest.begin(),
            digest.end(),
            [](unsigned char value) { return std::isxdigit(value) != 0; });
}

bool publicationMustMatchCurrentCheckpoint(ls_phase_t phase)
{
    return phase == LS_PHASE_RECOVERY_REQUIRED
        || phase == LS_PHASE_COMPLETE
        || phase == LS_PHASE_INCOMPLETE_SOURCES
        || phase == LS_PHASE_INTERRUPTED
        || phase == LS_PHASE_FAILED_TO_START;
}

} // namespace

RecoveryJournal::RecoveryJournal(sqlite3 *database, std::string path)
    : database_(database), path_(std::move(path))
{
}

RecoveryJournal::~RecoveryJournal()
{
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
    }
}

Expected<std::shared_ptr<RecoveryJournal>>
RecoveryJournal::open(const std::string &path)
{
    if (path.empty() || path.find('\0') != std::string::npos) {
        return Error{LS_INVALID_ARGUMENT, "journal path is empty or invalid"};
    }

    sqlite3 *database = nullptr;
    const int result = sqlite3_open_v2(
        path.c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK) {
        const std::string detail =
            database == nullptr ? "unknown SQLite open error"
                                : sqlite3_errmsg(database);
        if (database != nullptr) {
            sqlite3_close_v2(database);
        }
        return Error{LS_SQLITE_ERROR, "cannot open journal: " + detail};
    }

    auto migration = applyMigrations(database);
    if (!migration) {
        sqlite3_close_v2(database);
        return migration.error();
    }
    return std::shared_ptr<RecoveryJournal>(
        new RecoveryJournal(database, path));
}

Expected<void> RecoveryJournal::createSession(
    const SessionRecord &session,
    std::span<const SourceRecord> sources)
{
    std::lock_guard lock(mutex_);
    if (!validSessionId(session.sessionId)) {
        return Error{LS_INVALID_ARGUMENT, "session ID is invalid"};
    }
    if (session.phase != LS_PHASE_PREPARING) {
        return Error{
            LS_INVALID_STATE,
            "new journal session must start in preparing"};
    }
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin session transaction");
    }

    Statement statement;
    auto prepared = prepare(
        database_,
        R"SQL(
INSERT INTO sessions(
    session_id, phase, created_at, ended_at, source_app,
    local_speaker_name, asr_backend_id, asr_backend_version,
    diarization_backend_id, diarization_backend_version, language_mode,
    microphone_source_id, system_audio_source_id, required_source_mask,
    completeness_threshold_ns, journal_checkpoint
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
)SQL",
        statement);
    if (!prepared) {
        return prepared;
    }
    bindText(statement.get(), 1, session.sessionId);
    sqlite3_bind_int(statement.get(), 2, session.phase);
    bindText(statement.get(), 3, session.createdAt);
    bindText(statement.get(), 4, session.endedAt);
    bindText(statement.get(), 5, session.sourceApp);
    bindText(statement.get(), 6, session.localSpeakerName);
    bindText(statement.get(), 7, session.asrBackendId);
    bindText(statement.get(), 8, session.asrBackendVersion);
    bindText(statement.get(), 9, session.diarizationBackendId);
    bindText(statement.get(), 10, session.diarizationBackendVersion);
    sqlite3_bind_int(statement.get(), 11, session.languageMode);
    sqlite3_bind_int64(
        statement.get(),
        12,
        static_cast<sqlite3_int64>(session.microphoneSourceId));
    sqlite3_bind_int64(
        statement.get(),
        13,
        static_cast<sqlite3_int64>(session.systemAudioSourceId));
    sqlite3_bind_int64(statement.get(), 14, session.requiredSourceMask);
    sqlite3_bind_int64(
        statement.get(),
        15,
        session.completenessThresholdNs);
    if (auto stepped = stepDone(database_, statement.get()); !stepped) {
        return stepped;
    }

    Statement event;
    prepared = prepare(
        database_,
        "INSERT INTO state_events(session_id, event_sequence, phase, reason) "
        "VALUES (?, 1, ?, 0)",
        event);
    if (!prepared) {
        return prepared;
    }
    bindText(event.get(), 1, session.sessionId);
    sqlite3_bind_int(event.get(), 2, session.phase);
    if (auto stepped = stepDone(database_, event.get()); !stepped) {
        return stepped;
    }

    for (const auto &source : sources) {
        Statement sourceStatement;
        prepared = prepare(
            database_,
            R"SQL(
INSERT INTO sources(
    session_id, source_id, source_kind, required, health
) VALUES (?, ?, ?, ?, ?)
)SQL",
            sourceStatement);
        if (!prepared) {
            return prepared;
        }
        bindText(sourceStatement.get(), 1, session.sessionId);
        sqlite3_bind_int64(
            sourceStatement.get(),
            2,
            static_cast<sqlite3_int64>(source.sourceId));
        sqlite3_bind_int(sourceStatement.get(), 3, source.sourceKind);
        sqlite3_bind_int(sourceStatement.get(), 4, source.required ? 1 : 0);
        sqlite3_bind_int(sourceStatement.get(), 5, source.health);
        if (auto stepped = stepDone(database_, sourceStatement.get());
            !stepped) {
            return stepped;
        }
    }
    return transaction.commit();
}

Expected<SessionRecord>
RecoveryJournal::loadSessionLocked(const std::string &sessionId)
{
    Statement statement;
    auto prepared = prepare(
        database_,
        R"SQL(
SELECT
    session_id, phase, created_at, ended_at, source_app,
    local_speaker_name, asr_backend_id, asr_backend_version,
    diarization_backend_id, diarization_backend_version, language_mode,
    microphone_source_id, system_audio_source_id, required_source_mask,
    completeness_threshold_ns, timeline_origin_ns, journal_checkpoint,
    highest_segment_revision, finalize_reason
FROM sessions
WHERE session_id = ?
)SQL",
        statement);
    if (!prepared) {
        return prepared.error();
    }
    bindText(statement.get(), 1, sessionId);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Error{LS_NOT_FOUND, "journal session was not found"};
    }
    if (step != SQLITE_ROW) {
        return sqliteError(database_, "cannot load journal session");
    }

    SessionRecord record;
    record.sessionId = columnText(statement.get(), 0);
    record.phase = sqlite3_column_int(statement.get(), 1);
    record.createdAt = columnText(statement.get(), 2);
    record.endedAt = columnText(statement.get(), 3);
    record.sourceApp = columnText(statement.get(), 4);
    record.localSpeakerName = columnText(statement.get(), 5);
    record.asrBackendId = columnText(statement.get(), 6);
    record.asrBackendVersion = columnText(statement.get(), 7);
    record.diarizationBackendId = columnText(statement.get(), 8);
    record.diarizationBackendVersion = columnText(statement.get(), 9);
    record.languageMode = sqlite3_column_int(statement.get(), 10);
    record.microphoneSourceId =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 11));
    record.systemAudioSourceId =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 12));
    record.requiredSourceMask =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 13));
    record.completenessThresholdNs = sqlite3_column_int64(statement.get(), 14);
    record.timelineOriginNs = sqlite3_column_int64(statement.get(), 15);
    record.journalCheckpoint =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 16));
    record.highestSegmentRevision =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 17));
    record.finalizeReason = sqlite3_column_int(statement.get(), 18);
    return record;
}

Expected<SessionRecord>
RecoveryJournal::loadSession(const std::string &sessionId)
{
    std::lock_guard lock(mutex_);
    return loadSessionLocked(sessionId);
}

Expected<std::uint64_t> RecoveryJournal::transition(
    const std::string &sessionId,
    ls_phase_t expected,
    ls_phase_t next,
    ls_finalize_reason_t reason)
{
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin phase transaction");
    }

    auto current = loadSessionLocked(sessionId);
    if (!current) {
        return current.error();
    }
    if (current.value().phase != expected) {
        return Error{
            LS_INVALID_STATE,
            "journal phase changed before transition"};
    }

    Statement sequence;
    auto prepared = prepare(
        database_,
        "SELECT COALESCE(MAX(event_sequence), 0) + 1 "
        "FROM state_events WHERE session_id = ?",
        sequence);
    if (!prepared) {
        return prepared.error();
    }
    bindText(sequence.get(), 1, sessionId);
    if (sqlite3_step(sequence.get()) != SQLITE_ROW) {
        return sqliteError(database_, "cannot allocate state event sequence");
    }
    const sqlite3_int64 nextSequence = sqlite3_column_int64(sequence.get(), 0);
    const std::uint64_t checkpoint =
        current.value().journalCheckpoint + 1u;

    Statement update;
    prepared = prepare(
        database_,
        "UPDATE sessions SET phase = ?, finalize_reason = ?, "
        "journal_checkpoint = ? WHERE session_id = ? AND phase = ?",
        update);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(update.get(), 1, next);
    sqlite3_bind_int(update.get(), 2, reason);
    sqlite3_bind_int64(
        update.get(),
        3,
        static_cast<sqlite3_int64>(checkpoint));
    bindText(update.get(), 4, sessionId);
    sqlite3_bind_int(update.get(), 5, expected);
    if (auto stepped = stepDone(database_, update.get()); !stepped) {
        return stepped.error();
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{LS_CONFLICT, "session phase update lost a race"};
    }

    Statement event;
    prepared = prepare(
        database_,
        "INSERT INTO state_events(session_id, event_sequence, phase, reason) "
        "VALUES (?, ?, ?, ?)",
        event);
    if (!prepared) {
        return prepared.error();
    }
    bindText(event.get(), 1, sessionId);
    sqlite3_bind_int64(event.get(), 2, nextSequence);
    sqlite3_bind_int(event.get(), 3, next);
    sqlite3_bind_int(event.get(), 4, reason);
    if (auto stepped = stepDone(database_, event.get()); !stepped) {
        return stepped.error();
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }
    return checkpoint;
}

Expected<std::uint64_t> RecoveryJournal::appendFinalSegment(
    const std::string &sessionId,
    const TranscriptSegment &segment)
{
    if ((segment.flags & LS_SEGMENT_FLAG_FINAL) == 0
        || segment.revision == 0 || segment.endTimeNs < segment.startTimeNs
        || !std::isfinite(segment.confidence)
        || segment.confidence < 0.0F || segment.confidence > 1.0F) {
        return Error{LS_INVALID_ARGUMENT, "final segment is invalid"};
    }

    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin segment transaction");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }

    Statement existing;
    auto prepared = prepare(
        database_,
        R"SQL(
SELECT stable_id, source_id, start_time_ns, end_time_ns, speaker_id,
       speaker_label, text, language, confidence, flags,
       revision, journal_checkpoint
FROM segments
WHERE session_id = ? AND stable_id = ?
ORDER BY revision DESC
LIMIT 1
)SQL",
        existing);
    if (!prepared) {
        return prepared.error();
    }
    bindText(existing.get(), 1, sessionId);
    sqlite3_bind_blob(
        existing.get(),
        2,
        segment.stableId.data(),
        static_cast<int>(segment.stableId.size()),
        SQLITE_TRANSIENT);
    const int existingStep = sqlite3_step(existing.get());
    if (existingStep != SQLITE_ROW && existingStep != SQLITE_DONE) {
        return sqliteError(database_, "cannot inspect segment revision");
    }
    if (existingStep == SQLITE_ROW) {
        const auto currentRevision =
            static_cast<std::uint32_t>(sqlite3_column_int64(existing.get(), 10));
        if (segment.revision < currentRevision) {
            return Error{
                LS_CONFLICT,
                "segment revision would move backwards"};
        }
        if (segment.revision == currentRevision) {
            if (!sameSegment(existing.get(), segment)) {
                return Error{
                    LS_CONFLICT,
                    "same segment revision has different content"};
            }
            const auto checkpoint = static_cast<std::uint64_t>(
                sqlite3_column_int64(existing.get(), 11));
            return checkpoint;
        }
    }

    const std::uint64_t checkpoint =
        session.value().journalCheckpoint + 1u;
    Statement insert;
    prepared = prepare(
        database_,
        R"SQL(
INSERT INTO segments(
    session_id, stable_id, revision, source_id, start_time_ns, end_time_ns,
    speaker_id, speaker_label, text, language, confidence, flags,
    journal_checkpoint
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
        insert);
    if (!prepared) {
        return prepared.error();
    }
    bindText(insert.get(), 1, sessionId);
    sqlite3_bind_blob(
        insert.get(),
        2,
        segment.stableId.data(),
        static_cast<int>(segment.stableId.size()),
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 3, segment.revision);
    sqlite3_bind_int64(
        insert.get(),
        4,
        static_cast<sqlite3_int64>(segment.sourceId));
    sqlite3_bind_int64(insert.get(), 5, segment.startTimeNs);
    sqlite3_bind_int64(insert.get(), 6, segment.endTimeNs);
    sqlite3_bind_int64(
        insert.get(),
        7,
        static_cast<sqlite3_int64>(segment.speakerId));
    bindText(insert.get(), 8, segment.speakerLabel);
    bindText(insert.get(), 9, segment.text);
    bindText(insert.get(), 10, segment.language);
    sqlite3_bind_double(insert.get(), 11, segment.confidence);
    sqlite3_bind_int64(insert.get(), 12, segment.flags);
    sqlite3_bind_int64(
        insert.get(),
        13,
        static_cast<sqlite3_int64>(checkpoint));
    if (auto stepped = stepDone(database_, insert.get()); !stepped) {
        return stepped.error();
    }

    Statement update;
    prepared = prepare(
        database_,
        "UPDATE sessions SET journal_checkpoint = ?, "
        "highest_segment_revision = MAX(highest_segment_revision, ?), "
        "timeline_origin_ns = CASE "
        "WHEN timeline_origin_ns = 0 OR timeline_origin_ns > ? THEN ? "
        "ELSE timeline_origin_ns END "
        "WHERE session_id = ?",
        update);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        update.get(),
        1,
        static_cast<sqlite3_int64>(checkpoint));
    sqlite3_bind_int64(update.get(), 2, segment.revision);
    sqlite3_bind_int64(update.get(), 3, segment.startTimeNs);
    sqlite3_bind_int64(update.get(), 4, segment.startTimeNs);
    bindText(update.get(), 5, sessionId);
    if (auto stepped = stepDone(database_, update.get()); !stepped) {
        return stepped.error();
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }
    return checkpoint;
}

Expected<std::uint64_t> RecoveryJournal::recordSourceEvent(
    const std::string &sessionId,
    const SourceGap &event)
{
    if (event.sourceId == 0 || event.endTimeNs < event.startTimeNs) {
        return Error{LS_INVALID_ARGUMENT, "source event is invalid"};
    }
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin source event transaction");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }
    const std::uint64_t checkpoint =
        session.value().journalCheckpoint + 1u;

    Statement insert;
    auto prepared = prepare(
        database_,
        R"SQL(
INSERT INTO source_events(
    session_id, source_id, source_kind, event_kind, health,
    start_time_ns, end_time_ns, reason, test_injected
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
        insert);
    if (!prepared) {
        return prepared.error();
    }
    bindText(insert.get(), 1, sessionId);
    sqlite3_bind_int64(
        insert.get(),
        2,
        static_cast<sqlite3_int64>(event.sourceId));
    sqlite3_bind_int(insert.get(), 3, event.sourceKind);
    sqlite3_bind_int(insert.get(), 4, event.eventKind);
    sqlite3_bind_int(insert.get(), 5, event.health);
    sqlite3_bind_int64(insert.get(), 6, event.startTimeNs);
    sqlite3_bind_int64(insert.get(), 7, event.endTimeNs);
    bindText(insert.get(), 8, event.reason);
    sqlite3_bind_int(insert.get(), 9, event.testInjected ? 1 : 0);
    if (auto stepped = stepDone(database_, insert.get()); !stepped) {
        return stepped.error();
    }

    Statement updateSource;
    const bool changesHealth =
        event.eventKind == LS_SOURCE_EVENT_READY
        || event.eventKind == LS_SOURCE_EVENT_ACTIVE
        || event.eventKind == LS_SOURCE_EVENT_UNAVAILABLE
        || event.eventKind == LS_SOURCE_EVENT_RECOVERED
        || event.eventKind == LS_SOURCE_EVENT_PERMANENTLY_LOST;
    prepared = prepare(
        database_,
        "UPDATE sources SET health = CASE WHEN ? != 0 THEN ? ELSE health END, "
        "discontinuities = "
        "discontinuities + CASE WHEN ? IN (?, ?, ?) THEN 1 ELSE 0 END "
        "WHERE session_id = ? AND source_id = ?",
        updateSource);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(updateSource.get(), 1, changesHealth ? 1 : 0);
    sqlite3_bind_int(updateSource.get(), 2, event.health);
    sqlite3_bind_int(updateSource.get(), 3, event.eventKind);
    sqlite3_bind_int(
        updateSource.get(),
        4,
        LS_SOURCE_EVENT_DISCONTINUITY);
    sqlite3_bind_int(updateSource.get(), 5, LS_SOURCE_EVENT_UNAVAILABLE);
    sqlite3_bind_int(
        updateSource.get(),
        6,
        LS_SOURCE_EVENT_PERMANENTLY_LOST);
    bindText(updateSource.get(), 7, sessionId);
    sqlite3_bind_int64(
        updateSource.get(),
        8,
        static_cast<sqlite3_int64>(event.sourceId));
    if (auto stepped = stepDone(database_, updateSource.get()); !stepped) {
        return stepped.error();
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{LS_INVALID_ARGUMENT, "source is not configured"};
    }

    Statement updateSession;
    prepared = prepare(
        database_,
        "UPDATE sessions SET journal_checkpoint = ? WHERE session_id = ?",
        updateSession);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        updateSession.get(),
        1,
        static_cast<sqlite3_int64>(checkpoint));
    bindText(updateSession.get(), 2, sessionId);
    if (auto stepped = stepDone(database_, updateSession.get()); !stepped) {
        return stepped.error();
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }
    return checkpoint;
}

Expected<void> RecoveryJournal::recordFrameAccepted(
    const std::string &sessionId,
    std::uint64_t sourceId)
{
    return recordFramesAccepted(sessionId, sourceId, 1);
}

Expected<void> RecoveryJournal::recordFramesAccepted(
    const std::string &sessionId,
    std::uint64_t sourceId,
    std::uint64_t count)
{
    if (count == 0
        || count
            > static_cast<std::uint64_t>(
                std::numeric_limits<sqlite3_int64>::max())) {
        return Error{LS_INVALID_ARGUMENT, "accepted frame count is invalid"};
    }
    std::lock_guard lock(mutex_);
    Statement statement;
    auto prepared = prepare(
        database_,
        "UPDATE sources SET accepted_frames = accepted_frames + ? "
        "WHERE session_id = ? AND source_id = ?",
        statement);
    if (!prepared) {
        return prepared;
    }
    sqlite3_bind_int64(
        statement.get(),
        1,
        static_cast<sqlite3_int64>(count));
    bindText(statement.get(), 2, sessionId);
    sqlite3_bind_int64(
        statement.get(),
        3,
        static_cast<sqlite3_int64>(sourceId));
    if (auto stepped = stepDone(database_, statement.get()); !stepped) {
        return stepped;
    }
    return sqlite3_changes(database_) == 1
        ? success()
        : Expected<void>{
              Error{LS_INVALID_ARGUMENT, "source is not configured"}};
}

Expected<void> RecoveryJournal::recordFrameRejected(
    const std::string &sessionId,
    std::uint64_t sourceId,
    bool discontinuity)
{
    return recordFramesRejected(
        sessionId,
        sourceId,
        1,
        discontinuity);
}

Expected<void> RecoveryJournal::recordFramesRejected(
    const std::string &sessionId,
    std::uint64_t sourceId,
    std::uint64_t count,
    bool discontinuity)
{
    if (count == 0
        || count
            > static_cast<std::uint64_t>(
                std::numeric_limits<sqlite3_int64>::max())) {
        return Error{LS_INVALID_ARGUMENT, "rejected frame count is invalid"};
    }
    std::lock_guard lock(mutex_);
    Statement statement;
    auto prepared = prepare(
        database_,
        "UPDATE sources SET rejected_frames = rejected_frames + ?, "
        "discontinuities = discontinuities + ? "
        "WHERE session_id = ? AND source_id = ?",
        statement);
    if (!prepared) {
        return prepared;
    }
    sqlite3_bind_int64(
        statement.get(),
        1,
        static_cast<sqlite3_int64>(count));
    sqlite3_bind_int64(
        statement.get(),
        2,
        discontinuity
            ? static_cast<sqlite3_int64>(count)
            : sqlite3_int64{0});
    bindText(statement.get(), 3, sessionId);
    sqlite3_bind_int64(
        statement.get(),
        4,
        static_cast<sqlite3_int64>(sourceId));
    if (auto stepped = stepDone(database_, statement.get()); !stepped) {
        return stepped;
    }
    return sqlite3_changes(database_) == 1
        ? success()
        : Expected<void>{
              Error{LS_INVALID_ARGUMENT, "source is not configured"}};
}

Expected<JournalSnapshot>
RecoveryJournal::snapshot(const std::string &sessionId)
{
    std::lock_guard lock(mutex_);
    if (auto begun = execute(database_, "BEGIN"); !begun) {
        return begun.error();
    }
    bool active = true;
    auto rollback = [&]() {
        if (active) {
            (void)execute(database_, "ROLLBACK");
        }
    };

    auto session = loadSessionLocked(sessionId);
    if (!session) {
        rollback();
        return session.error();
    }
    JournalSnapshot result;
    result.session = session.takeValue();

    Statement sources;
    auto prepared = prepare(
        database_,
        "SELECT source_id, source_kind, required, health, accepted_frames, "
        "rejected_frames, discontinuities FROM sources "
        "WHERE session_id = ? ORDER BY source_kind, source_id",
        sources);
    if (!prepared) {
        rollback();
        return prepared.error();
    }
    bindText(sources.get(), 1, sessionId);
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(sources.get())) == SQLITE_ROW) {
        SourceRecord source;
        source.sourceId = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 0));
        source.sourceKind = sqlite3_column_int(sources.get(), 1);
        source.required = sqlite3_column_int(sources.get(), 2) != 0;
        source.health = sqlite3_column_int(sources.get(), 3);
        source.acceptedFrames = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 4));
        source.rejectedFrames = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 5));
        source.discontinuities = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 6));
        result.sources.push_back(std::move(source));
    }
    if (step != SQLITE_DONE) {
        rollback();
        return sqliteError(database_, "cannot read source snapshot");
    }

    Statement gaps;
    prepared = prepare(
        database_,
        "SELECT source_id, source_kind, event_kind, health, start_time_ns, "
        "end_time_ns, reason, test_injected FROM source_events "
        "WHERE session_id = ? ORDER BY start_time_ns, event_id",
        gaps);
    if (!prepared) {
        rollback();
        return prepared.error();
    }
    bindText(gaps.get(), 1, sessionId);
    while ((step = sqlite3_step(gaps.get())) == SQLITE_ROW) {
        SourceGap gap;
        gap.sourceId = static_cast<std::uint64_t>(
            sqlite3_column_int64(gaps.get(), 0));
        gap.sourceKind = sqlite3_column_int(gaps.get(), 1);
        gap.eventKind = sqlite3_column_int(gaps.get(), 2);
        gap.health = sqlite3_column_int(gaps.get(), 3);
        gap.startTimeNs = sqlite3_column_int64(gaps.get(), 4);
        gap.endTimeNs = sqlite3_column_int64(gaps.get(), 5);
        gap.reason = columnText(gaps.get(), 6);
        gap.testInjected = sqlite3_column_int(gaps.get(), 7) != 0;
        result.gaps.push_back(std::move(gap));
    }
    if (step != SQLITE_DONE) {
        rollback();
        return sqliteError(database_, "cannot read source event snapshot");
    }

    Statement segments;
    prepared = prepare(
        database_,
        R"SQL(
SELECT
    s.stable_id, s.source_id, s.start_time_ns, s.end_time_ns, s.speaker_id,
    s.speaker_label, s.text, s.language, s.confidence, s.revision, s.flags,
    s.journal_checkpoint
FROM segments AS s
WHERE s.session_id = ?
  AND (s.flags & ?) != 0
  AND NOT EXISTS (
      SELECT 1 FROM segments AS newer
      WHERE newer.session_id = s.session_id
        AND newer.stable_id = s.stable_id
        AND newer.revision > s.revision
  )
ORDER BY s.start_time_ns, s.end_time_ns, s.source_id, hex(s.stable_id)
)SQL",
        segments);
    if (!prepared) {
        rollback();
        return prepared.error();
    }
    bindText(segments.get(), 1, sessionId);
    sqlite3_bind_int(segments.get(), 2, LS_SEGMENT_FLAG_FINAL);
    while ((step = sqlite3_step(segments.get())) == SQLITE_ROW) {
        TranscriptSegment segment;
        const auto *stable = static_cast<const std::uint8_t *>(
            sqlite3_column_blob(segments.get(), 0));
        if (stable == nullptr || sqlite3_column_bytes(segments.get(), 0) != 16) {
            rollback();
            return Error{LS_RECOVERY_ERROR, "journal has invalid stable ID"};
        }
        std::copy_n(stable, 16, segment.stableId.begin());
        segment.sourceId = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 1));
        segment.startTimeNs = sqlite3_column_int64(segments.get(), 2);
        segment.endTimeNs = sqlite3_column_int64(segments.get(), 3);
        segment.speakerId = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 4));
        segment.speakerLabel = columnText(segments.get(), 5);
        segment.text = columnText(segments.get(), 6);
        segment.language = columnText(segments.get(), 7);
        segment.confidence =
            static_cast<float>(sqlite3_column_double(segments.get(), 8));
        segment.revision = static_cast<std::uint32_t>(
            sqlite3_column_int64(segments.get(), 9));
        segment.flags = static_cast<std::uint32_t>(
            sqlite3_column_int64(segments.get(), 10));
        segment.journalCheckpoint = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 11));
        result.segments.push_back(std::move(segment));
    }
    if (step != SQLITE_DONE) {
        rollback();
        return sqliteError(database_, "cannot read segment snapshot");
    }

    if (auto committed = execute(database_, "COMMIT"); !committed) {
        rollback();
        return committed.error();
    }
    active = false;
    return result;
}

Expected<void> RecoveryJournal::acknowledgePublication(
    const std::string &sessionId,
    const PublicationReceipt &receipt)
{
    if (receipt.destination < LS_PUBLICATION_DESTINATION_VAULT
        || receipt.destination > LS_PUBLICATION_DESTINATION_RECOVERY_COPY
        || receipt.publishedAtUnixNs < 0 || !validDigest(receipt.sha256Hex)
        || receipt.fileIdentity.find('\0') != std::string::npos) {
        return Error{LS_INVALID_ARGUMENT, "publication receipt is invalid"};
    }
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin receipt transaction");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }
    if (receipt.journalCheckpoint == 0
        || receipt.journalCheckpoint > session.value().journalCheckpoint) {
        return Error{
            LS_CONFLICT,
            "publication receipt checkpoint is in the future"};
    }
    if (publicationMustMatchCurrentCheckpoint(session.value().phase)
        && receipt.journalCheckpoint
            != session.value().journalCheckpoint) {
        return Error{
            LS_CONFLICT,
            "terminal or recovery publication must match current checkpoint"};
    }

    Statement expectedRevision;
    auto prepared = prepare(
        database_,
        "SELECT COALESCE(MAX(revision), 0) FROM segments "
        "WHERE session_id = ? AND journal_checkpoint <= ?",
        expectedRevision);
    if (!prepared) {
        return prepared;
    }
    bindText(expectedRevision.get(), 1, sessionId);
    sqlite3_bind_int64(
        expectedRevision.get(),
        2,
        static_cast<sqlite3_int64>(receipt.journalCheckpoint));
    if (sqlite3_step(expectedRevision.get()) != SQLITE_ROW) {
        return sqliteError(
            database_,
            "cannot inspect historical segment revision");
    }
    const auto revisionAtCheckpoint = static_cast<std::uint32_t>(
        sqlite3_column_int64(expectedRevision.get(), 0));
    if (receipt.highestSegmentRevision != revisionAtCheckpoint) {
        return Error{
            LS_CONFLICT,
            "publication receipt revision does not match its checkpoint"};
    }

    Statement latest;
    prepared = prepare(
        database_,
        "SELECT COALESCE(MAX(journal_checkpoint), 0) "
        "FROM publication_receipts WHERE session_id = ?",
        latest);
    if (!prepared) {
        return prepared;
    }
    bindText(latest.get(), 1, sessionId);
    if (sqlite3_step(latest.get()) != SQLITE_ROW) {
        return sqliteError(database_, "cannot inspect publication receipts");
    }
    const auto previous =
        static_cast<std::uint64_t>(sqlite3_column_int64(latest.get(), 0));
    if (previous > receipt.journalCheckpoint) {
        return Error{LS_CONFLICT, "publication checkpoint regressed"};
    }

    Statement insert;
    prepared = prepare(
        database_,
        R"SQL(
INSERT INTO publication_receipts(
    session_id, journal_checkpoint, highest_segment_revision, destination,
    published_at_unix_ns, sha256_hex, file_identity
) VALUES (?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(session_id, journal_checkpoint) DO UPDATE SET
    highest_segment_revision = excluded.highest_segment_revision,
    destination = excluded.destination,
    published_at_unix_ns = excluded.published_at_unix_ns,
    sha256_hex = excluded.sha256_hex,
    file_identity = excluded.file_identity
WHERE publication_receipts.highest_segment_revision =
          excluded.highest_segment_revision
  AND publication_receipts.destination = excluded.destination
  AND publication_receipts.sha256_hex = excluded.sha256_hex
  AND publication_receipts.file_identity = excluded.file_identity
)SQL",
        insert);
    if (!prepared) {
        return prepared;
    }
    bindText(insert.get(), 1, sessionId);
    sqlite3_bind_int64(
        insert.get(),
        2,
        static_cast<sqlite3_int64>(receipt.journalCheckpoint));
    sqlite3_bind_int64(insert.get(), 3, receipt.highestSegmentRevision);
    sqlite3_bind_int(insert.get(), 4, receipt.destination);
    sqlite3_bind_int64(insert.get(), 5, receipt.publishedAtUnixNs);
    bindText(insert.get(), 6, receipt.sha256Hex);
    bindText(insert.get(), 7, receipt.fileIdentity);
    if (auto stepped = stepDone(database_, insert.get()); !stepped) {
        return stepped;
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{
            LS_CONFLICT,
            "publication receipt conflicts with an existing receipt"};
    }
    return transaction.commit();
}

Expected<std::vector<std::string>>
RecoveryJournal::markAndListRecoverableSessions()
{
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin recovery transaction");
    }

    Statement candidates;
    auto prepared = prepare(
        database_,
        "SELECT session_id, phase, journal_checkpoint FROM sessions "
        "WHERE phase IN (?, ?, ?, ?) ORDER BY session_id",
        candidates);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(candidates.get(), 1, LS_PHASE_PREPARING);
    sqlite3_bind_int(candidates.get(), 2, LS_PHASE_RECORDING);
    sqlite3_bind_int(candidates.get(), 3, LS_PHASE_PAUSED);
    sqlite3_bind_int(candidates.get(), 4, LS_PHASE_FINALIZING);

    struct Candidate {
        std::string id;
        ls_phase_t phase{};
        std::uint64_t checkpoint{};
    };
    std::vector<Candidate> rows;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(candidates.get())) == SQLITE_ROW) {
        rows.push_back(Candidate{
            columnText(candidates.get(), 0),
            sqlite3_column_int(candidates.get(), 1),
            static_cast<std::uint64_t>(
                sqlite3_column_int64(candidates.get(), 2))});
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot discover recoverable sessions");
    }

    for (const auto &candidate : rows) {
        Statement sequence;
        prepared = prepare(
            database_,
            "SELECT COALESCE(MAX(event_sequence), 0) + 1 "
            "FROM state_events WHERE session_id = ?",
            sequence);
        if (!prepared) {
            return prepared.error();
        }
        bindText(sequence.get(), 1, candidate.id);
        if (sqlite3_step(sequence.get()) != SQLITE_ROW) {
            return sqliteError(database_, "cannot allocate recovery event");
        }
        const auto eventSequence = sqlite3_column_int64(sequence.get(), 0);

        Statement update;
        prepared = prepare(
            database_,
            "UPDATE sessions SET phase = ?, recovery_marked = 1, "
            "journal_checkpoint = journal_checkpoint + 1 "
            "WHERE session_id = ? AND phase = ?",
            update);
        if (!prepared) {
            return prepared.error();
        }
        sqlite3_bind_int(update.get(), 1, LS_PHASE_RECOVERY_REQUIRED);
        bindText(update.get(), 2, candidate.id);
        sqlite3_bind_int(update.get(), 3, candidate.phase);
        if (auto stepped = stepDone(database_, update.get()); !stepped) {
            return stepped.error();
        }
        if (sqlite3_changes(database_) == 0) {
            continue;
        }

        Statement event;
        prepared = prepare(
            database_,
            "INSERT INTO state_events(session_id, event_sequence, phase, "
            "reason) VALUES (?, ?, ?, ?)",
            event);
        if (!prepared) {
            return prepared.error();
        }
        bindText(event.get(), 1, candidate.id);
        sqlite3_bind_int64(event.get(), 2, eventSequence);
        sqlite3_bind_int(event.get(), 3, LS_PHASE_RECOVERY_REQUIRED);
        sqlite3_bind_int(
            event.get(),
            4,
            LS_FINALIZE_REASON_PROCESS_INTERRUPTED);
        if (auto stepped = stepDone(database_, event.get()); !stepped) {
            return stepped.error();
        }
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }

    /* Re-enter through the public query after releasing no lock would deadlock;
       query the now-canonical set directly under this lock instead. */
    Statement list;
    prepared = prepare(
        database_,
        R"SQL(
SELECT s.session_id
FROM sessions AS s
WHERE s.phase = ?
   OR (
       s.phase IN (?, ?, ?)
       AND NOT EXISTS (
           SELECT 1
           FROM publication_receipts AS p
           WHERE p.session_id = s.session_id
             AND p.journal_checkpoint = s.journal_checkpoint
             AND p.highest_segment_revision =
                   s.highest_segment_revision
       )
   )
ORDER BY s.session_id
)SQL",
        list);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(list.get(), 1, LS_PHASE_RECOVERY_REQUIRED);
    sqlite3_bind_int(list.get(), 2, LS_PHASE_COMPLETE);
    sqlite3_bind_int(list.get(), 3, LS_PHASE_INCOMPLETE_SOURCES);
    sqlite3_bind_int(list.get(), 4, LS_PHASE_INTERRUPTED);
    std::vector<std::string> result;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW) {
        result.push_back(columnText(list.get(), 0));
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot list recoverable sessions");
    }
    return result;
}

Expected<std::vector<std::string>>
RecoveryJournal::listRecoverableSessions()
{
    std::lock_guard lock(mutex_);
    Statement list;
    auto prepared = prepare(
        database_,
        R"SQL(
SELECT s.session_id
FROM sessions AS s
WHERE s.phase = ?
   OR (
       s.phase IN (?, ?, ?)
       AND NOT EXISTS (
           SELECT 1
           FROM publication_receipts AS p
           WHERE p.session_id = s.session_id
             AND p.journal_checkpoint = s.journal_checkpoint
             AND p.highest_segment_revision =
                   s.highest_segment_revision
       )
   )
ORDER BY s.session_id
)SQL",
        list);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(list.get(), 1, LS_PHASE_RECOVERY_REQUIRED);
    sqlite3_bind_int(list.get(), 2, LS_PHASE_COMPLETE);
    sqlite3_bind_int(list.get(), 3, LS_PHASE_INCOMPLETE_SOURCES);
    sqlite3_bind_int(list.get(), 4, LS_PHASE_INTERRUPTED);
    std::vector<std::string> result;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW) {
        result.push_back(columnText(list.get(), 0));
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot list recoverable sessions");
    }
    return result;
}

Expected<void> RecoveryJournal::quickCheck()
{
    std::lock_guard lock(mutex_);
    for (const char *sql :
         {"PRAGMA quick_check", "PRAGMA foreign_key_check"}) {
        Statement statement;
        auto prepared = prepare(database_, sql, statement);
        if (!prepared) {
            return prepared;
        }
        int step = sqlite3_step(statement.get());
        if (std::strcmp(sql, "PRAGMA quick_check") == 0) {
            if (step != SQLITE_ROW || columnText(statement.get(), 0) != "ok") {
                return Error{LS_RECOVERY_ERROR, "SQLite quick_check failed"};
            }
            step = sqlite3_step(statement.get());
        }
        if (step != SQLITE_DONE) {
            return Error{
                LS_RECOVERY_ERROR,
                "SQLite foreign-key or integrity check failed"};
        }
    }
    return success();
}

} // namespace localscribe
