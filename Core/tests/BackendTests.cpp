#include "TestSupport.hpp"

#include "../src/common/TranscriptLanguagePolicy.hpp"
#include "../src/inference/AsrBackend.hpp"
#include "../src/inference/DiarizationBackend.hpp"
#include "../src/inference/SpeakerFeatureExtractor.hpp"
#include "../src/inference/WhisperChunker.hpp"
#include "../src/inference/WhisperSpeechGate.hpp"
#include "../src/inference/WhisperStreamingResampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <vector>

using namespace localscribe;

namespace {

void appendSamples(
    std::vector<float> &destination,
    const std::vector<ResampledAudioBlock> &blocks)
{
    for (const auto &block : blocks) {
        destination.insert(
            destination.end(),
            block.samples.begin(),
            block.samples.end());
    }
}

std::vector<float> speakerEmbeddingAtAngle(float radians)
{
    return {std::cos(radians), std::sin(radians)};
}

} // namespace

LS_TEST(fixture_backend_requires_explicit_test_configuration)
{
    auto denied = createAsrBackend("fixture", false);
    LS_CHECK(!denied);
    LS_CHECK_EQ(denied.error().code, LS_BACKEND_UNAVAILABLE);

    auto unknown = createAsrBackend("not-installed", true);
    LS_CHECK(!unknown);
    LS_CHECK_EQ(unknown.error().code, LS_BACKEND_UNAVAILABLE);
}

LS_TEST(fixture_backend_is_deterministic_and_revisioned)
{
    auto backend = createAsrBackend("fixture", true);
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        AsrConfiguration{"", LS_LANGUAGE_MODE_RUSSIAN_ENGLISH}));

    AudioWindow window;
    window.sourceId = 7;
    window.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    window.sequenceNumber = 10;
    window.monotonicTimeNs = 5'000'000'000;
    window.sampleRateHz = 16'000;
    window.channelCount = 1;
    window.frameCount = 160;
    window.samples.assign(160, 0.042F);

    auto first = backend.value()->accept(window);
    LS_CHECK(first);
    LS_CHECK_EQ(first.value().size(), std::size_t{1});
    LS_CHECK_EQ(first.value()[0].text, std::string{"fixture cue 42"});
    LS_CHECK_EQ(first.value()[0].revision, std::uint32_t{1});
    LS_CHECK(first.value()[0].final);
    LS_CHECK_EQ(first.value()[0].language, std::string{"en"});

    window.sequenceNumber = 11;
    window.samples.front() = -0.042F;
    auto revision = backend.value()->accept(window);
    LS_CHECK(revision);
    LS_CHECK_EQ(revision.value()[0].stableId, first.value()[0].stableId);
    LS_CHECK_EQ(revision.value()[0].revision, std::uint32_t{2});
    LS_CHECK_EQ(
        revision.value()[0].text,
        std::string{"fixture cue 42 revised"});
}

LS_TEST(transcript_language_policy_allows_only_russian_or_english)
{
    LS_CHECK_EQ(
        TranscriptLanguagePolicy::select(
            LS_LANGUAGE_MODE_RUSSIAN_ENGLISH,
            "Привіт, як справи?",
            "uk"),
        std::string{"ru"});
    LS_CHECK_EQ(
        TranscriptLanguagePolicy::select(
            LS_LANGUAGE_MODE_RUSSIAN_ENGLISH,
            "Dzień dobry",
            "pl"),
        std::string{"en"});
    LS_CHECK_EQ(
        TranscriptLanguagePolicy::select(
            LS_LANGUAGE_MODE_RUSSIAN_ENGLISH,
            "123",
            "de",
            "ru"),
        std::string{"ru"});
    LS_CHECK_EQ(
        TranscriptLanguagePolicy::select(
            LS_LANGUAGE_MODE_RUSSIAN,
            "English text",
            "en"),
        std::string{"ru"});
    LS_CHECK_EQ(
        TranscriptLanguagePolicy::select(
            LS_LANGUAGE_MODE_ENGLISH,
            "Русский текст",
            "ru"),
        std::string{"en"});
}

