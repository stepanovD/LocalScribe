#include "../src/inference/DiarizationBackend.hpp"
#include "TestSupport.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace localscribe;

namespace {

std::vector<float> embeddingAt(float angle)
{
    return {std::cos(angle), std::sin(angle)};
}

AsrHypothesis remoteHypothesis(
    std::uint8_t id,
    float angle,
    std::int64_t startTimeNs,
    std::int64_t endTimeNs,
    std::uint32_t revision = 1)
{
    AsrHypothesis hypothesis;
    hypothesis.stableId[15] = id;
    hypothesis.sourceId = 22;
    hypothesis.startTimeNs = startTimeNs;
    hypothesis.endTimeNs = endTimeNs;
    hypothesis.final = true;
    hypothesis.revision = revision;
    hypothesis.speakerEmbeddingModel = "test-speaker-v1";
    hypothesis.speakerEmbedding = embeddingAt(angle);
    return hypothesis;
}

AsrTimelineBatch remoteBatch(
    std::int64_t processedStartTimeNs,
    std::int64_t finalizedThroughTimeNs,
    std::vector<AsrHypothesis> hypotheses,
    bool discontinuityBefore = false)
{
    AsrTimelineBatch batch;
    batch.sourceId = 22;
    batch.processedStartTimeNs = processedStartTimeNs;
    batch.finalizedThroughTimeNs = finalizedThroughTimeNs;
    batch.discontinuityBefore = discontinuityBefore;
    batch.hypotheses = std::move(hypotheses);
    return batch;
}

std::unique_ptr<IDiarizationBackend> preparedAcousticBackend()
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    LS_CHECK(backend.value()->prepare(
        DiarizationConfiguration{11, 22, "Me", "Speaker 1", {}}));
    return backend.takeValue();
}

VoiceProfile voiceProfile(
    std::uint64_t profileId,
    std::string name,
    float angle)
{
    VoiceProfile profile;
    profile.profileId = profileId;
    profile.displayName = std::move(name);
    profile.embeddingModelId = "test-speaker-v1";
    profile.centroid = embeddingAt(angle);
    profile.prototypes = {profile.centroid};
    profile.observationCount = 1;
    return profile;
}

std::unique_ptr<IDiarizationBackend> preparedAcousticBackend(
    std::vector<VoiceProfile> profiles)
{
    auto backend = createDiarizationBackend("acoustic-clustering");
    LS_CHECK(backend);
    DiarizationConfiguration configuration{
        11,
        22,
        "Me",
        "Speaker 1",
        {}};
    configuration.voiceProfiles = std::move(profiles);
    LS_CHECK(backend.value()->prepare(configuration));
    return backend.takeValue();
}

} // namespace

