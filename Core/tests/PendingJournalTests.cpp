#include "TestSupport.hpp"

#include "../src/storage/RecoveryJournal.hpp"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace localscribe;

namespace {

class PendingTemporaryJournal {
public:
    PendingTemporaryJournal()
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("localscribe-pending-journal-" + std::to_string(stamp)
               + ".sqlite3");
    }

    ~PendingTemporaryJournal()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

SessionRecord pendingSession(std::string id)
{
    SessionRecord record;
    record.sessionId = std::move(id);
    record.phase = LS_PHASE_PREPARING;
    record.createdAt = "2026-08-17T10:00:00+05:00";
    record.sourceApp = "Pending Journal Test";
    record.localSpeakerName = "Me";
    record.asrBackendId = "fixture";
    record.asrBackendVersion = "1";
    record.diarizationBackendId = "acoustic-clustering";
    record.diarizationBackendVersion = "6";
    record.languageMode = LS_LANGUAGE_MODE_RUSSIAN_ENGLISH;
    record.microphoneSourceId = 1;
    record.systemAudioSourceId = 2;
    record.requiredSourceMask =
        LS_REQUIRED_SOURCE_MICROPHONE | LS_REQUIRED_SOURCE_SYSTEM_AUDIO;
    record.completenessThresholdNs = 30'000'000'000;
    return record;
}

std::array<SourceRecord, 2> pendingSources()
{
    return {
        SourceRecord{
            1,
            LS_SOURCE_KIND_MICROPHONE,
            true,
            LS_SOURCE_HEALTH_READY},
        SourceRecord{
            2,
            LS_SOURCE_KIND_SYSTEM_AUDIO,
            true,
            LS_SOURCE_HEALTH_READY}};
}

TranscriptSegment pendingSegment(
    std::uint8_t stable,
    std::uint32_t revision,
    std::int64_t startTimeNs,
    std::string text,
    std::uint64_t speakerId = kAnonymousSpeakerFlag | 1u,
    std::string speakerLabel = "Speaker 1")
{
    TranscriptSegment segment;
    segment.stableId[15] = stable;
    segment.sourceId = 2;
    segment.startTimeNs = startTimeNs;
    segment.endTimeNs = startTimeNs + 800'000'000;
    segment.speakerId = speakerId;
    segment.speakerLabel = std::move(speakerLabel);
    segment.text = std::move(text);
    segment.language = "en";
    segment.confidence = 0.91F;
    segment.revision = revision;
    segment.flags = LS_SEGMENT_FLAG_FINAL;
    segment.speakerEmbeddingModel = "speaker-feature-v1";
    segment.speakerEmbedding = {1.0F, 0.0F};
    return segment;
}

SpeakerTurn attributedTurn(
    const TranscriptSegment &segment,
    std::uint64_t speakerId,
    std::string speakerLabel)
{
    SpeakerTurn turn;
    turn.stableId = segment.stableId;
    turn.sourceId = segment.sourceId;
    turn.startTimeNs = segment.startTimeNs;
    turn.endTimeNs = segment.endTimeNs;
    turn.speakerId = speakerId;
    turn.speakerLabel = std::move(speakerLabel);
    turn.confidence = 0.96F;
    turn.revision = segment.revision;
    return turn;
}

std::shared_ptr<RecoveryJournal> createRecordingSession(
    const PendingTemporaryJournal &temporary,
    const std::string &sessionId)
{
    auto journal = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(journal);
    const auto sources = pendingSources();
    LS_CHECK(journal.value()->createSession(
        pendingSession(sessionId),
        sources));
    LS_CHECK(journal.value()->transition(
        sessionId,
        LS_PHASE_PREPARING,
        LS_PHASE_RECORDING));
    return journal.takeValue();
}

void createLegacyPendingFixture(
    const std::filesystem::path &path,
    int version)
{
    sqlite3 *database = nullptr;
    LS_CHECK_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    const char *v1 = R"SQL(
CREATE TABLE sessions(session_id TEXT PRIMARY KEY NOT NULL);
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
INSERT INTO sessions(session_id) VALUES ('legacy');
INSERT INTO segments(
    session_id, stable_id, revision, source_id, start_time_ns, end_time_ns,
    speaker_id, speaker_label, text, language, confidence, flags,
    journal_checkpoint
) VALUES (
    'legacy', X'00000000000000000000000000000001', 3, 2, 100, 200,
    1, 'Me', 'legacy visible', 'en', 0.9, 1, 4
);
PRAGMA user_version = 1;
)SQL";
    const char *v2 = R"SQL(
CREATE TABLE sessions(session_id TEXT PRIMARY KEY NOT NULL);
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
    speaker_embedding_model TEXT NOT NULL DEFAULT '',
    speaker_embedding_dimension INTEGER NOT NULL DEFAULT 0,
    speaker_embedding BLOB NOT NULL DEFAULT X'',
    PRIMARY KEY (session_id, stable_id, revision)
);
INSERT INTO sessions(session_id) VALUES ('legacy');
INSERT INTO segments(
    session_id, stable_id, revision, source_id, start_time_ns, end_time_ns,
    speaker_id, speaker_label, text, language, confidence, flags,
    journal_checkpoint, speaker_embedding_model,
    speaker_embedding_dimension, speaker_embedding
) VALUES (
    'legacy', X'00000000000000000000000000000002', 5, 2, 300, 400,
    1, 'Me', 'legacy v2 visible', 'en', 0.8, 1, 6,
    'speaker-feature-v1', 2, X'0000803F00000000'
);
PRAGMA user_version = 2;
)SQL";
    LS_CHECK_EQ(
        sqlite3_exec(
            database,
            version == 1 ? v1 : v2,
            nullptr,
            nullptr,
            nullptr),
        SQLITE_OK);
    sqlite3_close(database);
}

