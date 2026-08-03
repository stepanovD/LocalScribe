#include "SourceDiarizationBackend.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>

namespace localscribe {
namespace {

constexpr std::size_t kMaximumRemoteSpeakers = 8;
constexpr std::size_t kMaximumClusterPrototypes = 6;
constexpr std::size_t kCandidateConfirmationObservations = 2;
constexpr float kSpeakerMatchSimilarity = 0.87F;
constexpr float kVoiceProfileMatchSimilarity = 0.94F;
constexpr float kVoiceProfileContinuationSimilarity = 0.90F;
constexpr float kVoiceProfileMatchMargin = 0.04F;
constexpr float kContinuousVoiceAdaptationSimilarity = 0.85F;
constexpr float kAfterPauseVoiceAdaptationSimilarity = 0.89F;
constexpr float kCandidateConsistencySimilarity = 0.92F;
constexpr float kConsecutiveSpeakerSimilarity = 0.84F;
constexpr float kConsecutiveSpeakerMargin = 0.04F;
constexpr float kExplicitTurnContinuationSimilarity = 0.97F;
constexpr float kPrototypeAdmissionSimilarity = 0.995F;
constexpr std::int64_t kConsecutiveSpeakerGapNs = 2'000'000'000;
constexpr std::int64_t kCandidateGapNs = 5'000'000'000;

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
        if (!std::isfinite(left[index])
            || !std::isfinite(right[index])) {
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
    const AudioWindow &audio)
{
    if (audio.sourceId == configuration.microphoneSourceId) {
        return true;
    }
    if (audio.sourceId == configuration.systemAudioSourceId) {
        return false;
    }
    return Error{
        LS_INVALID_ARGUMENT,
        "diarization received audio from an unknown source"};
}

Expected<void> validateHypothesisSources(
    const AudioWindow &audio,
    std::span<const AsrHypothesis> hypotheses)
{
    const auto mismatched = std::find_if(
        hypotheses.begin(),
        hypotheses.end(),
        [&](const AsrHypothesis &hypothesis) {
            return hypothesis.sourceId != audio.sourceId;
        });
    if (mismatched != hypotheses.end()) {
        return Error{
            LS_INVALID_ARGUMENT,
            "diarization hypothesis source does not match its audio"};
    }
    return success();
}

void sanitizeSpeakerNames(DiarizationConfiguration &configuration)
{
    if (configuration.localSpeakerName.empty()) {
        configuration.localSpeakerName = "Me";
    }
    if (configuration.remoteSpeakerName.empty()
        || configuration.remoteSpeakerName
            == configuration.localSpeakerName) {
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

Expected<std::vector<SpeakerTurn>> SourceDiarizationBackend::assign(
    const AudioWindow &audio,
    std::span<const AsrHypothesis> hypotheses)
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "source-aware diarization is not prepared"};
    }

    auto local = sourceIsLocal(configuration_, audio);
    if (!local) {
        return local.error();
    }
    auto validSources = validateHypothesisSources(audio, hypotheses);
    if (!validSources) {
        return validSources.error();
    }
    const std::uint64_t speakerId =
        local.value() ? 1u : (kAnonymousSpeakerFlag | audio.sourceId);
    const std::string &label =
        local.value() ? configuration_.localSpeakerName
                      : configuration_.remoteSpeakerName;

    std::vector<SpeakerTurn> turns;
    turns.reserve(hypotheses.size());
    for (const auto &hypothesis : hypotheses) {
        SpeakerTurn turn;
        turn.stableId = hypothesis.stableId;
        turn.sourceId = hypothesis.sourceId;
        turn.startTimeNs = hypothesis.startTimeNs;
        turn.endTimeNs = hypothesis.endTimeNs;
        turn.speakerId = speakerId;
        turn.speakerLabel = label;
        turn.confidence = 1.0F;
        turn.revision = hypothesis.revision;
        turns.push_back(std::move(turn));
    }
    return turns;
}

Expected<std::vector<SpeakerTurn>>
SourceDiarizationBackend::flush()
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "source-aware diarization is not prepared"};
    }
    return std::vector<SpeakerTurn>{};
}

BackendInfo AcousticDiarizationBackend::info() const
{
    return BackendInfo{"acoustic-clustering", "5", false};
}