LS_TEST(acoustic_inertia_holds_the_first_ambiguous_remote_switch)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(
        1,
        0.0F,
        0,
        500'000'000);
    const auto seeded = backend->assign(remoteBatch(
        0,
        500'000'000,
        {first}));
    LS_CHECK(seeded);
    LS_CHECK_EQ(seeded.value().decisions.size(), std::size_t{1});
    LS_CHECK_EQ(
        seeded.value().decisions[0].kind,
        SpeakerTurnDecisionKind::commit);

    const auto candidate = remoteHypothesis(
        2,
        std::numbers::pi_v<float> / 2.0F,
        1'000'000'000,
        1'500'000'000);
    const auto update = backend->assign(remoteBatch(
        500'000'000,
        1'500'000'000,
        {candidate}));
    LS_CHECK(update);
    LS_CHECK_EQ(update.value().decisions.size(), std::size_t{1});
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(update.value().decisions[0].pendingGroupId != 0);
    LS_CHECK_EQ(
        update.value().decisions[0].deadlineTimeNs,
        std::int64_t{6'500'000'000});
    LS_CHECK(update.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_confirms_the_exact_boundary_inside_one_batch)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(
        1,
        0.0F,
        0,
        500'000'000);
    LS_CHECK(backend->assign(remoteBatch(
        0,
        500'000'000,
        {first})));

    const auto candidate = remoteHypothesis(
        2,
        1.20F,
        1'000'000'000,
        1'400'000'000);
    const auto confirmation = remoteHypothesis(
        3,
        1.18F,
        1'500'000'000,
        1'900'000'000);
    const auto update = backend->assign(remoteBatch(
        500'000'000,
        1'900'000'000,
        {candidate, confirmation}));
    LS_CHECK(update);
    LS_CHECK_EQ(update.value().decisions.size(), std::size_t{2});
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK_EQ(
        update.value().decisions[1].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK_EQ(
        update.value().decisions[0].pendingGroupId,
        update.value().decisions[1].pendingGroupId);
    LS_CHECK_EQ(update.value().resolutions.size(), std::size_t{1});
    const auto &resolution = update.value().resolutions[0];
    LS_CHECK_EQ(
        resolution.reason,
        PendingSpeakerResolutionReason::confirmed);
    LS_CHECK_EQ(resolution.turns.size(), std::size_t{2});
    LS_CHECK_EQ(
        resolution.turns[0].stableId,
        candidate.stableId);
    LS_CHECK_EQ(
        resolution.turns[1].stableId,
        confirmation.stableId);
    LS_CHECK(
        resolution.turns[0].speakerId
        != update.value().decisions[0].turn.speakerId);
    LS_CHECK_EQ(
        resolution.turns[0].speakerId,
        resolution.turns[1].speakerId);
}

LS_TEST(acoustic_inertia_confirms_after_enough_unique_suspected_speech)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 400'000'000, {first})));

    const auto longCandidate = remoteHypothesis(
        2,
        1.10F,
        1'000'000'000,
        2'500'000'000);
    const auto update = backend->assign(remoteBatch(
        400'000'000,
        2'500'000'000,
        {longCandidate}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK_EQ(update.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        update.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::confirmed);
}

LS_TEST(acoustic_inertia_replaces_same_segment_evidence_on_revision)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 400'000'000, {first})));

    const auto candidate = remoteHypothesis(
        2,
        1.10F,
        1'000'000'000,
        1'400'000'000,
        1);
    const auto firstUpdate = backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {candidate}));
    LS_CHECK(firstUpdate);
    LS_CHECK_EQ(
        firstUpdate.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);

    auto revision = candidate;
    revision.revision = 2;
    revision.endTimeNs = 1'600'000'000;
    revision.speakerEmbedding = embeddingAt(1.12F);
    const auto revised = backend->assign(remoteBatch(
        1'000'000'000,
        1'600'000'000,
        {revision}));
    LS_CHECK(revised);
    LS_CHECK_EQ(
        revised.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(revised.value().resolutions.empty());
    LS_CHECK_EQ(
        revised.value().decisions[0].pendingGroupId,
        firstUpdate.value().decisions[0].pendingGroupId);
}