int scalarInt(sqlite3 *database, const char *sql)
{
    sqlite3_stmt *statement = nullptr;
    LS_CHECK_EQ(
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr),
        SQLITE_OK);
    LS_CHECK_EQ(sqlite3_step(statement), SQLITE_ROW);
    const int result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

void checkMigratedPendingSchema(
    const std::filesystem::path &path,
    int expectedLegacyEmbeddingDimension)
{
    sqlite3 *database = nullptr;
    LS_CHECK_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    LS_CHECK_EQ(scalarInt(database, "PRAGMA user_version"), 3);
    LS_CHECK_EQ(
        scalarInt(
            database,
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
            "AND name IN ('pending_speaker_groups', "
            "'pending_speaker_segments')"),
        2);
    LS_CHECK_EQ(
        scalarInt(
            database,
            "SELECT COUNT(*) FROM pragma_table_info("
            "'pending_speaker_segments') WHERE name LIKE '%target%'"),
        0);
    LS_CHECK_EQ(
        scalarInt(
            database,
            "SELECT speaker_embedding_dimension FROM segments "
            "WHERE session_id = 'legacy'"),
        expectedLegacyEmbeddingDimension);
    const char *insertPending = R"SQL(
PRAGMA foreign_keys = ON;
INSERT INTO pending_speaker_groups(
    session_id, group_id, deadline_monotonic_ns, created_checkpoint
) VALUES ('legacy', 9, 5000, 7);
INSERT INTO pending_speaker_segments(
    session_id, group_id, stable_id, revision, source_id, start_time_ns,
    end_time_ns, speaker_id, speaker_label, text, language, confidence,
    flags, staged_checkpoint, speaker_embedding_model,
    speaker_embedding_dimension, speaker_embedding
) VALUES (
    'legacy', 9, X'00000000000000000000000000000009', 7, 2, 500, 600,
    1, 'Me', 'durable pending', 'en', 0.75, 1, 7,
    'speaker-feature-v1', 2, X'0000803F00000000'
);
)SQL";
    LS_CHECK_EQ(
        sqlite3_exec(database, insertPending, nullptr, nullptr, nullptr),
        SQLITE_OK);
    LS_CHECK_EQ(
        scalarInt(
            database,
            "SELECT length(speaker_embedding) FROM "
            "pending_speaker_segments WHERE session_id = 'legacy'"),
        8);
    sqlite3_close(database);
}

} // namespace

