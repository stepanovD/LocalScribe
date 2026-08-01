#pragma once

#include "DiarizationBackend.hpp"

#include <map>
#include <optional>
#include <vector>

namespace localscribe {

class SourceDiarizationBackend final : public IDiarizationBackend {
public:
    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] Expected<void>
    prepare(const DiarizationConfiguration &configuration) override;
    [[nodiscard]] Expected<std::vector<SpeakerTurn>>
    assign(
        const AudioWindow &audio,
        std::span<const AsrHypothesis> hypotheses) override;
    [[nodiscard]] Expected<std::vector<SpeakerTurn>> flush() override;

private:
    DiarizationConfiguration configuration_;
    bool prepared_{false};
};

class AcousticDiarizationBackend final : public IDiarizationBackend {
public:
    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] Expected<void>
    prepare(const DiarizationConfiguration &configuration) override;
    [[nodiscard]] Expected<std::vector<SpeakerTurn>>
    assign(
        const AudioWindow &audio,
        std::span<const AsrHypothesis> hypotheses) override;
    [[nodiscard]] Expected<std::vector<SpeakerTurn>> flush() override;

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

    struct PendingSpeakerCandidate {
        std::size_t fallbackCluster{};
        std::vector<float> centroid;
        std::string embeddingModelId;
        std::size_t observations{};
        std::int64_t lastEndTimeNs{};
        bool continuousFromFallback{};
    };

    struct SpeakerAssignment {
        std::size_t cluster{};
        std::uint32_t revision{};
        bool final{};
        bool hadUsableEmbedding{};
    };

    [[nodiscard]] std::size_t createCluster(
        std::span<const float> embedding,
        std::string_view embeddingModelId);
    [[nodiscard]] std::size_t selectCluster(
        const AsrHypothesis &hypothesis);
    [[nodiscard]] float clusterSimilarity(
        const SpeakerCluster &cluster,
        std::span<const float> embedding) const;
    [[nodiscard]] std::vector<float> profileMatchEmbedding(
        const AsrHypothesis &hypothesis) const;
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

    DiarizationConfiguration configuration_;
    std::vector<SpeakerCluster> clusters_;
    std::map<StableId, SpeakerAssignment> assignments_;
    std::optional<std::size_t> previousCluster_;
    std::optional<PendingSpeakerCandidate> pendingCandidate_;
    std::int64_t previousEndTimeNs_{};
    std::size_t nextAnonymousOrdinal_{1};
    bool forceDifferentSpeaker_{false};
    bool prepared_{false};
};

} // namespace localscribe
