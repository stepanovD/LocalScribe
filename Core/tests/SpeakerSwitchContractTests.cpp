#include "TestSupport.hpp"

#include <LocalScribeCore/LocalScribeCore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
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

class SpeakerFixtureSession {
public:
    ~SpeakerFixtureSession()
    {
        ls_owned_bytes_destroy(markdown);
        ls_session_destroy(session);
        ls_core_destroy(core);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    SpeakerFixtureSession() = default;
    SpeakerFixtureSession(const SpeakerFixtureSession &) = delete;
    SpeakerFixtureSession &operator=(const SpeakerFixtureSession &) = delete;
    SpeakerFixtureSession(SpeakerFixtureSession &&other) noexcept
        : core(std::exchange(other.core, nullptr)),
          session(std::exchange(other.session, nullptr)),
          markdown(std::exchange(other.markdown, nullptr)),
          path(std::move(other.path)),
          sessionId(std::move(other.sessionId))
    {
    }

    ls_core_t *core{};
    ls_session_t *session{};
    ls_owned_bytes_t *markdown{};
    std::filesystem::path path;
    std::string sessionId;
};

SpeakerFixtureSession createSpeakerFixtureSession(
    std::string sessionId,
    std::string asrBackend = "fixture-speakers",
    std::string diarizationBackend = "acoustic-clustering")
{
    SpeakerFixtureSession result;
    result.sessionId = std::move(sessionId);
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    result.path = std::filesystem::temp_directory_path()
        / ("localscribe-speaker-switch-" + std::to_string(stamp)
           + ".sqlite3");
    const std::string path = result.path.string();

    ls_core_config_v1 coreConfiguration{};
    coreConfiguration.struct_size = sizeof(coreConfiguration);
    coreConfiguration.abi_version = LS_CORE_ABI_VERSION;
    coreConfiguration.flags = LS_CORE_CONFIG_ALLOW_TEST_BACKENDS;
    coreConfiguration.journal_path = view(path);
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_core_create_v1(&coreConfiguration, &result.core, &error),
        LS_OK);

    const std::string sourceApp{"Speaker fixture"};
    const std::string localSpeaker{"Me"};
    const std::string empty;
    const std::string createdAt{"2026-08-17T10:00:00+05:00"};
    ls_session_config_v1 sessionConfiguration{};
    sessionConfiguration.struct_size = sizeof(sessionConfiguration);
    sessionConfiguration.abi_version = LS_CORE_ABI_VERSION;
    sessionConfiguration.session_id = view(result.sessionId);
    sessionConfiguration.journal_path = view(path);
    sessionConfiguration.source_app = view(sourceApp);
    sessionConfiguration.local_speaker_name = view(localSpeaker);
    sessionConfiguration.asr_backend_id = view(asrBackend);
    sessionConfiguration.asr_model_path = view(empty);
    sessionConfiguration.diarization_backend_id = view(diarizationBackend);
    sessionConfiguration.created_at_iso8601 = view(createdAt);
    sessionConfiguration.language_mode = LS_LANGUAGE_MODE_ENGLISH;
    sessionConfiguration.audio_queue_capacity_frames = 8;
    sessionConfiguration.microphone_source_id = 1;
    sessionConfiguration.system_audio_source_id = 2;
    sessionConfiguration.required_source_mask =
        LS_REQUIRED_SOURCE_MICROPHONE | LS_REQUIRED_SOURCE_SYSTEM_AUDIO;
    sessionConfiguration.source_completeness_threshold_ns =
        30'000'000'000LL;
    LS_CHECK_EQ(
        ls_session_create_after_consent_v1(
            result.core,
            &sessionConfiguration,
            &result.session,
            &error),
        LS_OK);
    LS_CHECK_EQ(ls_session_mark_sources_ready_v1(result.session), LS_OK);
    return result;
}

bool waitForBackendFailureWithoutFinal(ls_session_t *session)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status = ls_session_next_event_v1(session, 50, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        LS_CHECK_EQ(status, LS_OK);
        const auto kind = ls_event_kind(event);
        LS_CHECK(kind != LS_EVENT_FINAL_SEGMENT);
        ls_event_destroy(event);
        if (kind == LS_EVENT_ERROR) {
            return true;
        }
    }
    return false;
}

