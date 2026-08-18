#pragma once

#include "DiarizationBackend.hpp"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace localscribe {

class SourceDiarizationBackend final : public IDiarizationBackend {
public:
    using IDiarizationBackend::assign;
    using IDiarizationBackend::flush;

    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] Expected<void>
    prepare(const DiarizationConfiguration &configuration) override;
    [[nodiscard]] Expected<DiarizationUpdate>
    assign(const AsrTimelineBatch &batch) override;
    [[nodiscard]] Expected<DiarizationUpdate>
    flush(DiarizationFlushReason reason) override;

private:
    DiarizationConfiguration configuration_;
    bool prepared_{false};
};

class AcousticDiarizationBackend final : public IDiarizationBackend {
public:
    using IDiarizationBackend::assign;
    using IDiarizationBackend::flush;

    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] Expected<void>
    prepare(const DiarizationConfiguration &configuration) override;
    [[nodiscard]] Expected<DiarizationUpdate>
    assign(const AsrTimelineBatch &batch) override;
    [[nodiscard]] Expected<DiarizationUpdate>
    flush(DiarizationFlushReason reason) override;

private:
    struct SpeakerCluster {
        std::uint64_t speakerId{};
        std::string label;
        std::string embeddingModelId;
        std::vector<float> centroid;
        std::vector<std::vector<float>> prototypes;
        std::size_t observations{};
        bool persistedProfile{};
        bool retired{};
        std::optional<StableId> soleEvidenceStableId;
        bool hasMultipleEvidenceIds{};
    };

    struct SpeakerAssignment {
        std::size_t cluster{};
        std::uint32_t revision{};
        bool final{};
        bool hadUsableEmbedding{};
    };

    struct PendingRevision {
        AsrHypothesis hypothesis;
        SpeakerTurn fallback;
    };

    struct PendingSpeakerSwitch {
        std::uint64_t groupId{};
        std::size_t fallbackCluster{};
        std::optional<std::size_t> targetCluster;
        std::string embeddingModelId;
        std::int64_t deadlineTimeNs{};
        std::int64_t lastEndTimeNs{};
        bool continuousFromFallback{};
        std::vector<PendingRevision> revisions;
        std::map<StableId, AsrHypothesis> evidenceByStableId;
    };

    struct CandidateSelection {
        std::optional<std::size_t> targetCluster;
        float similarity{-1.0F};
        float margin{-1.0F};
        bool activeSpeaker{};
        bool strong{};
    };

    [[nodiscard]] std::size_t createCluster(
        std::span<const float> embedding,
        std::string_view embeddingModelId);
    [[nodiscard]] CandidateSelection selectCandidate(
        const AsrHypothesis &hypothesis,
        bool turnHintBefore,
        std::optional<std::size_t> reconsideredAssignment) const;
    [[nodiscard]] float clusterSimilarity(
        const SpeakerCluster &cluster,
        std::span<const float> embedding) const;
    void updateCluster(
        SpeakerCluster &cluster,
        std::span<const float> embedding,
        float minimumSimilarity,
        const StableId *evidenceStableId);
    [[nodiscard]] bool profileIsCompatible(
        const SpeakerCluster &cluster,
        const AsrHypothesis &hypothesis) const;
    [[nodiscard]] bool clusterIsCompatible(
        const SpeakerCluster &cluster,
        const AsrHypothesis &hypothesis) const;
    [[nodiscard]] std::size_t anonymousClusterCount() const;
    [[nodiscard]] SpeakerTurn makeTurn(
        const AsrHypothesis &hypothesis,
        std::size_t clusterIndex) const;
    void recordAssignment(
        const AsrHypothesis &hypothesis,
        std::size_t clusterIndex);
    void commitDirect(
        const AsrHypothesis &hypothesis,
        std::size_t clusterIndex,
        DiarizationUpdate &update,
        bool addEvidence);
    void beginPending(
        const AsrHypothesis &hypothesis,
        std::optional<std::size_t> targetCluster,
        DiarizationUpdate &update);
    [[nodiscard]] bool pendingAccepts(
        const AsrHypothesis &hypothesis,
        std::optional<std::size_t> targetCluster) const;
    void addPendingRevision(
        const AsrHypothesis &hypothesis,
        DiarizationUpdate &update);
    [[nodiscard]] bool pendingIsConfirmed() const;
    void confirmPending(DiarizationUpdate &update);
    void resolvePendingFallback(
        PendingSpeakerResolutionReason reason,
        DiarizationUpdate &update);
    void processRemoteHypothesis(
        const AsrHypothesis &hypothesis,
        DiarizationUpdate &update);
    [[nodiscard]] std::vector<float> pendingCentroid() const;
    [[nodiscard]] std::int64_t pendingUniqueSpeechDurationNs() const;

    DiarizationConfiguration configuration_;
    std::vector<SpeakerCluster> clusters_;
    std::map<StableId, SpeakerAssignment> assignments_;
    std::optional<std::size_t> previousCluster_;
    std::optional<PendingSpeakerSwitch> pendingSwitch_;
    std::map<std::uint64_t, std::int64_t> finalizedWatermarks_;
    std::int64_t previousEndTimeNs_{};
    std::size_t nextAnonymousOrdinal_{1};
    std::uint64_t nextPendingGroupId_{1};
    bool forceDifferentSpeaker_{false};
    bool prepared_{false};
};

} // namespace localscribe
