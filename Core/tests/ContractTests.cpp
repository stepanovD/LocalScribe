#include "TestSupport.hpp"

#include <LocalScribeCore/LocalScribeCore.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

ls_utf8_view_v1 view(const std::string &value)
{
    return ls_utf8_view_v1{
        sizeof(ls_utf8_view_v1),
        LS_CORE_ABI_VERSION,
        reinterpret_cast<const std::uint8_t *>(value.data()),
        value.size()};
}

ls_status_code_t pushEventually(
    ls_session_t *session,
    const ls_audio_frame_v1 &frame)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto status = ls_session_push_audio_v1(session, &frame);
        if (status != LS_BACKPRESSURE) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return LS_BACKPRESSURE;
}

class Handles {
public:
    ~Handles()
    {
        ls_owned_bytes_destroy(markdown);
        ls_session_destroy(session);
        ls_core_destroy(core);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    ls_core_t *core{};
    ls_session_t *session{};
    ls_owned_bytes_t *markdown{};
    std::filesystem::path path;
};

Handles createFixtureSession(
    std::string id,
    std::string backend = "fixture",
    std::uint32_t queueCapacity = 8,
    std::string diarization = "source-aware")
{
    Handles handles;
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    handles.path = std::filesystem::temp_directory_path()
        / ("localscribe-contract-" + std::to_string(stamp) + ".sqlite3");
    const std::string path = handles.path.string();

    ls_core_config_v1 coreConfig{};
    coreConfig.struct_size = sizeof(coreConfig);
    coreConfig.abi_version = LS_CORE_ABI_VERSION;
    coreConfig.flags = LS_CORE_CONFIG_ALLOW_TEST_BACKENDS;
    coreConfig.journal_path = view(path);
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_core_create_v1(&coreConfig, &handles.core, &error),
        LS_OK);

    const std::string sourceApp = "Fixture";
    const std::string speaker = "Me";
    const std::string model;
    const std::string created = "2026-07-29T10:00:00+04:00";
    ls_session_config_v1 sessionConfig{};
    sessionConfig.struct_size = sizeof(sessionConfig);
    sessionConfig.abi_version = LS_CORE_ABI_VERSION;
    sessionConfig.session_id = view(id);
    sessionConfig.journal_path = view(path);
    sessionConfig.source_app = view(sourceApp);
    sessionConfig.local_speaker_name = view(speaker);
    sessionConfig.asr_backend_id = view(backend);
    sessionConfig.asr_model_path = view(model);
    sessionConfig.diarization_backend_id = view(diarization);
    sessionConfig.created_at_iso8601 = view(created);
    sessionConfig.language_mode = LS_LANGUAGE_MODE_RUSSIAN_ENGLISH;
    sessionConfig.audio_queue_capacity_frames = queueCapacity;
    sessionConfig.microphone_source_id = 1;
    sessionConfig.system_audio_source_id = 2;
    sessionConfig.required_source_mask =
        LS_REQUIRED_SOURCE_MICROPHONE | LS_REQUIRED_SOURCE_SYSTEM_AUDIO;
    sessionConfig.source_completeness_threshold_ns = 30'000'000'000;
    LS_CHECK_EQ(
        ls_session_create_after_consent_v1(
            handles.core,
            &sessionConfig,
            &handles.session,
            &error),
        LS_OK);
    return handles;
}

bool waitForFinalEvent(ls_session_t *session)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(session, 100, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        if (status != LS_OK) {
            return false;
        }
        const bool final =
            ls_event_kind(event) == LS_EVENT_FINAL_SEGMENT;
        ls_event_destroy(event);
        if (final) {
            return true;
        }
    }
    return false;
}

bool waitForErrorEvent(ls_session_t *session)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(session, 100, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        if (status != LS_OK) {
            return false;
        }
        const bool error = ls_event_kind(event) == LS_EVENT_ERROR;
        ls_event_destroy(event);
        if (error) {
            return true;
        }
    }
    return false;
}

bool waitForTerminalEvent(
    ls_session_t *session,
    ls_state_event_copy_v1 &state)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(session, 100, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        if (status != LS_OK) {
            return false;
        }
        const bool terminal =
            ls_event_kind(event) == LS_EVENT_TERMINAL;
        if (terminal) {
            state.struct_size = sizeof(state);
            state.abi_version = LS_CORE_ABI_VERSION;
            const auto copied = ls_event_copy_state_v1(event, &state);
            ls_event_destroy(event);
            return copied == LS_OK;
        }
        ls_event_destroy(event);
    }
    return false;
}