LS_TEST(pending_journal_stages_group_without_exposing_final_segments)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"hidden-pending"};
    auto journal = createRecordingSession(temporary, sessionId);

    auto visible = pendingSegment(
        1,
        2,
        1'000'000'000,
        "visible active speaker");
    const auto visibleCheckpoint =
        journal->appendFinalSegment(sessionId, visible);
    LS_CHECK(visibleCheckpoint);
    const auto before = journal->snapshot(sessionId);
    LS_CHECK(before);

    auto held = pendingSegment(
        2,
        7,
        3'000'000'000,
        "hidden candidate");
    const auto staged = journal->stagePendingSegments(
        sessionId,
        41,
        8'000'000'000,
        std::span<const TranscriptSegment>(&held, 1));
    LS_CHECK(staged);
    LS_CHECK(staged.value().wasChanged);
    LS_CHECK_EQ(
        staged.value().journalCheckpoint,
        visibleCheckpoint.value() + 1u);
    LS_CHECK(staged.value().visibleSegments.empty());
    LS_CHECK_EQ(
        staged.value().highestSegmentRevision,
        std::uint32_t{2});

    const auto after = journal->snapshot(sessionId);
    LS_CHECK(after);
    LS_CHECK_EQ(after.value().segments.size(), std::size_t{1});
    LS_CHECK_EQ(
        after.value().segments[0].text,
        std::string{"visible active speaker"});
    LS_CHECK_EQ(
        after.value().session.journalCheckpoint,
        staged.value().journalCheckpoint);
    LS_CHECK_EQ(
        after.value().session.highestSegmentRevision,
        std::uint32_t{2});
    LS_CHECK_EQ(
        after.value().session.timelineOriginNs,
        before.value().session.timelineOriginNs);
}

LS_TEST(pending_journal_staging_is_exactly_idempotent_and_can_extend_group)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"idempotent-pending"};
    auto journal = createRecordingSession(temporary, sessionId);

    const auto first = pendingSegment(
        11,
        3,
        2'000'000'000,
        "first held segment");
    const auto staged = journal->stagePendingSegments(
        sessionId,
        77,
        7'000'000'000,
        std::span<const TranscriptSegment>(&first, 1));
    LS_CHECK(staged);
    LS_CHECK(staged.value().wasChanged);

    const auto retried = journal->stagePendingSegments(
        sessionId,
        77,
        7'000'000'000,
        std::span<const TranscriptSegment>(&first, 1));
    LS_CHECK(retried);
    LS_CHECK(!retried.value().wasChanged);
    LS_CHECK_EQ(
        retried.value().journalCheckpoint,
        staged.value().journalCheckpoint);

    const auto second = pendingSegment(
        12,
        9,
        3'000'000'000,
        "second held segment");
    const auto extended = journal->stagePendingSegments(
        sessionId,
        77,
        7'000'000'000,
        std::span<const TranscriptSegment>(&second, 1));
    LS_CHECK(extended);
    LS_CHECK(extended.value().wasChanged);
    LS_CHECK_EQ(
        extended.value().journalCheckpoint,
        staged.value().journalCheckpoint + 1u);
    LS_CHECK_EQ(
        extended.value().highestSegmentRevision,
        std::uint32_t{0});

    auto conflicting = first;
    conflicting.text = "different payload for same identity";
    const auto conflict = journal->stagePendingSegments(
        sessionId,
        77,
        7'000'000'000,
        std::span<const TranscriptSegment>(&conflicting, 1));
    LS_CHECK(!conflict);
    LS_CHECK_EQ(conflict.error().code, LS_CONFLICT);

    const auto changedDeadline = journal->stagePendingSegments(
        sessionId,
        77,
        7'000'000'001,
        std::span<const TranscriptSegment>(&second, 1));
    LS_CHECK(!changedDeadline);
    LS_CHECK_EQ(changedDeadline.error().code, LS_CONFLICT);

    const auto snapshot = journal->snapshot(sessionId);
    LS_CHECK(snapshot);
    LS_CHECK(snapshot.value().segments.empty());
    LS_CHECK_EQ(
        snapshot.value().session.journalCheckpoint,
        extended.value().journalCheckpoint);
}

