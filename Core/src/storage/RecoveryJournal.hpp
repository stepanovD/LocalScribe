#pragma once

#include "../common/Expected.hpp"
#include "../common/Types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace localscribe {

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
        const TranscriptSegment &segment);

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

    sqlite3 *database_{};
    std::string path_;
    mutable std::mutex mutex_;
};

} // namespace localscribe