std::uint64_t countSourceEvents(
    const std::filesystem::path &path,
    std::uint64_t sourceId,
    std::string_view reason)
{
    sqlite3 *database = nullptr;
    LS_CHECK_EQ(
        sqlite3_open_v2(
            path.c_str(),
            &database,
            SQLITE_OPEN_READONLY,
            nullptr),
        SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    LS_CHECK_EQ(
        sqlite3_prepare_v2(
            database,
            "SELECT COUNT(*) FROM source_events "
            "WHERE source_id = ? AND reason = ?",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    sqlite3_bind_int64(
        statement,
        1,
        static_cast<sqlite3_int64>(sourceId));
    sqlite3_bind_text(
        statement,
        2,
        reason.data(),
        static_cast<int>(reason.size()),
        SQLITE_TRANSIENT);
    LS_CHECK_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto count = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return count;
}

std::uint64_t countFinalSegments(
    const std::filesystem::path &path,
    std::uint64_t sourceId,
    std::string_view text)
{
    sqlite3 *database = nullptr;
    LS_CHECK_EQ(
        sqlite3_open_v2(
            path.c_str(),
            &database,
            SQLITE_OPEN_READONLY,
            nullptr),
        SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    LS_CHECK_EQ(
        sqlite3_prepare_v2(
            database,
            "SELECT COUNT(*) FROM segments "
            "WHERE source_id = ? AND text = ?",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    sqlite3_bind_int64(
        statement,
        1,
        static_cast<sqlite3_int64>(sourceId));
    sqlite3_bind_text(
        statement,
        2,
        text.data(),
        static_cast<int>(text.size()),
        SQLITE_TRANSIENT);
    LS_CHECK_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto count = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return count;
}

std::uint64_t countSegmentsWithSpeaker(
    const std::filesystem::path &path,
    std::uint64_t sourceId,
    std::string_view speaker)
{
    sqlite3 *database = nullptr;
    LS_CHECK_EQ(
        sqlite3_open_v2(
            path.c_str(),
            &database,
            SQLITE_OPEN_READONLY,
            nullptr),
        SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    LS_CHECK_EQ(
        sqlite3_prepare_v2(
            database,
            "SELECT COUNT(*) FROM segments "
            "WHERE source_id = ? AND speaker_label = ?",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    sqlite3_bind_int64(
        statement,
        1,
        static_cast<sqlite3_int64>(sourceId));
    sqlite3_bind_text(
        statement,
        2,
        speaker.data(),
        static_cast<int>(speaker.size()),
        SQLITE_TRANSIENT);
    LS_CHECK_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto count = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return count;
}

void verifyTerminalRecovery(
    ls_phase_t expectedPhase,
    std::string expectedStatus)
{
    const std::string sessionId =
        expectedPhase == LS_PHASE_COMPLETE
        ? "terminal-complete"
        : "terminal-incomplete";
    auto handles = createFixtureSession(sessionId);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);
    if (expectedPhase == LS_PHASE_INCOMPLETE_SOURCES) {
        const std::string reason{"permanent test loss"};
        ls_source_event_v1 event{};
        event.struct_size = sizeof(event);
        event.abi_version = LS_CORE_ABI_VERSION;
        event.source_id = 2;
        event.source_kind = LS_SOURCE_KIND_SYSTEM_AUDIO;
        event.event_kind = LS_SOURCE_EVENT_PERMANENTLY_LOST;
        event.health = LS_SOURCE_HEALTH_PERMANENTLY_LOST;
        event.flags = LS_SOURCE_EVENT_FLAG_TEST_INJECTED;
        event.start_time_ns = 1;
        event.end_time_ns = 1;
        event.reason = view(reason);
        LS_CHECK_EQ(
            ls_session_source_event_v1(handles.session, &event),
            LS_OK);
    }
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);

    ls_session_destroy(handles.session);
    handles.session = nullptr;
    ls_core_destroy(handles.core);
    handles.core = nullptr;

    const std::string journalPath = handles.path.string();
    ls_core_config_v1 coreConfig{};
    coreConfig.struct_size = sizeof(coreConfig);
    coreConfig.abi_version = LS_CORE_ABI_VERSION;
    coreConfig.flags = LS_CORE_CONFIG_ALLOW_TEST_BACKENDS;
    coreConfig.journal_path = view(journalPath);
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_core_create_v1(&coreConfig, &handles.core, &error),
        LS_OK);

    ls_recovery_list_t *list = nullptr;
    LS_CHECK_EQ(
        ls_core_list_recoverable_sessions_v1(handles.core, &list),
        LS_OK);
    LS_CHECK_EQ(ls_recovery_list_count(list), std::size_t{1});
    ls_recovery_list_destroy(list);
    LS_CHECK_EQ(
        ls_core_open_recoverable_session_v1(
            handles.core,
            view(sessionId),
            &handles.session),
        LS_OK);
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_RECOVERY),
        LS_OK);

    const std::string empty;
    const std::string title{"Terminal recovery"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    ls_render_snapshot_v1 token{};
    token.struct_size = sizeof(token);
    token.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_render_markdown_with_snapshot_v1(
            handles.session,
            &options,
            &handles.markdown,
            &token,
            &error),
        LS_OK);
    const std::string markdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        markdown.find("status: \"" + expectedStatus + "\"")
        != std::string::npos);

    const std::string digest(64, 'e');
    const std::string identity{"terminal-recovery"};
    ls_publication_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.abi_version = LS_CORE_ABI_VERSION;
    receipt.journal_checkpoint = token.journal_checkpoint;
    receipt.highest_segment_revision =
        token.highest_segment_revision;
    receipt.destination = LS_PUBLICATION_DESTINATION_STAGING;
    receipt.published_at_unix_ns = 1;
    receipt.sha256_hex = view(digest);
    receipt.file_identity = view(identity);
    LS_CHECK_EQ(
        ls_session_ack_publication_v1(handles.session, &receipt),
        LS_OK);

    list = nullptr;
    LS_CHECK_EQ(
        ls_core_list_recoverable_sessions_v1(handles.core, &list),
        LS_OK);
    LS_CHECK_EQ(ls_recovery_list_count(list), std::size_t{0});
    ls_recovery_list_destroy(list);
}

} // namespace

LS_TEST(c_abi_rejects_bad_struct_versions_without_creating_a_handle)
{
    ls_core_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = 99;
    ls_core_t *core = reinterpret_cast<ls_core_t *>(0x1);
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_core_create_v1(&config, &core, &error),
        LS_INVALID_ABI_VERSION);
    LS_CHECK(core == nullptr);
    LS_CHECK_EQ(error.code, LS_INVALID_ABI_VERSION);
    ls_core_destroy(nullptr);
    ls_session_destroy(nullptr);
    ls_event_destroy(nullptr);
    ls_owned_bytes_destroy(nullptr);
}

LS_TEST(c_abi_state_snapshot_is_authoritative_and_does_not_consume_events)
{
    auto handles = createFixtureSession("authoritative-state-snapshot");
    ls_state_event_copy_v1 state{};
    state.struct_size = sizeof(state);
    state.abi_version = LS_CORE_ABI_VERSION;

    LS_CHECK_EQ(
        ls_session_copy_state_v1(handles.session, &state),
        LS_OK);
    LS_CHECK_EQ(state.phase, LS_PHASE_PREPARING);
    LS_CHECK_EQ(
        state.published_status,
        LS_PUBLISHED_STATUS_UNKNOWN);
    LS_CHECK_EQ(
        state.finalize_reason,
        LS_FINALIZE_REASON_UNKNOWN);

    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);
    LS_CHECK_EQ(
        ls_session_copy_state_v1(handles.session, &state),
        LS_OK);
    LS_CHECK_EQ(state.phase, LS_PHASE_RECORDING);
    LS_CHECK_EQ(
        state.published_status,
        LS_PUBLISHED_STATUS_RECORDING);

    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    LS_CHECK_EQ(
        ls_session_copy_state_v1(handles.session, &state),
        LS_OK);
    LS_CHECK_EQ(state.phase, LS_PHASE_COMPLETE);
    LS_CHECK_EQ(
        state.published_status,
        LS_PUBLISHED_STATUS_COMPLETE);
    LS_CHECK_EQ(
        state.finalize_reason,
        LS_FINALIZE_REASON_USER_STOP);

    /*
     * Snapshot reads above must not consume or reorder the older queued
     * PREPARING event, even though authoritative state is already COMPLETE.
     */
    ls_event_t *event = nullptr;
    LS_CHECK_EQ(
        ls_session_next_event_v1(handles.session, 0, &event),
        LS_OK);
    LS_CHECK_EQ(ls_event_kind(event), LS_EVENT_STATE_CHANGED);
    ls_state_event_copy_v1 queued{};
    queued.struct_size = sizeof(queued);
    queued.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_event_copy_state_v1(event, &queued),
        LS_OK);
    LS_CHECK_EQ(queued.phase, LS_PHASE_PREPARING);
    ls_event_destroy(event);

    ls_state_event_copy_v1 terminal{};
    LS_CHECK(waitForTerminalEvent(handles.session, terminal));
    LS_CHECK_EQ(terminal.phase, LS_PHASE_COMPLETE);
}