LS_TEST(acoustic_inertia_commits_a_strong_profile_match_immediately)
{
    auto backend = preparedAcousticBackend({voiceProfile(42, "Alice", 0.0F)});
    const auto unknown = remoteHypothesis(
        1,
        std::numbers::pi_v<float> / 2.0F,
        0,
        500'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 500'000'000, {unknown})));

    const auto alice = remoteHypothesis(
        2,
        0.02F,
        1'000'000'000,
        1'500'000'000);
    const auto update = backend->assign(remoteBatch(
        500'000'000,
        1'500'000'000,
        {alice}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::commit);
    LS_CHECK_EQ(
        update.value().decisions[0].turn.speakerId,
        persistentSpeakerId(42));
    LS_CHECK(update.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_holds_an_ambiguous_profile_match)
{
    auto backend = preparedAcousticBackend({
        voiceProfile(41, "Alice", -0.10F),
        voiceProfile(42, "Bob", 0.10F),
    });
    const auto unknown = remoteHypothesis(
        1,
        std::numbers::pi_v<float> / 2.0F,
        0,
        500'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 500'000'000, {unknown})));

    const auto ambiguous = remoteHypothesis(
        2,
        0.0F,
        1'000'000'000,
        1'500'000'000);
    const auto update = backend->assign(remoteBatch(
        500'000'000,
        1'500'000'000,
        {ambiguous}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(update.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_does_not_adapt_a_profile_on_gray_zone_fallback)
{
    auto backend = preparedAcousticBackend({voiceProfile(42, "Alice", 0.0F)});
    const auto alice = backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)}));
    LS_CHECK(alice);
    LS_CHECK_EQ(
        alice.value().decisions[0].turn.speakerId,
        persistentSpeakerId(42));

    const auto firstGray = backend->assign(remoteBatch(
        400'000'000,
        900'000'000,
        {remoteHypothesis(2, 0.53F, 500'000'000, 900'000'000)}));
    LS_CHECK(firstGray);
    LS_CHECK_EQ(
        firstGray.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);

    const auto secondGray = backend->assign(remoteBatch(
        900'000'000,
        1'400'000'000,
        {remoteHypothesis(3, 0.54F, 1'000'000'000, 1'400'000'000)}));
    LS_CHECK(secondGray);
    LS_CHECK_EQ(secondGray.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        secondGray.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::contradicted);
    LS_CHECK_EQ(
        secondGray.value().resolutions[0].turns[0].speakerId,
        persistentSpeakerId(42));

    const auto probe = backend->assign(remoteBatch(
        1'400'000'000,
        1'900'000'000,
        {remoteHypothesis(4, 1.0F, 1'500'000'000, 1'900'000'000)}));
    LS_CHECK(probe);
    LS_CHECK_EQ(
        probe.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(probe.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_reports_backend_policy_version_six)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK_EQ(backend->info().version, std::string{"6"});
}

LS_TEST(acoustic_inertia_rejects_hypotheses_outside_the_processed_range)
{
    auto backend = preparedAcousticBackend();

    const auto startsBeforeBatch = backend->assign(remoteBatch(
        200'000'000,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 100'000'000, 300'000'000)}));
    LS_CHECK(!startsBeforeBatch);
    LS_CHECK_EQ(startsBeforeBatch.error().code, LS_INVALID_ARGUMENT);

    const auto endsAfterBatch = backend->assign(remoteBatch(
        200'000'000,
        400'000'000,
        {remoteHypothesis(2, 0.0F, 300'000'000, 500'000'000)}));
    LS_CHECK(!endsAfterBatch);
    LS_CHECK_EQ(endsAfterBatch.error().code, LS_INVALID_ARGUMENT);
}

LS_TEST(acoustic_inertia_times_out_at_the_hard_watermark)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 400'000'000, {first})));
    const auto candidate = remoteHypothesis(
        2,
        1.10F,
        1'000'000'000,
        1'400'000'000);
    const auto held = backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {candidate}));
    LS_CHECK(held);

    const auto timeout = backend->assign(remoteBatch(
        1'400'000'000,
        6'400'000'000,
        {}));
    LS_CHECK(timeout);
    LS_CHECK(timeout.value().decisions.empty());
    LS_CHECK_EQ(timeout.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        timeout.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::timeout);
    LS_CHECK_EQ(
        timeout.value().resolutions[0].pendingGroupId,
        held.value().decisions[0].pendingGroupId);
    LS_CHECK_EQ(
        timeout.value().resolutions[0].turns[0].speakerId,
        held.value().decisions[0].turn.speakerId);
}

LS_TEST(acoustic_inertia_processes_confirmation_at_the_exact_deadline)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 400'000'000, {first})));
    const auto candidate = remoteHypothesis(
        2,
        1.10F,
        1'000'000'000,
        1'400'000'000);
    LS_CHECK(backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {candidate})));

    const auto exactDeadline = remoteHypothesis(
        3,
        1.12F,
        6'000'000'000,
        6'400'000'000);
    const auto update = backend->assign(remoteBatch(
        1'400'000'000,
        6'400'000'000,
        {exactDeadline}));
    LS_CHECK(update);
    LS_CHECK_EQ(update.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        update.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::confirmed);
    LS_CHECK_EQ(update.value().resolutions[0].turns.size(), std::size_t{2});
}

