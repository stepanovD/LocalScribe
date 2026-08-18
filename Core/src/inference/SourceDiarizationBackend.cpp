#include "SourceDiarizationBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>
#include <utility>

namespace localscribe {
namespace {

constexpr std::size_t kMaximumRemoteSpeakers = 8;
constexpr std::size_t kMaximumClusterPrototypes = 6;
constexpr std::size_t kMaximumPendingRevisions = 64;
constexpr float kSpeakerMatchSimilarity = 0.87F;
constexpr float kStrongSwitchSimilarity = 0.94F;
constexpr float kStrongSwitchMargin = 0.04F;
constexpr float kContinuousVoiceAdaptationSimilarity = 0.85F;
constexpr float kAfterPauseVoiceAdaptationSimilarity = 0.89F;
constexpr float kCandidateConsistencySimilarity = 0.92F;
constexpr float kExplicitTurnContinuationSimilarity = 0.97F;
constexpr float kPrototypeAdmissionSimilarity = 0.995F;
constexpr std::int64_t kConsecutiveSpeakerGapNs = 2'000'000'000;
constexpr std::int64_t kPendingSpeechConfirmationNs = 1'500'000'000;
constexpr std::int64_t kPendingTimeoutNs = 5'000'000'000;

float cosineSimilarity(
    std::span<const float> left,
    std::span<const float> right)
{
    if (left.empty() || left.size() != right.size()) {
        return -1.0F;
    }
    long double dot = 0.0L;
    long double leftMagnitude = 0.0L;
    long double rightMagnitude = 0.0L;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!std::isfinite(left[index]) || !std::isfinite(right[index])) {
            return -1.0F;
        }
        dot += static_cast<long double>(left[index]) * right[index];
        leftMagnitude +=
            static_cast<long double>(left[index]) * left[index];
        rightMagnitude +=
            static_cast<long double>(right[index]) * right[index];
    }
    if (leftMagnitude <= 1.0e-12L || rightMagnitude <= 1.0e-12L) {
        return -1.0F;
    }
    const long double similarity =
        dot / std::sqrt(leftMagnitude * rightMagnitude);
    if (!std::isfinite(similarity)) {
        return -1.0F;
    }
    return std::clamp(static_cast<float>(similarity), -1.0F, 1.0F);
}

bool embeddingIsUsable(std::span<const float> embedding)
{
    if (embedding.empty()) {
        return false;
    }
    long double magnitude = 0.0L;
    for (const float value : embedding) {
        if (!std::isfinite(value)) {
            return false;
        }
        magnitude += static_cast<long double>(value) * value;
    }
    return std::isfinite(magnitude) && magnitude > 1.0e-12L;
}

bool sameEmbeddingModel(std::string_view left, std::string_view right)
{
    return left.empty() ? right.empty() : left == right;
}

void normalize(std::vector<float> &values)
{
    long double magnitude = 0.0L;
    for (const float value : values) {
        if (!std::isfinite(value)) {
            values.clear();
            return;
        }
        magnitude += static_cast<long double>(value) * value;
    }
    if (!std::isfinite(magnitude) || magnitude <= 1.0e-12L) {
        values.clear();
        return;
    }
    const float scale =
        1.0F / static_cast<float>(std::sqrt(magnitude));
    for (float &value : values) {
        value *= scale;
    }
}

Expected<bool> sourceIsLocal(
    const DiarizationConfiguration &configuration,
    std::uint64_t sourceId)
{
    if (sourceId == configuration.microphoneSourceId) {
        return true;
    }
    if (sourceId == configuration.systemAudioSourceId) {
        return false;
    }
    return Error{
        LS_INVALID_ARGUMENT,
        "diarization received audio from an unknown source"};
}

Expected<void> validateBatch(const AsrTimelineBatch &batch)
{
    if (batch.processedStartTimeNs < 0
        || batch.finalizedThroughTimeNs < batch.processedStartTimeNs) {
        return Error{
            LS_INVALID_ARGUMENT,
            "diarization received an invalid ASR timeline batch"};
    }
    const auto mismatched = std::find_if(
        batch.hypotheses.begin(),
        batch.hypotheses.end(),
        [&](const AsrHypothesis &hypothesis) {
            return hypothesis.sourceId != batch.sourceId
                || hypothesis.startTimeNs < batch.processedStartTimeNs
                || hypothesis.endTimeNs < hypothesis.startTimeNs
                || hypothesis.endTimeNs > batch.finalizedThroughTimeNs;
        });
    if (mismatched != batch.hypotheses.end()) {
        return Error{
            LS_INVALID_ARGUMENT,
            "diarization hypothesis does not match its ASR timeline batch"};
    }
    return success();
}

void sanitizeSpeakerNames(DiarizationConfiguration &configuration)
{
    if (configuration.localSpeakerName.empty()) {
        configuration.localSpeakerName = "Me";
    }
    if (configuration.remoteSpeakerName.empty()
        || configuration.remoteSpeakerName == configuration.localSpeakerName) {
        configuration.remoteSpeakerName =
            configuration.localSpeakerName == "Speaker 1"
            ? "Remote Speaker 1"
            : "Speaker 1";
    }
}

bool sameSpeakerName(std::string_view left, std::string_view right)
{
    return left.size() == right.size()
        && std::equal(
            left.begin(),
            left.end(),
            right.begin(),
            [](unsigned char first, unsigned char second) {
                return std::tolower(first) == std::tolower(second);
            });
}

std::int64_t saturatedDeadline(std::int64_t endTimeNs)
{
    if (endTimeNs
        > std::numeric_limits<std::int64_t>::max() - kPendingTimeoutNs) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return endTimeNs + kPendingTimeoutNs;
}