void pushSpeakerCue(
    ls_session_t *session,
    std::uint64_t cue,
    std::int64_t startTimeNs,
    float embeddingX,
    float embeddingY,
    float embeddingZ = 0.0F,
    bool speakerTurnAfter = false)
{
    std::vector<float> samples(160, 0.0F);
    samples[0] = static_cast<float>(cue) / 1000.0F;
    samples[1] = embeddingX;
    samples[2] = embeddingY;
    samples[3] = embeddingZ;
    samples[4] = speakerTurnAfter ? 1.0F : 0.0F;

    ls_audio_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = LS_CORE_ABI_VERSION;
    frame.source_id = 2;
    frame.sequence_number = cue;
    frame.monotonic_time_ns = startTimeNs;
    frame.sample_rate_hz = 16'000;
    frame.channel_count = 1;
    frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
    frame.frame_count = static_cast<std::uint32_t>(samples.size());
    frame.samples = samples.data();
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto status = ls_session_push_audio_v1(session, &frame);
        if (status != LS_BACKPRESSURE) {
            LS_CHECK_EQ(status, LS_OK);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LS_CHECK(false);
}

void reportSystemAudioDiscontinuity(
    ls_session_t *session,
    std::int64_t boundaryTimeNs)
{
    const std::string reason{"scripted system-audio discontinuity"};
    ls_source_event_v1 event{};
    event.struct_size = sizeof(event);
    event.abi_version = LS_CORE_ABI_VERSION;
    event.source_id = 2;
    event.source_kind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    event.event_kind = LS_SOURCE_EVENT_DISCONTINUITY;
    event.health = LS_SOURCE_HEALTH_ACTIVE;
    event.flags = LS_SOURCE_EVENT_FLAG_TEST_INJECTED;
    event.start_time_ns = boundaryTimeNs;
    event.end_time_ns = boundaryTimeNs;
    event.reason = view(reason);
    LS_CHECK_EQ(ls_session_source_event_v1(session, &event), LS_OK);
}

struct FinalEvent {
    std::string text;
    std::string speakerLabel;
    std::uint64_t speakerId{};
    std::int64_t startTimeNs{};
};

std::vector<FinalEvent> collectFinalEvents(
    ls_session_t *session,
    std::size_t expected,
    int maximumAttempts = 50)
{
    std::vector<FinalEvent> result;
    for (int attempt = 0;
         attempt < maximumAttempts && result.size() < expected;
         ++attempt) {
        ls_event_t *event = nullptr;
        const auto status = ls_session_next_event_v1(session, 50, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        LS_CHECK_EQ(status, LS_OK);
        if (ls_event_kind(event) == LS_EVENT_FINAL_SEGMENT) {
            ls_transcript_segment_copy_v1 copy{};
            copy.struct_size = sizeof(copy);
            copy.abi_version = LS_CORE_ABI_VERSION;
            LS_CHECK_EQ(ls_event_copy_segment_v1(event, &copy), LS_OK);
            result.push_back(FinalEvent{
                std::string{
                    reinterpret_cast<const char *>(copy.text.data),
                    copy.text.size},
                std::string{
                    reinterpret_cast<const char *>(copy.speaker_label.data),
                    copy.speaker_label.size},
                copy.speaker_id,
                copy.start_time_ns});
        }
        ls_event_destroy(event);
    }
    return result;
}

ls_pipeline_metrics_v1 metrics(ls_session_t *session)
{
    ls_pipeline_metrics_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(ls_session_copy_metrics_v1(session, &result), LS_OK);
    return result;
}

void waitForCheckpointAfter(ls_session_t *session, std::uint64_t checkpoint)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (metrics(session).journal_checkpoint > checkpoint) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LS_CHECK(false);
}

std::string renderRecording(SpeakerFixtureSession &handles)
{
    const std::string empty;
    const std::string title{"Speaker switch"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    options.system_audio_captured = 1;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    ls_owned_bytes_destroy(handles.markdown);
    handles.markdown = nullptr;
    LS_CHECK_EQ(
        ls_session_render_markdown_v1(
            handles.session,
            &options,
            &handles.markdown,
            &error),
        LS_OK);
    return std::string{
        reinterpret_cast<const char *>(
            ls_owned_bytes_data(handles.markdown)),
        ls_owned_bytes_size(handles.markdown)};
}

std::uint64_t renderCheckpoint(SpeakerFixtureSession &handles)
{
    const std::string empty;
    const std::string title{"Checkpoint race"};
    ls_markdown_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_version = LS_CORE_ABI_VERSION;
    options.title = view(title);
    options.created_at_iso8601 = view(empty);
    options.ended_at_iso8601 = view(empty);
    options.duration_seconds = -1;
    options.system_audio_captured = 1;
    ls_render_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.abi_version = LS_CORE_ABI_VERSION;
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    ls_owned_bytes_destroy(handles.markdown);
    handles.markdown = nullptr;
    LS_CHECK_EQ(
        ls_session_render_markdown_with_snapshot_v1(
            handles.session,
            &options,
            &handles.markdown,
            &snapshot,
            &error),
        LS_OK);
    return snapshot.journal_checkpoint;
}

} // namespace

LS_TEST(c_abi_hides_suspected_switch_then_publishes_the_whole_group)
{
    auto handles = createSpeakerFixtureSession("speaker-switch-confirmed");

    pushSpeakerCue(handles.session, 101, 1'000'000'000, 1.0F, 0.0F);
    const auto initial = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(initial.size(), std::size_t{1});
    const auto originalSpeakerId = initial.front().speakerId;

    const auto checkpointBeforeSuspicion =
        metrics(handles.session).journal_checkpoint;
    pushSpeakerCue(handles.session, 102, 2'000'000'000, 0.0F, 1.0F);
    waitForCheckpointAfter(handles.session, checkpointBeforeSuspicion);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());
    const auto hiddenMarkdown = renderRecording(handles);
    LS_CHECK(hiddenMarkdown.find("fixture cue 101") != std::string::npos);
    LS_CHECK(hiddenMarkdown.find("fixture cue 102") == std::string::npos);
    LS_CHECK_EQ(
        metrics(handles.session).final_segments_committed,
        std::uint64_t{1});

    pushSpeakerCue(handles.session, 103, 3'000'000'000, 0.0F, 1.0F);
    const auto confirmed = collectFinalEvents(handles.session, 2);
    LS_CHECK_EQ(confirmed.size(), std::size_t{2});
    LS_CHECK_EQ(confirmed[0].text, std::string{"fixture cue 102"});
    LS_CHECK_EQ(confirmed[1].text, std::string{"fixture cue 103"});
    LS_CHECK(confirmed[0].startTimeNs < confirmed[1].startTimeNs);
    LS_CHECK_EQ(confirmed[0].speakerId, confirmed[1].speakerId);
    LS_CHECK(confirmed[0].speakerId != originalSpeakerId);
    LS_CHECK_EQ(
        metrics(handles.session).final_segments_committed,
        std::uint64_t{3});

    const auto confirmedMarkdown = renderRecording(handles);
    const auto firstPosition = confirmedMarkdown.find("fixture cue 102");
    const auto secondPosition = confirmedMarkdown.find("fixture cue 103");
    LS_CHECK(firstPosition != std::string::npos);
    LS_CHECK(secondPosition != std::string::npos);
    LS_CHECK(firstPosition < secondPosition);
}

LS_TEST(c_abi_pause_resolves_a_suspected_switch_to_the_previous_speaker)
{
    auto handles = createSpeakerFixtureSession("speaker-switch-pause");
    pushSpeakerCue(handles.session, 201, 1'000'000'000, 1.0F, 0.0F);
    const auto initial = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(initial.size(), std::size_t{1});

    const auto checkpoint = metrics(handles.session).journal_checkpoint;
    pushSpeakerCue(handles.session, 202, 2'000'000'000, 0.0F, 1.0F);
    waitForCheckpointAfter(handles.session, checkpoint);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());

    LS_CHECK_EQ(ls_session_pause_v1(handles.session), LS_OK);
    const auto fallback = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(fallback.size(), std::size_t{1});
    LS_CHECK_EQ(fallback.front().text, std::string{"fixture cue 202"});
    LS_CHECK_EQ(fallback.front().speakerId, initial.front().speakerId);
}

LS_TEST(c_abi_source_discontinuity_resolves_pending_before_following_audio)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-source-discontinuity");
    pushSpeakerCue(handles.session, 191, 1'000'000'000, 1.0F, 0.0F);
    const auto initial = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(initial.size(), std::size_t{1});

    const auto checkpoint = metrics(handles.session).journal_checkpoint;
    pushSpeakerCue(handles.session, 192, 2'000'000'000, 0.0F, 1.0F);
    waitForCheckpointAfter(handles.session, checkpoint);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());

    reportSystemAudioDiscontinuity(handles.session, 2'500'000'000);
    const auto fallback = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(fallback.size(), std::size_t{1});
    LS_CHECK_EQ(fallback.front().text, std::string{"fixture cue 192"});
    LS_CHECK_EQ(fallback.front().speakerId, initial.front().speakerId);

    /* Evidence after the boundary must begin a fresh pending group. */
    pushSpeakerCue(handles.session, 193, 3'000'000'000, 0.0F, 1.0F);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());
}

LS_TEST(c_abi_pause_publishes_flushed_asr_fallback_before_paused_state)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-buffered-pause",
        "fixture-speakers-buffered");
    pushSpeakerCue(handles.session, 211, 1'000'000'000, 1.0F, 0.0F);
    const auto initial = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(initial.size(), std::size_t{1});

    pushSpeakerCue(handles.session, 212, 2'000'000'000, 0.0F, 1.0F);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());

    LS_CHECK_EQ(ls_session_pause_v1(handles.session), LS_OK);

    bool sawFallback = false;
    bool sawPaused = false;
    for (int attempt = 0; attempt < 50 && !sawPaused; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(handles.session, 50, &event);
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
            LS_CHECK(!sawFallback);
            const std::string text{
                reinterpret_cast<const char *>(segment.text.data),
                segment.text.size};
            LS_CHECK_EQ(
                text,
                std::string{"fixture cue 212"});
            LS_CHECK_EQ(segment.speaker_id, initial.front().speakerId);
            sawFallback = true;
        } else if (ls_event_kind(event) == LS_EVENT_STATE_CHANGED) {
            ls_state_event_copy_v1 state{};
            state.struct_size = sizeof(state);
            state.abi_version = LS_CORE_ABI_VERSION;
            LS_CHECK_EQ(ls_event_copy_state_v1(event, &state), LS_OK);
            if (state.phase == LS_PHASE_PAUSED) {
                LS_CHECK(sawFallback);
                sawPaused = true;
            }
        }
        ls_event_destroy(event);
    }
    LS_CHECK(sawFallback);
    LS_CHECK(sawPaused);
    LS_CHECK_EQ(
        metrics(handles.session).final_segments_committed,
        std::uint64_t{2});
}

LS_TEST(c_abi_rejects_accept_batch_for_a_different_source)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-wrong-source",
        "fixture-wrong-source");
    pushSpeakerCue(handles.session, 221, 1'000'000'000, 1.0F, 0.0F);

    LS_CHECK(waitForBackendFailureWithoutFinal(handles.session));
    LS_CHECK_EQ(ls_session_pause_v1(handles.session), LS_BACKEND_FAILURE);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());
    LS_CHECK_EQ(
        metrics(handles.session).final_segments_committed,
        std::uint64_t{0});
}

LS_TEST(c_abi_rejects_hypothesis_outside_its_processed_timeline_batch)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-out-of-range-timestamp",
        "fixture-out-of-range-timestamp");
    pushSpeakerCue(handles.session, 231, 1'000'000'000, 1.0F, 0.0F);

    LS_CHECK(waitForBackendFailureWithoutFinal(handles.session));
    LS_CHECK_EQ(ls_session_pause_v1(handles.session), LS_BACKEND_FAILURE);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());
    LS_CHECK_EQ(
        metrics(handles.session).final_segments_committed,
        std::uint64_t{0});
}

LS_TEST(c_abi_metrics_checkpoint_never_regresses_behind_durable_progress)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-checkpoint-race",
        "fixture-speakers",
        "source-aware");
    constexpr std::uint64_t operationCount = 32;
    std::atomic<bool> sourceEventsDone{false};
    std::atomic<bool> sourceEventFailed{false};

    std::thread sourceEvents([&] {
        const std::string reason{"checkpoint race activity"};
        for (std::uint64_t index = 0; index < operationCount; ++index) {
            ls_source_event_v1 event{};
            event.struct_size = sizeof(event);
            event.abi_version = LS_CORE_ABI_VERSION;
            event.source_id = 2;
            event.source_kind = LS_SOURCE_KIND_SYSTEM_AUDIO;
            event.event_kind = LS_SOURCE_EVENT_ACTIVE;
            event.health = LS_SOURCE_HEALTH_ACTIVE;
            event.flags = LS_SOURCE_EVENT_FLAG_TEST_INJECTED;
            event.start_time_ns = static_cast<std::int64_t>(
                10'000'000'000 + index);
            event.end_time_ns = event.start_time_ns;
            event.reason = view(reason);
            if (ls_session_source_event_v1(handles.session, &event) != LS_OK) {
                sourceEventFailed.store(true, std::memory_order_relaxed);
                break;
            }
        }
        sourceEventsDone.store(true, std::memory_order_release);
    });

    for (std::uint64_t index = 0; index < operationCount; ++index) {
        pushSpeakerCue(
            handles.session,
            600 + index,
            static_cast<std::int64_t>(
                1'000'000'000 + index * 20'000'000),
            1.0F,
            0.0F);
    }

    std::uint64_t observedCheckpoint = 0;
    bool checkpointRegressed = false;
    while (!sourceEventsDone.load(std::memory_order_acquire)) {
        const auto current = metrics(handles.session).journal_checkpoint;
        checkpointRegressed = checkpointRegressed
            || current < observedCheckpoint;
        observedCheckpoint = std::max(observedCheckpoint, current);
        std::this_thread::yield();
    }
    sourceEvents.join();
    LS_CHECK(!sourceEventFailed.load(std::memory_order_relaxed));
    LS_CHECK(!checkpointRegressed);

    const auto finals = collectFinalEvents(
        handles.session,
        operationCount,
        500);
    LS_CHECK_EQ(finals.size(), std::size_t{operationCount});
    const auto durableCheckpoint = renderCheckpoint(handles);
    LS_CHECK_EQ(
        metrics(handles.session).journal_checkpoint,
        durableCheckpoint);
}

LS_TEST(c_abi_finalize_resolves_a_suspected_switch_to_the_previous_speaker)
{
    auto handles = createSpeakerFixtureSession("speaker-switch-finalize");
    pushSpeakerCue(handles.session, 301, 1'000'000'000, 1.0F, 0.0F);
    const auto initial = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(initial.size(), std::size_t{1});

    const auto checkpoint = metrics(handles.session).journal_checkpoint;
    pushSpeakerCue(handles.session, 302, 2'000'000'000, 0.0F, 1.0F);
    waitForCheckpointAfter(handles.session, checkpoint);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());

    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    const auto fallback = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(fallback.size(), std::size_t{1});
    LS_CHECK_EQ(fallback.front().text, std::string{"fixture cue 302"});
    LS_CHECK_EQ(fallback.front().speakerId, initial.front().speakerId);
}

LS_TEST(c_abi_rejected_resolution_cannot_override_the_stored_fallback)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-malicious-fallback",
        "fixture-speakers",
        "test-malicious-fallback-resolution");

    const auto checkpoint = metrics(handles.session).journal_checkpoint;
    pushSpeakerCue(handles.session, 501, 1'000'000'000, 1.0F, 0.0F);
    waitForCheckpointAfter(handles.session, checkpoint);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());

    LS_CHECK_EQ(ls_session_pause_v1(handles.session), LS_OK);
    const auto fallback = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(fallback.size(), std::size_t{1});
    LS_CHECK_EQ(fallback.front().text, std::string{"fixture cue 501"});
    LS_CHECK_EQ(
        fallback.front().speakerLabel,
        std::string{"Stored fallback"});
    LS_CHECK(fallback.front().speakerId != std::uint64_t{1});

    const auto markdown = renderRecording(handles);
    LS_CHECK(markdown.find("Stored fallback") != std::string::npos);
    LS_CHECK(markdown.find("**Me:** fixture cue 501") == std::string::npos);
}