LS_TEST(acoustic_inertia_resolves_pending_speech_on_pause)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 400'000'000, {first})));
    const auto candidate = remoteHypothesis(
        2,
        1.10F,
        1'000'000'000,
        1'400'000'000);
    LS_CHECK(backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {candidate})));

    const auto flushed = backend->flush(DiarizationFlushReason::pause);
    LS_CHECK(flushed);
    LS_CHECK_EQ(flushed.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        flushed.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::pause);
}

LS_TEST(acoustic_inertia_resolves_pending_speech_at_a_discontinuity)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    LS_CHECK(backend->assign(remoteBatch(0, 400'000'000, {first})));
    const auto candidate = remoteHypothesis(
        2,
        1.10F,
        1'000'000'000,
        1'400'000'000);
    LS_CHECK(backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {candidate})));

    const auto boundary = backend->assign(remoteBatch(
        1'400'000'000,
        1'400'000'000,
        {},
        true));
    LS_CHECK(boundary);
    LS_CHECK_EQ(boundary.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        boundary.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::discontinuity);
}

LS_TEST(acoustic_inertia_accepts_a_rebased_watermark_after_a_discontinuity)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        10'000'000'000,
        10'400'000'000,
        {remoteHypothesis(
            1,
            0.0F,
            10'000'000'000,
            10'400'000'000)})));
    const auto held = backend->assign(remoteBatch(
        10'400'000'000,
        11'400'000'000,
        {remoteHypothesis(
            2,
            1.10F,
            11'000'000'000,
            11'400'000'000)}));
    LS_CHECK(held);
    LS_CHECK_EQ(
        held.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);

    const auto boundary = backend->assign(remoteBatch(
        0,
        0,
        {},
        true));
    LS_CHECK(boundary);
    LS_CHECK_EQ(boundary.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        boundary.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::discontinuity);

    const auto fresh = backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(3, 0.01F, 0, 400'000'000)}));
    LS_CHECK(fresh);
    LS_CHECK_EQ(fresh.value().decisions.size(), std::size_t{1});
}

LS_TEST(acoustic_inertia_rejects_a_suspicion_when_the_active_voice_returns)
{
    auto backend = preparedAcousticBackend();
    const auto first = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    const auto seeded = backend->assign(remoteBatch(0, 400'000'000, {first}));
    LS_CHECK(seeded);
    const auto activeSpeaker = seeded.value().decisions[0].turn.speakerId;
    const auto candidate = remoteHypothesis(
        2,
        1.10F,
        1'000'000'000,
        1'400'000'000);
    LS_CHECK(backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {candidate})));

    const auto returned = remoteHypothesis(
        3,
        0.01F,
        1'500'000'000,
        1'900'000'000);
    const auto update = backend->assign(remoteBatch(
        1'400'000'000,
        1'900'000'000,
        {returned}));
    LS_CHECK(update);
    LS_CHECK_EQ(update.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        update.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::contradicted);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::commit);
    LS_CHECK_EQ(update.value().decisions[0].turn.speakerId, activeSpeaker);
}

LS_TEST(acoustic_inertia_does_not_start_from_a_tdrz_hint_without_voice_evidence)
{
    auto backend = preparedAcousticBackend();
    auto before = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    before.speakerTurnAfter = true;
    const auto seeded = backend->assign(remoteBatch(0, 400'000'000, {before}));
    LS_CHECK(seeded);

    AsrHypothesis after;
    after.stableId[15] = 2;
    after.sourceId = 22;
    after.startTimeNs = 500'000'000;
    after.endTimeNs = 800'000'000;
    after.final = true;
    after.revision = 1;
    const auto update = backend->assign(remoteBatch(
        400'000'000,
        800'000'000,
        {after}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::commit);
    LS_CHECK_EQ(
        update.value().decisions[0].turn.speakerId,
        seeded.value().decisions[0].turn.speakerId);
    LS_CHECK(update.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_keeps_the_active_profile_without_voice_evidence)
{
    auto backend = preparedAcousticBackend({voiceProfile(42, "Alice", 0.0F)});
    auto alice = remoteHypothesis(1, 0.01F, 0, 400'000'000);
    alice.speakerTurnAfter = true;
    const auto seeded = backend->assign(remoteBatch(
        0,
        400'000'000,
        {alice}));
    LS_CHECK(seeded);
    LS_CHECK_EQ(
        seeded.value().decisions[0].turn.speakerId,
        persistentSpeakerId(42));

    AsrHypothesis withoutEvidence;
    withoutEvidence.stableId[15] = 2;
    withoutEvidence.sourceId = 22;
    withoutEvidence.startTimeNs = 500'000'000;
    withoutEvidence.endTimeNs = 800'000'000;
    withoutEvidence.final = true;
    withoutEvidence.revision = 1;
    const auto update = backend->assign(remoteBatch(
        400'000'000,
        800'000'000,
        {withoutEvidence}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::commit);
    LS_CHECK_EQ(
        update.value().decisions[0].turn.speakerId,
        persistentSpeakerId(42));
    LS_CHECK(update.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_compares_a_tdrz_alternative_with_the_active_voice)
{
    auto backend = preparedAcousticBackend({
        voiceProfile(41, "Alice", 0.0F),
        voiceProfile(42, "Bob", 0.20F),
    });
    auto alice = remoteHypothesis(1, -0.30F, 0, 400'000'000);
    alice.speakerTurnAfter = true;
    const auto seeded = backend->assign(remoteBatch(
        0,
        400'000'000,
        {alice}));
    LS_CHECK(seeded);
    LS_CHECK_EQ(
        seeded.value().decisions[0].turn.speakerId,
        persistentSpeakerId(41));

    const auto ambiguous = remoteHypothesis(
        2,
        0.09F,
        500'000'000,
        900'000'000);
    const auto update = backend->assign(remoteBatch(
        400'000'000,
        900'000'000,
        {ambiguous}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK_EQ(
        update.value().decisions[0].turn.speakerId,
        persistentSpeakerId(41));
    LS_CHECK(update.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_starts_a_tdrz_suspicion_with_voice_evidence)
{
    auto backend = preparedAcousticBackend();
    auto before = remoteHypothesis(1, 0.0F, 0, 400'000'000);
    before.speakerTurnAfter = true;
    LS_CHECK(backend->assign(remoteBatch(0, 400'000'000, {before})));

    const auto after = remoteHypothesis(
        2,
        0.40F,
        500'000'000,
        900'000'000);
    const auto update = backend->assign(remoteBatch(
        400'000'000,
        900'000'000,
        {after}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(update.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_restarts_confirmation_when_the_candidate_changes)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)})));
    const auto firstCandidate = backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {remoteHypothesis(2, 1.10F, 1'000'000'000, 1'400'000'000)}));
    LS_CHECK(firstCandidate);
    const auto firstGroup =
        firstCandidate.value().decisions[0].pendingGroupId;

    const auto changed = backend->assign(remoteBatch(
        1'400'000'000,
        1'900'000'000,
        {remoteHypothesis(3, -1.10F, 1'500'000'000, 1'900'000'000)}));
    LS_CHECK(changed);
    LS_CHECK_EQ(changed.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        changed.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::contradicted);
    LS_CHECK_EQ(
        changed.value().resolutions[0].pendingGroupId,
        firstGroup);
    LS_CHECK_EQ(
        changed.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(
        changed.value().decisions[0].pendingGroupId != firstGroup);
}

LS_TEST(acoustic_inertia_restarts_confirmation_when_the_model_changes)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)})));
    const auto firstCandidate = backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {remoteHypothesis(2, 1.10F, 1'000'000'000, 1'400'000'000)}));
    LS_CHECK(firstCandidate);
    const auto firstGroup =
        firstCandidate.value().decisions[0].pendingGroupId;

    auto otherModel = remoteHypothesis(
        3,
        0.01F,
        1'500'000'000,
        1'900'000'000);
    otherModel.speakerEmbeddingModel = "test-speaker-v2";
    const auto changed = backend->assign(remoteBatch(
        1'400'000'000,
        1'900'000'000,
        {otherModel}));
    LS_CHECK(changed);
    LS_CHECK_EQ(changed.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        changed.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::contradicted);
    LS_CHECK_EQ(
        changed.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(
        changed.value().decisions[0].pendingGroupId != firstGroup);

    auto confirmingModel = remoteHypothesis(
        4,
        0.02F,
        2'000'000'000,
        2'400'000'000);
    confirmingModel.speakerEmbeddingModel = "test-speaker-v2";
    const auto confirmed = backend->assign(remoteBatch(
        1'900'000'000,
        2'400'000'000,
        {confirmingModel}));
    LS_CHECK(confirmed);
    LS_CHECK_EQ(confirmed.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        confirmed.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::confirmed);
    LS_CHECK(
        confirmed.value().resolutions[0].turns[0].speakerId
        != changed.value().decisions[0].turn.speakerId);
}

LS_TEST(acoustic_inertia_does_not_update_a_centroid_before_confirmation)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)})));
    const auto speculative = remoteHypothesis(
        2,
        0.55F,
        1'000'000'000,
        1'400'000'000);
    const auto held = backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {speculative}));
    LS_CHECK(held);
    LS_CHECK_EQ(
        held.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(backend->flush(DiarizationFlushReason::pause));

    const auto repeated = remoteHypothesis(
        3,
        0.55F,
        1'500'000'000,
        1'900'000'000);
    const auto update = backend->assign(remoteBatch(
        1'400'000'000,
        1'900'000'000,
        {repeated}));
    LS_CHECK(update);
    LS_CHECK_EQ(
        update.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
}

LS_TEST(acoustic_inertia_switches_immediately_to_a_strong_known_cluster)
{
    auto backend = preparedAcousticBackend();
    const auto first = backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)}));
    LS_CHECK(first);
    const auto firstSpeaker = first.value().decisions[0].turn.speakerId;

    const auto confirmed = backend->assign(remoteBatch(
        400'000'000,
        1'900'000'000,
        {
            remoteHypothesis(2, 1.20F, 1'000'000'000, 1'400'000'000),
            remoteHypothesis(3, 1.18F, 1'500'000'000, 1'900'000'000),
        }));
    LS_CHECK(confirmed);
    LS_CHECK_EQ(confirmed.value().resolutions.size(), std::size_t{1});
    LS_CHECK(
        confirmed.value().resolutions[0].turns[0].speakerId
        != firstSpeaker);

    const auto returned = backend->assign(remoteBatch(
        1'900'000'000,
        2'400'000'000,
        {remoteHypothesis(4, 0.01F, 2'000'000'000, 2'400'000'000)}));
    LS_CHECK(returned);
    LS_CHECK_EQ(
        returned.value().decisions[0].kind,
        SpeakerTurnDecisionKind::commit);
    LS_CHECK_EQ(returned.value().decisions[0].turn.speakerId, firstSpeaker);
}