enum class GuardScriptMode {
    rejectedResolution,
    confirmedLocalOwnership,
    blockingAssignment,
};

class GuardScriptDiarizationBackend final : public IDiarizationBackend {
public:
    explicit GuardScriptDiarizationBackend(GuardScriptMode mode)
        : mode_(mode)
    {
    }

    [[nodiscard]] BackendInfo info() const override
    {
        switch (mode_) {
        case GuardScriptMode::rejectedResolution:
            return BackendInfo{
                "test-malicious-fallback-resolution",
                "1",
                true};
        case GuardScriptMode::confirmedLocalOwnership:
            return BackendInfo{
                "test-malicious-confirmed-local-ownership",
                "1",
                true};
        case GuardScriptMode::blockingAssignment:
            return BackendInfo{
                "test-blocking-diarization-assign",
                "1",
                true};
        }
        return BackendInfo{"test-unknown-diarization", "1", true};
    }

    [[nodiscard]] Expected<void> prepare(
        const DiarizationConfiguration &configuration) override
    {
        configuration_ = configuration;
        sanitizeSpeakerNames(configuration_);
        prepared_ = true;
        return success();
    }

    [[nodiscard]] Expected<DiarizationUpdate> assign(
        const AsrTimelineBatch &batch) override
    {
        if (!prepared_) {
            return Error{
                LS_INVALID_STATE,
                "scripted diarization is not prepared"};
        }
        if (auto valid = validateBatch(batch); !valid) {
            return valid.error();
        }
        auto local = sourceIsLocal(configuration_, batch.sourceId);
        if (!local) {
            return local.error();
        }

        if (mode_ == GuardScriptMode::blockingAssignment) {
            /* Longer than the runtime's complete finalize budget. */
            std::this_thread::sleep_for(std::chrono::seconds(7));
        }

        DiarizationUpdate update;
        update.decisions.reserve(batch.hypotheses.size());
        for (const auto &hypothesis : batch.hypotheses) {
            SpeakerTurn turn;
            turn.stableId = hypothesis.stableId;
            turn.sourceId = hypothesis.sourceId;
            turn.startTimeNs = hypothesis.startTimeNs;
            turn.endTimeNs = hypothesis.endTimeNs;
            turn.revision = hypothesis.revision;
            turn.confidence = 0.8F;

            if (local.value()) {
                turn.speakerId = 1;
                turn.speakerLabel = configuration_.localSpeakerName;
            } else {
                turn.speakerId = kAnonymousSpeakerFlag | 77u;
                turn.speakerLabel = "Stored fallback";
            }

            const bool shouldHold =
                mode_ != GuardScriptMode::blockingAssignment
                && !local.value() && hypothesis.final;
            if (!shouldHold) {
                update.decisions.push_back(SpeakerTurnDecision{
                    SpeakerTurnDecisionKind::commit,
                    std::move(turn),
                    0,
                    0});
                continue;
            }

            if (heldTurns_.empty()) {
                deadlineTimeNs_ = saturatedDeadline(hypothesis.endTimeNs);
            }
            heldTurns_.push_back(turn);
            update.decisions.push_back(SpeakerTurnDecision{
                SpeakerTurnDecisionKind::hold,
                std::move(turn),
                kPendingGroupId,
                deadlineTimeNs_});
        }
        return update;
    }

    [[nodiscard]] Expected<DiarizationUpdate> flush(
        DiarizationFlushReason) override
    {
        if (!prepared_) {
            return Error{
                LS_INVALID_STATE,
                "scripted diarization is not prepared"};
        }
        DiarizationUpdate update;
        if (heldTurns_.empty()) {
            return update;
        }

        PendingSpeakerResolution resolution;
        resolution.pendingGroupId = kPendingGroupId;
        resolution.reason =
            mode_ == GuardScriptMode::confirmedLocalOwnership
            ? PendingSpeakerResolutionReason::confirmed
            : PendingSpeakerResolutionReason::contradicted;
        resolution.turns.reserve(heldTurns_.size());
        for (auto turn : heldTurns_) {
            /* Deliberately attempts to steal System Audio for Local Speaker. */
            turn.speakerId = 1;
            turn.speakerLabel = configuration_.localSpeakerName;
            turn.confidence = 0.01F;
            resolution.turns.push_back(std::move(turn));
        }
        update.resolutions.push_back(std::move(resolution));
        heldTurns_.clear();
        deadlineTimeNs_ = 0;
        return update;
    }

private:
    static constexpr std::uint64_t kPendingGroupId = 9'001;

    GuardScriptMode mode_;
    DiarizationConfiguration configuration_;
    std::vector<SpeakerTurn> heldTurns_;
    std::int64_t deadlineTimeNs_{};
    bool prepared_{false};
};

} // namespace

BackendInfo SourceDiarizationBackend::info() const
{
    return BackendInfo{"source-aware", "2", false};
}

Expected<void> SourceDiarizationBackend::prepare(
    const DiarizationConfiguration &configuration)
{
    configuration_ = configuration;
    sanitizeSpeakerNames(configuration_);
    prepared_ = true;
    return success();
}