LS_TEST(pending_journal_resolves_entire_group_at_one_new_checkpoint)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"resolved-pending"};
    auto journal = createRecordingSession(temporary, sessionId);

    const std::array held{
        pendingSegment(21, 4, 4'000'000'000, "candidate one"),
        pendingSegment(22, 8, 5'000'000'000, "candidate two")};
    const auto staged = journal->stagePendingSegments(
        sessionId,
        91,
        9'000'000'000,
        held);
    LS_CHECK(staged);

    const std::array turns{
        attributedTurn(
            held[0],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2"),
        attributedTurn(
            held[1],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2")};
    const auto resolved = journal->resolvePendingSpeakerGroup(
        sessionId,
        91,
        turns);
    LS_CHECK(resolved);
    LS_CHECK(resolved.value().wasChanged);
    LS_CHECK_EQ(
        resolved.value().journalCheckpoint,
        staged.value().journalCheckpoint + 1u);
    LS_CHECK_EQ(resolved.value().visibleSegments.size(), std::size_t{2});
    LS_CHECK_EQ(
        resolved.value().highestSegmentRevision,
        std::uint32_t{8});
    for (const auto &segment : resolved.value().visibleSegments) {
        LS_CHECK_EQ(
            segment.journalCheckpoint,
            resolved.value().journalCheckpoint);
        LS_CHECK_EQ(segment.speakerId, kAnonymousSpeakerFlag | 2u);
        LS_CHECK_EQ(segment.speakerLabel, std::string{"Speaker 2"});
        LS_CHECK_EQ(
            segment.speakerEmbeddingModel,
            std::string{"speaker-feature-v1"});
        LS_CHECK_EQ(segment.speakerEmbedding.size(), std::size_t{2});
    }

    const auto snapshot = journal->snapshot(sessionId);
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{2});
    LS_CHECK_EQ(
        snapshot.value().session.journalCheckpoint,
        resolved.value().journalCheckpoint);
    LS_CHECK_EQ(
        snapshot.value().session.highestSegmentRevision,
        std::uint32_t{8});
    LS_CHECK_EQ(
        snapshot.value().session.timelineOriginNs,
        std::int64_t{4'000'000'000});

    const auto retried = journal->resolvePendingSpeakerGroup(
        sessionId,
        91,
        turns);
    LS_CHECK(retried);
    LS_CHECK(!retried.value().wasChanged);
    LS_CHECK(retried.value().visibleSegments.empty());
    LS_CHECK_EQ(
        retried.value().journalCheckpoint,
        resolved.value().journalCheckpoint);
}

LS_TEST(pending_journal_rejects_mixed_confirmed_speakers_atomically)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"mixed-resolution-pending"};
    auto journal = createRecordingSession(temporary, sessionId);

    const std::array held{
        pendingSegment(23, 5, 6'000'000'000, "candidate one"),
        pendingSegment(24, 6, 7'000'000'000, "candidate two")};
    const auto staged = journal->stagePendingSegments(
        sessionId,
        92,
        12'000'000'000,
        held);
    LS_CHECK(staged);

    const auto checkStillPending = [&] {
        const auto snapshot = journal->snapshot(sessionId);
        LS_CHECK(snapshot);
        LS_CHECK(snapshot.value().segments.empty());
        LS_CHECK_EQ(
            snapshot.value().session.journalCheckpoint,
            staged.value().journalCheckpoint);
        LS_CHECK_EQ(
            snapshot.value().session.highestSegmentRevision,
            std::uint32_t{0});
    };

    const std::array mixedLabels{
        attributedTurn(
            held[0],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2"),
        attributedTurn(
            held[1],
            kAnonymousSpeakerFlag | 2u,
            "Different label")};
    const auto rejectedLabels = journal->resolvePendingSpeakerGroup(
        sessionId,
        92,
        mixedLabels);
    LS_CHECK(!rejectedLabels);
    LS_CHECK_EQ(rejectedLabels.error().code, LS_CONFLICT);
    checkStillPending();

    const std::array mixedIds{
        attributedTurn(
            held[0],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2"),
        attributedTurn(
            held[1],
            kAnonymousSpeakerFlag | 3u,
            "Speaker 2")};
    const auto rejectedIds = journal->resolvePendingSpeakerGroup(
        sessionId,
        92,
        mixedIds);
    LS_CHECK(!rejectedIds);
    LS_CHECK_EQ(rejectedIds.error().code, LS_CONFLICT);
    checkStillPending();

    const std::array localTurns{
        attributedTurn(held[0], 1u, "Me"),
        attributedTurn(held[1], 1u, "Me")};
    const auto rejectedLocal = journal->resolvePendingSpeakerGroup(
        sessionId,
        92,
        localTurns);
    LS_CHECK(!rejectedLocal);
    LS_CHECK_EQ(rejectedLocal.error().code, LS_CONFLICT);
    checkStillPending();

    std::array wrongSourceTurns{
        attributedTurn(
            held[0],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2"),
        attributedTurn(
            held[1],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2")};
    wrongSourceTurns[1].sourceId = 1;
    const auto rejectedSource = journal->resolvePendingSpeakerGroup(
        sessionId,
        92,
        wrongSourceTurns);
    LS_CHECK(!rejectedSource);
    LS_CHECK_EQ(rejectedSource.error().code, LS_CONFLICT);
    checkStillPending();

    const std::array consistentTurns{
        attributedTurn(
            held[0],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2"),
        attributedTurn(
            held[1],
            kAnonymousSpeakerFlag | 2u,
            "Speaker 2")};
    const auto resolved = journal->resolvePendingSpeakerGroup(
        sessionId,
        92,
        consistentTurns);
    LS_CHECK(resolved);
    LS_CHECK_EQ(resolved.value().visibleSegments.size(), std::size_t{2});
    LS_CHECK_EQ(
        resolved.value().journalCheckpoint,
        staged.value().journalCheckpoint + 1u);
    for (const auto &segment : resolved.value().visibleSegments) {
        LS_CHECK_EQ(segment.sourceId, std::uint64_t{2});
        LS_CHECK_EQ(segment.speakerId, kAnonymousSpeakerFlag | 2u);
        LS_CHECK_EQ(segment.speakerLabel, std::string{"Speaker 2"});
    }
}

LS_TEST(pending_journal_hold_resolve_and_commit_share_one_batch_checkpoint)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"single-batch-pending"};
    auto journal = createRecordingSession(temporary, sessionId);
    const auto before = journal->loadSession(sessionId);
    LS_CHECK(before);

    const auto held = pendingSegment(
        31,
        5,
        7'000'000'000,
        "held and immediately confirmed");
    const auto committed = pendingSegment(
        32,
        6,
        8'000'000'000,
        "ordinary committed segment");
    const auto confirmed = attributedTurn(
        held,
        kAnonymousSpeakerFlag | 4u,
        "Speaker 4");

    DiarizationJournalBatch batch;
    batch.holds.push_back(PendingSpeakerGroupStage{
        101,
        12'000'000'000,
        {held}});
    batch.resolutions.push_back(PendingSpeakerGroupResolution{
        101,
        {confirmed}});
    batch.commits.push_back(committed);
    const auto applied = journal->applyDiarizationBatch(sessionId, batch);
    LS_CHECK(applied);
    LS_CHECK(applied.value().wasChanged);
    LS_CHECK_EQ(
        applied.value().journalCheckpoint,
        before.value().journalCheckpoint + 1u);
    LS_CHECK_EQ(applied.value().visibleSegments.size(), std::size_t{2});
    LS_CHECK_EQ(
        applied.value().highestSegmentRevision,
        std::uint32_t{6});
    for (const auto &segment : applied.value().visibleSegments) {
        LS_CHECK_EQ(
            segment.journalCheckpoint,
            applied.value().journalCheckpoint);
    }

    const auto snapshot = journal->snapshot(sessionId);
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{2});
    LS_CHECK_EQ(
        snapshot.value().session.journalCheckpoint,
        before.value().journalCheckpoint + 1u);

    const auto retry = journal->applyDiarizationBatch(sessionId, batch);
    LS_CHECK(retry);
    LS_CHECK(!retry.value().wasChanged);
    LS_CHECK(retry.value().visibleSegments.empty());
    LS_CHECK_EQ(
        retry.value().journalCheckpoint,
        applied.value().journalCheckpoint);
}

LS_TEST(pending_journal_empty_resolution_promotes_saved_fallback_one_for_one)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"fallback-pending"};
    auto journal = createRecordingSession(temporary, sessionId);

    const std::array held{
        pendingSegment(
            41,
            10,
            10'000'000'000,
            "fallback active speaker",
            kAnonymousSpeakerFlag | 1u,
            "Speaker 1"),
        pendingSegment(
            42,
            11,
            11'000'000'000,
            "fallback remains active",
            kAnonymousSpeakerFlag | 1u,
            "Speaker 1")};
    const auto staged = journal->stagePendingSegments(
        sessionId,
        111,
        15'000'000'000,
        held);
    LS_CHECK(staged);

    const auto fallback = journal->resolvePendingSpeakerGroup(
        sessionId,
        111);
    LS_CHECK(fallback);
    LS_CHECK_EQ(fallback.value().visibleSegments.size(), std::size_t{2});
    for (std::size_t index = 0; index < held.size(); ++index) {
        const auto &visible = fallback.value().visibleSegments[index];
        LS_CHECK_EQ(visible.stableId, held[index].stableId);
        LS_CHECK_EQ(visible.revision, held[index].revision);
        LS_CHECK_EQ(visible.speakerId, held[index].speakerId);
        LS_CHECK_EQ(visible.speakerLabel, held[index].speakerLabel);
        LS_CHECK_EQ(visible.text, held[index].text);
        LS_CHECK_EQ(
            visible.speakerEmbedding,
            held[index].speakerEmbedding);
        LS_CHECK_EQ(
            visible.journalCheckpoint,
            fallback.value().journalCheckpoint);
    }
}

LS_TEST(pending_journal_fallback_promotes_all_unresolved_groups_at_one_checkpoint)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"fallback-all-pending"};
    auto journal = createRecordingSession(temporary, sessionId);

    const std::array firstGroup{
        pendingSegment(
            43,
            31,
            30'000'000'000,
            "last fallback",
            kAnonymousSpeakerFlag | 3u,
            "Speaker 3"),
        pendingSegment(
            41,
            29,
            10'000'000'000,
            "first fallback",
            kAnonymousSpeakerFlag | 1u,
            "Speaker 1")};
    const auto secondGroup = pendingSegment(
        42,
        30,
        20'000'000'000,
        "middle fallback",
        kAnonymousSpeakerFlag | 2u,
        "Speaker 2");
    LS_CHECK(journal->stagePendingSegments(
        sessionId,
        112,
        35'000'000'000,
        firstGroup));
    LS_CHECK(journal->stagePendingSegments(
        sessionId,
        113,
        25'000'000'000,
        std::span<const TranscriptSegment>(&secondGroup, 1)));
    const auto before = journal->loadSession(sessionId);
    LS_CHECK(before);

    const auto fallback =
        journal->resolveAllPendingSpeakerGroupsToFallback(sessionId);
    LS_CHECK(fallback);
    LS_CHECK(fallback.value().wasChanged);
    LS_CHECK_EQ(
        fallback.value().journalCheckpoint,
        before.value().journalCheckpoint + 1u);
    LS_CHECK_EQ(fallback.value().visibleSegments.size(), std::size_t{3});
    LS_CHECK_EQ(
        fallback.value().highestSegmentRevision,
        std::uint32_t{31});

    const std::array expectedTexts{
        std::string{"first fallback"},
        std::string{"middle fallback"},
        std::string{"last fallback"}};
    for (std::size_t index = 0; index < expectedTexts.size(); ++index) {
        const auto &visible = fallback.value().visibleSegments[index];
        LS_CHECK_EQ(visible.text, expectedTexts[index]);
        LS_CHECK_EQ(
            visible.speakerId,
            kAnonymousSpeakerFlag | static_cast<std::uint64_t>(index + 1));
        LS_CHECK_EQ(
            visible.speakerLabel,
            "Speaker " + std::to_string(index + 1));
        LS_CHECK_EQ(visible.confidence, 0.91F);
        LS_CHECK_EQ(
            visible.speakerEmbeddingModel,
            std::string{"speaker-feature-v1"});
        LS_CHECK_EQ(
            visible.speakerEmbedding,
            std::vector<float>({1.0F, 0.0F}));
        LS_CHECK_EQ(
            visible.journalCheckpoint,
            fallback.value().journalCheckpoint);
    }

    const auto snapshot = journal->snapshot(sessionId);
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{3});
    LS_CHECK_EQ(
        snapshot.value().session.journalCheckpoint,
        fallback.value().journalCheckpoint);
    LS_CHECK_EQ(
        snapshot.value().session.timelineOriginNs,
        std::int64_t{10'000'000'000});

    const auto retried =
        journal->resolveAllPendingSpeakerGroupsToFallback(sessionId);
    LS_CHECK(retried);
    LS_CHECK(!retried.value().wasChanged);
    LS_CHECK(retried.value().visibleSegments.empty());
    LS_CHECK_EQ(
        retried.value().journalCheckpoint,
        fallback.value().journalCheckpoint);

    const auto firstAlreadyResolved =
        journal->resolvePendingSpeakerGroup(sessionId, 112);
    LS_CHECK(firstAlreadyResolved);
    LS_CHECK(!firstAlreadyResolved.value().wasChanged);
    const auto secondAlreadyResolved =
        journal->resolvePendingSpeakerGroup(sessionId, 113);
    LS_CHECK(secondAlreadyResolved);
    LS_CHECK(!secondAlreadyResolved.value().wasChanged);

    LS_CHECK(journal->transition(
        sessionId,
        LS_PHASE_RECORDING,
        LS_PHASE_FINALIZING,
        LS_FINALIZE_REASON_USER_STOP));
    LS_CHECK(journal->transition(
        sessionId,
        LS_PHASE_FINALIZING,
        LS_PHASE_COMPLETE,
        LS_FINALIZE_REASON_USER_STOP));
}

LS_TEST(pending_journal_resolution_preserves_historical_publication_receipt)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"historical-pending-receipt"};
    auto journal = createRecordingSession(temporary, sessionId);

    auto visible = pendingSegment(
        51,
        2,
        1'000'000'000,
        "already visible");
    LS_CHECK(journal->appendFinalSegment(sessionId, visible));
    const auto held = pendingSegment(
        52,
        17,
        3'000'000'000,
        "not in staged publication");
    const auto staged = journal->stagePendingSegments(
        sessionId,
        121,
        8'000'000'000,
        std::span<const TranscriptSegment>(&held, 1));
    LS_CHECK(staged);

    PublicationReceipt receipt;
    receipt.journalCheckpoint = staged.value().journalCheckpoint;
    receipt.highestSegmentRevision = 2;
    receipt.destination = LS_PUBLICATION_DESTINATION_STAGING;
    receipt.publishedAtUnixNs = 1;
    receipt.sha256Hex = std::string(64, 'a');
    receipt.fileIdentity = "before-pending-resolution";
    LS_CHECK(journal->acknowledgePublication(sessionId, receipt));

    const auto resolved = journal->resolvePendingSpeakerGroup(
        sessionId,
        121);
    LS_CHECK(resolved);
    LS_CHECK_EQ(
        resolved.value().visibleSegments[0].journalCheckpoint,
        resolved.value().journalCheckpoint);
    LS_CHECK(resolved.value().journalCheckpoint > receipt.journalCheckpoint);

    /* A later promotion must not rewrite the historical checkpoint view. */
    LS_CHECK(journal->acknowledgePublication(sessionId, receipt));
}

LS_TEST(pending_journal_recovery_fallback_promotes_all_groups_atomically)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"recovered-pending"};
    auto journal = createRecordingSession(temporary, sessionId);

    const std::array firstGroup{
        pendingSegment(61, 12, 12'000'000'000, "recovered one"),
        pendingSegment(62, 19, 13'000'000'000, "recovered two")};
    const auto secondGroup = pendingSegment(
        63,
        15,
        14'000'000'000,
        "recovered three",
        kAnonymousSpeakerFlag | 3u,
        "Speaker 3");
    LS_CHECK(journal->stagePendingSegments(
        sessionId,
        131,
        17'000'000'000,
        firstGroup));
    const auto staged = journal->stagePendingSegments(
        sessionId,
        132,
        18'000'000'000,
        std::span<const TranscriptSegment>(&secondGroup, 1));
    LS_CHECK(staged);
    const auto stagedCheckpoint = staged.value().journalCheckpoint;

    journal.reset();
    auto reopened = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(reopened);
    const auto recovered = reopened.value()->markAndListRecoverableSessions();
    LS_CHECK(recovered);
    LS_CHECK_EQ(recovered.value().size(), std::size_t{1});
    LS_CHECK_EQ(recovered.value()[0], sessionId);

    const auto snapshot = reopened.value()->snapshot(sessionId);
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().session.phase, LS_PHASE_RECOVERY_REQUIRED);
    LS_CHECK_EQ(
        snapshot.value().session.journalCheckpoint,
        stagedCheckpoint + 1u);
    LS_CHECK_EQ(
        snapshot.value().session.highestSegmentRevision,
        std::uint32_t{19});
    LS_CHECK_EQ(
        snapshot.value().session.timelineOriginNs,
        std::int64_t{12'000'000'000});
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{3});
    for (const auto &segment : snapshot.value().segments) {
        LS_CHECK_EQ(
            segment.journalCheckpoint,
            snapshot.value().session.journalCheckpoint);
    }
    LS_CHECK_EQ(
        snapshot.value().segments[2].speakerId,
        kAnonymousSpeakerFlag | 3u);

    const auto repeated = reopened.value()->markAndListRecoverableSessions();
    LS_CHECK(repeated);
    const auto repeatedSnapshot = reopened.value()->snapshot(sessionId);
    LS_CHECK(repeatedSnapshot);
    LS_CHECK_EQ(
        repeatedSnapshot.value().session.journalCheckpoint,
        snapshot.value().session.journalCheckpoint);
    LS_CHECK_EQ(repeatedSnapshot.value().segments.size(), std::size_t{3});

    const auto alreadyResolved =
        reopened.value()->resolvePendingSpeakerGroup(sessionId, 131);
    LS_CHECK(alreadyResolved);
    LS_CHECK(!alreadyResolved.value().wasChanged);
}

LS_TEST(pending_journal_refuses_terminal_phase_with_unresolved_group)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"terminal-pending"};
    auto journal = createRecordingSession(temporary, sessionId);
    const auto held = pendingSegment(
        71,
        20,
        20'000'000'000,
        "must resolve before terminal");
    LS_CHECK(journal->stagePendingSegments(
        sessionId,
        141,
        25'000'000'000,
        std::span<const TranscriptSegment>(&held, 1)));
    LS_CHECK(journal->transition(
        sessionId,
        LS_PHASE_RECORDING,
        LS_PHASE_FINALIZING,
        LS_FINALIZE_REASON_USER_STOP));
    const auto finalizing = journal->loadSession(sessionId);
    LS_CHECK(finalizing);

    const auto terminal = journal->transition(
        sessionId,
        LS_PHASE_FINALIZING,
        LS_PHASE_COMPLETE,
        LS_FINALIZE_REASON_USER_STOP);
    LS_CHECK(!terminal);
    LS_CHECK_EQ(terminal.error().code, LS_INVALID_STATE);
    const auto unchanged = journal->loadSession(sessionId);
    LS_CHECK(unchanged);
    LS_CHECK_EQ(unchanged.value().phase, LS_PHASE_FINALIZING);
    LS_CHECK_EQ(
        unchanged.value().journalCheckpoint,
        finalizing.value().journalCheckpoint);

    LS_CHECK(journal->resolvePendingSpeakerGroup(sessionId, 141));
    LS_CHECK(journal->transition(
        sessionId,
        LS_PHASE_FINALIZING,
        LS_PHASE_COMPLETE,
        LS_FINALIZE_REASON_USER_STOP));
}

LS_TEST(pending_journal_staged_speaker_is_absent_from_profiles_until_promotion)
{
    PendingTemporaryJournal temporary;
    const std::string sessionId{"profile-isolation-pending"};
    auto journal = createRecordingSession(temporary, sessionId);
    constexpr std::uint64_t anonymous = kAnonymousSpeakerFlag | 6u;
    const auto held = pendingSegment(
        81,
        21,
        30'000'000'000,
        "hidden voice evidence",
        anonymous,
        "Speaker 6");
    LS_CHECK(journal->stagePendingSegments(
        sessionId,
        151,
        35'000'000'000,
        std::span<const TranscriptSegment>(&held, 1)));

    const auto hiddenSnapshot = journal->snapshot(sessionId);
    LS_CHECK(hiddenSnapshot);
    LS_CHECK(hiddenSnapshot.value().segments.empty());
    const auto hiddenEnrollment = journal->enrollVoiceProfile(
        sessionId,
        anonymous,
        "Pending Alice");
    LS_CHECK(!hiddenEnrollment);
    LS_CHECK_EQ(hiddenEnrollment.error().code, LS_NOT_FOUND);
    const auto hiddenProfiles = journal->listVoiceProfiles();
    LS_CHECK(hiddenProfiles);
    LS_CHECK(hiddenProfiles.value().empty());

    LS_CHECK(journal->resolvePendingSpeakerGroup(sessionId, 151));
    const auto visibleEnrollment = journal->enrollVoiceProfile(
        sessionId,
        anonymous,
        "Pending Alice");
    LS_CHECK(visibleEnrollment);
    LS_CHECK_EQ(
        visibleEnrollment.value().profile.observationCount,
        std::uint64_t{1});
    const auto visibleProfiles = journal->listVoiceProfiles();
    LS_CHECK(visibleProfiles);
    LS_CHECK_EQ(visibleProfiles.value().size(), std::size_t{1});
}

LS_TEST(pending_journal_migrates_v1_database_to_hidden_group_schema)
{
    PendingTemporaryJournal temporary;
    createLegacyPendingFixture(temporary.path, 1);
    auto opened = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(opened);
    opened.value().reset();
    checkMigratedPendingSchema(temporary.path, 0);
}

LS_TEST(pending_journal_migrates_v2_database_to_hidden_group_schema)
{
    PendingTemporaryJournal temporary;
    createLegacyPendingFixture(temporary.path, 2);
    auto opened = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(opened);
    opened.value().reset();
    checkMigratedPendingSchema(temporary.path, 2);
}