Expected<void> AcousticDiarizationBackend::prepare(
    const DiarizationConfiguration &configuration)
{
    configuration_ = configuration;
    sanitizeSpeakerNames(configuration_);
    clusters_.clear();
    assignments_.clear();
    previousCluster_.reset();
    pendingCandidate_.reset();
    previousEndTimeNs_ = 0;
    nextAnonymousOrdinal_ = 1;
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
    cluster.observations = 0;
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

std::size_t AcousticDiarizationBackend::selectCluster(
    const AsrHypothesis &hypothesis)
{
    std::optional<std::size_t> reconsideredAssignment;
    if (const auto existing = assignments_.find(hypothesis.stableId);
        existing != assignments_.end()) {
        const bool sameRevision =
            hypothesis.revision == existing->second.revision;
        const bool hasBetterEvidence =
            hypothesis.revision > existing->second.revision
            || (sameRevision
                && ((!existing->second.final && hypothesis.final)
                    || (!existing->second.hadUsableEmbedding
                        && embeddingIsUsable(
                            hypothesis.speakerEmbedding))));
        if (!hasBetterEvidence) {
            return existing->second.cluster;
        }
        reconsideredAssignment = existing->second.cluster;
    }
    const bool honorTurnBoundary =
        forceDifferentSpeaker_ && !reconsideredAssignment.has_value();
    if (honorTurnBoundary) {
        pendingCandidate_.reset();
    }
    const std::vector<float> profileEmbedding =
        profileMatchEmbedding(hypothesis);
    std::optional<std::size_t> bestProfile;
    float bestProfileSimilarity = -std::numeric_limits<float>::infinity();
    float secondProfileSimilarity = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < clusters_.size(); ++index) {
        const auto &cluster = clusters_[index];
        if (!profileIsCompatible(cluster, hypothesis)
            || (honorTurnBoundary && previousCluster_ == index)) {
            continue;
        }
        const float similarity = clusterSimilarity(
            cluster,
            profileEmbedding);
        if (similarity > bestProfileSimilarity) {
            secondProfileSimilarity = bestProfileSimilarity;
            bestProfileSimilarity = similarity;
            bestProfile = index;
        } else if (similarity > secondProfileSimilarity) {
            secondProfileSimilarity = similarity;
        }
    }
    const float profileMargin =
        std::isfinite(secondProfileSimilarity)
        ? bestProfileSimilarity - secondProfileSimilarity
        : std::numeric_limits<float>::infinity();
    float strongestAnonymousSimilarity =
        -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < clusters_.size(); ++index) {
        if (clusters_[index].persistedProfile
            || !clusterIsCompatible(clusters_[index], hypothesis)
            || (reconsideredAssignment == index
                && !clusters_[index].hasMultipleEvidenceIds
                && clusters_[index].soleEvidenceStableId
                    == hypothesis.stableId)
            || (honorTurnBoundary && previousCluster_ == index)) {
            continue;
        }
        strongestAnonymousSimilarity = std::max(
            strongestAnonymousSimilarity,
            clusterSimilarity(
                clusters_[index],
                profileEmbedding));
    }
    const float anonymousMargin =
        std::isfinite(strongestAnonymousSimilarity)
        ? bestProfileSimilarity - strongestAnonymousSimilarity
        : std::numeric_limits<float>::infinity();
    if (honorTurnBoundary && previousCluster_
        && clusterIsCompatible(
            clusters_[*previousCluster_],
            hypothesis)) {
        const float previousSimilarity = clusterSimilarity(
            clusters_[*previousCluster_],
            profileEmbedding);
        const float strongestAlternative = std::max(
            bestProfileSimilarity,
            strongestAnonymousSimilarity);
        if (previousSimilarity
                >= kExplicitTurnContinuationSimilarity
            && (!std::isfinite(strongestAlternative)
                || previousSimilarity - strongestAlternative
                    >= kConsecutiveSpeakerMargin)) {
            /*
             * TinyDiarize turn markers are useful hints, not proof of a new
             * identity. A near-identical acoustic signature with no close
             * alternative is stronger evidence that this was a false turn.
             */
            pendingCandidate_.reset();
            return *previousCluster_;
        }
    }
    if (bestProfile
        && bestProfileSimilarity >= kVoiceProfileMatchSimilarity
        && profileMargin >= kVoiceProfileMatchMargin
        && anonymousMargin >= kVoiceProfileMatchMargin) {
        pendingCandidate_.reset();
        return *bestProfile;
    }

    if (!honorTurnBoundary && previousCluster_
        && clusters_[*previousCluster_].persistedProfile
        && profileIsCompatible(
            clusters_[*previousCluster_],
            hypothesis)
        && hypothesis.startTimeNs >= previousEndTimeNs_
        && hypothesis.startTimeNs - previousEndTimeNs_
            <= kConsecutiveSpeakerGapNs) {
        const float previousSimilarity = clusterSimilarity(
            clusters_[*previousCluster_],
            profileEmbedding);
        float strongestOther = -std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < clusters_.size(); ++index) {
            if (index == *previousCluster_
                || !clusterIsCompatible(clusters_[index], hypothesis)
                || (reconsideredAssignment == index
                    && !clusters_[index].hasMultipleEvidenceIds
                    && clusters_[index].soleEvidenceStableId
                        == hypothesis.stableId)) {
                continue;
            }
            strongestOther = std::max(
                strongestOther,
                clusterSimilarity(
                    clusters_[index],
                    profileEmbedding));
        }
        if (previousSimilarity >= kVoiceProfileContinuationSimilarity
            && (!std::isfinite(strongestOther)
                || previousSimilarity - strongestOther
                    >= kVoiceProfileMatchMargin)) {
            pendingCandidate_.reset();
            return *previousCluster_;
        }
    }

    std::vector<std::size_t> anonymousClusters;
    anonymousClusters.reserve(clusters_.size());
    for (std::size_t index = 0; index < clusters_.size(); ++index) {
        if (!clusters_[index].persistedProfile
            && !clusters_[index].retired) {
            anonymousClusters.push_back(index);
        }
    }
    if (anonymousClusters.empty()) {
        pendingCandidate_.reset();
        return createCluster(
            hypothesis.speakerEmbedding,
            hypothesis.speakerEmbeddingModel);
    }

    if (!embeddingIsUsable(hypothesis.speakerEmbedding)) {
        pendingCandidate_.reset();
        if (!honorTurnBoundary && previousCluster_
            && !clusters_[*previousCluster_].persistedProfile) {
            return *previousCluster_;
        }
        const auto alternative = std::find_if(
            anonymousClusters.begin(),
            anonymousClusters.end(),
            [&](std::size_t index) { return previousCluster_ != index; });
        if (alternative != anonymousClusters.end()) {
            return *alternative;
        }
        if (previousCluster_
            && !clusters_[*previousCluster_].persistedProfile) {
            /*
             * A turn hint without acoustic evidence must not manufacture a
             * new speaker. Keep the existing anonymous identity until a
             * voiced segment can support a split.
             */
            return *previousCluster_;
        }
        if (anonymousClusterCount() < kMaximumRemoteSpeakers) {
            return createCluster({}, hypothesis.speakerEmbeddingModel);
        }
        return anonymousClusters.front();
    }

    std::size_t bestCluster = anonymousClusters.front();
    float bestSimilarity = -std::numeric_limits<float>::infinity();
    for (const std::size_t index : anonymousClusters) {
        if (!clusterIsCompatible(clusters_[index], hypothesis)
            || (honorTurnBoundary && previousCluster_ == index)) {
            continue;
        }
        const float similarity = clusterSimilarity(
            clusters_[index],
            hypothesis.speakerEmbedding);
        if (similarity > bestSimilarity) {
            bestSimilarity = similarity;
            bestCluster = index;
        }
    }

    if (!std::isfinite(bestSimilarity)) {
        const auto reusable = std::find_if(
            anonymousClusters.begin(),
            anonymousClusters.end(),
            [&](std::size_t index) {
                return clusters_[index].centroid.empty()
                    && (!honorTurnBoundary || previousCluster_ != index);
            });
        if (reusable != anonymousClusters.end()) {
            clusters_[*reusable].embeddingModelId =
                hypothesis.speakerEmbeddingModel;
            pendingCandidate_.reset();
            return *reusable;
        }
    }

    if (honorTurnBoundary) {
        pendingCandidate_.reset();
        if (std::isfinite(bestSimilarity)
            && bestSimilarity >= kSpeakerMatchSimilarity) {
            return bestCluster;
        }
        if (anonymousClusterCount() < kMaximumRemoteSpeakers) {
            return createCluster(
                hypothesis.speakerEmbedding,
                hypothesis.speakerEmbeddingModel);
        }
        return bestCluster;
    }

    if (!std::isfinite(bestSimilarity)) {
        pendingCandidate_.reset();
        if (anonymousClusterCount() < kMaximumRemoteSpeakers) {
            return createCluster(
                hypothesis.speakerEmbedding,
                hypothesis.speakerEmbeddingModel);
        }
        return bestCluster;
    }

    if (previousCluster_
        && !clusters_[*previousCluster_].persistedProfile
        && clusterIsCompatible(
            clusters_[*previousCluster_],
            hypothesis)
        && anonymousClusters.size() > 1
        && bestCluster != *previousCluster_
        && hypothesis.startTimeNs >= previousEndTimeNs_
        && hypothesis.startTimeNs - previousEndTimeNs_
            <= kConsecutiveSpeakerGapNs) {
        const float previousSimilarity = clusterSimilarity(
            clusters_[*previousCluster_],
            hypothesis.speakerEmbedding);
        if (previousSimilarity >= kConsecutiveSpeakerSimilarity
            && bestSimilarity - previousSimilarity
                < kConsecutiveSpeakerMargin) {
            pendingCandidate_.reset();
            return *previousCluster_;
        }
    }

    if (std::isfinite(bestSimilarity)
        && bestSimilarity >= kSpeakerMatchSimilarity) {
        pendingCandidate_.reset();
        return bestCluster;
    }

    const std::size_t fallbackCluster =
        previousCluster_
            && !clusters_[*previousCluster_].persistedProfile
            && clusterIsCompatible(
                clusters_[*previousCluster_],
                hypothesis)
        ? *previousCluster_
        : bestCluster;
    std::vector<float> normalized(
        hypothesis.speakerEmbedding.begin(),
        hypothesis.speakerEmbedding.end());
    normalize(normalized);
    if (normalized.empty()) {
        pendingCandidate_.reset();
        return fallbackCluster;
    }

    const bool candidateContinues =
        pendingCandidate_.has_value()
        && pendingCandidate_->fallbackCluster == fallbackCluster
        && sameEmbeddingModel(
            pendingCandidate_->embeddingModelId,
            hypothesis.speakerEmbeddingModel)
        && hypothesis.startTimeNs >= pendingCandidate_->lastEndTimeNs
        && hypothesis.startTimeNs - pendingCandidate_->lastEndTimeNs
            <= kCandidateGapNs
        && cosineSimilarity(
               normalized,
               pendingCandidate_->centroid)
            >= kCandidateConsistencySimilarity;
    if (!candidateContinues) {
        const bool continuousFromFallback =
            previousCluster_.has_value()
            && *previousCluster_ == fallbackCluster
            && hypothesis.startTimeNs >= previousEndTimeNs_
            && hypothesis.startTimeNs - previousEndTimeNs_
                <= kConsecutiveSpeakerGapNs;
        pendingCandidate_ = PendingSpeakerCandidate{
            fallbackCluster,
            std::move(normalized),
            hypothesis.speakerEmbeddingModel,
            1,
            hypothesis.endTimeNs,
            continuousFromFallback};
        return fallbackCluster;
    }

    auto &candidate = *pendingCandidate_;
    const float retained =
        static_cast<float>(candidate.observations);
    for (std::size_t index = 0;
         index < candidate.centroid.size();
         ++index) {
        candidate.centroid[index] =
            (candidate.centroid[index] * retained
             + normalized[index])
            / (retained + 1.0F);
    }
    normalize(candidate.centroid);
    ++candidate.observations;
    candidate.lastEndTimeNs = hypothesis.endTimeNs;
    if (candidate.observations
        < kCandidateConfirmationObservations) {
        return fallbackCluster;
    }

    const bool continuousFromFallback =
        candidate.continuousFromFallback;
    std::vector<float> confirmed = std::move(candidate.centroid);
    pendingCandidate_.reset();
    const float fallbackSimilarity = clusterSimilarity(
        clusters_[fallbackCluster],
        confirmed);
    const float adaptationSimilarity =
        continuousFromFallback
        ? kContinuousVoiceAdaptationSimilarity
        : kAfterPauseVoiceAdaptationSimilarity;
    if (fallbackSimilarity >= adaptationSimilarity) {
        /*
         * Repeated gray-zone evidence is a stable variation of the current
         * voice. Teach the cluster this prototype instead of fragmenting it.
         */
        updateCluster(
            clusters_[fallbackCluster],
            confirmed,
            adaptationSimilarity,
            nullptr);
        return fallbackCluster;
    }
    if (anonymousClusterCount() < kMaximumRemoteSpeakers) {
        const std::size_t created =
            createCluster(confirmed, hypothesis.speakerEmbeddingModel);
        clusters_[created].hasMultipleEvidenceIds = true;
        return created;
    }
    return bestCluster;
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
    float similarity = cosineSimilarity(
        embedding,
        cluster.centroid);
    for (const auto &prototype : cluster.prototypes) {
        similarity = std::max(
            similarity,
            cosineSimilarity(embedding, prototype));
    }
    return similarity;
}