Expected<DiarizationUpdate> SourceDiarizationBackend::assign(
    const AsrTimelineBatch &batch)
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "source-aware diarization is not prepared"};
    }
    auto local = sourceIsLocal(configuration_, batch.sourceId);
    if (!local) {
        return local.error();
    }
    if (auto valid = validateBatch(batch); !valid) {
        return valid.error();
    }

    const std::uint64_t speakerId = local.value()
        ? 1u
        : (kAnonymousSpeakerFlag | batch.sourceId);
    const std::string &label = local.value()
        ? configuration_.localSpeakerName
        : configuration_.remoteSpeakerName;
    DiarizationUpdate update;
    update.decisions.reserve(batch.hypotheses.size());
    for (const auto &hypothesis : batch.hypotheses) {
        SpeakerTurn turn;
        turn.stableId = hypothesis.stableId;
        turn.sourceId = hypothesis.sourceId;
        turn.startTimeNs = hypothesis.startTimeNs;
        turn.endTimeNs = hypothesis.endTimeNs;
        turn.speakerId = speakerId;
        turn.speakerLabel = label;
        turn.confidence = 1.0F;
        turn.revision = hypothesis.revision;
        update.decisions.push_back(SpeakerTurnDecision{
            SpeakerTurnDecisionKind::commit,
            std::move(turn),
            0,
            0});
    }
    return update;
}

Expected<DiarizationUpdate> SourceDiarizationBackend::flush(
    DiarizationFlushReason)
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "source-aware diarization is not prepared"};
    }
    return DiarizationUpdate{};
}

BackendInfo AcousticDiarizationBackend::info() const
{
    return BackendInfo{"acoustic-clustering", "6", false};
}

Expected<void> AcousticDiarizationBackend::prepare(
    const DiarizationConfiguration &configuration)
{
    configuration_ = configuration;
    sanitizeSpeakerNames(configuration_);
    clusters_.clear();
    assignments_.clear();
    previousCluster_.reset();
    pendingSwitch_.reset();
    finalizedWatermarks_.clear();
    previousEndTimeNs_ = 0;
    nextAnonymousOrdinal_ = 1;
    nextPendingGroupId_ = 1;
    forceDifferentSpeaker_ = false;
    for (const auto &profile : configuration_.voiceProfiles) {
        if (profile.profileId == 0
            || profile.profileId > kSpeakerIdPayloadMask
            || profile.displayName.empty()
            || sameSpeakerName(
                profile.displayName,
                configuration_.localSpeakerName)
            || profile.embeddingModelId.empty()
            || profile.centroid.empty()) {
            continue;
        }
        SpeakerCluster cluster;
        cluster.speakerId = persistentSpeakerId(profile.profileId);
        cluster.label = profile.displayName;
        cluster.embeddingModelId = profile.embeddingModelId;
        cluster.centroid = profile.centroid;
        normalize(cluster.centroid);
        if (cluster.centroid.empty()) {
            continue;
        }
        for (const auto &prototype : profile.prototypes) {
            if (prototype.size() != cluster.centroid.size()) {
                continue;
            }
            auto normalized = prototype;
            normalize(normalized);
            if (!normalized.empty()) {
                cluster.prototypes.push_back(std::move(normalized));
            }
        }
        if (cluster.prototypes.empty()) {
            cluster.prototypes.push_back(cluster.centroid);
        }
        cluster.observations = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                profile.observationCount,
                std::numeric_limits<std::size_t>::max()));
        cluster.persistedProfile = true;
        clusters_.push_back(std::move(cluster));
    }
    prepared_ = true;
    return success();
}

std::size_t AcousticDiarizationBackend::createCluster(
    std::span<const float> embedding,
    std::string_view embeddingModelId)
{
    SpeakerCluster cluster;
    const std::size_t ordinal = nextAnonymousOrdinal_++;
    cluster.speakerId =
        kAnonymousSpeakerFlag | static_cast<std::uint64_t>(ordinal);
    cluster.label = ordinal == 1
        ? configuration_.remoteSpeakerName
        : "Speaker " + std::to_string(ordinal);
    cluster.embeddingModelId = embeddingModelId;
    cluster.centroid.assign(embedding.begin(), embedding.end());
    normalize(cluster.centroid);
    if (!cluster.centroid.empty()) {
        cluster.prototypes.push_back(cluster.centroid);
    }
    const auto reusable = std::find_if(
        clusters_.begin(),
        clusters_.end(),
        [](const SpeakerCluster &existing) { return existing.retired; });
    if (reusable != clusters_.end()) {
        *reusable = std::move(cluster);
        return static_cast<std::size_t>(
            std::distance(clusters_.begin(), reusable));
    }
    clusters_.push_back(std::move(cluster));
    return clusters_.size() - 1;
}