LS_TEST(acoustic_inertia_resolves_pending_speech_at_end_of_stream)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)})));
    LS_CHECK(backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {remoteHypothesis(2, 1.10F, 1'000'000'000, 1'400'000'000)})));

    const auto flushed = backend->flush(DiarizationFlushReason::endOfStream);
    LS_CHECK(flushed);
    LS_CHECK_EQ(flushed.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        flushed.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::endOfStream);
}

LS_TEST(acoustic_inertia_limits_a_pending_group_to_64_revisions)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)})));

    std::uint64_t firstGroup = 0;
    for (std::uint32_t revision = 1; revision <= 64; ++revision) {
        const auto held = backend->assign(remoteBatch(
            400'000'000,
            1'400'000'000,
            {remoteHypothesis(
                2,
                1.10F,
                1'000'000'000,
                1'400'000'000,
                revision)}));
        LS_CHECK(held);
        LS_CHECK(held.value().resolutions.empty());
        LS_CHECK_EQ(
            held.value().decisions[0].kind,
            SpeakerTurnDecisionKind::hold);
        if (revision == 1) {
            firstGroup = held.value().decisions[0].pendingGroupId;
        } else {
            LS_CHECK_EQ(
                held.value().decisions[0].pendingGroupId,
                firstGroup);
        }
    }

    const auto overflow = backend->assign(remoteBatch(
        400'000'000,
        1'400'000'000,
        {remoteHypothesis(
            2,
            1.10F,
            1'000'000'000,
            1'400'000'000,
            65)}));
    LS_CHECK(overflow);
    LS_CHECK_EQ(overflow.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        overflow.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::capacity);
    LS_CHECK_EQ(
        overflow.value().resolutions[0].turns.size(),
        std::size_t{64});
    LS_CHECK_EQ(
        overflow.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK(
        overflow.value().decisions[0].pendingGroupId != firstGroup);
}

