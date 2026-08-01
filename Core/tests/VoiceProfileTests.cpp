#include "TestSupport.hpp"

#include "../src/storage/RecoveryJournal.hpp"

#include <LocalScribeCore/LocalScribeCore.h>
#include <sqlite3.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace localscribe;

namespace {

class TemporaryDatabase {
public:
    TemporaryDatabase()
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("localscribe-profile-" + std::to_string(stamp) + ".sqlite3");
    }

    ~TemporaryDatabase()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

ls_utf8_view_v1 view(const std::string &value)
{
    return ls_utf8_view_v1{
        sizeof(ls_utf8_view_v1),
        LS_CORE_ABI_VERSION,
        reinterpret_cast<const std::uint8_t *>(value.data()),
        value.size()};
}

SessionRecord profileSession(std::string id)
{
    SessionRecord record;
    record.sessionId = std::move(id);
    record.phase = LS_PHASE_PREPARING;
    record.createdAt = "2026-08-01T10:00:00+05:00";
    record.sourceApp = "Profile Test";
    record.localSpeakerName = "Me";
    record.asrBackendId = "fixture";
    record.asrBackendVersion = "1";
    record.diarizationBackendId = "acoustic-clustering";
    record.diarizationBackendVersion = "4";
    record.languageMode = LS_LANGUAGE_MODE_RUSSIAN_ENGLISH;
    record.microphoneSourceId = 1;
    record.systemAudioSourceId = 2;
    record.requiredSourceMask =
        LS_REQUIRED_SOURCE_MICROPHONE | LS_REQUIRED_SOURCE_SYSTEM_AUDIO;
    record.completenessThresholdNs = 30'000'000'000;
    return record;
}

std::array<SourceRecord, 2> profileSources()
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

TranscriptSegment remoteSegment(
    std::uint8_t stable,
    std::uint64_t speakerId,
    std::vector<float> embedding)
{
    TranscriptSegment segment;
    segment.stableId[15] = stable;
    segment.sourceId = 2;
    segment.startTimeNs = static_cast<std::int64_t>(stable) * 1'000'000'000;
    segment.endTimeNs = segment.startTimeNs + 900'000'000;
    segment.speakerId = speakerId;
    segment.speakerLabel = "Speaker 1";
    segment.text = "voice profile evidence";
    segment.language = "en";
    segment.confidence = 0.95F;
    segment.revision = 1;
    segment.flags = LS_SEGMENT_FLAG_FINAL;
    segment.speakerEmbeddingModel = std::string{kSpeakerFeatureModelId};
    segment.speakerEmbedding = std::move(embedding);
    return segment;
}

void createTerminalSession(
    const std::shared_ptr<RecoveryJournal> &journal,
    const std::string &sessionId)
{
    const auto sources = profileSources();
    LS_CHECK(journal->createSession(profileSession(sessionId), sources));
    LS_CHECK(journal->transition(
        sessionId,
        LS_PHASE_PREPARING,
        LS_PHASE_RECORDING));
}

void finishSession(
    const std::shared_ptr<RecoveryJournal> &journal,
    const std::string &sessionId)
{
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

} // namespace

LS_TEST(journal_migrates_v1_voice_profile_schema_without_recreating_v1)
{
    TemporaryDatabase databaseFile;
    sqlite3 *database = nullptr;
    LS_CHECK_EQ(
        sqlite3_open(databaseFile.path.c_str(), &database),
        SQLITE_OK);
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
PRAGMA user_version = 1;
)SQL";
    LS_CHECK_EQ(sqlite3_exec(database, v1, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(database);

    auto opened = RecoveryJournal::open(databaseFile.path.string());
    LS_CHECK(opened);
    opened.value().reset();

    LS_CHECK_EQ(
        sqlite3_open_v2(
            databaseFile.path.c_str(),
            &database,
            SQLITE_OPEN_READONLY,
            nullptr),
        SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    LS_CHECK_EQ(
        sqlite3_prepare_v2(
            database,
            "PRAGMA user_version",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    LS_CHECK_EQ(sqlite3_step(statement), SQLITE_ROW);
    LS_CHECK_EQ(sqlite3_column_int(statement, 0), 2);
    sqlite3_finalize(statement);
    LS_CHECK_EQ(
        sqlite3_prepare_v2(
            database,
            "SELECT speaker_embedding_model, speaker_embedding_dimension, "
            "speaker_embedding FROM segments LIMIT 1",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    sqlite3_finalize(statement);
    sqlite3_close(database);
}

LS_TEST(voice_profile_enrollment_overlays_merges_and_maps_future_revisions)
{
    TemporaryDatabase databaseFile;
    auto journal = RecoveryJournal::open(databaseFile.path.string());
    LS_CHECK(journal);
    constexpr std::uint64_t anonymous = kAnonymousSpeakerFlag | 1u;

    createTerminalSession(journal.value(), "first-call");
    auto originalCheckpoint = journal.value()->appendFinalSegment(
        "first-call",
        remoteSegment(1, anonymous, {1.0F, 0.0F}));
    LS_CHECK(originalCheckpoint);
    LS_CHECK(journal.value()->appendFinalSegment(
        "first-call",
        remoteSegment(2, anonymous, {0.99F, 0.01F})));

    auto first = journal.value()->enrollVoiceProfile(
        "first-call",
        anonymous,
        "Alice");
    LS_CHECK(first);
    LS_CHECK_EQ(first.value().profile.profileId, std::uint64_t{1});
    LS_CHECK_EQ(first.value().profile.observationCount, std::uint64_t{2});
    LS_CHECK_EQ(first.value().relabeledSegments, std::uint32_t{2});
    LS_CHECK_EQ(first.value().highestSegmentRevision, std::uint32_t{1});
    LS_CHECK(isPersistentSpeakerId(first.value().speakerId));

    auto duplicateSegment = remoteSegment(1, anonymous, {1.0F, 0.0F});
    auto duplicate = journal.value()->appendFinalSegment(
        "first-call",
        duplicateSegment);
    LS_CHECK(duplicate);
    LS_CHECK_EQ(duplicate.value(), first.value().journalCheckpoint);
    LS_CHECK(duplicate.value() > originalCheckpoint.value());
    LS_CHECK_EQ(duplicateSegment.speakerId, first.value().speakerId);
    LS_CHECK_EQ(duplicateSegment.speakerLabel, std::string{"Alice"});
    LS_CHECK_EQ(
        duplicateSegment.journalCheckpoint,
        first.value().journalCheckpoint);

    auto revised = remoteSegment(1, anonymous, {0.995F, 0.005F});
    revised.revision = 2;
    revised.text = "revised voice profile evidence";
    LS_CHECK(journal.value()->appendFinalSegment("first-call", revised));
    LS_CHECK_EQ(revised.speakerId, first.value().speakerId);
    LS_CHECK_EQ(revised.speakerLabel, std::string{"Alice"});

    auto future = remoteSegment(3, anonymous, {0.98F, 0.02F});
    LS_CHECK(journal.value()->appendFinalSegment("first-call", future));
    LS_CHECK_EQ(future.speakerId, first.value().speakerId);
    LS_CHECK_EQ(future.speakerLabel, std::string{"Alice"});
    auto snapshot = journal.value()->snapshot("first-call");
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{3});
    LS_CHECK_EQ(
        snapshot.value().session.highestSegmentRevision,
        std::uint32_t{2});
    LS_CHECK_EQ(snapshot.value().segments[0].revision, std::uint32_t{2});
    LS_CHECK_EQ(
        snapshot.value().segments[0].text,
        std::string{"revised voice profile evidence"});
    for (const auto &segment : snapshot.value().segments) {
        LS_CHECK_EQ(segment.speakerId, first.value().speakerId);
        LS_CHECK_EQ(segment.speakerLabel, std::string{"Alice"});
        LS_CHECK_EQ(
            segment.speakerEmbeddingModel,
            std::string{kSpeakerFeatureModelId});
        LS_CHECK_EQ(segment.speakerEmbedding.size(), std::size_t{2});
    }
    finishSession(journal.value(), "first-call");

    createTerminalSession(journal.value(), "second-call");
    LS_CHECK(journal.value()->appendFinalSegment(
        "second-call",
        remoteSegment(4, anonymous, {0.97F, 0.03F})));
    auto merged = journal.value()->enrollVoiceProfile(
        "second-call",
        anonymous,
        "alice");
    LS_CHECK(merged);
    LS_CHECK_EQ(merged.value().profile.profileId, std::uint64_t{1});
    LS_CHECK_EQ(merged.value().profile.displayName, std::string{"alice"});
    LS_CHECK_EQ(merged.value().profile.observationCount, std::uint64_t{3});

    auto profiles = journal.value()->listVoiceProfiles();
    LS_CHECK(profiles);
    LS_CHECK_EQ(profiles.value().size(), std::size_t{1});
    LS_CHECK_EQ(profiles.value()[0].displayName, std::string{"alice"});

    auto reserved = journal.value()->enrollVoiceProfile(
        "second-call",
        anonymous,
        "ME");
    LS_CHECK(!reserved);
    LS_CHECK_EQ(reserved.error().code, LS_INVALID_ARGUMENT);
    auto whitespace = journal.value()->enrollVoiceProfile(
        "second-call",
        anonymous,
        std::string{"\xC2\xA0"});
    LS_CHECK(!whitespace);
    LS_CHECK_EQ(whitespace.error().code, LS_INVALID_ARGUMENT);

    LS_CHECK(journal.value()->renameVoiceProfile(1, "Bob"));
    auto historicalFirst = journal.value()->snapshot("first-call");
    LS_CHECK(historicalFirst);
    for (const auto &segment : historicalFirst.value().segments) {
        LS_CHECK_EQ(segment.speakerLabel, std::string{"Alice"});
    }
    auto historicalSecond = journal.value()->snapshot("second-call");
    LS_CHECK(historicalSecond);
    for (const auto &segment : historicalSecond.value().segments) {
        LS_CHECK_EQ(segment.speakerLabel, std::string{"alice"});
    }

    auto duplicateDelete = journal.value()->deleteVoiceProfile(1);
    LS_CHECK(duplicateDelete);
    profiles = journal.value()->listVoiceProfiles();
    LS_CHECK(profiles);
    LS_CHECK(profiles.value().empty());

    historicalFirst = journal.value()->snapshot("first-call");
    LS_CHECK(historicalFirst);
    for (const auto &segment : historicalFirst.value().segments) {
        LS_CHECK_EQ(segment.speakerLabel, std::string{"Alice"});
        LS_CHECK_EQ(segment.speakerId, first.value().speakerId);
    }
    historicalSecond = journal.value()->snapshot("second-call");
    LS_CHECK(historicalSecond);
    for (const auto &segment : historicalSecond.value().segments) {
        LS_CHECK_EQ(segment.speakerLabel, std::string{"alice"});
        LS_CHECK_EQ(segment.speakerId, first.value().speakerId);
    }
}

LS_TEST(corrupt_voice_profile_is_skipped_but_historical_overlay_survives)
{
    TemporaryDatabase databaseFile;
    auto journal = RecoveryJournal::open(databaseFile.path.string());
    LS_CHECK(journal);
    constexpr std::uint64_t anonymous = kAnonymousSpeakerFlag | 1u;

    createTerminalSession(journal.value(), "corrupt-profile-call");
    LS_CHECK(journal.value()->appendFinalSegment(
        "corrupt-profile-call",
        remoteSegment(9, anonymous, {1.0F, 0.0F})));
    auto enrollment = journal.value()->enrollVoiceProfile(
        "corrupt-profile-call",
        anonymous,
        "Alice");
    LS_CHECK(enrollment);

    sqlite3 *database = nullptr;
    LS_CHECK_EQ(
        sqlite3_open(databaseFile.path.c_str(), &database),
        SQLITE_OK);
    LS_CHECK_EQ(
        sqlite3_exec(
            database,
            "UPDATE voice_profiles "
            "SET centroid = X'0000C07F00000000' WHERE profile_id = 1",
            nullptr,
            nullptr,
            nullptr),
        SQLITE_OK);
    sqlite3_close(database);

    auto profiles = journal.value()->listVoiceProfiles();
    LS_CHECK(profiles);
    LS_CHECK(profiles.value().empty());

    auto snapshot = journal.value()->snapshot("corrupt-profile-call");
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{1});
    LS_CHECK_EQ(
        snapshot.value().segments[0].speakerId,
        enrollment.value().speakerId);
    LS_CHECK_EQ(
        snapshot.value().segments[0].speakerLabel,
        std::string{"Alice"});
}

LS_TEST(c_abi_lists_enrolls_renames_and_deletes_voice_profiles)
{
    TemporaryDatabase databaseFile;
    constexpr std::uint64_t anonymous = kAnonymousSpeakerFlag | 1u;
    {
        auto journal = RecoveryJournal::open(databaseFile.path.string());
        LS_CHECK(journal);
        createTerminalSession(journal.value(), "abi-profile-call");
        LS_CHECK(journal.value()->appendFinalSegment(
            "abi-profile-call",
            remoteSegment(7, anonymous, {1.0F, 0.0F})));
        finishSession(journal.value(), "abi-profile-call");
    }

    const std::string path = databaseFile.path.string();
    ls_core_config_v1 configuration{};
    configuration.struct_size = sizeof(configuration);
    configuration.abi_version = LS_CORE_ABI_VERSION;
    configuration.journal_path = view(path);
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    ls_core_t *core = nullptr;
    LS_CHECK_EQ(
        ls_core_create_v1(&configuration, &core, &error),
        LS_OK);

    const std::string sessionId{"abi-profile-call"};
    const std::string name{"Alice"};
    ls_voice_profile_enrollment_v1 enrollment{};
    enrollment.struct_size = sizeof(enrollment);
    enrollment.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_core_enroll_voice_profile_v1(
            core,
            view(sessionId),
            anonymous,
            view(name),
            &enrollment,
            &error),
        LS_OK);
    LS_CHECK_EQ(enrollment.profile_id, std::uint64_t{1});
    LS_CHECK(isPersistentSpeakerId(enrollment.speaker_id));
    LS_CHECK_EQ(enrollment.observation_count, std::uint64_t{1});
    LS_CHECK_EQ(enrollment.relabeled_segments, std::uint32_t{1});

    ls_voice_profile_list_t *list = nullptr;
    LS_CHECK_EQ(
        ls_core_list_voice_profiles_v1(core, &list, &error),
        LS_OK);
    LS_CHECK_EQ(ls_voice_profile_list_count(list), std::size_t{1});
    ls_voice_profile_copy_v1 copied{};
    copied.struct_size = sizeof(copied);
    copied.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_voice_profile_list_copy_v1(list, 0, &copied),
        LS_OK);
    LS_CHECK_EQ(copied.profile_id, enrollment.profile_id);
    LS_CHECK_EQ(copied.observation_count, std::uint64_t{1});
    ls_voice_profile_list_destroy(list);

    const std::string renamed{"Alice Cooper"};
    LS_CHECK_EQ(
        ls_core_rename_voice_profile_v1(
            core,
            enrollment.profile_id,
            view(renamed),
            &error),
        LS_OK);
    LS_CHECK_EQ(
        ls_core_delete_voice_profile_v1(
            core,
            enrollment.profile_id,
            &error),
        LS_OK);
    ls_core_destroy(core);
}