AcousticDiarizationBackend::CandidateSelection
AcousticDiarizationBackend::selectCandidate(
    const AsrHypothesis &hypothesis,
    bool turnHintBefore,
    std::optional<std::size_t> reconsideredAssignment) const
{
    CandidateSelection selection;
    if (!embeddingIsUsable(hypothesis.speakerEmbedding)) {
        selection.targetCluster = previousCluster_;
        selection.activeSpeaker = previousCluster_.has_value();
        return selection;
    }

    struct Score {
        std::size_t cluster{};
        float similarity{};
    };
    std::vector<Score> scores;
    for (std::size_t index = 0; index < clusters_.size(); ++index) {
        const auto &cluster = clusters_[index];
        if (!clusterIsCompatible(cluster, hypothesis)
            || (reconsideredAssignment == index
                && !cluster.hasMultipleEvidenceIds
                && cluster.soleEvidenceStableId == hypothesis.stableId)) {
            continue;
        }
        scores.push_back(Score{
            index,
            clusterSimilarity(cluster, hypothesis.speakerEmbedding)});
    }
    std::sort(
        scores.begin(),
        scores.end(),
        [](const Score &left, const Score &right) {
            if (left.similarity != right.similarity) {
                return left.similarity > right.similarity;
            }
            return left.cluster < right.cluster;
        });
    if (scores.empty()) {
        return selection;
    }

    const Score best = scores.front();
    const float secondSimilarity = scores.size() > 1
        ? scores[1].similarity
        : -std::numeric_limits<float>::infinity();
    selection.similarity = best.similarity;
    selection.margin = std::isfinite(secondSimilarity)
        ? best.similarity - secondSimilarity
        : std::numeric_limits<float>::infinity();

    if (previousCluster_ && best.cluster == *previousCluster_) {
        const bool ignoreTurnHint =
            best.similarity >= kExplicitTurnContinuationSimilarity
            && selection.margin >= kStrongSwitchMargin;
        if (!turnHintBefore || ignoreTurnHint) {
            if (best.similarity >= kSpeakerMatchSimilarity) {
                selection.targetCluster = best.cluster;
                selection.activeSpeaker = true;
            }
            return selection;
        }

        const auto alternative = std::find_if(
            scores.begin() + 1,
            scores.end(),
            [&](const Score &score) {
                return score.similarity >= kSpeakerMatchSimilarity;
            });
        if (alternative == scores.end()) {
            return CandidateSelection{};
        }
        const float followingSimilarity =
            alternative + 1 != scores.end()
            ? (alternative + 1)->similarity
            : -std::numeric_limits<float>::infinity();
        const float strongestOtherSimilarity = std::max(
            best.similarity,
            followingSimilarity);
        selection.targetCluster = alternative->cluster;
        selection.similarity = alternative->similarity;
        selection.margin = std::isfinite(strongestOtherSimilarity)
            ? alternative->similarity - strongestOtherSimilarity
            : std::numeric_limits<float>::infinity();
        selection.strong =
            selection.similarity >= kStrongSwitchSimilarity
            && selection.margin >= kStrongSwitchMargin;
        return selection;
    }

    if (best.similarity < kSpeakerMatchSimilarity) {
        return CandidateSelection{};
    }
    selection.targetCluster = best.cluster;
    selection.strong = best.similarity >= kStrongSwitchSimilarity
        && selection.margin >= kStrongSwitchMargin;
    return selection;
}

bool AcousticDiarizationBackend::profileIsCompatible(
    const SpeakerCluster &cluster,
    const AsrHypothesis &hypothesis) const
{
    return cluster.persistedProfile
        && !hypothesis.speakerEmbeddingModel.empty()
        && clusterIsCompatible(cluster, hypothesis);
}

bool AcousticDiarizationBackend::clusterIsCompatible(
    const SpeakerCluster &cluster,
    const AsrHypothesis &hypothesis) const
{
    if (cluster.retired
        || !embeddingIsUsable(hypothesis.speakerEmbedding)
        || cluster.centroid.size() != hypothesis.speakerEmbedding.size()) {
        return false;
    }
    if (cluster.embeddingModelId.empty()
        || hypothesis.speakerEmbeddingModel.empty()) {
        return cluster.embeddingModelId.empty()
            && hypothesis.speakerEmbeddingModel.empty();
    }
    return cluster.embeddingModelId == hypothesis.speakerEmbeddingModel;
}

std::size_t AcousticDiarizationBackend::anonymousClusterCount() const
{
    return static_cast<std::size_t>(std::count_if(
        clusters_.begin(),
        clusters_.end(),
        [](const SpeakerCluster &cluster) {
            return !cluster.persistedProfile && !cluster.retired;
        }));
}

float AcousticDiarizationBackend::clusterSimilarity(
    const SpeakerCluster &cluster,
    std::span<const float> embedding) const
{
    float similarity = cosineSimilarity(embedding, cluster.centroid);
    for (const auto &prototype : cluster.prototypes) {
        similarity = std::max(
            similarity,
            cosineSimilarity(embedding, prototype));
    }
    return similarity;
}

void AcousticDiarizationBackend::updateCluster(
    SpeakerCluster &cluster,
    std::span<const float> embedding,
    float minimumSimilarity,
    const StableId *evidenceStableId)
{
    if (embedding.empty()) {
        return;
    }
    std::vector<float> normalized(embedding.begin(), embedding.end());
    normalize(normalized);
    if (normalized.empty()) {
        return;
    }
    const auto recordEvidenceOwner = [&] {
        if (cluster.hasMultipleEvidenceIds) {
            return;
        }
        if (evidenceStableId == nullptr) {
            cluster.soleEvidenceStableId.reset();
            cluster.hasMultipleEvidenceIds = true;
            return;
        }
        if (!cluster.soleEvidenceStableId) {
            cluster.soleEvidenceStableId = *evidenceStableId;
        } else if (*cluster.soleEvidenceStableId != *evidenceStableId) {
            cluster.soleEvidenceStableId.reset();
            cluster.hasMultipleEvidenceIds = true;
        }
    };
    if (cluster.centroid.empty()) {
        cluster.centroid = normalized;
        cluster.prototypes = {normalized};
        cluster.observations = 1;
        recordEvidenceOwner();
        return;
    }
    if (cluster.centroid.size() != normalized.size()) {
        return;
    }
    const float similarity = clusterSimilarity(cluster, normalized);
    if (cluster.observations != 0 && similarity < minimumSimilarity) {
        return;
    }
    recordEvidenceOwner();
    const float retained = static_cast<float>(
        std::min<std::size_t>(cluster.observations, 12));
    const float incoming = 1.0F / (retained + 4.0F);
    const float existing = 1.0F - incoming;
    for (std::size_t index = 0; index < cluster.centroid.size(); ++index) {
        cluster.centroid[index] = cluster.centroid[index] * existing
            + normalized[index] * incoming;
    }
    normalize(cluster.centroid);
    const bool distinctPrototype = std::all_of(
        cluster.prototypes.begin(),
        cluster.prototypes.end(),
        [&](const std::vector<float> &prototype) {
            return cosineSimilarity(normalized, prototype)
                < kPrototypeAdmissionSimilarity;
        });
    if (distinctPrototype) {
        if (cluster.prototypes.size() >= kMaximumClusterPrototypes) {
            cluster.prototypes.erase(cluster.prototypes.begin());
        }
        cluster.prototypes.push_back(std::move(normalized));
    }
    ++cluster.observations;
}