LS_TEST(acoustic_inertia_holds_an_ambiguous_known_cluster_match)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        0,
        400'000'000,
        {remoteHypothesis(1, 0.0F, 0, 400'000'000)})));
    const auto secondSpeaker = backend->assign(remoteBatch(
        400'000'000,
        1'900'000'000,
        {
            remoteHypothesis(2, 1.20F, 1'000'000'000, 1'400'000'000),
            remoteHypothesis(3, 1.18F, 1'500'000'000, 1'900'000'000),
        }));
    LS_CHECK(secondSpeaker);
    LS_CHECK_EQ(secondSpeaker.value().resolutions.size(), std::size_t{1});
    const auto activeSpeaker =
        secondSpeaker.value().resolutions[0].turns.back().speakerId;

    const auto ambiguous = backend->assign(remoteBatch(
        1'900'000'000,
        2'400'000'000,
        {remoteHypothesis(4, 0.40F, 2'000'000'000, 2'400'000'000)}));
    LS_CHECK(ambiguous);
    LS_CHECK_EQ(
        ambiguous.value().decisions[0].kind,
        SpeakerTurnDecisionKind::hold);
    LS_CHECK_EQ(
        ambiguous.value().decisions[0].turn.speakerId,
        activeSpeaker);
    LS_CHECK(ambiguous.value().resolutions.empty());
}