LS_TEST(source_aware_diarization_preserves_source_identity)
{
    auto backend = createDiarizationBackend("source-aware");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1"}));

    AsrHypothesis hypothesis;
    hypothesis.sourceId = 11;
    hypothesis.final = true;
    hypothesis.revision = 1;
    AudioWindow microphone;
    microphone.sourceId = 11;
    /*
     * The configured source ID is authoritative. A stale/mistyped kind must
     * never make microphone speech remote or system speech local.
     */
    microphone.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    auto local = backend.value()->assign(
        microphone,
        std::span<const AsrHypothesis>(&hypothesis, 1));
    LS_CHECK(local);
    LS_CHECK_EQ(local.value()[0].speakerId, std::uint64_t{1});
    LS_CHECK_EQ(local.value()[0].speakerLabel, std::string{"Me"});

    hypothesis.sourceId = 22;
    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_MICROPHONE;
    auto remote = backend.value()->assign(
        system,
        std::span<const AsrHypothesis>(&hypothesis, 1));
    LS_CHECK(remote);
    LS_CHECK(remote.value()[0].speakerId != 1);
    LS_CHECK_EQ(remote.value()[0].speakerLabel, std::string{"Speaker 1"});
}

LS_TEST(diarization_rejects_hypotheses_from_a_different_source)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1"}));

    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    AsrHypothesis mismatched;
    mismatched.sourceId = 11;
    mismatched.final = true;
    const auto result = backend.value()->assign(
        system,
        std::span<const AsrHypothesis>(&mismatched, 1));
    LS_CHECK(!result);
    LS_CHECK_EQ(result.error().code, LS_INVALID_ARGUMENT);
}

LS_TEST(acoustic_diarization_clusters_remote_voices)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1"}));

    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;

    AsrHypothesis first;
    first.stableId[15] = 1;
    first.sourceId = 22;
    first.startTimeNs = 1'000'000'000;
    first.endTimeNs = 2'000'000'000;
    first.revision = 1;
    first.final = true;
    first.speakerEmbedding = {1.0F, 0.0F};
    auto firstTurn = backend.value()->assign(system, {&first, 1});
    LS_CHECK(firstTurn);
    LS_CHECK_EQ(
        firstTurn.value()[0].speakerLabel,
        std::string{"Speaker 1"});

    AsrHypothesis second = first;
    second.stableId[15] = 2;
    second.startTimeNs = 3'000'000'000;
    second.endTimeNs = 4'000'000'000;
    second.speakerEmbedding = {0.0F, 1.0F};
    auto secondCandidate = backend.value()->assign(system, {&second, 1});
    LS_CHECK(secondCandidate);
    LS_CHECK_EQ(
        secondCandidate.value()[0].speakerId,
        firstTurn.value()[0].speakerId);

    AsrHypothesis secondConfirmed = second;
    secondConfirmed.stableId[15] = 3;
    secondConfirmed.startTimeNs = 4'100'000'000;
    secondConfirmed.endTimeNs = 5'000'000'000;
    secondConfirmed.speakerEmbedding = {0.01F, 0.99F};
    auto secondTurn =
        backend.value()->assign(system, {&secondConfirmed, 1});
    LS_CHECK(secondTurn);
    LS_CHECK_EQ(
        secondTurn.value()[0].speakerLabel,
        std::string{"Speaker 2"});
    LS_CHECK(
        secondTurn.value()[0].speakerId
        != firstTurn.value()[0].speakerId);

    AsrHypothesis firstAgain = first;
    firstAgain.stableId[15] = 4;
    firstAgain.startTimeNs = 6'000'000'000;
    firstAgain.endTimeNs = 7'000'000'000;
    firstAgain.speakerEmbedding = {0.99F, 0.01F};
    auto firstAgainTurn =
        backend.value()->assign(system, {&firstAgain, 1});
    LS_CHECK(firstAgainTurn);
    LS_CHECK_EQ(
        firstAgainTurn.value()[0].speakerId,
        firstTurn.value()[0].speakerId);
}