SpeakerTurn AcousticDiarizationBackend::makeTurn(
    const AsrHypothesis &hypothesis,
    std::size_t clusterIndex) const
{
    const auto &cluster = clusters_[clusterIndex];
    const bool usable = embeddingIsUsable(hypothesis.speakerEmbedding);
    const float similarity = clusterIsCompatible(cluster, hypothesis)
        ? clusterSimilarity(cluster, hypothesis.speakerEmbedding)
        : -1.0F;
    SpeakerTurn turn;
    turn.stableId = hypothesis.stableId;
    turn.sourceId = hypothesis.sourceId;
    turn.startTimeNs = hypothesis.startTimeNs;
    turn.endTimeNs = hypothesis.endTimeNs;
    turn.speakerId = cluster.speakerId;
    turn.speakerLabel = cluster.label;
    turn.confidence = !usable
        ? 0.60F
        : std::clamp(
              (std::max(similarity, 0.50F) - 0.50F) * 2.0F,
              0.50F,
              0.95F);
    turn.revision = hypothesis.revision;
    return turn;
}

void AcousticDiarizationBackend::recordAssignment(
    const AsrHypothesis &hypothesis,
    std::size_t clusterIndex)
{
    const auto existing = assignments_.find(hypothesis.stableId);
    const std::optional<std::size_t> previous = existing == assignments_.end()
        ? std::nullopt
        : std::optional<std::size_t>{existing->second.cluster};
    if (existing != assignments_.end()
        && existing->second.revision > hypothesis.revision) {
        return;
    }
    assignments_[hypothesis.stableId] = SpeakerAssignment{
        clusterIndex,
        hypothesis.revision,
        hypothesis.final,
        embeddingIsUsable(hypothesis.speakerEmbedding)};

    if (!previous || *previous == clusterIndex) {
        return;
    }
    auto &oldCluster = clusters_[*previous];
    if (oldCluster.persistedProfile) {
        return;
    }
    const bool stillHasEvidence = std::any_of(
        assignments_.begin(),
        assignments_.end(),
        [&](const auto &entry) {
            return entry.second.cluster == *previous
                && entry.second.hadUsableEmbedding;
        });
    const bool stillReferenced = std::any_of(
        assignments_.begin(),
        assignments_.end(),
        [&](const auto &entry) {
            return entry.second.cluster == *previous;
        });
    if (!stillHasEvidence) {
        oldCluster.retired = !stillReferenced;
        oldCluster.embeddingModelId.clear();
        oldCluster.centroid.clear();
        oldCluster.prototypes.clear();
        oldCluster.soleEvidenceStableId.reset();
        oldCluster.hasMultipleEvidenceIds = false;
        oldCluster.observations = 0;
    }
}

void AcousticDiarizationBackend::commitDirect(
    const AsrHypothesis &hypothesis,
    std::size_t clusterIndex,
    DiarizationUpdate &update,
    bool addEvidence)
{
    if (addEvidence && !clusters_[clusterIndex].persistedProfile
        && embeddingIsUsable(hypothesis.speakerEmbedding)) {
        if (clusters_[clusterIndex].centroid.empty()) {
            clusters_[clusterIndex].embeddingModelId =
                hypothesis.speakerEmbeddingModel;
        }
        if (clusterIsCompatible(clusters_[clusterIndex], hypothesis)
            || clusters_[clusterIndex].centroid.empty()) {
            updateCluster(
                clusters_[clusterIndex],
                hypothesis.speakerEmbedding,
                kSpeakerMatchSimilarity,
                &hypothesis.stableId);
        }
    }
    recordAssignment(hypothesis, clusterIndex);
    update.decisions.push_back(SpeakerTurnDecision{
        SpeakerTurnDecisionKind::commit,
        makeTurn(hypothesis, clusterIndex),
        0,
        0});
    previousCluster_ = clusterIndex;
    previousEndTimeNs_ = hypothesis.endTimeNs;
    forceDifferentSpeaker_ = hypothesis.speakerTurnAfter;
}

void AcousticDiarizationBackend::beginPending(
    const AsrHypothesis &hypothesis,
    std::optional<std::size_t> targetCluster,
    DiarizationUpdate &update)
{
    const bool continuous = previousCluster_.has_value()
        && hypothesis.startTimeNs >= previousEndTimeNs_
        && hypothesis.startTimeNs - previousEndTimeNs_
            <= kConsecutiveSpeakerGapNs;
    PendingSpeakerSwitch pending;
    pending.groupId = nextPendingGroupId_++;
    pending.fallbackCluster = *previousCluster_;
    pending.targetCluster = targetCluster;
    pending.embeddingModelId = hypothesis.speakerEmbeddingModel;
    pending.deadlineTimeNs = saturatedDeadline(hypothesis.endTimeNs);
    pending.lastEndTimeNs = hypothesis.endTimeNs;
    pending.continuousFromFallback = continuous;
    pendingSwitch_ = std::move(pending);
    addPendingRevision(hypothesis, update);
}