std::vector<float> AcousticDiarizationBackend::profileMatchEmbedding(
    const AsrHypothesis &hypothesis) const
{
    std::vector<float> result(
        hypothesis.speakerEmbedding.begin(),
        hypothesis.speakerEmbedding.end());
    normalize(result);
    if (result.empty() || !pendingCandidate_
        || !sameEmbeddingModel(
            pendingCandidate_->embeddingModelId,
            hypothesis.speakerEmbeddingModel)
        || pendingCandidate_->centroid.size() != result.size()
        || hypothesis.startTimeNs < pendingCandidate_->lastEndTimeNs
        || hypothesis.startTimeNs - pendingCandidate_->lastEndTimeNs
            > kCandidateGapNs
        || cosineSimilarity(result, pendingCandidate_->centroid)
            < kCandidateConsistencySimilarity) {
        return result;
    }
    const float retained =
        static_cast<float>(pendingCandidate_->observations);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] =
            (pendingCandidate_->centroid[index] * retained + result[index])
            / (retained + 1.0F);
    }
    normalize(result);
    return result;
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
    std::vector<float> normalized(
        embedding.begin(),
        embedding.end());
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
    if (cluster.observations != 0
        && similarity < minimumSimilarity) {
        return;
    }
    recordEvidenceOwner();
    const float retained = static_cast<float>(
        std::min<std::size_t>(cluster.observations, 12));
    const float incoming = 1.0F / (retained + 4.0F);
    const float existing = 1.0F - incoming;
    for (std::size_t index = 0; index < cluster.centroid.size(); ++index) {
        cluster.centroid[index] =
            cluster.centroid[index] * existing
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

Expected<std::vector<SpeakerTurn>> AcousticDiarizationBackend::assign(
    const AudioWindow &audio,
    std::span<const AsrHypothesis> hypotheses)
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "acoustic diarization is not prepared"};
    }

    auto local = sourceIsLocal(configuration_, audio);
    if (!local) {
        return local.error();
    }
    auto validSources = validateHypothesisSources(audio, hypotheses);
    if (!validSources) {
        return validSources.error();
    }
    std::vector<SpeakerTurn> turns;
    turns.reserve(hypotheses.size());
    for (const auto &hypothesis : hypotheses) {
        SpeakerTurn turn;
        turn.stableId = hypothesis.stableId;
        turn.sourceId = hypothesis.sourceId;
        turn.startTimeNs = hypothesis.startTimeNs;
        turn.endTimeNs = hypothesis.endTimeNs;
        turn.revision = hypothesis.revision;
        if (local.value()) {
            turn.speakerId = 1;
            turn.speakerLabel = configuration_.localSpeakerName;
            turn.confidence = 1.0F;
        } else {
            const auto existing = assignments_.find(hypothesis.stableId);
            const std::optional<SpeakerAssignment> previousAssignment =
                existing == assignments_.end()
                ? std::nullopt
                : std::optional<SpeakerAssignment>{existing->second};
            const bool usableEmbedding =
                embeddingIsUsable(hypothesis.speakerEmbedding);
            const bool updatesAssignment =
                !previousAssignment
                || hypothesis.revision > previousAssignment->revision
                || (hypothesis.revision == previousAssignment->revision
                    && ((!previousAssignment->final && hypothesis.final)
                        || (!previousAssignment->hadUsableEmbedding
                            && usableEmbedding)));
            const std::size_t selected = updatesAssignment
                ? selectCluster(hypothesis)
                : previousAssignment->cluster;
            if (updatesAssignment) {
                assignments_[hypothesis.stableId] = SpeakerAssignment{
                    selected,
                    hypothesis.revision,
                    hypothesis.final,
                    usableEmbedding};
            }
            auto &cluster = clusters_[selected];
            float similarity = clusterIsCompatible(cluster, hypothesis)
                ? clusterSimilarity(cluster, hypothesis.speakerEmbedding)
                : -1.0F;
            const bool addsEvidence =
                updatesAssignment
                && (!previousAssignment
                    || previousAssignment->cluster != selected
                    || (!previousAssignment->hadUsableEmbedding
                        && usableEmbedding));
            const bool initializesEmptyCluster =
                usableEmbedding && cluster.centroid.empty();
            if (addsEvidence && !cluster.persistedProfile
                && (clusterIsCompatible(cluster, hypothesis)
                    || initializesEmptyCluster)) {
                if (initializesEmptyCluster) {
                    cluster.embeddingModelId =
                        hypothesis.speakerEmbeddingModel;
                }
                updateCluster(
                    cluster,
                    hypothesis.speakerEmbedding,
                    kSpeakerMatchSimilarity,
                    &hypothesis.stableId);
                similarity = clusterSimilarity(
                    cluster,
                    hypothesis.speakerEmbedding);
            }
            if (updatesAssignment && previousAssignment
                && previousAssignment->cluster != selected) {
                auto &previous = clusters_[previousAssignment->cluster];
                const bool stillReferenced = std::any_of(
                    assignments_.begin(),
                    assignments_.end(),
                    [&](const auto &entry) {
                        return entry.second.cluster
                            == previousAssignment->cluster;
                    });
                const bool stillHasEvidence = std::any_of(
                    assignments_.begin(),
                    assignments_.end(),
                    [&](const auto &entry) {
                        return entry.second.cluster
                                == previousAssignment->cluster
                            && entry.second.hadUsableEmbedding;
                    });
                if (!previous.persistedProfile && !stillHasEvidence) {
                    previous.retired = !stillReferenced;
                    previous.embeddingModelId.clear();
                    previous.centroid.clear();
                    previous.prototypes.clear();
                    previous.soleEvidenceStableId.reset();
                    previous.hasMultipleEvidenceIds = false;
                    previous.observations = 0;
                    if (pendingCandidate_
                        && pendingCandidate_->fallbackCluster
                            == previousAssignment->cluster) {
                        pendingCandidate_.reset();
                    }
                }
            }
            turn.speakerId = cluster.speakerId;
            turn.speakerLabel = cluster.label;
            turn.confidence = !usableEmbedding
                ? 0.60F
                : std::clamp(
                    (std::max(similarity, 0.50F) - 0.50F) * 2.0F,
                    0.50F,
                    0.95F);
            if (updatesAssignment) {
                previousCluster_ = selected;
                previousEndTimeNs_ = hypothesis.endTimeNs;
                forceDifferentSpeaker_ = hypothesis.speakerTurnAfter;
            }
        }
        turns.push_back(std::move(turn));
    }
    return turns;
}

Expected<std::vector<SpeakerTurn>>
AcousticDiarizationBackend::flush()
{
    if (!prepared_) {
        return Error{
            LS_INVALID_STATE,
            "acoustic diarization is not prepared"};
    }
    return std::vector<SpeakerTurn>{};
}

Expected<std::unique_ptr<IDiarizationBackend>>
createDiarizationBackend(std::string_view backendId)
{
    if (backendId.empty() || backendId == "source-aware") {
        return std::unique_ptr<IDiarizationBackend>(
            std::make_unique<SourceDiarizationBackend>());
    }
    if (backendId == "acoustic-clustering") {
        return std::unique_ptr<IDiarizationBackend>(
            std::make_unique<AcousticDiarizationBackend>());
    }
    return Error{
        LS_BACKEND_UNAVAILABLE,
        "requested diarization backend is not available"};
}

} // namespace localscribe