LS_TEST(c_abi_fixture_runs_audio_to_journal_to_markdown)
{
    auto handles = createFixtureSession("abi-vertical");

    const std::string empty;
    const std::string title{"Fixture call"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_INVALID_STATE);
    LS_CHECK(handles.markdown == nullptr);

    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);
    std::vector<float> borrowed(160, 0.042F);
    ls_audio_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = LS_CORE_ABI_VERSION;
    frame.source_id = 1;
    frame.sequence_number = 1;
    frame.monotonic_time_ns = 5'000'000'000;
    frame.sample_rate_hz = 16'000;
    frame.channel_count = 1;
    frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
    frame.frame_count = static_cast<std::uint32_t>(borrowed.size());
    frame.samples = borrowed.data();
    LS_CHECK_EQ(
        pushEventually(handles.session, frame),
        LS_OK);
    std::fill(borrowed.begin(), borrowed.end(), 0.0F);

    bool sawFinal = false;
    for (int attempt = 0; attempt < 30 && !sawFinal; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(handles.session, 100, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        LS_CHECK_EQ(status, LS_OK);
        if (ls_event_kind(event) == LS_EVENT_FINAL_SEGMENT) {
            ls_transcript_segment_copy_v1 segment{};
            segment.struct_size = sizeof(segment);
            segment.abi_version = LS_CORE_ABI_VERSION;
            LS_CHECK_EQ(
                ls_event_copy_segment_v1(event, &segment),
                LS_OK);
            const std::string text{
                reinterpret_cast<const char *>(segment.text.data),
                segment.text.size};
            LS_CHECK_EQ(text, std::string{"fixture cue 42"});
            LS_CHECK_EQ(
                segment.flags & LS_SEGMENT_FLAG_FINAL,
                std::uint32_t{LS_SEGMENT_FLAG_FINAL});
            sawFinal = true;
        }
        ls_event_destroy(event);
    }
    LS_CHECK(sawFinal);

    options.microphone_captured = 1;
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_OK);
    const std::string markdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        markdown.find("status: \"recording\"") != std::string::npos);
    LS_CHECK(markdown.find("fixture cue 42") != std::string::npos);
    LS_CHECK(
        markdown.find("<!-- transcript:start -->") != std::string::npos);

    ls_pipeline_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_metrics_v1(handles.session, &metrics),
        LS_OK);
    LS_CHECK_EQ(metrics.frames_accepted, std::uint64_t{1});
    LS_CHECK_EQ(
        metrics.frames_offered,
        metrics.frames_accepted + metrics.frames_rejected);
    LS_CHECK_EQ(metrics.final_segments_committed, std::uint64_t{1});
    LS_CHECK_EQ(metrics.highest_segment_revision, std::uint32_t{1});

    LS_CHECK_EQ(ls_session_pause_v1(handles.session), LS_OK);
    LS_CHECK_EQ(
        ls_session_push_audio_v1(handles.session, &frame),
        LS_INVALID_STATE);
    LS_CHECK_EQ(
        ls_session_resume_after_consent_v1(handles.session),
        LS_OK);
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
}

