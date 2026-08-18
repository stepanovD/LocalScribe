#include "TestSupport.hpp"

#include "inference/FixtureAsrBackend.hpp"
#include "inference/WhisperChunker.hpp"

#include <cstdint>
#include <vector>

using namespace localscribe;

LS_TEST(whisper_chunker_preserves_discontinuity_on_the_first_new_chunk)
{
    WhisperChunker chunker(10, 10);

    const std::vector<float> before(6, 0.25F);
    const auto buffered = chunker.accept(
        22,
        1'000'000'000,
        before,
        false,
        false);
    LS_CHECK(buffered.empty());

    const std::vector<float> after(10, 0.50F);
    const auto boundary = chunker.accept(
        22,
        3'000'000'000,
        after,
        true,
        false);
    LS_CHECK_EQ(boundary.size(), std::size_t{2});
    LS_CHECK(!boundary[0].discontinuityBefore);
    LS_CHECK(boundary[1].discontinuityBefore);
    LS_CHECK_EQ(boundary[0].startTimeNs, std::int64_t{1'000'000'000});
    LS_CHECK_EQ(boundary[1].startTimeNs, std::int64_t{3'000'000'000});

    const std::vector<float> continued(10, 0.75F);
    const auto next = chunker.accept(
        22,
        4'000'000'000,
        continued,
        false,
        false);
    LS_CHECK_EQ(next.size(), std::size_t{1});
    LS_CHECK(!next[0].discontinuityBefore);
}

LS_TEST(whisper_chunker_emits_a_boundary_before_a_deferred_tail)
{
    WhisperChunker chunker(10, 10);
    const std::vector<float> tail(4, 0.50F);

    const auto boundary = chunker.accept(
        22,
        7'000'000'000,
        tail,
        true,
        false);
    LS_CHECK_EQ(boundary.size(), std::size_t{1});
    LS_CHECK(boundary[0].discontinuityBefore);
    LS_CHECK_EQ(boundary[0].startTimeNs, std::int64_t{7'000'000'000});
    LS_CHECK(boundary[0].samples.empty());

    const auto flushed = chunker.flush();
    LS_CHECK_EQ(flushed.size(), std::size_t{1});
    LS_CHECK(!flushed[0].discontinuityBefore);
    LS_CHECK_EQ(flushed[0].startTimeNs, std::int64_t{7'000'000'000});
    LS_CHECK_EQ(flushed[0].samples.size(), std::size_t{4});
}

LS_TEST(fixture_asr_emits_a_silent_timeline_batch)
{
    FixtureAsrBackend backend;
    LS_CHECK(backend.prepare(AsrConfiguration{
        {},
        LS_LANGUAGE_MODE_ENGLISH}));

    AudioWindow audio;
    audio.sourceId = 22;
    audio.monotonicTimeNs = 1'000'000'000;
    audio.sampleRateHz = 4;
    audio.channelCount = 1;
    audio.frameCount = 4;
    audio.flags = LS_AUDIO_FLAG_DISCONTINUITY;
    audio.samples = std::vector<float>(4, 0.0F);

    const auto accepted = backend.accept(audio);
    LS_CHECK(accepted);
    LS_CHECK_EQ(accepted.value().size(), std::size_t{1});
    const auto &batch = accepted.value()[0];
    LS_CHECK_EQ(batch.sourceId, std::uint64_t{22});
    LS_CHECK_EQ(batch.processedStartTimeNs, std::int64_t{1'000'000'000});
    LS_CHECK_EQ(batch.finalizedThroughTimeNs, std::int64_t{2'000'000'000});
    LS_CHECK(batch.discontinuityBefore);
    LS_CHECK(batch.hypotheses.empty());
}

LS_TEST(fixture_asr_wraps_its_final_segment_in_the_processed_batch)
{
    FixtureAsrBackend backend;
    LS_CHECK(backend.prepare(AsrConfiguration{
        {},
        LS_LANGUAGE_MODE_ENGLISH}));

    AudioWindow audio;
    audio.sourceId = 22;
    audio.sequenceNumber = 9;
    audio.monotonicTimeNs = 5'000'000'000;
    audio.sampleRateHz = 8;
    audio.channelCount = 1;
    audio.frameCount = 4;
    audio.samples = std::vector<float>(4, 0.009F);

    const auto accepted = backend.accept(audio);
    LS_CHECK(accepted);
    LS_CHECK_EQ(accepted.value().size(), std::size_t{1});
    const auto &batch = accepted.value()[0];
    LS_CHECK_EQ(batch.processedStartTimeNs, std::int64_t{5'000'000'000});
    LS_CHECK_EQ(batch.finalizedThroughTimeNs, std::int64_t{5'500'000'000});
    LS_CHECK(!batch.discontinuityBefore);
    LS_CHECK_EQ(batch.hypotheses.size(), std::size_t{1});
    LS_CHECK_EQ(batch.hypotheses[0].sourceId, std::uint64_t{22});
    LS_CHECK_EQ(
        batch.hypotheses[0].startTimeNs,
        batch.processedStartTimeNs);
    LS_CHECK_EQ(
        batch.hypotheses[0].endTimeNs,
        batch.finalizedThroughTimeNs);
}

LS_TEST(fixture_asr_decodes_a_cue_from_its_aggregated_callback)
{
    FixtureAsrBackend backend;
    LS_CHECK(backend.prepare(AsrConfiguration{
        {},
        LS_LANGUAGE_MODE_ENGLISH}));

    AudioWindow audio;
    audio.sourceId = 22;
    audio.sequenceNumber = 100;
    audio.monotonicTimeNs = 5'000'000'000;
    audio.sampleRateHz = 4;
    audio.channelCount = 1;
    audio.frameCount = 12;
    audio.callbackCount = 3;
    audio.samples = std::vector<float>(12, 0.0F);
    audio.samples[8] = -0.042F;

    const auto accepted = backend.accept(audio);
    LS_CHECK(accepted);
    LS_CHECK_EQ(accepted.value().size(), std::size_t{1});
    const auto &batch = accepted.value()[0];
    LS_CHECK_EQ(batch.processedStartTimeNs, std::int64_t{5'000'000'000});
    LS_CHECK_EQ(batch.finalizedThroughTimeNs, std::int64_t{8'000'000'000});
    LS_CHECK_EQ(batch.hypotheses.size(), std::size_t{1});
    const auto &hypothesis = batch.hypotheses[0];
    LS_CHECK_EQ(hypothesis.text, std::string{"fixture cue 42 revised"});
    LS_CHECK_EQ(hypothesis.revision, std::uint32_t{2});
    LS_CHECK_EQ(hypothesis.startTimeNs, std::int64_t{7'000'000'000});
    LS_CHECK_EQ(hypothesis.endTimeNs, std::int64_t{8'000'000'000});
}

LS_TEST(fixture_speakers_requires_test_authorization_and_encodes_voice_evidence)
{
    const auto denied = createAsrBackend("fixture-speakers", false);
    LS_CHECK(!denied);
    LS_CHECK_EQ(denied.error().code, LS_BACKEND_UNAVAILABLE);

    auto allowed = createAsrBackend("fixture-speakers", true);
    LS_CHECK(allowed);
    LS_CHECK_EQ(
        allowed.value()->info().id,
        std::string{"fixture-speakers"});
    LS_CHECK(allowed.value()->info().testOnly);
    LS_CHECK(allowed.value()->prepare(AsrConfiguration{
        {},
        LS_LANGUAGE_MODE_ENGLISH}));

    AudioWindow audio;
    audio.sourceId = 22;
    audio.sequenceNumber = 11;
    audio.monotonicTimeNs = 9'000'000'000;
    audio.sampleRateHz = 5;
    audio.channelCount = 1;
    audio.frameCount = 5;
    audio.flags = LS_AUDIO_FLAG_DISCONTINUITY;
    audio.samples = {-0.011F, 0.25F, -0.50F, 0.75F, 0.80F};

    const auto accepted = allowed.value()->accept(audio);
    LS_CHECK(accepted);
    LS_CHECK_EQ(accepted.value().size(), std::size_t{1});
    const auto &batch = accepted.value()[0];
    LS_CHECK_EQ(batch.finalizedThroughTimeNs, std::int64_t{10'000'000'000});
    LS_CHECK(batch.discontinuityBefore);
    LS_CHECK_EQ(batch.hypotheses.size(), std::size_t{1});
    const auto &hypothesis = batch.hypotheses[0];
    LS_CHECK_EQ(hypothesis.revision, std::uint32_t{2});
    LS_CHECK_EQ(
        hypothesis.speakerEmbeddingModel,
        std::string{"fixture-speaker-v1"});
    LS_CHECK_EQ(hypothesis.speakerEmbedding.size(), std::size_t{3});
    LS_CHECK_EQ(hypothesis.speakerEmbedding[0], 0.25F);
    LS_CHECK_EQ(hypothesis.speakerEmbedding[1], -0.50F);
    LS_CHECK_EQ(hypothesis.speakerEmbedding[2], 0.75F);
    LS_CHECK(hypothesis.speakerTurnAfter);
}

LS_TEST(fixture_speakers_reads_voice_evidence_from_the_cue_callback)
{
    auto backend = createAsrBackend("fixture-speakers", true);
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(AsrConfiguration{
        {},
        LS_LANGUAGE_MODE_ENGLISH}));

    AudioWindow audio;
    audio.sourceId = 22;
    audio.sequenceNumber = 20;
    audio.monotonicTimeNs = 2'000'000'000;
    audio.sampleRateHz = 5;
    audio.channelCount = 1;
    audio.frameCount = 10;
    audio.callbackCount = 2;
    audio.samples = std::vector<float>(10, 0.0F);
    audio.samples[5] = 0.077F;
    audio.samples[6] = 0.25F;
    audio.samples[7] = -0.50F;
    audio.samples[8] = 0.75F;
    audio.samples[9] = 0.80F;

    const auto accepted = backend.value()->accept(audio);
    LS_CHECK(accepted);
    LS_CHECK_EQ(accepted.value().size(), std::size_t{1});
    const auto &batch = accepted.value()[0];
    LS_CHECK_EQ(batch.hypotheses.size(), std::size_t{1});
    const auto &hypothesis = batch.hypotheses[0];
    LS_CHECK_EQ(hypothesis.text, std::string{"fixture cue 77"});
    LS_CHECK_EQ(hypothesis.startTimeNs, std::int64_t{3'000'000'000});
    LS_CHECK_EQ(hypothesis.endTimeNs, std::int64_t{4'000'000'000});
    LS_CHECK_EQ(
        hypothesis.speakerEmbeddingModel,
        std::string{"fixture-speaker-v1"});
    LS_CHECK_EQ(hypothesis.speakerEmbedding.size(), std::size_t{3});
    LS_CHECK_EQ(hypothesis.speakerEmbedding[0], 0.25F);
    LS_CHECK_EQ(hypothesis.speakerEmbedding[1], -0.50F);
    LS_CHECK_EQ(hypothesis.speakerEmbedding[2], 0.75F);
    LS_CHECK(hypothesis.speakerTurnAfter);
}

LS_TEST(fixture_speakers_preserves_empty_silence_batches)
{
    auto backend = createAsrBackend("fixture-speakers", true);
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(AsrConfiguration{
        {},
        LS_LANGUAGE_MODE_ENGLISH}));

    AudioWindow audio;
    audio.sourceId = 22;
    audio.monotonicTimeNs = 12'000'000'000;
    audio.sampleRateHz = 4;
    audio.channelCount = 1;
    audio.frameCount = 4;
    audio.samples = std::vector<float>(4, 0.0F);

    const auto accepted = backend.value()->accept(audio);
    LS_CHECK(accepted);
    LS_CHECK_EQ(accepted.value().size(), std::size_t{1});
    LS_CHECK_EQ(
        accepted.value()[0].finalizedThroughTimeNs,
        std::int64_t{13'000'000'000});
    LS_CHECK(accepted.value()[0].hypotheses.empty());
}
