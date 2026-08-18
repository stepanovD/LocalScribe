#pragma once

#include "../common/Expected.hpp"
#include "../common/Types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace localscribe {

struct PendingSpeakerGroupStage {
    std::uint64_t groupId{};
    std::int64_t deadlineMonotonicNs{};
    std::vector<TranscriptSegment> fallbackSegments;
};

struct PendingSpeakerGroupResolution {
    std::uint64_t groupId{};
    /*
     * Empty means resolve every held segment with its stored fallback.
     * SpeakerTurn confidence is decision evidence; promotion keeps the
     * durable Final Segment confidence staged with the transcript payload.
     */
    std::vector<SpeakerTurn> attributions;
};

struct DiarizationJournalBatch {
    std::vector<PendingSpeakerGroupStage> holds;
    std::vector<PendingSpeakerGroupResolution> resolutions;
    std::vector<TranscriptSegment> commits;
};

struct DiarizationJournalBatchResult {
    std::uint64_t journalCheckpoint{};
    std::uint32_t highestSegmentRevision{};
    std::vector<TranscriptSegment> visibleSegments;
    bool wasChanged{};
};

class RecoveryJournal {
public:
    RecoveryJournal(const RecoveryJournal &) = delete;
    RecoveryJournal &operator=(const RecoveryJournal &) = delete;
    ~RecoveryJournal();

    [[nodiscard]] static Expected<std::shared_ptr<RecoveryJournal>>
    open(const std::string &path);

    [[nodiscard]] Expected<void> createSession(
        const SessionRecord &session,
        std::span<const SourceRecord> sources);

    [[nodiscard]] Expected<SessionRecord>
    loadSession(const std::string &sessionId);

    [[nodiscard]] Expected<std::uint64_t> transition(
        const std::string &sessionId,
        ls_phase_t expected,
        ls_phase_t next,
        ls_finalize_reason_t reason = LS_FINALIZE_REASON_UNKNOWN);

    [[nodiscard]] Expected<std::uint64_t>
    appendFinalSegment(
        const std::string &sessionId,
        TranscriptSegment &segment);

    [[nodiscard]] Expected<std::uint64_t>
    appendFinalSegment(
        const std::string &sessionId,
        const TranscriptSegment &segment);

    [[nodiscard]] Expected<DiarizationJournalBatchResult>
    applyDiarizationBatch(
        const std::string &sessionId,
        const DiarizationJournalBatch &batch);

    [[nodiscard]] Expected<DiarizationJournalBatchResult>
    stagePendingSegments(
        const std::string &sessionId,
        std::uint64_t groupId,
        std::int64_t deadlineMonotonicNs,
        std::span<const TranscriptSegment> fallbackSegments);

    [[nodiscard]] Expected<DiarizationJournalBatchResult>
    resolvePendingSpeakerGroup(
        const std::string &sessionId,
        std::uint64_t groupId,
        std::span<const SpeakerTurn> attributions = {});

    [[nodiscard]] Expected<DiarizationJournalBatchResult>
    resolveAllPendingSpeakerGroupsToFallback(
        const std::string &sessionId);

    [[nodiscard]] Expected<std::uint64_t>
    recordSourceEvent(const std::string &sessionId, const SourceGap &event);

    [[nodiscard]] Expected<void>
    recordFrameAccepted(const std::string &sessionId, std::uint64_t sourceId);

    [[nodiscard]] Expected<void> recordFramesAccepted(
        const std::string &sessionId,
        std::uint64_t sourceId,
        std::uint64_t count);

    [[nodiscard]] Expected<void> recordFrameRejected(
        const std::string &sessionId,
        std::uint64_t sourceId,
        bool discontinuity);

    [[nodiscard]] Expected<void> recordFramesRejected(
        const std::string &sessionId,
        std::uint64_t sourceId,
        std::uint64_t count,
        bool discontinuity);

    [[nodiscard]] Expected<JournalSnapshot>
    snapshot(const std::string &sessionId);

    [[nodiscard]] Expected<std::vector<VoiceProfile>> listVoiceProfiles();

    [[nodiscard]] Expected<VoiceProfileEnrollment> enrollVoiceProfile(
        const std::string &sessionId,
        std::uint64_t speakerId,
        const std::string &displayName);

    [[nodiscard]] Expected<void> renameVoiceProfile(
        std::uint64_t profileId,
        const std::string &displayName);

    [[nodiscard]] Expected<void> deleteVoiceProfile(
        std::uint64_t profileId);

    [[nodiscard]] Expected<void>
    acknowledgePublication(
        const std::string &sessionId,
        const PublicationReceipt &receipt);

    [[nodiscard]] Expected<std::vector<std::string>>
    markAndListRecoverableSessions();

    [[nodiscard]] Expected<std::vector<std::string>>
    listRecoverableSessions();

    [[nodiscard]] Expected<void> quickCheck();

    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    RecoveryJournal(sqlite3 *database, std::string path);

    [[nodiscard]] Expected<SessionRecord>
    loadSessionLocked(const std::string &sessionId);

    [[nodiscard]] Expected<VoiceProfile>
    loadVoiceProfileLocked(std::uint64_t profileId);

    [[nodiscard]] Expected<DiarizationJournalBatchResult>
    applyDiarizationBatchImpl(
        const std::string &sessionId,
        const DiarizationJournalBatch &batch,
        bool resolveAllPendingToFallback);

    sqlite3 *database_{};
    std::string path_;
    mutable std::mutex mutex_;
};

} // namespace localscribe
