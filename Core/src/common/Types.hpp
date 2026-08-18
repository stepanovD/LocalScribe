#pragma once

#include <LocalScribeCore/LocalScribeCore.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace localscribe {

using StableId = std::array<std::uint8_t, 16>;

/*
 * Speaker ID 1 is permanently reserved for the microphone owner. Anonymous
 * per-session remote speakers keep the high bit used by Stage 0. Persisted
 * voice profiles use the next bit, leaving their SQLite identifier intact in
 * the low 62 bits and making the two namespaces impossible to confuse.
 */
inline constexpr std::uint64_t kPersistentSpeakerFlag = 1ULL << 62u;
inline constexpr std::uint64_t kAnonymousSpeakerFlag = 1ULL << 63u;
inline constexpr std::uint64_t kSpeakerIdPayloadMask =
    kPersistentSpeakerFlag - 1u;
inline constexpr std::string_view kSpeakerFeatureModelId =
    "spectral-envelope-v1";

[[nodiscard]] inline bool
isPersistentSpeakerId(std::uint64_t speakerId) noexcept
{
    return (speakerId & (kAnonymousSpeakerFlag | kPersistentSpeakerFlag))
        == kPersistentSpeakerFlag;
}

[[nodiscard]] inline std::uint64_t
persistentSpeakerId(std::uint64_t profileId) noexcept
{
    return kPersistentSpeakerFlag | (profileId & kSpeakerIdPayloadMask);
}

[[nodiscard]] inline std::uint64_t
profileIdFromSpeakerId(std::uint64_t speakerId) noexcept
{
    return speakerId & kSpeakerIdPayloadMask;
}

struct BackendInfo {
    std::string id;
    std::string version;
    bool testOnly{false};
};

struct AsrConfiguration {
    std::string modelPath;
    ls_language_mode_t languageMode{LS_LANGUAGE_MODE_UNKNOWN};
};

struct AudioWindow {
    std::uint64_t sourceId{};
    ls_source_kind_t sourceKind{LS_SOURCE_KIND_UNKNOWN};
    std::uint64_t sequenceNumber{};
    std::int64_t monotonicTimeNs{};
    std::uint32_t sampleRateHz{};
    std::uint16_t channelCount{};
    std::uint32_t frameCount{};
    std::uint32_t flags{};
    std::vector<float> samples;
    std::uint64_t callbackCount{1};
    std::uint64_t rejectedCallbacksBefore{};
    std::int64_t overloadGapStartTimeNs{};
    std::int64_t overloadGapEndTimeNs{};
    bool overloadGapBefore{};
    /* Ordered runtime control item; it carries no captured audio callback. */
    bool inferenceBoundaryOnly{};
};

struct AsrHypothesis {
    StableId stableId{};
    std::uint64_t sourceId{};
    std::int64_t startTimeNs{};
    std::int64_t endTimeNs{};
    std::string text;
    std::string language;
    float confidence{};
    std::uint32_t revision{};
    bool final{};
    bool unintelligible{};
    bool speakerTurnAfter{};
    std::string speakerEmbeddingModel;
    std::vector<float> speakerEmbedding;
};

/*
 * A batch is emitted only after ASR has fully processed the represented audio
 * interval.  Empty batches are meaningful: their watermark lets downstream
 * components advance deterministic, audio-time deadlines through silence.
 */
struct AsrTimelineBatch {
    std::uint64_t sourceId{};
    std::int64_t processedStartTimeNs{};
    std::int64_t finalizedThroughTimeNs{};
    bool discontinuityBefore{};
    std::vector<AsrHypothesis> hypotheses;
};

struct SpeakerTurn {
    StableId stableId{};
    std::uint64_t sourceId{};
    std::int64_t startTimeNs{};
    std::int64_t endTimeNs{};
    std::uint64_t speakerId{};
    std::string speakerLabel;
    float confidence{};
    std::uint32_t revision{};
};

enum class SpeakerTurnDecisionKind {
    commit,
    hold,
};

struct SpeakerTurnDecision {
    SpeakerTurnDecisionKind kind{SpeakerTurnDecisionKind::commit};
    SpeakerTurn turn;
    std::uint64_t pendingGroupId{};
    std::int64_t deadlineTimeNs{};
};

enum class PendingSpeakerResolutionReason {
    confirmed,
    contradicted,
    timeout,
    discontinuity,
    pause,
    endOfStream,
    capacity,
};

struct PendingSpeakerResolution {
    std::uint64_t pendingGroupId{};
    PendingSpeakerResolutionReason reason{
        PendingSpeakerResolutionReason::contradicted};
    std::vector<SpeakerTurn> turns;
};