bool AcousticDiarizationBackend::pendingAccepts(
    const AsrHypothesis &hypothesis,
    std::optional<std::size_t> targetCluster) const
{
    if (!pendingSwitch_
        || pendingSwitch_->targetCluster != targetCluster
        || !embeddingIsUsable(hypothesis.speakerEmbedding)
        || !sameEmbeddingModel(
            pendingSwitch_->embeddingModelId,
            hypothesis.speakerEmbeddingModel)) {
        return false;
    }
    for (const auto &[stableId, evidence] :
         pendingSwitch_->evidenceByStableId) {
        if (stableId == hypothesis.stableId) {
            continue;
        }
        if (cosineSimilarity(
                evidence.speakerEmbedding,
                hypothesis.speakerEmbedding)
            < kCandidateConsistencySimilarity) {
            return false;
        }
    }
    return true;
}

void AcousticDiarizationBackend::addPendingRevision(
    const AsrHypothesis &hypothesis,
    DiarizationUpdate &update)
{
    auto &pending = *pendingSwitch_;
    const auto duplicate = std::find_if(
        pending.revisions.begin(),
        pending.revisions.end(),
        [&](const PendingRevision &revision) {
            return revision.hypothesis.stableId == hypothesis.stableId
                && revision.hypothesis.revision == hypothesis.revision;
        });
    const SpeakerTurn fallback = makeTurn(
        hypothesis,
        pending.fallbackCluster);
    if (duplicate == pending.revisions.end()) {
        pending.revisions.push_back(PendingRevision{hypothesis, fallback});
    } else {
        duplicate->hypothesis = hypothesis;
        duplicate->fallback = fallback;
    }
    if (hypothesis.final && embeddingIsUsable(hypothesis.speakerEmbedding)) {
        const auto evidence = pending.evidenceByStableId.find(
            hypothesis.stableId);
        if (evidence == pending.evidenceByStableId.end()
            || hypothesis.revision >= evidence->second.revision) {
            pending.evidenceByStableId[hypothesis.stableId] = hypothesis;
        }
    }
    pending.lastEndTimeNs = std::max(
        pending.lastEndTimeNs,
        hypothesis.endTimeNs);
    update.decisions.push_back(SpeakerTurnDecision{
        SpeakerTurnDecisionKind::hold,
        fallback,
        pending.groupId,
        pending.deadlineTimeNs});
    forceDifferentSpeaker_ = hypothesis.speakerTurnAfter;
}

