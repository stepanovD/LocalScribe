#include "TestSupport.hpp"

#include "../src/storage/RecoveryJournal.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace localscribe;

namespace {

class TemporaryJournal {
public:
    TemporaryJournal()
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("localscribe-journal-" + std::to_string(stamp) + ".sqlite3");
    }

    ~TemporaryJournal()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

SessionRecord sessionRecord(std::string id)
{
    SessionRecord record;
    record.sessionId = std::move(id);
    record.phase = LS_PHASE_PREPARING;
    record.createdAt = "2026-07-29T10:00:00+04:00";
    record.sourceApp = "Fixture";
    record.localSpeakerName = "Me";
    record.asrBackendId = "fixture";
    record.asrBackendVersion = "1";
    record.diarizationBackendId = "source-aware";
    record.diarizationBackendVersion = "1";
    record.languageMode = LS_LANGUAGE_MODE_RUSSIAN_ENGLISH;
    record.microphoneSourceId = 1;
    record.systemAudioSourceId = 2;
    record.requiredSourceMask =
        LS_REQUIRED_SOURCE_MICROPHONE | LS_REQUIRED_SOURCE_SYSTEM_AUDIO;
    record.completenessThresholdNs = 30'000'000'000;
    return record;
}

std::array<SourceRecord, 2> sourceRecords()
{
    return {
        SourceRecord{
            1,
            LS_SOURCE_KIND_MICROPHONE,
            true,
            LS_SOURCE_HEALTH_READY},
        SourceRecord{
            2,
            LS_SOURCE_KIND_SYSTEM_AUDIO,
            true,
            LS_SOURCE_HEALTH_READY}};
}

TranscriptSegment segment(std::uint32_t revision)
{
    TranscriptSegment result;
    result.stableId[15] = 9;
    result.sourceId = 1;
    result.startTimeNs = 1'000'000'000;
    result.endTimeNs = 2'000'000'000;
    result.speakerId = 1;
    result.speakerLabel = "Me";
    result.text =
        revision == 1 ? "first revision" : "second revision";
    result.language = "en";
    result.confidence = 0.95F;
    result.revision = revision;
    result.flags = LS_SEGMENT_FLAG_FINAL;
    return result;
}

} // namespace

LS_TEST(journal_migrates_and_keeps_only_highest_final_revision_in_snapshot)
{
    TemporaryJournal temporary;
    auto journal = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(journal);
    const auto sources = sourceRecords();
    LS_CHECK(journal.value()->createSession(
        sessionRecord("revision-test"),
        sources));
    auto recording = journal.value()->transition(
        "revision-test",
        LS_PHASE_PREPARING,
        LS_PHASE_RECORDING);
    LS_CHECK(recording);

    auto first = journal.value()->appendFinalSegment(
        "revision-test",
        segment(1));
    auto second = journal.value()->appendFinalSegment(
        "revision-test",
        segment(2));
    LS_CHECK(first);
    LS_CHECK(second);
    LS_CHECK(second.value() > first.value());

    auto stale = journal.value()->appendFinalSegment(
        "revision-test",
        segment(1));
    LS_CHECK(!stale);
    LS_CHECK_EQ(stale.error().code, LS_CONFLICT);

    auto snapshot = journal.value()->snapshot("revision-test");
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{1});
    LS_CHECK_EQ(snapshot.value().segments[0].revision, std::uint32_t{2});
    LS_CHECK_EQ(
        snapshot.value().segments[0].text,
        std::string{"second revision"});
    LS_CHECK(journal.value()->quickCheck());
}

