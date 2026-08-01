#include "SourceDiarizationBackend.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace localscribe {
namespace {

constexpr std::size_t kMaximumRemoteSpeakers = 8;
constexpr std::size_t kMaximumClusterPrototypes = 6;
constexpr std::size_t kCandidateConfirmationObservations = 2;
constexpr float kSpeakerMatchSimilarity = 0.87F;
constexpr float kContinuousVoiceAdaptationSimilarity = 0.85F;
constexpr float kAfterPauseVoiceAdaptationSimilarity = 0.89F;
constexpr float kCandidateConsistencySimilarity = 0.92F;
constexpr float kConsecutiveSpeakerSimilarity = 0.84F;
constexpr float kConsecutiveSpeakerMargin = 0.04F;
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
        dot += static_cast<long double>(left[index]) * right[index];
        leftMagnitude +=
            static_cast<long double>(left[index]) * left[index];
        rightMagnitude +=
            static_cast<long double>(right[index]) * right[index];
    }
    if (leftMagnitude <= 1.0e-12L || rightMagnitude <= 1.0e-12L) {
        return -1.0F;
    }
    return std::clamp(
        static_cast<float>(
            dot / std::sqrt(leftMagnitude * rightMagnitude)),
        -1.0F,
        1.0F);
}

void normalize(std::vector<float> &values)
{
    long double magnitude = 0.0L;
    for (const float value : values) {
        magnitude += static_cast<long double>(value) * value;
    }
    if (magnitude <= 1.0e-12L) {
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
        local.value() ? 1u : (0x8000000000000000ULL | audio.sourceId);
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
    return BackendInfo{"acoustic-clustering", "3", false};
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
    forceDifferentSpeaker_ = false;
    prepared_ = true;
    return success();
}

std::size_t AcousticDiarizationBackend::createCluster(
    std::span<const float> embedding)
{
    SpeakerCluster cluster;
    const std::size_t ordinal = clusters_.size() + 1;
    cluster.speakerId =
        0x8000000000000000ULL | static_cast<std::uint64_t>(ordinal);
    cluster.label = ordinal == 1
        ? configuration_.remoteSpeakerName
        : "Speaker " + std::to_string(ordinal);
    cluster.centroid.assign(embedding.begin(), embedding.end());
    normalize(cluster.centroid);
    if (!cluster.centroid.empty()) {
        cluster.prototypes.push_back(cluster.centroid);
    }
    cluster.observations = 0;
    clusters_.push_back(std::move(cluster));
    return clusters_.size() - 1;
}

std::size_t AcousticDiarizationBackend::selectCluster(
    const AsrHypothesis &hypothesis)
{
    if (const auto existing = assignments_.find(hypothesis.stableId);
        existing != assignments_.end()) {
        return existing->second;
    }
    if (clusters_.empty()) {
        pendingCandidate_.reset();
        return createCluster(hypothesis.speakerEmbedding);
    }

    if (hypothesis.speakerEmbedding.empty()) {
        pendingCandidate_.reset();
        if (forceDifferentSpeaker_) {
            if (clusters_.size() == 1
                && clusters_.size() < kMaximumRemoteSpeakers) {
                return createCluster({});
            }
            if (previousCluster_ && clusters_.size() > 1) {
                return (*previousCluster_ + 1) % clusters_.size();
            }
        }
        return previousCluster_.value_or(0);
    }

    std::size_t bestCluster = 0;
    float bestSimilarity = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < clusters_.size(); ++index) {
        if (forceDifferentSpeaker_ && previousCluster_ == index) {
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

    if (forceDifferentSpeaker_) {
        pendingCandidate_.reset();
        if (std::isfinite(bestSimilarity)
            && bestSimilarity >= kSpeakerMatchSimilarity) {
            return bestCluster;
        }
        if (clusters_.size() < kMaximumRemoteSpeakers) {
            return createCluster(hypothesis.speakerEmbedding);
        }
        return bestCluster;
    }

    if (!forceDifferentSpeaker_ && previousCluster_
        && clusters_.size() > 1
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
        previousCluster_.value_or(bestCluster);
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
            adaptationSimilarity);
        return fallbackCluster;
    }
    if (clusters_.size() < kMaximumRemoteSpeakers) {
        return createCluster(confirmed);
    }
    return bestCluster;
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

void AcousticDiarizationBackend::updateCluster(
    SpeakerCluster &cluster,
    std::span<const float> embedding,
    float minimumSimilarity)
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
    if (cluster.centroid.empty()
        || cluster.centroid.size() != normalized.size()) {
        cluster.centroid = normalized;
        cluster.prototypes = {normalized};
        cluster.observations = 1;
        return;
    }

    const float similarity = clusterSimilarity(cluster, normalized);
    if (cluster.observations != 0
        && similarity < minimumSimilarity) {
        return;
    }
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
            const bool existingAssignment =
                assignments_.contains(hypothesis.stableId);
            const std::size_t selected = selectCluster(hypothesis);
            assignments_[hypothesis.stableId] = selected;
            auto &cluster = clusters_[selected];
            const float similarity = clusterSimilarity(
                cluster,
                hypothesis.speakerEmbedding);
            if (!existingAssignment) {
                updateCluster(
                    cluster,
                    hypothesis.speakerEmbedding,
                    kSpeakerMatchSimilarity);
            }
            turn.speakerId = cluster.speakerId;
            turn.speakerLabel = cluster.label;
            turn.confidence = hypothesis.speakerEmbedding.empty()
                ? 0.60F
                : std::clamp(
                    (std::max(similarity, 0.50F) - 0.50F) * 2.0F,
                    0.50F,
                    0.95F);
            previousCluster_ = selected;
            previousEndTimeNs_ = hypothesis.endTimeNs;
            forceDifferentSpeaker_ = hypothesis.speakerTurnAfter;
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