std::int64_t AcousticDiarizationBackend::pendingUniqueSpeechDurationNs() const
{
    if (!pendingSwitch_) {
        return 0;
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
    intervals.reserve(pendingSwitch_->evidenceByStableId.size());
    for (const auto &[stableId, evidence] :
         pendingSwitch_->evidenceByStableId) {
        (void)stableId;
        intervals.emplace_back(evidence.startTimeNs, evidence.endTimeNs);
    }
    std::sort(intervals.begin(), intervals.end());
    std::int64_t total = 0;
    std::int64_t currentStart = 0;
    std::int64_t currentEnd = 0;
    bool hasCurrent = false;
    for (const auto &[start, end] : intervals) {
        if (!hasCurrent) {
            currentStart = start;
            currentEnd = end;
            hasCurrent = true;
        } else if (start <= currentEnd) {
            currentEnd = std::max(currentEnd, end);
        } else {
            total += currentEnd - currentStart;
            currentStart = start;
            currentEnd = end;
        }
    }
    if (hasCurrent) {
        total += currentEnd - currentStart;
    }
    return total;
}

bool AcousticDiarizationBackend::pendingIsConfirmed() const
{
    return pendingSwitch_
        && (pendingSwitch_->evidenceByStableId.size() >= 2
            || pendingUniqueSpeechDurationNs()
                >= kPendingSpeechConfirmationNs);
}

std::vector<float> AcousticDiarizationBackend::pendingCentroid() const
{
    std::vector<float> centroid;
    if (!pendingSwitch_ || pendingSwitch_->evidenceByStableId.empty()) {
        return centroid;
    }
    for (const auto &[stableId, evidence] :
         pendingSwitch_->evidenceByStableId) {
        (void)stableId;
        std::vector<float> normalized = evidence.speakerEmbedding;
        normalize(normalized);
        if (normalized.empty()) {
            continue;
        }
        if (centroid.empty()) {
            centroid.assign(normalized.size(), 0.0F);
        }
        if (centroid.size() != normalized.size()) {
            return {};
        }
        for (std::size_t index = 0; index < centroid.size(); ++index) {
            centroid[index] += normalized[index];
        }
    }
    normalize(centroid);
    return centroid;
}

void AcousticDiarizationBackend::resolvePendingFallback(
    PendingSpeakerResolutionReason reason,
    DiarizationUpdate &update)
{
    if (!pendingSwitch_) {
        return;
    }
    PendingSpeakerResolution resolution;
    resolution.pendingGroupId = pendingSwitch_->groupId;
    resolution.reason = reason;
    resolution.turns.reserve(pendingSwitch_->revisions.size());
    std::int64_t latestEnd = previousEndTimeNs_;
    for (const auto &revision : pendingSwitch_->revisions) {
        resolution.turns.push_back(revision.fallback);
        recordAssignment(
            revision.hypothesis,
            pendingSwitch_->fallbackCluster);
        latestEnd = std::max(latestEnd, revision.hypothesis.endTimeNs);
    }
    previousCluster_ = pendingSwitch_->fallbackCluster;
    previousEndTimeNs_ = latestEnd;
    update.resolutions.push_back(std::move(resolution));
    pendingSwitch_.reset();
}

void AcousticDiarizationBackend::confirmPending(DiarizationUpdate &update)
{
    if (!pendingSwitch_) {
        return;
    }
    std::size_t selected = pendingSwitch_->fallbackCluster;
    if (pendingSwitch_->targetCluster) {
        selected = *pendingSwitch_->targetCluster;
    } else {
        const std::vector<float> centroid = pendingCentroid();
        const auto &fallback = clusters_[pendingSwitch_->fallbackCluster];
        const bool fallbackIsCompatible =
            sameEmbeddingModel(
                fallback.embeddingModelId,
                pendingSwitch_->embeddingModelId)
            && fallback.centroid.size() == centroid.size();
        const float fallbackSimilarity = fallbackIsCompatible
            ? clusterSimilarity(fallback, centroid)
            : -1.0F;
        const float adaptationThreshold =
            pendingSwitch_->continuousFromFallback
            ? kContinuousVoiceAdaptationSimilarity
            : kAfterPauseVoiceAdaptationSimilarity;
        if (fallbackSimilarity >= adaptationThreshold) {
            if (!fallback.persistedProfile) {
                for (const auto &[stableId, evidence] :
                     pendingSwitch_->evidenceByStableId) {
                    updateCluster(
                        clusters_[pendingSwitch_->fallbackCluster],
                        evidence.speakerEmbedding,
                        adaptationThreshold,
                        &stableId);
                }
            }
            resolvePendingFallback(
                PendingSpeakerResolutionReason::contradicted,
                update);
            return;
        }
        if (anonymousClusterCount() >= kMaximumRemoteSpeakers
            || centroid.empty()) {
            resolvePendingFallback(
                PendingSpeakerResolutionReason::capacity,
                update);
            return;
        }
        selected = createCluster(
            centroid,
            pendingSwitch_->embeddingModelId);
    }

    if (!clusters_[selected].persistedProfile) {
        for (const auto &[stableId, evidence] :
             pendingSwitch_->evidenceByStableId) {
            updateCluster(
                clusters_[selected],
                evidence.speakerEmbedding,
                kSpeakerMatchSimilarity,
                &stableId);
        }
    }

    PendingSpeakerResolution resolution;
    resolution.pendingGroupId = pendingSwitch_->groupId;
    resolution.reason = PendingSpeakerResolutionReason::confirmed;
    resolution.turns.reserve(pendingSwitch_->revisions.size());
    std::int64_t latestEnd = previousEndTimeNs_;
    for (const auto &revision : pendingSwitch_->revisions) {
        resolution.turns.push_back(makeTurn(revision.hypothesis, selected));
        recordAssignment(revision.hypothesis, selected);
        latestEnd = std::max(latestEnd, revision.hypothesis.endTimeNs);
    }
    previousCluster_ = selected;
    previousEndTimeNs_ = latestEnd;
    update.resolutions.push_back(std::move(resolution));
    pendingSwitch_.reset();
}

void AcousticDiarizationBackend::processRemoteHypothesis(
    const AsrHypothesis &hypothesis,
    DiarizationUpdate &update)
{
    if (pendingSwitch_
        && hypothesis.endTimeNs > pendingSwitch_->deadlineTimeNs) {
        resolvePendingFallback(
            PendingSpeakerResolutionReason::timeout,
            update);
    }

    const auto existing = assignments_.find(hypothesis.stableId);
    std::optional<std::size_t> reconsideredAssignment;
    if (existing != assignments_.end()) {
        const bool sameRevision =
            hypothesis.revision == existing->second.revision;
        const bool usable = embeddingIsUsable(hypothesis.speakerEmbedding);
        const bool hasBetterEvidence =
            hypothesis.revision > existing->second.revision
            || (sameRevision
                && ((!existing->second.final && hypothesis.final)
                    || (!existing->second.hadUsableEmbedding && usable)));
        if (!hasBetterEvidence) {
            update.decisions.push_back(SpeakerTurnDecision{
                SpeakerTurnDecisionKind::commit,
                makeTurn(hypothesis, existing->second.cluster),
                0,
                0});
            return;
        }
        reconsideredAssignment = existing->second.cluster;
    }

    const bool usable = embeddingIsUsable(hypothesis.speakerEmbedding);
    if (!previousCluster_) {
        const auto candidate = selectCandidate(
            hypothesis,
            false,
            reconsideredAssignment);
        if (candidate.targetCluster && candidate.strong) {
            commitDirect(
                hypothesis,
                *candidate.targetCluster,
                update,
                true);
            return;
        }
        const std::size_t created = createCluster(
            hypothesis.speakerEmbedding,
            hypothesis.speakerEmbeddingModel);
        commitDirect(hypothesis, created, update, true);
        return;
    }

    if (pendingSwitch_) {
        const bool exactRevisionAlreadyHeld = std::any_of(
            pendingSwitch_->revisions.begin(),
            pendingSwitch_->revisions.end(),
            [&](const PendingRevision &revision) {
                return revision.hypothesis.stableId == hypothesis.stableId
                    && revision.hypothesis.revision == hypothesis.revision;
            });
        if (!exactRevisionAlreadyHeld
            && pendingSwitch_->revisions.size()
                >= kMaximumPendingRevisions) {
            resolvePendingFallback(
                PendingSpeakerResolutionReason::capacity,
                update);
        }
    }

    const bool turnHintBefore = forceDifferentSpeaker_;
    CandidateSelection candidate = selectCandidate(
        hypothesis,
        turnHintBefore,
        reconsideredAssignment);

    if (pendingSwitch_) {
        if (candidate.activeSpeaker || !usable || !hypothesis.final) {
            resolvePendingFallback(
                PendingSpeakerResolutionReason::contradicted,
                update);
            commitDirect(
                hypothesis,
                *previousCluster_,
                update,
                usable);
            return;
        }
        if (pendingAccepts(hypothesis, candidate.targetCluster)) {
            addPendingRevision(hypothesis, update);
            if (candidate.strong || pendingIsConfirmed()) {
                confirmPending(update);
            }
            return;
        }
        resolvePendingFallback(
            PendingSpeakerResolutionReason::contradicted,
            update);
        candidate = selectCandidate(
            hypothesis,
            turnHintBefore,
            reconsideredAssignment);
    }

    if (candidate.activeSpeaker) {
        commitDirect(
            hypothesis,
            *candidate.targetCluster,
            update,
            true);
        return;
    }
    if (candidate.targetCluster && candidate.strong) {
        commitDirect(
            hypothesis,
            *candidate.targetCluster,
            update,
            true);
        return;
    }
    if (!usable || !hypothesis.final) {
        commitDirect(hypothesis, *previousCluster_, update, false);
        return;
    }
    beginPending(hypothesis, candidate.targetCluster, update);
    if (pendingIsConfirmed()) {
        confirmPending(update);
    }
}

Expected<DiarizationUpdate> AcousticDiarizationBackend::assign(
    const AsrTimelineBatch &batch)
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "acoustic diarization is not prepared"};
    }
    auto local = sourceIsLocal(configuration_, batch.sourceId);
    if (!local) {
        return local.error();
    }
    if (auto valid = validateBatch(batch); !valid) {
        return valid.error();
    }
    const auto watermark = finalizedWatermarks_.find(batch.sourceId);
    if (!batch.discontinuityBefore
        && watermark != finalizedWatermarks_.end()
        && batch.finalizedThroughTimeNs < watermark->second) {
        return Error{
            LS_INVALID_ARGUMENT,
            "diarization ASR timeline watermark moved backwards"};
    }

    DiarizationUpdate update;
    update.decisions.reserve(batch.hypotheses.size());
    if (!local.value() && batch.discontinuityBefore) {
        if (pendingSwitch_) {
            resolvePendingFallback(
                PendingSpeakerResolutionReason::discontinuity,
                update);
        }
        forceDifferentSpeaker_ = false;
    }

    for (const auto &hypothesis : batch.hypotheses) {
        if (local.value()) {
            SpeakerTurn turn;
            turn.stableId = hypothesis.stableId;
            turn.sourceId = hypothesis.sourceId;
            turn.startTimeNs = hypothesis.startTimeNs;
            turn.endTimeNs = hypothesis.endTimeNs;
            turn.speakerId = 1;
            turn.speakerLabel = configuration_.localSpeakerName;
            turn.confidence = 1.0F;
            turn.revision = hypothesis.revision;
            update.decisions.push_back(SpeakerTurnDecision{
                SpeakerTurnDecisionKind::commit,
                std::move(turn),
                0,
                0});
        } else {
            processRemoteHypothesis(hypothesis, update);
        }
    }

    if (!local.value() && pendingSwitch_
        && batch.finalizedThroughTimeNs
            >= pendingSwitch_->deadlineTimeNs) {
        resolvePendingFallback(
            PendingSpeakerResolutionReason::timeout,
            update);
    }
    finalizedWatermarks_[batch.sourceId] = batch.finalizedThroughTimeNs;
    return update;
}