LS_TEST(late_queued_frame_and_discontinuity_preserve_unavailable_health)
{
    TemporaryJournal temporary;
    auto journal = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(journal);
    const auto sources = sourceRecords();
    const std::string sessionId{"late-source-accounting"};
    LS_CHECK(journal.value()->createSession(
        sessionRecord(sessionId),
        sources));
    LS_CHECK(journal.value()->transition(
        sessionId,
        LS_PHASE_PREPARING,
        LS_PHASE_RECORDING));

    SourceGap unavailable;
    unavailable.sourceId = 1;
    unavailable.sourceKind = LS_SOURCE_KIND_MICROPHONE;
    unavailable.eventKind = LS_SOURCE_EVENT_UNAVAILABLE;
    unavailable.health = LS_SOURCE_HEALTH_TEMPORARILY_UNAVAILABLE;
    unavailable.startTimeNs = 2'000'000'000;
    unavailable.endTimeNs = 2'000'000'000;
    unavailable.reason = "adapter unavailable";
    LS_CHECK(journal.value()->recordSourceEvent(
        sessionId,
        unavailable));

    /*
     * This frame was already queued when the adapter reported unavailable.
     * Its older discontinuity is journaled after the transition and must
     * update accounting without rolling durable health back to ACTIVE.
     */
    LS_CHECK(journal.value()->recordFrameAccepted(sessionId, 1));
    SourceGap lateDiscontinuity;
    lateDiscontinuity.sourceId = 1;
    lateDiscontinuity.sourceKind = LS_SOURCE_KIND_MICROPHONE;
    lateDiscontinuity.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
    lateDiscontinuity.health = LS_SOURCE_HEALTH_ACTIVE;
    lateDiscontinuity.startTimeNs = 1'000'000'000;
    lateDiscontinuity.endTimeNs = 1'000'000'000;
    lateDiscontinuity.reason = "late queued frame";
    LS_CHECK(journal.value()->recordSourceEvent(
        sessionId,
        lateDiscontinuity));

    auto snapshot = journal.value()->snapshot(sessionId);
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().sources.size(), std::size_t{2});
    const auto &microphone = snapshot.value().sources[0];
    LS_CHECK_EQ(microphone.sourceId, std::uint64_t{1});
    LS_CHECK_EQ(
        microphone.health,
        LS_SOURCE_HEALTH_TEMPORARILY_UNAVAILABLE);
    LS_CHECK_EQ(microphone.acceptedFrames, std::uint64_t{1});
    LS_CHECK_EQ(microphone.discontinuities, std::uint64_t{2});
    LS_CHECK_EQ(snapshot.value().gaps.size(), std::size_t{2});
    LS_CHECK_EQ(
        snapshot.value().gaps[0].eventKind,
        LS_SOURCE_EVENT_DISCONTINUITY);
    LS_CHECK_EQ(
        snapshot.value().gaps[1].eventKind,
        LS_SOURCE_EVENT_UNAVAILABLE);
    LS_CHECK(journal.value()->quickCheck());
}

LS_TEST(journal_marks_nonterminal_sessions_recoverable_exactly_once)
{
    TemporaryJournal temporary;
    auto journal = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(journal);
    const auto sources = sourceRecords();
    LS_CHECK(journal.value()->createSession(
        sessionRecord("crash-test"),
        sources));
    LS_CHECK(journal.value()->transition(
        "crash-test",
        LS_PHASE_PREPARING,
        LS_PHASE_RECORDING));
    LS_CHECK(journal.value()->appendFinalSegment(
        "crash-test",
        segment(1)));
    journal.value().reset();

    auto relaunched = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(relaunched);
    auto first =
        relaunched.value()->markAndListRecoverableSessions();
    LS_CHECK(first);
    LS_CHECK_EQ(first.value().size(), std::size_t{1});
    LS_CHECK_EQ(first.value()[0], std::string{"crash-test"});
    const auto once =
        relaunched.value()->loadSession("crash-test");
    LS_CHECK(once);
    LS_CHECK_EQ(once.value().phase, LS_PHASE_RECOVERY_REQUIRED);
    const auto checkpoint = once.value().journalCheckpoint;

    auto second =
        relaunched.value()->markAndListRecoverableSessions();
    LS_CHECK(second);
    const auto twice =
        relaunched.value()->loadSession("crash-test");
    LS_CHECK(twice);
    LS_CHECK_EQ(twice.value().journalCheckpoint, checkpoint);
    auto snapshot = relaunched.value()->snapshot("crash-test");
    LS_CHECK(snapshot);
    LS_CHECK_EQ(snapshot.value().segments.size(), std::size_t{1});
}