LS_TEST(c_abi_confirmed_system_audio_cannot_become_the_local_speaker)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-malicious-confirmed",
        "fixture-speakers",
        "test-malicious-confirmed-local-ownership");

    const auto checkpoint = metrics(handles.session).journal_checkpoint;
    pushSpeakerCue(handles.session, 511, 1'000'000'000, 1.0F, 0.0F);
    waitForCheckpointAfter(handles.session, checkpoint);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());

    LS_CHECK_EQ(ls_session_pause_v1(handles.session), LS_OK);
    const auto confirmed = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(confirmed.size(), std::size_t{1});
    LS_CHECK_EQ(confirmed.front().text, std::string{"fixture cue 511"});
    LS_CHECK_EQ(confirmed.front().speakerLabel, std::string{"Speaker 1"});
    LS_CHECK(confirmed.front().speakerId != std::uint64_t{1});

    const auto markdown = renderRecording(handles);
    LS_CHECK(markdown.find("Speaker 1") != std::string::npos);
    LS_CHECK(markdown.find("fixture cue 511") != std::string::npos);
    LS_CHECK(markdown.find("**Me:** fixture cue 511") == std::string::npos);
}

LS_TEST(c_abi_finalize_discards_a_diarization_result_that_finishes_late)
{
    auto handles = createSpeakerFixtureSession(
        "speaker-switch-blocking-diarization",
        "fixture-speakers",
        "test-blocking-diarization-assign");
    pushSpeakerCue(handles.session, 521, 1'000'000'000, 1.0F, 0.0F);

    bool workerClaimedFrame = false;
    for (int attempt = 0; attempt < 100 && !workerClaimedFrame; ++attempt) {
        workerClaimedFrame = metrics(handles.session).audio_queue_depth == 0;
        if (!workerClaimedFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    LS_CHECK(workerClaimedFrame);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto finalizeStarted = std::chrono::steady_clock::now();
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK);
    const auto finalizeElapsed =
        std::chrono::steady_clock::now() - finalizeStarted;
    LS_CHECK(finalizeElapsed < std::chrono::milliseconds(6'500));

    ls_state_event_copy_v1 state{};
    state.struct_size = sizeof(state);
    state.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_session_copy_state_v1(handles.session, &state),
        LS_OK);
    LS_CHECK_EQ(state.phase, LS_PHASE_INTERRUPTED);
    LS_CHECK_EQ(
        state.finalize_reason,
        LS_FINALIZE_REASON_PROCESS_INTERRUPTED);

    bool sawTerminal = false;
    for (int attempt = 0; attempt < 100 && !sawTerminal; ++attempt) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(handles.session, 50, &event);
        if (status == LS_TIMEOUT) {
            continue;
        }
        LS_CHECK_EQ(status, LS_OK);
        LS_CHECK(ls_event_kind(event) != LS_EVENT_FINAL_SEGMENT);
        sawTerminal = ls_event_kind(event) == LS_EVENT_TERMINAL;
        ls_event_destroy(event);
    }
    LS_CHECK(sawTerminal);
    const auto checkpointAtTerminal =
        metrics(handles.session).journal_checkpoint;

    /* Let the blocked backend return after the durable terminal boundary. */
    std::this_thread::sleep_for(std::chrono::seconds(3));
    LS_CHECK(collectFinalEvents(handles.session, 1, 6).empty());
    LS_CHECK_EQ(
        metrics(handles.session).final_segments_committed,
        std::uint64_t{0});
    LS_CHECK_EQ(
        metrics(handles.session).journal_checkpoint,
        checkpointAtTerminal);
    LS_CHECK(
        renderRecording(handles).find("fixture cue 521")
        == std::string::npos);
}

LS_TEST(c_abi_recovery_keeps_pending_text_under_the_fallback_speaker)
{
    auto handles = createSpeakerFixtureSession("speaker-switch-recovery");
    pushSpeakerCue(handles.session, 401, 1'000'000'000, 1.0F, 0.0F);
    const auto initial = collectFinalEvents(handles.session, 1);
    LS_CHECK_EQ(initial.size(), std::size_t{1});

    const auto checkpoint = metrics(handles.session).journal_checkpoint;
    pushSpeakerCue(handles.session, 402, 2'000'000'000, 0.0F, 1.0F);
    waitForCheckpointAfter(handles.session, checkpoint);
    LS_CHECK(collectFinalEvents(handles.session, 1, 4).empty());

    ls_session_destroy(handles.session);
    handles.session = nullptr;
    ls_core_destroy(handles.core);
    handles.core = nullptr;

    const std::string journalPath = handles.path.string();
    ls_core_config_v1 configuration{};
    configuration.struct_size = sizeof(configuration);
    configuration.abi_version = LS_CORE_ABI_VERSION;
    configuration.flags = LS_CORE_CONFIG_ALLOW_TEST_BACKENDS;
    configuration.journal_path = view(journalPath);
    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    LS_CHECK_EQ(
        ls_core_create_v1(&configuration, &handles.core, &error),
        LS_OK);
    LS_CHECK_EQ(
        ls_core_open_recoverable_session_v1(
            handles.core,
            view(handles.sessionId),
            &handles.session),
        LS_OK);
    LS_CHECK_EQ(
        ls_session_finalize_v1(
            handles.session,
            LS_FINALIZE_REASON_RECOVERY),
        LS_OK);

    const auto markdown = renderRecording(handles);
    LS_CHECK(markdown.find("fixture cue 401") != std::string::npos);
    LS_CHECK(markdown.find("fixture cue 402") != std::string::npos);
    LS_CHECK(markdown.find("Speaker 2") == std::string::npos);
}