LS_TEST(c_abi_persists_microphone_as_me_and_system_as_remote)
{
    auto handles = createFixtureSession(
        "source-speaker-invariant",
        "fixture",
        8,
        "acoustic-clustering");
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> microphoneSamples(320, 0.041F);
    std::vector<float> systemSamples(320, 0.042F);
    ls_audio_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = LS_CORE_ABI_VERSION;
    frame.sequence_number = 1;
    frame.monotonic_time_ns = 1'000'000'000;
    frame.sample_rate_hz = 16'000;
    frame.channel_count = 1;
    frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
    frame.frame_count =
        static_cast<std::uint32_t>(microphoneSamples.size());

    frame.source_id = 1;
    frame.samples = microphoneSamples.data();
    LS_CHECK_EQ(pushEventually(handles.session, frame), LS_OK);
    frame.source_id = 2;
    frame.samples = systemSamples.data();
    LS_CHECK_EQ(pushEventually(handles.session, frame), LS_OK);

    bool persisted = false;
    for (int attempt = 0; attempt < 200 && !persisted; ++attempt) {
        persisted =
            countSegmentsWithSpeaker(handles.path, 1, "Me") == 1
            && countSegmentsWithSpeaker(handles.path, 2, "Speaker 1")
                == 1;
        if (!persisted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    LS_CHECK(persisted);
    LS_CHECK_EQ(
        countSegmentsWithSpeaker(handles.path, 2, "Me"),
        std::uint64_t{0});
    LS_CHECK_EQ(
        countSegmentsWithSpeaker(handles.path, 1, "Speaker 1"),
        std::uint64_t{0});
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
}

LS_TEST(render_snapshot_token_stays_bound_to_bytes_while_capture_advances)
{
    auto handles = createFixtureSession("render-token-race");
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> firstSamples(160, 0.042F);
    ls_audio_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = LS_CORE_ABI_VERSION;
    frame.source_id = 1;
    frame.sequence_number = 1;
    frame.monotonic_time_ns = 5'000'000'000;
    frame.sample_rate_hz = 16'000;
    frame.channel_count = 1;
    frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
    frame.frame_count = static_cast<std::uint32_t>(firstSamples.size());
    frame.samples = firstSamples.data();
    LS_CHECK_EQ(
        pushEventually(handles.session, frame),
        LS_OK);
    LS_CHECK(waitForFinalEvent(handles.session));

    const std::string empty;
    const std::string title{"Snapshot race"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    ls_render_snapshot_v1 firstToken{};
    firstToken.struct_size = sizeof(firstToken);
    firstToken.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_render_markdown_with_snapshot_v1(
            handles.session,
            &options,
            &handles.markdown,
            &firstToken,
            &error),
        LS_OK);
    const std::string firstMarkdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        firstMarkdown.find("fixture cue 42")
        != std::string::npos);
    LS_CHECK(
        firstMarkdown.find("fixture cue 42 revised")
        == std::string::npos);
    LS_CHECK_EQ(
        firstToken.highest_segment_revision,
        std::uint32_t{1});
    ls_owned_bytes_destroy(handles.markdown);
    handles.markdown = nullptr;

    std::vector<float> revisedSamples(160, 0.042F);
    revisedSamples.front() = -0.042F;
    frame.sequence_number = 2;
    frame.monotonic_time_ns = 6'000'000'000;
    frame.samples = revisedSamples.data();
    std::atomic<ls_status_code_t> pushStatus{LS_STATUS_UNKNOWN};
    std::thread capture([&] {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto status =
                ls_session_push_audio_v1(handles.session, &frame);
            if (status != LS_BACKPRESSURE) {
                pushStatus.store(status);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        pushStatus.store(LS_BACKPRESSURE);
    });
    capture.join();
    LS_CHECK_EQ(pushStatus.load(), LS_OK);
    LS_CHECK(waitForFinalEvent(handles.session));

    ls_pipeline_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_metrics_v1(handles.session, &metrics),
        LS_OK);
    LS_CHECK(metrics.journal_checkpoint > firstToken.journal_checkpoint);
    LS_CHECK_EQ(metrics.highest_segment_revision, std::uint32_t{2});
    LS_CHECK_EQ(
        firstToken.highest_segment_revision,
        std::uint32_t{1});

    const std::string firstDigest(64, 'a');
    const std::string firstIdentity{"render-one"};
    ls_publication_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.abi_version = LS_CORE_ABI_VERSION;
    receipt.journal_checkpoint = firstToken.journal_checkpoint;
    receipt.highest_segment_revision =
        firstToken.highest_segment_revision;
    receipt.destination = LS_PUBLICATION_DESTINATION_STAGING;
    receipt.published_at_unix_ns = 1;
    receipt.sha256_hex = view(firstDigest);
    receipt.file_identity = view(firstIdentity);
    LS_CHECK_EQ(
        ls_session_ack_publication_v1(handles.session, &receipt),
        LS_OK);

    ls_render_snapshot_v1 secondToken{};
    secondToken.struct_size = sizeof(secondToken);
    secondToken.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_render_markdown_with_snapshot_v1(
            handles.session,
            &options,
            &handles.markdown,
            &secondToken,
            &error),
        LS_OK);
    const std::string secondMarkdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        secondMarkdown.find("fixture cue 42 revised")
        != std::string::npos);
    LS_CHECK_EQ(
        secondToken.highest_segment_revision,
        std::uint32_t{2});
    const std::string secondDigest(64, 'b');
    const std::string secondIdentity{"render-two"};
    receipt.journal_checkpoint = secondToken.journal_checkpoint;
    receipt.highest_segment_revision =
        secondToken.highest_segment_revision;
    receipt.sha256_hex = view(secondDigest);
    receipt.file_identity = view(secondIdentity);
    LS_CHECK_EQ(
        ls_session_ack_publication_v1(handles.session, &receipt),
        LS_OK);

    receipt.journal_checkpoint = firstToken.journal_checkpoint;
    receipt.highest_segment_revision =
        firstToken.highest_segment_revision;
    receipt.sha256_hex = view(firstDigest);
    receipt.file_identity = view(firstIdentity);
    LS_CHECK_EQ(
        ls_session_ack_publication_v1(handles.session, &receipt),
        LS_CONFLICT);
}