LS_TEST(acoustic_diarization_never_labels_system_audio_as_local)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Me"}));

    AsrHypothesis hypothesis;
    hypothesis.stableId[15] = 1;
    hypothesis.sourceId = 22;
    hypothesis.final = true;
    hypothesis.speakerEmbedding = {1.0F, 0.0F};
    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_MICROPHONE;
    const auto remote = backend.value()->assign(
        system,
        std::span<const AsrHypothesis>(&hypothesis, 1));
    LS_CHECK(remote);
    LS_CHECK(remote.value()[0].speakerId != std::uint64_t{1});
    LS_CHECK(remote.value()[0].speakerLabel != std::string{"Me"});

    hypothesis.sourceId = 11;
    hypothesis.stableId[15] = 2;
    AudioWindow microphone;
    microphone.sourceId = 11;
    microphone.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    const auto local = backend.value()->assign(
        microphone,
        std::span<const AsrHypothesis>(&hypothesis, 1));
    LS_CHECK(local);
    LS_CHECK_EQ(local.value()[0].speakerId, std::uint64_t{1});
    LS_CHECK_EQ(local.value()[0].speakerLabel, std::string{"Me"});
}

LS_TEST(acoustic_diarization_requires_repeated_novel_speaker_evidence)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1"}));

    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    const auto hypothesis = [](std::uint8_t id,
                               float angle,
                               std::int64_t startTimeNs) {
        AsrHypothesis value;
        value.stableId[15] = id;
        value.sourceId = 22;
        value.startTimeNs = startTimeNs;
        value.endTimeNs = startTimeNs + 800'000'000;
        value.revision = 1;
        value.final = true;
        value.speakerEmbedding = speakerEmbeddingAtAngle(angle);
        return value;
    };

    const auto first = hypothesis(1, 0.00F, 1'000'000'000);
    /*
     * cos(0.60) ~= 0.825: one novel segment remains provisional; a second
     * consistent segment confirms the new remote speaker.
     */
    const auto secondCandidate =
        hypothesis(2, 0.60F, 2'000'000'000);
    const auto secondConfirmed =
        hypothesis(3, 0.62F, 3'000'000'000);
    const auto firstAgain =
        hypothesis(4, 0.03F, 4'000'000'000);
    const auto secondAgain =
        hypothesis(5, 0.58F, 5'000'000'000);

    const auto firstTurn = backend.value()->assign(system, {&first, 1});
    const auto candidateTurn =
        backend.value()->assign(system, {&secondCandidate, 1});
    const auto secondTurn =
        backend.value()->assign(system, {&secondConfirmed, 1});
    const auto firstAgainTurn =
        backend.value()->assign(system, {&firstAgain, 1});
    const auto secondAgainTurn =
        backend.value()->assign(system, {&secondAgain, 1});
    LS_CHECK(firstTurn);
    LS_CHECK(candidateTurn);
    LS_CHECK(secondTurn);
    LS_CHECK(firstAgainTurn);
    LS_CHECK(secondAgainTurn);
    LS_CHECK_EQ(
        candidateTurn.value()[0].speakerId,
        firstTurn.value()[0].speakerId);
    LS_CHECK(
        firstTurn.value()[0].speakerId
        != secondTurn.value()[0].speakerId);
    LS_CHECK_EQ(
        firstAgainTurn.value()[0].speakerId,
        firstTurn.value()[0].speakerId);
    LS_CHECK_EQ(
        secondAgainTurn.value()[0].speakerId,
        secondTurn.value()[0].speakerId);
}

LS_TEST(acoustic_diarization_absorbs_consistent_gray_zone_voice_variation)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1"}));

    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    const auto hypothesis = [](std::uint8_t id,
                               float angle,
                               std::int64_t startTimeNs) {
        AsrHypothesis value;
        value.stableId[15] = id;
        value.sourceId = 22;
        value.startTimeNs = startTimeNs;
        value.endTimeNs = startTimeNs + 800'000'000;
        value.revision = 1;
        value.final = true;
        value.speakerEmbedding = speakerEmbeddingAtAngle(angle);
        return value;
    };
    const auto original = hypothesis(1, 0.00F, 1'000'000'000);
    const auto variationOne = hypothesis(2, 0.52F, 2'000'000'000);
    const auto variationTwo = hypothesis(3, 0.54F, 3'000'000'000);
    const auto variationThree = hypothesis(4, 0.55F, 4'000'000'000);

    const auto originalTurn =
        backend.value()->assign(system, {&original, 1});
    const auto firstVariationTurn =
        backend.value()->assign(system, {&variationOne, 1});
    const auto secondVariationTurn =
        backend.value()->assign(system, {&variationTwo, 1});
    const auto thirdVariationTurn =
        backend.value()->assign(system, {&variationThree, 1});
    LS_CHECK(originalTurn);
    LS_CHECK(firstVariationTurn);
    LS_CHECK(secondVariationTurn);
    LS_CHECK(thirdVariationTurn);
    LS_CHECK_EQ(
        firstVariationTurn.value()[0].speakerId,
        originalTurn.value()[0].speakerId);
    LS_CHECK_EQ(
        secondVariationTurn.value()[0].speakerId,
        originalTurn.value()[0].speakerId);
    LS_CHECK_EQ(
        thirdVariationTurn.value()[0].speakerId,
        originalTurn.value()[0].speakerId);
}

LS_TEST(acoustic_diarization_can_confirm_a_similar_voice_after_a_pause)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1"}));

    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    const auto hypothesis = [](std::uint8_t id,
                               float angle,
                               std::int64_t startTimeNs) {
        AsrHypothesis value;
        value.stableId[15] = id;
        value.sourceId = 22;
        value.startTimeNs = startTimeNs;
        value.endTimeNs = startTimeNs + 800'000'000;
        value.revision = 1;
        value.final = true;
        value.speakerEmbedding = speakerEmbeddingAtAngle(angle);
        return value;
    };
    const auto original = hypothesis(1, 0.00F, 1'000'000'000);
    const auto candidate = hypothesis(2, 0.52F, 5'000'000'000);
    const auto confirmation = hypothesis(3, 0.54F, 6'000'000'000);

    const auto originalTurn =
        backend.value()->assign(system, {&original, 1});
    const auto candidateTurn =
        backend.value()->assign(system, {&candidate, 1});
    const auto confirmedTurn =
        backend.value()->assign(system, {&confirmation, 1});
    LS_CHECK(originalTurn);
    LS_CHECK(candidateTurn);
    LS_CHECK(confirmedTurn);
    LS_CHECK_EQ(
        candidateTurn.value()[0].speakerId,
        originalTurn.value()[0].speakerId);
    LS_CHECK(
        confirmedTurn.value()[0].speakerId
        != originalTurn.value()[0].speakerId);
}

LS_TEST(acoustic_diarization_ignores_a_single_acoustic_outlier)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1"}));

    AudioWindow system;
    system.sourceId = 22;
    system.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;
    const auto hypothesis = [](std::uint8_t id,
                               float angle,
                               std::int64_t startTimeNs) {
        AsrHypothesis value;
        value.stableId[15] = id;
        value.sourceId = 22;
        value.startTimeNs = startTimeNs;
        value.endTimeNs = startTimeNs + 800'000'000;
        value.revision = 1;
        value.final = true;
        value.speakerEmbedding = speakerEmbeddingAtAngle(angle);
        return value;
    };
    const auto original = hypothesis(1, 0.00F, 1'000'000'000);
    const auto outlier = hypothesis(2, 1.00F, 2'000'000'000);
    const auto recovered = hypothesis(3, 0.02F, 3'000'000'000);

    const auto originalTurn =
        backend.value()->assign(system, {&original, 1});
    const auto outlierTurn =
        backend.value()->assign(system, {&outlier, 1});
    const auto recoveredTurn =
        backend.value()->assign(system, {&recovered, 1});
    LS_CHECK(originalTurn);
    LS_CHECK(outlierTurn);
    LS_CHECK(recoveredTurn);
    LS_CHECK_EQ(
        outlierTurn.value()[0].speakerId,
        originalTurn.value()[0].speakerId);
    LS_CHECK_EQ(
        recoveredTurn.value()[0].speakerId,
        originalTurn.value()[0].speakerId);
}

LS_TEST(acoustic_diarization_honors_tinydiarize_turns_without_embedding)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(DiarizationConfiguration{}));

    AudioWindow system;
    system.sourceId = 2;
    system.sourceKind = LS_SOURCE_KIND_SYSTEM_AUDIO;

    AsrHypothesis before;
    before.stableId[15] = 1;
    before.sourceId = 2;
    before.speakerTurnAfter = true;
    auto first = backend.value()->assign(system, {&before, 1});
    LS_CHECK(first);

    AsrHypothesis after = before;
    after.stableId[15] = 2;
    after.speakerTurnAfter = false;
    auto second = backend.value()->assign(system, {&after, 1});
    LS_CHECK(second);
    LS_CHECK(
        first.value()[0].speakerId != second.value()[0].speakerId);
}

LS_TEST(speaker_feature_extractor_distinguishes_spectral_envelopes)
{
    constexpr std::uint32_t sampleRate = 16'000;
    constexpr std::size_t sampleCount = sampleRate;
    std::vector<float> low(sampleCount);
    std::vector<float> high(sampleCount);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const float time =
            static_cast<float>(index) / static_cast<float>(sampleRate);
        low[index] =
            0.3F * std::sin(2.0F * std::numbers::pi_v<float> * 140.0F * time)
            + 0.2F
                * std::sin(
                    2.0F * std::numbers::pi_v<float> * 700.0F * time);
        high[index] =
            0.3F * std::sin(2.0F * std::numbers::pi_v<float> * 240.0F * time)
            + 0.2F
                * std::sin(
                    2.0F * std::numbers::pi_v<float> * 2'100.0F * time);
    }
    const auto lowFeatures =
        SpeakerFeatureExtractor::extract(low, sampleRate);
    const auto highFeatures =
        SpeakerFeatureExtractor::extract(high, sampleRate);
    LS_CHECK(!lowFeatures.empty());
    LS_CHECK_EQ(lowFeatures.size(), highFeatures.size());
    const float similarity = std::inner_product(
        lowFeatures.begin(),
        lowFeatures.end(),
        highFeatures.begin(),
        0.0F);
    LS_CHECK(similarity < 0.82F);
}

LS_TEST(speaker_feature_extractor_ignores_short_unstable_segments)
{
    constexpr std::uint32_t sampleRate = 16'000;
    std::vector<float> samples(2'000);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const float time =
            static_cast<float>(index) / static_cast<float>(sampleRate);
        samples[index] =
            0.3F
            * std::sin(
                2.0F * std::numbers::pi_v<float> * 180.0F * time);
    }
    LS_CHECK(
        SpeakerFeatureExtractor::extract(samples, sampleRate).empty());
}

LS_TEST(whisper_chunker_flushes_before_a_timeline_discontinuity)
{
    WhisperChunker chunker(10, 100);
    const std::array<float, 4> before{0.1F, 0.2F, 0.3F, 0.4F};
    const std::array<float, 3> after{0.5F, 0.6F, 0.7F};

    auto first = chunker.accept(7, 1'000'000'000, before, false, false);
    LS_CHECK(first.empty());
    auto boundary =
        chunker.accept(7, 9'000'000'000, after, true, false);
    LS_CHECK_EQ(boundary.size(), std::size_t{1});
    LS_CHECK_EQ(boundary[0].startTimeNs, std::int64_t{1'000'000'000});
    LS_CHECK_EQ(boundary[0].samples.size(), before.size());

    auto tail = chunker.flush();
    LS_CHECK_EQ(tail.size(), std::size_t{1});
    LS_CHECK_EQ(tail[0].startTimeNs, std::int64_t{9'000'000'000});
    LS_CHECK_EQ(tail[0].samples.size(), after.size());
    LS_CHECK(boundary[0].ordinal != tail[0].ordinal);
}

LS_TEST(whisper_chunker_preserves_end_of_stream_flush)
{
    WhisperChunker chunker(10, 5);
    const std::array<float, 7> samples{
        0.1F,
        0.2F,
        0.3F,
        0.4F,
        0.5F,
        0.6F,
        0.7F};
    auto chunks =
        chunker.accept(3, 2'000'000'000, samples, false, true);
    LS_CHECK_EQ(chunks.size(), std::size_t{2});
    LS_CHECK_EQ(chunks[0].samples.size(), std::size_t{5});
    LS_CHECK_EQ(chunks[1].samples.size(), std::size_t{2});
    LS_CHECK_EQ(
        chunks[1].startTimeNs,
        std::int64_t{2'500'000'000});
    LS_CHECK(chunker.flush().empty());
}

LS_TEST(whisper_chunker_drops_only_short_backpressure_fragments)
{
    WhisperChunker chunker(10, 50, 10);
    const std::array<float, 4> shortBefore{0.1F, 0.2F, 0.3F, 0.4F};
    const std::array<float, 12> meaningfulBefore{
        0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F,
        0.7F, 0.8F, 0.9F, 1.0F, 0.9F, 0.8F};
    const std::array<float, 2> after{0.6F, 0.7F};

    LS_CHECK(chunker.accept(
        1, 0, shortBefore, false, false).empty());
    LS_CHECK(chunker.accept(
        1, 1'000'000'000, after, true, false, true).empty());
    auto shortTail = chunker.flush();
    LS_CHECK_EQ(shortTail.size(), std::size_t{1});
    LS_CHECK_EQ(shortTail[0].samples.size(), after.size());

    LS_CHECK(chunker.accept(
        2, 0, meaningfulBefore, false, false).empty());
    auto boundary = chunker.accept(
        2, 2'000'000'000, after, true, false, true);
    LS_CHECK_EQ(boundary.size(), std::size_t{1});
    LS_CHECK_EQ(boundary[0].samples.size(), meaningfulBefore.size());
}

LS_TEST(local_speech_gate_preserves_context_and_rejects_clear_non_speech)
{
    constexpr std::uint32_t sampleRate = 16'000;
    WhisperSpeechGate gate;
    std::vector<float> silence(sampleRate * 5u, 0.0F);
    LS_CHECK(!gate.shouldTranscribe(1, silence, sampleRate));

    std::vector<float> hiss(sampleRate * 5u);
    for (std::size_t index = 0; index < hiss.size(); ++index) {
        hiss[index] = index % 2 == 0 ? 0.02F : -0.02F;
    }
    LS_CHECK(!gate.shouldTranscribe(1, hiss, sampleRate));

    std::vector<float> speechLike(sampleRate * 5u, 0.0F);
    for (std::size_t index = sampleRate;
         index < sampleRate * 4u;
         ++index) {
        const float time =
            static_cast<float>(index) / static_cast<float>(sampleRate);
        speechLike[index] =
            0.08F * std::sin(
                2.0F * std::numbers::pi_v<float> * 180.0F * time)
            + 0.03F * std::sin(
                2.0F * std::numbers::pi_v<float> * 720.0F * time);
    }
    LS_CHECK(gate.shouldTranscribe(2, speechLike, sampleRate));
}

LS_TEST(streaming_rate_clock_has_no_two_hour_callback_drift)
{
    constexpr std::uint64_t inputRate = 48'000;
    constexpr std::uint64_t outputRate = 16'000;
    constexpr std::uint64_t callbackFrames = 1'024;
    constexpr std::uint64_t inputFrames =
        2u * 60u * 60u * inputRate;
    constexpr std::uint64_t callbackCount =
        inputFrames / callbackFrames;
    static_assert(inputFrames % callbackFrames == 0);

    StreamingSampleRateClock clock(inputRate, outputRate);
    std::uint64_t produced = 0;
    for (std::uint64_t callback = 0; callback < callbackCount; ++callback) {
        produced += clock.advance(callbackFrames);
    }
    const std::uint64_t expected = inputFrames / 3u;
    LS_CHECK_EQ(produced, expected);
    LS_CHECK_EQ(clock.remainder(), std::uint64_t{0});

    const std::uint64_t callbackLocalFloor =
        callbackCount * (callbackFrames / 3u);
    LS_CHECK_EQ(
        produced - callbackLocalFloor,
        std::uint64_t{112'500});

    for (const std::uint64_t commonInputRate :
         std::array<std::uint64_t, 3>{44'100, 48'000, 96'000}) {
        StreamingSampleRateClock commonClock(
            static_cast<std::uint32_t>(commonInputRate),
            static_cast<std::uint32_t>(outputRate));
        std::uint64_t remaining =
            2u * 60u * 60u * commonInputRate;
        std::uint64_t commonProduced = 0;
        while (remaining != 0) {
            const auto callback =
                std::min<std::uint64_t>(callbackFrames, remaining);
            commonProduced += commonClock.advance(callback);
            remaining -= callback;
        }
        LS_CHECK_EQ(
            commonProduced,
            std::uint64_t{2u * 60u * 60u * outputRate});
        LS_CHECK_EQ(commonClock.remainder(), std::uint64_t{0});
    }
}

LS_TEST(whisper_resampler_bounds_rates_and_callback_expansion)
{
    for (const std::uint32_t inputRate :
         std::array<std::uint32_t, 5>{
             8'000,
             16'000,
             44'100,
             48'000,
             96'000}) {
        LS_CHECK(WhisperResamplerInputLimits::supportsRate(inputRate));
        LS_CHECK(WhisperResamplerInputLimits::callbackFits(
            inputRate,
            inputRate * 10u,
            16'000));
        LS_CHECK(!WhisperResamplerInputLimits::callbackFits(
            inputRate,
            inputRate * 10u + 1u,
            16'000));
    }
    LS_CHECK(!WhisperResamplerInputLimits::supportsRate(7'999));
    LS_CHECK(!WhisperResamplerInputLimits::supportsRate(96'001));
    LS_CHECK(!WhisperResamplerInputLimits::callbackFits(
        0,
        1'024,
        16'000));
}

LS_TEST(streaming_resampler_matches_one_buffer_across_many_callbacks)
{
    constexpr std::size_t callbackFrames = 1'024;
    constexpr std::size_t callbackCount = 257;
    constexpr std::size_t totalFrames = callbackFrames * callbackCount;
    constexpr std::uint32_t inputRate = 44'100;
    std::vector<float> input(totalFrames);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] =
            static_cast<float>(
                static_cast<int>(index % 97u) - 48)
            / 48.0F;
    }

    WhisperStreamingResampler contiguous(16'000);
    std::vector<float> expected;
    appendSamples(
        expected,
        contiguous.accept(
            1,
            1'000'000'000,
            inputRate,
            input,
            false,
            true));

    WhisperStreamingResampler chunked(16'000);
    std::vector<float> actual;
    for (std::size_t callback = 0; callback < callbackCount; ++callback) {
        const std::size_t offset = callback * callbackFrames;
        const auto timestamp =
            1'000'000'000
            + static_cast<std::int64_t>(
                offset * 1'000'000'000ULL / inputRate);
        appendSamples(
            actual,
            chunked.accept(
                1,
                timestamp,
                inputRate,
                std::span<const float>(
                    input.data() + offset,
                    callbackFrames),
                false,
                callback + 1u == callbackCount));
    }

    constexpr std::size_t expectedFrames =
        (static_cast<std::uint64_t>(totalFrames) * 16'000u
         + inputRate / 2u)
        / inputRate;
    LS_CHECK_EQ(actual.size(), expectedFrames);
    LS_CHECK_EQ(actual, expected);
    LS_CHECK(chunked.flush().empty());
}

LS_TEST(streaming_resampler_keeps_phase_isolated_per_source)
{
    WhisperStreamingResampler resampler(16);
    const std::array<float, 2> two{1.0F, 1.0F};
    const std::array<float, 1> one{1.0F};

    LS_CHECK(
        resampler.accept(1, 0, 48, two, false, false).empty());
    LS_CHECK(
        resampler.accept(2, 0, 48, one, false, false).empty());

    const auto sourceOne =
        resampler.accept(1, 0, 48, one, false, true);
    const auto sourceTwo =
        resampler.accept(2, 0, 48, two, false, true);
    LS_CHECK_EQ(sourceOne.size(), std::size_t{1});
    LS_CHECK_EQ(sourceTwo.size(), std::size_t{1});
    LS_CHECK_EQ(sourceOne[0].samples.size(), std::size_t{1});
    LS_CHECK_EQ(sourceTwo[0].samples.size(), std::size_t{1});
}

LS_TEST(streaming_resampler_flushes_then_resets_on_discontinuity_and_eos)
{
    WhisperStreamingResampler resampler(16);
    const std::array<float, 5> before{
        1.0F,
        1.0F,
        1.0F,
        1.0F,
        1.0F};
    const std::array<float, 3> after{-1.0F, -1.0F, -1.0F};

    const auto first =
        resampler.accept(7, 1'000'000'000, 48, before, false, false);
    LS_CHECK_EQ(first.size(), std::size_t{1});
    LS_CHECK_EQ(first[0].samples.size(), std::size_t{1});

    const auto boundary =
        resampler.accept(7, 9'000'000'000, 48, after, true, true);
    LS_CHECK_EQ(boundary.size(), std::size_t{2});
    LS_CHECK_EQ(boundary[0].samples.size(), std::size_t{1});
    LS_CHECK(!boundary[0].discontinuityBefore);
    LS_CHECK_EQ(boundary[0].samples[0], 1.0F);
    LS_CHECK(boundary[1].discontinuityBefore);
    LS_CHECK(boundary[1].endOfStream);
    LS_CHECK_EQ(boundary[1].samples.size(), std::size_t{1});
    LS_CHECK(std::fabs(boundary[1].samples[0] + 1.0F) < 0.00001F);
    LS_CHECK(resampler.flush().empty());

    const std::array<float, 3> restarted{0.25F, 0.25F, 0.25F};
    const auto fresh =
        resampler.accept(7, 12'000'000'000, 48, restarted, false, true);
    LS_CHECK_EQ(fresh.size(), std::size_t{1});
    LS_CHECK(!fresh[0].discontinuityBefore);
    LS_CHECK_EQ(fresh[0].samples.size(), std::size_t{1});
    LS_CHECK(std::fabs(fresh[0].samples[0] - 0.25F) < 0.00001F);

    WhisperStreamingResampler finalized(16);
    const auto pending =
        finalized.accept(9, 20'000'000'000, 48, before, false, false);
    LS_CHECK_EQ(pending[0].samples.size(), std::size_t{1});
    const auto flushed = finalized.flush();
    LS_CHECK_EQ(flushed.size(), std::size_t{1});
    LS_CHECK(flushed[0].endOfStream);
    LS_CHECK_EQ(flushed[0].samples.size(), std::size_t{1});
    LS_CHECK(finalized.flush().empty());
}

LS_TEST(streaming_resampler_filters_common_whisper_input_rates)
{
    constexpr double pi = 3.14159265358979323846264338327950288;
    const auto resampledRms = [pi](
                                  std::uint32_t inputRate,
                                  double frequency) {
        const std::size_t inputFrames = inputRate / 2u;
        std::vector<float> input(inputFrames);
        for (std::size_t index = 0; index < input.size(); ++index) {
            input[index] = static_cast<float>(
                std::sin(
                    2.0 * pi * frequency
                    * static_cast<double>(index)
                    / static_cast<double>(inputRate)));
        }
        WhisperStreamingResampler resampler(16'000);
        std::vector<float> output;
        appendSamples(
            output,
            resampler.accept(
                1,
                0,
                inputRate,
                input,
                false,
                true));
        LS_CHECK_EQ(output.size(), std::size_t{8'000});

        constexpr std::size_t warmup = 256;
        double squared = 0.0;
        for (std::size_t index = warmup; index < output.size(); ++index) {
            squared +=
                static_cast<double>(output[index]) * output[index];
        }
        return std::sqrt(
            squared / static_cast<double>(output.size() - warmup));
    };

    for (const std::uint32_t inputRate :
         std::array<std::uint32_t, 3>{44'100, 48'000, 96'000}) {
        const double passband = resampledRms(inputRate, 6'000.0);
        const double stopband = resampledRms(inputRate, 10'000.0);
        LS_CHECK(passband > 0.65);
        LS_CHECK(stopband < 0.03);
        LS_CHECK(stopband < passband * 0.05);
    }
}