LS_TEST(journal_accepts_historical_receipt_then_rejects_regression)
{
    TemporaryJournal temporary;
    auto journal = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(journal);
    const auto sources = sourceRecords();
    LS_CHECK(journal.value()->createSession(
        sessionRecord("historical-receipt"),
        sources));
    LS_CHECK(journal.value()->transition(
        "historical-receipt",
        LS_PHASE_PREPARING,
        LS_PHASE_RECORDING));

    auto first = journal.value()->appendFinalSegment(
        "historical-receipt",
        segment(1));
    LS_CHECK(first);
    const auto rendered = journal.value()->snapshot("historical-receipt");
    LS_CHECK(rendered);
    LS_CHECK_EQ(
        rendered.value().session.journalCheckpoint,
        first.value());
    LS_CHECK_EQ(
        rendered.value().session.highestSegmentRevision,
        std::uint32_t{1});

    auto second = journal.value()->appendFinalSegment(
        "historical-receipt",
        segment(2));
    LS_CHECK(second);
    LS_CHECK(second.value() > first.value());

    PublicationReceipt receipt;
    receipt.journalCheckpoint =
        rendered.value().session.journalCheckpoint;
    receipt.highestSegmentRevision =
        rendered.value().session.highestSegmentRevision;
    receipt.destination = LS_PUBLICATION_DESTINATION_STAGING;
    receipt.publishedAtUnixNs = 1;
    receipt.sha256Hex = std::string(64, 'a');
    receipt.fileIdentity = "historical";
    LS_CHECK(journal.value()->acknowledgePublication(
        "historical-receipt",
        receipt));
    /* The same receipt remains idempotent after capture has advanced. */
    LS_CHECK(journal.value()->acknowledgePublication(
        "historical-receipt",
        receipt));

    PublicationReceipt current = receipt;
    current.journalCheckpoint = second.value();
    current.highestSegmentRevision = 2;
    current.sha256Hex = std::string(64, 'b');
    current.fileIdentity = "current";
    LS_CHECK(journal.value()->acknowledgePublication(
        "historical-receipt",
        current));

    auto regression = journal.value()->acknowledgePublication(
        "historical-receipt",
        receipt);
    LS_CHECK(!regression);
    LS_CHECK_EQ(regression.error().code, LS_CONFLICT);

    auto finalizing = journal.value()->transition(
        "historical-receipt",
        LS_PHASE_RECORDING,
        LS_PHASE_FINALIZING,
        LS_FINALIZE_REASON_USER_STOP);
    LS_CHECK(finalizing);
    auto complete = journal.value()->transition(
        "historical-receipt",
        LS_PHASE_FINALIZING,
        LS_PHASE_COMPLETE,
        LS_FINALIZE_REASON_USER_STOP);
    LS_CHECK(complete);

    PublicationReceipt terminalOld = current;
    terminalOld.journalCheckpoint = finalizing.value();
    terminalOld.sha256Hex = std::string(64, 'c');
    auto staleTerminal = journal.value()->acknowledgePublication(
        "historical-receipt",
        terminalOld);
    LS_CHECK(!staleTerminal);
    LS_CHECK_EQ(staleTerminal.error().code, LS_CONFLICT);

    auto pending = journal.value()->listRecoverableSessions();
    LS_CHECK(pending);
    LS_CHECK_EQ(pending.value().size(), std::size_t{1});
    LS_CHECK_EQ(
        pending.value()[0],
        std::string{"historical-receipt"});

    PublicationReceipt terminal = current;
    terminal.journalCheckpoint = complete.value();
    terminal.sha256Hex = std::string(64, 'd');
    terminal.fileIdentity = "terminal";
    LS_CHECK(journal.value()->acknowledgePublication(
        "historical-receipt",
        terminal));
    pending = journal.value()->listRecoverableSessions();
    LS_CHECK(pending);
    LS_CHECK(pending.value().empty());
}

LS_TEST(recovery_crash_in_finalizing_is_marked_again_on_next_startup)
{
    TemporaryJournal temporary;
    auto journal = RecoveryJournal::open(temporary.path.string());
    LS_CHECK(journal);
    const auto sources = sourceRecords();
    LS_CHECK(journal.value()->createSession(
        sessionRecord("recovery-crash"),
        sources));
    LS_CHECK(journal.value()->transition(
        "recovery-crash",
        LS_PHASE_PREPARING,
        LS_PHASE_RECORDING));
    auto firstStartup =
        journal.value()->markAndListRecoverableSessions();
    LS_CHECK(firstStartup);
    LS_CHECK_EQ(firstStartup.value().size(), std::size_t{1});
    auto recovery = journal.value()->loadSession("recovery-crash");
    LS_CHECK(recovery);
    LS_CHECK_EQ(recovery.value().phase, LS_PHASE_RECOVERY_REQUIRED);

    LS_CHECK(journal.value()->transition(
        "recovery-crash",
        LS_PHASE_RECOVERY_REQUIRED,
        LS_PHASE_FINALIZING,
        LS_FINALIZE_REASON_RECOVERY));
    const auto beforeRestart =
        journal.value()->loadSession("recovery-crash");
    LS_CHECK(beforeRestart);
    LS_CHECK_EQ(beforeRestart.value().phase, LS_PHASE_FINALIZING);

    auto secondStartup =
        journal.value()->markAndListRecoverableSessions();
    LS_CHECK(secondStartup);
    LS_CHECK_EQ(secondStartup.value().size(), std::size_t{1});
    auto remarked = journal.value()->loadSession("recovery-crash");
    LS_CHECK(remarked);
    LS_CHECK_EQ(remarked.value().phase, LS_PHASE_RECOVERY_REQUIRED);
    LS_CHECK(
        remarked.value().journalCheckpoint
        > beforeRestart.value().journalCheckpoint);

    const auto checkpoint = remarked.value().journalCheckpoint;
    auto sameStartup =
        journal.value()->markAndListRecoverableSessions();
    LS_CHECK(sameStartup);
    auto unchanged = journal.value()->loadSession("recovery-crash");
    LS_CHECK(unchanged);
    LS_CHECK_EQ(unchanged.value().journalCheckpoint, checkpoint);
}