LS_TEST(live_render_does_not_wait_for_a_slow_asr_queue)
{
    auto handles =
        createFixtureSession("bounded-live-render", "fixture-slow", 64);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::atomic<bool> producing{true};
    std::atomic<std::uint64_t> sequence{0};
    std::vector<float> silence(160, 0.0F);
    std::thread producer([&] {
        while (producing.load()) {
            ls_audio_frame_v1 frame{};
            frame.struct_size = sizeof(frame);
            frame.abi_version = LS_CORE_ABI_VERSION;
            frame.source_id = 1;
            frame.sequence_number = sequence.fetch_add(1) + 1;
            frame.monotonic_time_ns =
                static_cast<std::int64_t>(frame.sequence_number)
                * 10'000'000;
            frame.sample_rate_hz = 16'000;
            frame.channel_count = 1;
            frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
            frame.frame_count =
                static_cast<std::uint32_t>(silence.size());
            frame.samples = silence.data();
            (void)ls_session_push_audio_v1(handles.session, &frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    bool queueBackedUp = false;
    for (int attempt = 0; attempt < 200 && !queueBackedUp; ++attempt) {
        ls_pipeline_metrics_v1 metrics{};
        metrics.struct_size = sizeof(metrics);
        metrics.abi_version = LS_CORE_ABI_VERSION;
        LS_CHECK_EQ(
            ls_session_copy_metrics_v1(handles.session, &metrics),
            LS_OK);
        queueBackedUp = metrics.audio_queue_depth >= 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!queueBackedUp) {
        producing.store(false);
        producer.join();
        LS_CHECK(queueBackedUp);
    }

    const std::string empty;
    const std::string title{"Live"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    ls_render_snapshot_v1 token{};
    token.struct_size = sizeof(token);
    token.abi_version = LS_CORE_ABI_VERSION;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    const auto renderStarted = std::chrono::steady_clock::now();
    const auto renderStatus =
        ls_session_render_markdown_with_snapshot_v1(
            handles.session,
            &options,
            &handles.markdown,
            &token,
            &error);
    const auto renderElapsed =
        std::chrono::steady_clock::now() - renderStarted;
    producing.store(false);
    producer.join();

    LS_CHECK_EQ(renderStatus, LS_OK);
    LS_CHECK(
        renderElapsed < std::chrono::milliseconds(500));
    LS_CHECK(token.journal_checkpoint > 0);

    const auto finalizeStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    LS_CHECK(
        std::chrono::steady_clock::now() - finalizeStarted
        < std::chrono::seconds(5));
}

LS_TEST(two_channel_callback_load_stays_bounded_and_recovers)
{
    auto handles = createFixtureSession(
        "two-channel-bounded-load",
        "fixture-slow",
        8);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::array<std::uint64_t, 2> sequences{0, 0};
    std::vector<float> samples(320, 0.05F);
    for (std::uint64_t tick = 0; tick < 50; ++tick) {
        for (std::size_t sourceIndex = 0; sourceIndex < 2; ++sourceIndex) {
            const std::uint64_t sourceId = sourceIndex + 1u;
            samples.front() = static_cast<float>(
                100u + sourceIndex * 200u + tick) / 1000.0F;
            ls_audio_frame_v1 frame{};
            frame.struct_size = sizeof(frame);
            frame.abi_version = LS_CORE_ABI_VERSION;
            frame.source_id = sourceId;
            frame.sequence_number = ++sequences[sourceIndex];
            frame.monotonic_time_ns =
                static_cast<std::int64_t>(tick) * 20'000'000;
            frame.sample_rate_hz = 16'000;
            frame.channel_count = 1;
            frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
            frame.frame_count =
                static_cast<std::uint32_t>(samples.size());
            frame.samples = samples.data();
            const auto status =
                ls_session_push_audio_v1(handles.session, &frame);
            LS_CHECK(status == LS_OK || status == LS_BACKPRESSURE);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    bool recovered = false;
    ls_pipeline_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LS_CORE_ABI_VERSION;
    for (int attempt = 0; attempt < 300 && !recovered; ++attempt) {
        LS_CHECK_EQ(
            ls_session_copy_metrics_v1(handles.session, &metrics),
            LS_OK);
        recovered = metrics.audio_queue_depth == 0
            && metrics.final_segments_committed != 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LS_CHECK(recovered);
    LS_CHECK_EQ(metrics.frames_offered, std::uint64_t{100});
    LS_CHECK_EQ(
        metrics.frames_offered,
        metrics.frames_accepted + metrics.frames_rejected);
    LS_CHECK(metrics.audio_queue_high_water <= std::uint32_t{16});
    std::cout
        << "two_channel_load queue_high_water="
        << metrics.audio_queue_high_water
        << " rejected_frames=" << metrics.frames_rejected
        << " final_segments=" << metrics.final_segments_committed
        << '\n';

    const auto finalizeStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    LS_CHECK(
        std::chrono::steady_clock::now() - finalizeStarted
        < std::chrono::seconds(2));
}

LS_TEST(backpressure_is_one_gap_per_source_episode_and_finals_resume)
{
    auto handles = createFixtureSession(
        "overload-episode-recovery",
        "fixture-overloaded",
        1);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> microphoneSamples(320, 0.041F);
    std::vector<float> systemSamples(320, 0.082F);
    const auto push = [&](std::uint64_t sourceId,
                          std::uint64_t sequence,
                          std::int64_t timestamp,
                          std::vector<float> &sourceSamples) {
        sourceSamples.front() = static_cast<float>(
            100u + (sequence % 700u)) / 1000.0F;
        ls_audio_frame_v1 frame{};
        frame.struct_size = sizeof(frame);
        frame.abi_version = LS_CORE_ABI_VERSION;
        frame.source_id = sourceId;
        frame.sequence_number = sequence;
        frame.monotonic_time_ns = timestamp;
        frame.sample_rate_hz = 16'000;
        frame.channel_count = 1;
        frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
        frame.frame_count =
            static_cast<std::uint32_t>(sourceSamples.size());
        frame.samples = sourceSamples.data();
        return ls_session_push_audio_v1(handles.session, &frame);
    };
    const auto pushAfterTransientContention =
        [&](std::uint64_t sourceId,
            std::uint64_t sequence,
            std::int64_t timestamp,
            std::vector<float> &sourceSamples) {
            ls_status_code_t status = LS_BACKPRESSURE;
            for (int attempt = 0;
                 attempt < 100 && status == LS_BACKPRESSURE;
                 ++attempt) {
                status = push(
                    sourceId,
                    sequence,
                    timestamp,
                    sourceSamples);
                if (status == LS_BACKPRESSURE) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
            return status;
        };

    LS_CHECK_EQ(
        pushAfterTransientContention(
            1,
            1,
            0,
            microphoneSamples),
        LS_OK);
    LS_CHECK_EQ(
        pushAfterTransientContention(
            2,
            1,
            0,
            systemSamples),
        LS_OK);

    std::uint64_t observedOverloadEpisodes = 0;
    bool overloadActive = false;
    const auto observeDisposition = [&](ls_status_code_t status) {
        if (status == LS_BACKPRESSURE) {
            if (!overloadActive) {
                ++observedOverloadEpisodes;
                overloadActive = true;
            }
        } else if (status == LS_OK) {
            overloadActive = false;
        }
    };

    std::uint64_t rejectedFirstEpisode = 0;
    for (std::uint64_t sequence = 2; sequence <= 200; ++sequence) {
        const auto status = push(
            1,
            sequence,
            static_cast<std::int64_t>(sequence) * 20'000'000,
            microphoneSamples);
        LS_CHECK(status == LS_OK || status == LS_BACKPRESSURE);
        observeDisposition(status);
        if (status == LS_BACKPRESSURE) {
            ++rejectedFirstEpisode;
        }
    }
    LS_CHECK(rejectedFirstEpisode > 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto firstRecovery =
        pushAfterTransientContention(
            1,
            201,
            4'020'000'000,
            microphoneSamples);
    LS_CHECK_EQ(firstRecovery, LS_OK);
    observeDisposition(firstRecovery);

    bool firstFinalRecovered = false;
    for (int attempt = 0; attempt < 100 && !firstFinalRecovered; ++attempt) {
        firstFinalRecovered =
            countFinalSegments(
                handles.path,
                1,
                "fixture cue 301")
            == 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LS_CHECK(firstFinalRecovered);

    std::uint64_t rejectedSecondEpisode = 0;
    for (std::uint64_t sequence = 202; sequence <= 400; ++sequence) {
        const auto status = push(
            1,
            sequence,
            static_cast<std::int64_t>(sequence) * 20'000'000,
            microphoneSamples);
        LS_CHECK(status == LS_OK || status == LS_BACKPRESSURE);
        observeDisposition(status);
        if (status == LS_BACKPRESSURE) {
            ++rejectedSecondEpisode;
        }
    }
    LS_CHECK(rejectedSecondEpisode > 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    LS_CHECK_EQ(
        pushAfterTransientContention(
            2,
            2,
            8'020'000'000,
            systemSamples),
        LS_OK);
    const auto secondRecovery =
        pushAfterTransientContention(
            1,
            401,
            8'020'000'000,
            microphoneSamples);
    LS_CHECK_EQ(secondRecovery, LS_OK);
    observeDisposition(secondRecovery);

    bool episodesPersisted = false;
    std::uint64_t microphoneOverloadEvents = 0;
    for (int attempt = 0; attempt < 200 && !episodesPersisted; ++attempt) {
        microphoneOverloadEvents = countSourceEvents(
            handles.path,
            1,
            "backpressure overload episode");
        episodesPersisted =
            microphoneOverloadEvents == observedOverloadEpisodes;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LS_CHECK(episodesPersisted);
    LS_CHECK(observedOverloadEpisodes >= std::uint64_t{2});
    LS_CHECK_EQ(
        microphoneOverloadEvents,
        observedOverloadEpisodes);
    LS_CHECK(
        microphoneOverloadEvents
        < (rejectedFirstEpisode + rejectedSecondEpisode) / 10u);
    LS_CHECK_EQ(
        countSourceEvents(
            handles.path,
            2,
            "backpressure overload episode"),
        std::uint64_t{0});

    bool bothSourcesProducedFinals = false;
    for (int attempt = 0;
         attempt < 200 && !bothSourcesProducedFinals;
         ++attempt) {
        bothSourcesProducedFinals =
            countFinalSegments(
                handles.path,
                1,
                "fixture cue 501")
                == 1
            && countFinalSegments(
                handles.path,
                2,
                "fixture cue 102")
                == 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LS_CHECK(bothSourcesProducedFinals);

    ls_pipeline_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_metrics_v1(handles.session, &metrics),
        LS_OK);
    LS_CHECK_EQ(
        metrics.discontinuities,
        observedOverloadEpisodes);
    LS_CHECK(
        metrics.frames_rejected
        >= rejectedFirstEpisode + rejectedSecondEpisode);
    LS_CHECK(metrics.audio_queue_high_water <= std::uint32_t{2});
    std::cout
        << "overload_recovery queue_high_water="
        << metrics.audio_queue_high_water
        << " rejected_frames=" << metrics.frames_rejected
        << " overload_episodes=" << observedOverloadEpisodes
        << " discontinuities=" << metrics.discontinuities
        << '\n';

    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
}

LS_TEST(finalize_aborts_slow_inference_and_publishes_incomplete_markdown)
{
    auto handles =
        createFixtureSession("bounded-finalize", "fixture-stall", 2);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> samples(320, 0.071F);
    for (std::uint64_t sequence = 1; sequence <= 30; ++sequence) {
        ls_audio_frame_v1 frame{};
        frame.struct_size = sizeof(frame);
        frame.abi_version = LS_CORE_ABI_VERSION;
        frame.source_id = 1;
        frame.sequence_number = sequence;
        frame.monotonic_time_ns =
            static_cast<std::int64_t>(sequence) * 20'000'000;
        frame.sample_rate_hz = 16'000;
        frame.channel_count = 1;
        frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
        frame.frame_count =
            static_cast<std::uint32_t>(samples.size());
        frame.samples = samples.data();
        const auto status =
            ls_session_push_audio_v1(handles.session, &frame);
        LS_CHECK(status == LS_OK || status == LS_BACKPRESSURE);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto finalizeStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    const auto finalizeElapsed =
        std::chrono::steady_clock::now() - finalizeStarted;
    LS_CHECK(finalizeElapsed >= std::chrono::seconds(2));
    LS_CHECK(finalizeElapsed < std::chrono::seconds(4));
    std::cout
        << "bounded_finalize_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               finalizeElapsed)
               .count()
        << '\n';

    ls_state_event_copy_v1 state{};
    state.struct_size = sizeof(state);
    state.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_state_v1(handles.session, &state),
        LS_OK);
    LS_CHECK_EQ(state.phase, LS_PHASE_INCOMPLETE_SOURCES);

    const std::string empty;
    const std::string title{"Bounded finalize"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = 1;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_OK);
    const std::string markdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        markdown.find("status: \"incomplete_sources\"")
        != std::string::npos);
    LS_CHECK(
        markdown.find("## Capture events")
        != std::string::npos);
    LS_CHECK_EQ(
        countSourceEvents(
            handles.path,
            1,
            "finalization deadline: accepted audio was not transcribed"),
        std::uint64_t{1});
}

LS_TEST(finalize_aborts_stalled_asr_flush_and_publishes_incomplete_markdown)
{
    auto handles =
        createFixtureSession("bounded-flush", "fixture-flush-stall", 2);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> samples(320, 0.071F);
    ls_audio_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = LS_CORE_ABI_VERSION;
    frame.source_id = 1;
    frame.sequence_number = 1;
    frame.monotonic_time_ns = 20'000'000;
    frame.sample_rate_hz = 16'000;
    frame.channel_count = 1;
    frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
    frame.frame_count = static_cast<std::uint32_t>(samples.size());
    frame.samples = samples.data();
    LS_CHECK_EQ(
        pushEventually(handles.session, frame),
        LS_OK);

    bool drained = false;
    for (int attempt = 0; attempt < 200 && !drained; ++attempt) {
        ls_pipeline_metrics_v1 metrics{};
        metrics.struct_size = sizeof(metrics);
        metrics.abi_version = LS_CORE_ABI_VERSION;
        LS_CHECK_EQ(
            ls_session_copy_metrics_v1(handles.session, &metrics),
            LS_OK);
        drained = metrics.audio_queue_depth == 0;
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    LS_CHECK(drained);

    const auto finalizeStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    const auto finalizeElapsed =
        std::chrono::steady_clock::now() - finalizeStarted;
    LS_CHECK(finalizeElapsed >= std::chrono::seconds(2));
    LS_CHECK(finalizeElapsed < std::chrono::seconds(4));
    std::cout
        << "bounded_flush_finalize_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               finalizeElapsed)
               .count()
        << '\n';

    ls_state_event_copy_v1 state{};
    state.struct_size = sizeof(state);
    state.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_state_v1(handles.session, &state),
        LS_OK);
    LS_CHECK_EQ(state.phase, LS_PHASE_INCOMPLETE_SOURCES);

    const std::string empty;
    const std::string title{"Bounded flush finalize"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = 1;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_OK);
    const std::string markdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        markdown.find("status: \"incomplete_sources\"")
        != std::string::npos);
    LS_CHECK(
        markdown.find("## Capture events")
        != std::string::npos);
    LS_CHECK_EQ(
        countSourceEvents(
            handles.path,
            1,
            "finalization deadline: ASR tail was not flushed"),
        std::uint64_t{1});
    LS_CHECK_EQ(
        countSourceEvents(
            handles.path,
            2,
            "finalization deadline: ASR tail was not flushed"),
        std::uint64_t{1});
}

LS_TEST(backend_failure_latches_data_loss_and_finalizes_interrupted)
{
    auto handles =
        createFixtureSession("fatal-backend-failure", "fixture-fail", 16);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> samples(160, 0.042F);
    ls_audio_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = LS_CORE_ABI_VERSION;
    frame.source_id = 1;
    frame.sequence_number = 1;
    frame.monotonic_time_ns = 1'000'000'000;
    frame.sample_rate_hz = 16'000;
    frame.channel_count = 1;
    frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
    frame.frame_count = static_cast<std::uint32_t>(samples.size());
    frame.samples = samples.data();
    LS_CHECK_EQ(
        pushEventually(handles.session, frame),
        LS_OK);
    LS_CHECK(waitForErrorEvent(handles.session));

    const std::string empty;
    const std::string title{"Fatal backend"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    const auto renderStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_BACKEND_FAILURE);
    LS_CHECK(
        std::chrono::steady_clock::now() - renderStarted
        < std::chrono::milliseconds(500));
    LS_CHECK(handles.markdown == nullptr);

    frame.sequence_number = 2;
    frame.monotonic_time_ns = 2'000'000'000;
    LS_CHECK_EQ(
        ls_session_push_audio_v1(handles.session, &frame),
        LS_BACKEND_FAILURE);

    const auto finalizeStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    LS_CHECK(
        std::chrono::steady_clock::now() - finalizeStarted
        < std::chrono::seconds(2));

    ls_state_event_copy_v1 terminal{};
    LS_CHECK(waitForTerminalEvent(handles.session, terminal));
    LS_CHECK_EQ(terminal.phase, LS_PHASE_INTERRUPTED);
    LS_CHECK_EQ(
        terminal.finalize_reason,
        LS_FINALIZE_REASON_PROCESS_INTERRUPTED);

    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_OK);
    const std::string markdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        markdown.find("status: \"interrupted\"")
        != std::string::npos);
}

LS_TEST(worker_exception_clears_queued_frames_and_finalize_never_hangs)
{
    auto handles =
        createFixtureSession("fatal-worker-throw", "fixture-throw", 64);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> samples(160, 0.042F);
    std::uint64_t acceptedFrames = 0;
    for (std::uint64_t sequence = 1;
         sequence <= 256 && acceptedFrames < 16;
         ++sequence) {
        ls_audio_frame_v1 frame{};
        frame.struct_size = sizeof(frame);
        frame.abi_version = LS_CORE_ABI_VERSION;
        frame.source_id = 1;
        frame.sequence_number = sequence;
        frame.monotonic_time_ns =
            static_cast<std::int64_t>(sequence) * 10'000'000;
        frame.sample_rate_hz = 16'000;
        frame.channel_count = 1;
        frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
        frame.frame_count =
            static_cast<std::uint32_t>(samples.size());
        frame.samples = samples.data();
        const auto status =
            ls_session_push_audio_v1(handles.session, &frame);
        LS_CHECK(status == LS_OK || status == LS_BACKPRESSURE);
        if (status == LS_OK) {
            ++acceptedFrames;
        }
    }
    LS_CHECK_EQ(acceptedFrames, std::uint64_t{16});
    LS_CHECK(waitForErrorEvent(handles.session));

    ls_pipeline_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_metrics_v1(handles.session, &metrics),
        LS_OK);
    LS_CHECK_EQ(metrics.audio_queue_depth, std::uint32_t{0});
    LS_CHECK(metrics.frames_offered >= std::uint64_t{16});
    LS_CHECK_EQ(
        metrics.frames_offered,
        metrics.frames_accepted + metrics.frames_rejected);

    const auto finalizeStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    LS_CHECK(
        std::chrono::steady_clock::now() - finalizeStarted
        < std::chrono::seconds(2));

    ls_state_event_copy_v1 terminal{};
    LS_CHECK(waitForTerminalEvent(handles.session, terminal));
    LS_CHECK_EQ(terminal.phase, LS_PHASE_INTERRUPTED);
    LS_CHECK_EQ(
        terminal.finalize_reason,
        LS_FINALIZE_REASON_PROCESS_INTERRUPTED);
}

LS_TEST(cancelled_preparing_session_becomes_failed_to_start)
{
    auto handles = createFixtureSession("cancelled-before-capture");
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_CANCELLED),
        LS_OK);

    ls_state_event_copy_v1 terminal{};
    LS_CHECK(waitForTerminalEvent(handles.session, terminal));
    LS_CHECK_EQ(terminal.phase, LS_PHASE_FAILED_TO_START);
    LS_CHECK_EQ(
        terminal.finalize_reason,
        LS_FINALIZE_REASON_CANCELLED);

    const std::string empty;
    const std::string title{"Must not publish"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_INVALID_STATE);
    LS_CHECK(handles.markdown == nullptr);

    ls_session_destroy(handles.session);
    handles.session = nullptr;
    ls_core_destroy(handles.core);
    handles.core = nullptr;

    const std::string journalPath = handles.path.string();
    ls_core_config_v1 coreConfig{};
    coreConfig.struct_size = sizeof(coreConfig);
    coreConfig.abi_version = LS_CORE_ABI_VERSION;
    coreConfig.flags = LS_CORE_CONFIG_ALLOW_TEST_BACKENDS;
    coreConfig.journal_path = view(journalPath);
    LS_CHECK_EQ(
        ls_core_create_v1(&coreConfig, &handles.core, &error),
        LS_OK);
    ls_recovery_list_t *list = nullptr;
    LS_CHECK_EQ(
        ls_core_list_recoverable_sessions_v1(handles.core, &list),
        LS_OK);
    LS_CHECK_EQ(ls_recovery_list_count(list), std::size_t{0});
    ls_recovery_list_destroy(list);
}

LS_TEST(complete_without_current_receipt_is_recoverable_until_ack)
{
    verifyTerminalRecovery(LS_PHASE_COMPLETE, "complete");
}

LS_TEST(incomplete_without_current_receipt_is_recoverable_until_ack)
{
    verifyTerminalRecovery(
        LS_PHASE_INCOMPLETE_SOURCES,
        "incomplete_sources");
}

LS_TEST(c_abi_recovery_exposes_exact_revision_and_original_created_time)
{
    const std::string sessionId{
        "11111111-2222-3333-4444-555555555555"};
    auto handles = createFixtureSession(sessionId);
    LS_CHECK_EQ(
        ls_session_mark_sources_ready_v1(handles.session),
        LS_OK);

    std::vector<float> samples(160, 0.042F);
    ls_audio_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = LS_CORE_ABI_VERSION;
    frame.source_id = 1;
    frame.sequence_number = 1;
    frame.monotonic_time_ns = 5'000'000'000;
    frame.sample_rate_hz = 16'000;
    frame.channel_count = 1;
    frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
    frame.frame_count = static_cast<std::uint32_t>(samples.size());
    frame.samples = samples.data();
    LS_CHECK_EQ(
        pushEventually(handles.session, frame),
        LS_OK);

    bool committed = false;
    for (int attempt = 0; attempt < 30 && !committed; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(handles.session, 100, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        LS_CHECK_EQ(status, LS_OK);
        committed = ls_event_kind(event) == LS_EVENT_FINAL_SEGMENT;
        ls_event_destroy(event);
    }
    LS_CHECK(committed);

    /*
     * Simulate process loss: destroying a live runtime must not finalize the
     * durable session. A new core marks it recovery-required on journal open.
     */
    ls_session_destroy(handles.session);
    handles.session = nullptr;
    ls_core_destroy(handles.core);
    handles.core = nullptr;

    const std::string journalPath = handles.path.string();
    ls_core_config_v1 coreConfig{};
    coreConfig.struct_size = sizeof(coreConfig);
    coreConfig.abi_version = LS_CORE_ABI_VERSION;
    coreConfig.flags = LS_CORE_CONFIG_ALLOW_TEST_BACKENDS;
    coreConfig.journal_path = view(journalPath);
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_core_create_v1(&coreConfig, &handles.core, &error),
        LS_OK);

    ls_recovery_list_t *list = nullptr;
    LS_CHECK_EQ(
        ls_core_list_recoverable_sessions_v1(handles.core, &list),
        LS_OK);
    LS_CHECK_EQ(ls_recovery_list_count(list), std::size_t{1});
    ls_utf8_view_v1 recoveredId{};
    recoveredId.struct_size = sizeof(recoveredId);
    recoveredId.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_recovery_list_session_id_v1(list, 0, &recoveredId),
        LS_OK);
    const std::string recoveredIdCopy{
        reinterpret_cast<const char *>(recoveredId.data),
        recoveredId.size};
    LS_CHECK_EQ(recoveredIdCopy, sessionId);
    ls_recovery_list_destroy(list);

    LS_CHECK_EQ(
        ls_core_open_recoverable_session_v1(
            handles.core,
            view(sessionId),
            &handles.session),
        LS_OK);
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_RECOVERY),
        LS_OK);

    ls_pipeline_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_metrics_v1(handles.session, &metrics),
        LS_OK);
    LS_CHECK_EQ(metrics.highest_segment_revision, std::uint32_t{1});
    LS_CHECK(metrics.journal_checkpoint > 0);

    const std::string empty;
    const std::string title{"Recovered Call"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_OK);
    const std::string markdown{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
    LS_CHECK(
        markdown.find("status: \"interrupted\"")
        != std::string::npos);
    LS_CHECK(
        markdown.find("created: \"2026-07-29T10:00:00+04:00\"")
        != std::string::npos);
    LS_CHECK(markdown.find("fixture cue 42") != std::string::npos);

    const std::string digest(64, 'a');
    ls_publication_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.abi_version = LS_CORE_ABI_VERSION;
    receipt.journal_checkpoint = metrics.journal_checkpoint;
    receipt.highest_segment_revision = 0;
    receipt.destination = LS_PUBLICATION_DESTINATION_STAGING;
    receipt.published_at_unix_ns = 1;
    receipt.sha256_hex = view(digest);
    receipt.file_identity = view(empty);

    list = nullptr;
    LS_CHECK_EQ(
        ls_core_list_recoverable_sessions_v1(handles.core, &list),
        LS_OK);
    LS_CHECK_EQ(ls_recovery_list_count(list), std::size_t{1});
    ls_recovery_list_destroy(list);

    LS_CHECK_EQ(
        ls_session_ack_publication_v1(handles.session, &receipt),
        LS_CONFLICT);
    receipt.highest_segment_revision =
        metrics.highest_segment_revision;
    LS_CHECK_EQ(
        ls_session_ack_publication_v1(handles.session, &receipt),
        LS_OK);

    list = nullptr;
    LS_CHECK_EQ(
        ls_core_list_recoverable_sessions_v1(handles.core, &list),
        LS_OK);
    LS_CHECK_EQ(ls_recovery_list_count(list), std::size_t{0});
    ls_recovery_list_destroy(list);
}