struct DiarizationUpdate {
    /* Exactly one decision for every input hypothesis, in input order. */
    std::vector<SpeakerTurnDecision> decisions;
    /* A resolution includes every revision staged in the referenced group. */
    std::vector<PendingSpeakerResolution> resolutions;
};

enum class DiarizationFlushReason {
    pause,
    endOfStream,
};

struct TranscriptSegment {
    StableId stableId{};
    std::uint64_t sourceId{};
    std::int64_t startTimeNs{};
    std::int64_t endTimeNs{};
    std::uint64_t speakerId{};
    std::string speakerLabel;
    std::string text;
    std::string language;
    float confidence{};
    std::uint32_t revision{};
    std::uint32_t flags{};
    std::uint64_t journalCheckpoint{};
    std::string speakerEmbeddingModel;
    std::vector<float> speakerEmbedding;
};

struct VoiceProfile {
    std::uint64_t profileId{};
    std::string displayName;
    std::string embeddingModelId;
    std::vector<float> centroid;
    std::vector<std::vector<float>> prototypes;
    std::uint64_t observationCount{};
    std::int64_t createdAtUnixNs{};
    std::int64_t updatedAtUnixNs{};
};

struct VoiceProfileEnrollment {
    VoiceProfile profile;
    std::uint64_t speakerId{};
    std::uint32_t relabeledSegments{};
    std::uint64_t journalCheckpoint{};
    std::uint32_t highestSegmentRevision{};
};

struct SourceRecord {
    std::uint64_t sourceId{};
    ls_source_kind_t sourceKind{LS_SOURCE_KIND_UNKNOWN};
    bool required{};
    ls_source_health_t health{LS_SOURCE_HEALTH_UNKNOWN};
    std::uint64_t acceptedFrames{};
    std::uint64_t rejectedFrames{};
    std::uint64_t discontinuities{};
};

struct SourceGap {
    std::uint64_t sourceId{};
    ls_source_kind_t sourceKind{LS_SOURCE_KIND_UNKNOWN};
    ls_source_event_kind_t eventKind{LS_SOURCE_EVENT_UNKNOWN};
    ls_source_health_t health{LS_SOURCE_HEALTH_UNKNOWN};
    std::int64_t startTimeNs{};
    std::int64_t endTimeNs{};
    std::string reason;
    bool testInjected{};
};

struct SessionRecord {
    std::string sessionId;
    ls_phase_t phase{LS_PHASE_UNKNOWN};
    std::string createdAt;
    std::string endedAt;
    std::string sourceApp;
    std::string localSpeakerName;
    std::string asrBackendId;
    std::string asrBackendVersion;
    std::string diarizationBackendId;
    std::string diarizationBackendVersion;
    ls_language_mode_t languageMode{LS_LANGUAGE_MODE_UNKNOWN};
    std::uint64_t microphoneSourceId{};
    std::uint64_t systemAudioSourceId{};
    std::uint32_t requiredSourceMask{};
    std::int64_t completenessThresholdNs{};
    std::int64_t timelineOriginNs{};
    std::uint64_t journalCheckpoint{};
    std::uint32_t highestSegmentRevision{};
    ls_finalize_reason_t finalizeReason{LS_FINALIZE_REASON_UNKNOWN};
};

struct JournalSnapshot {
    SessionRecord session;
    std::vector<SourceRecord> sources;
    std::vector<SourceGap> gaps;
    std::vector<TranscriptSegment> segments;
};

struct RenderSnapshot {
    std::uint64_t journalCheckpoint{};
    std::uint32_t highestSegmentRevision{};
};

struct RenderedMarkdown {
    std::string bytes;
    RenderSnapshot snapshot;
};

struct PublicationReceipt {
    std::uint64_t journalCheckpoint{};
    std::uint32_t highestSegmentRevision{};
    ls_publication_destination_t destination{
        LS_PUBLICATION_DESTINATION_UNKNOWN};
    std::int64_t publishedAtUnixNs{};
    std::string sha256Hex;
    std::string fileIdentity;
};

struct PipelineMetrics {
    std::uint64_t framesOffered{};
    std::uint64_t framesAccepted{};
    std::uint64_t framesRejected{};
    std::uint64_t discontinuities{};
    std::uint64_t finalSegmentsCommitted{};
    std::uint64_t partialEventsCoalesced{};
    std::uint32_t audioQueueDepth{};
    std::uint32_t audioQueueHighWater{};
    std::uint64_t journalCheckpoint{};
    std::uint32_t highestSegmentRevision{};
};

} // namespace localscribe