LS_TEST(acoustic_inertia_does_not_create_a_ninth_remote_cluster)
{
    auto backend = preparedAcousticBackend();
    LS_CHECK(backend->assign(remoteBatch(
        0,
        300'000'000,
        {remoteHypothesis(1, 0.0F, 0, 300'000'000)})));

    std::int64_t watermark = 300'000'000;
    std::uint64_t activeSpeaker = 0;
    for (std::uint8_t ordinal = 1; ordinal < 8; ++ordinal) {
        const std::int64_t start =
            static_cast<std::int64_t>(ordinal) * 1'000'000'000;
        const auto confirmed = backend->assign(remoteBatch(
            watermark,
            start + 700'000'000,
            {
                remoteHypothesis(
                    static_cast<std::uint8_t>(ordinal * 2),
                    static_cast<float>(ordinal) * 0.70F,
                    start,
                    start + 300'000'000),
                remoteHypothesis(
                    static_cast<std::uint8_t>(ordinal * 2 + 1),
                    static_cast<float>(ordinal) * 0.70F + 0.01F,
                    start + 400'000'000,
                    start + 700'000'000),
            }));
        LS_CHECK(confirmed);
        LS_CHECK_EQ(confirmed.value().resolutions.size(), std::size_t{1});
        LS_CHECK_EQ(
            confirmed.value().resolutions[0].reason,
            PendingSpeakerResolutionReason::confirmed);
        activeSpeaker =
            confirmed.value().resolutions[0].turns.back().speakerId;
        watermark = start + 700'000'000;
    }

    const std::int64_t ninthStart = 8'000'000'000;
    const auto ninth = backend->assign(remoteBatch(
        watermark,
        ninthStart + 700'000'000,
        {
            remoteHypothesis(
                16,
                5.60F,
                ninthStart,
                ninthStart + 300'000'000),
            remoteHypothesis(
                17,
                5.61F,
                ninthStart + 400'000'000,
                ninthStart + 700'000'000),
        }));
    LS_CHECK(ninth);
    LS_CHECK_EQ(ninth.value().resolutions.size(), std::size_t{1});
    LS_CHECK_EQ(
        ninth.value().resolutions[0].reason,
        PendingSpeakerResolutionReason::capacity);
    LS_CHECK_EQ(
        ninth.value().resolutions[0].turns[0].speakerId,
        activeSpeaker);
    LS_CHECK_EQ(
        ninth.value().resolutions[0].turns[1].speakerId,
        activeSpeaker);
}