Expected<DiarizationUpdate> AcousticDiarizationBackend::flush(
    DiarizationFlushReason reason)
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "acoustic diarization is not prepared"};
    }
    DiarizationUpdate update;
    if (pendingSwitch_) {
        resolvePendingFallback(
            reason == DiarizationFlushReason::pause
                ? PendingSpeakerResolutionReason::pause
                : PendingSpeakerResolutionReason::endOfStream,
            update);
    }
    forceDifferentSpeaker_ = false;
    return update;
}

Expected<std::unique_ptr<IDiarizationBackend>>
createDiarizationBackend(
    std::string_view backendId,
    bool allowTestBackends)
{
    if (backendId.empty() || backendId == "source-aware") {
        return std::unique_ptr<IDiarizationBackend>(
            std::make_unique<SourceDiarizationBackend>());
    }
    if (backendId == "acoustic-clustering") {
        return std::unique_ptr<IDiarizationBackend>(
            std::make_unique<AcousticDiarizationBackend>());
    }
    if (backendId == "test-malicious-fallback-resolution") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "test diarization backends are disabled"};
        }
        return std::unique_ptr<IDiarizationBackend>(
            std::make_unique<GuardScriptDiarizationBackend>(
                GuardScriptMode::rejectedResolution));
    }
    if (backendId == "test-malicious-confirmed-local-ownership") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "test diarization backends are disabled"};
        }
        return std::unique_ptr<IDiarizationBackend>(
            std::make_unique<GuardScriptDiarizationBackend>(
                GuardScriptMode::confirmedLocalOwnership));
    }
    if (backendId == "test-blocking-diarization-assign") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "test diarization backends are disabled"};
        }
        return std::unique_ptr<IDiarizationBackend>(
            std::make_unique<GuardScriptDiarizationBackend>(
                GuardScriptMode::blockingAssignment));
    }
    return Error{
        LS_BACKEND_UNAVAILABLE,
        "requested diarization backend is not available"};
}

} // namespace localscribe
