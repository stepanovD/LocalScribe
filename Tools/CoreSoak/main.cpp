#include <LocalScribeCore/LocalScribeCore.h>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    double durationSeconds{7200.0};
    double speed{1.0};
    std::filesystem::path journalPath;
    bool keepJournal{false};
};

class Sha256 {
public:
    void update(const std::uint8_t *data, std::size_t size)
    {
        totalBytes_ += size;
        while (size != 0) {
            const std::size_t copy =
                std::min<std::size_t>(size, block_.size() - blockSize_);
            std::memcpy(block_.data() + blockSize_, data, copy);
            blockSize_ += copy;
            data += copy;
            size -= copy;
            if (blockSize_ == block_.size()) {
                transform(block_.data());
                blockSize_ = 0;
            }
        }
    }

    std::string finish()
    {
        const std::uint64_t totalBits = totalBytes_ * 8u;
        block_[blockSize_++] = 0x80u;
        if (blockSize_ > 56) {
            std::fill(block_.begin() + blockSize_, block_.end(), 0);
            transform(block_.data());
            blockSize_ = 0;
        }
        std::fill(block_.begin() + blockSize_, block_.begin() + 56, 0);
        for (std::size_t index = 0; index < 8; ++index) {
            block_[56 + index] = static_cast<std::uint8_t>(
                totalBits >> ((7u - index) * 8u));
        }
        transform(block_.data());

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto value : state_) {
            output << std::setw(8) << value;
        }
        return output.str();
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRound{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    static std::uint32_t readBigEndian(const std::uint8_t *bytes)
    {
        return (static_cast<std::uint32_t>(bytes[0]) << 24u)
            | (static_cast<std::uint32_t>(bytes[1]) << 16u)
            | (static_cast<std::uint32_t>(bytes[2]) << 8u)
            | static_cast<std::uint32_t>(bytes[3]);
    }

    void transform(const std::uint8_t *block)
    {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = readBigEndian(block + index * 4);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t left =
                std::rotr(words[index - 15], 7)
                ^ std::rotr(words[index - 15], 18)
                ^ (words[index - 15] >> 3u);
            const std::uint32_t right =
                std::rotr(words[index - 2], 17)
                ^ std::rotr(words[index - 2], 19)
                ^ (words[index - 2] >> 10u);
            words[index] =
                words[index - 16] + left + words[index - 7] + right;
        }
        auto working = state_;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t choose =
                (working[4] & working[5])
                ^ (~working[4] & working[6]);
            const std::uint32_t majority =
                (working[0] & working[1])
                ^ (working[0] & working[2])
                ^ (working[1] & working[2]);
            const std::uint32_t upperOne =
                std::rotr(working[4], 6)
                ^ std::rotr(working[4], 11)
                ^ std::rotr(working[4], 25);
            const std::uint32_t upperZero =
                std::rotr(working[0], 2)
                ^ std::rotr(working[0], 13)
                ^ std::rotr(working[0], 22);
            const std::uint32_t first =
                working[7] + upperOne + choose + kRound[index]
                + words[index];
            const std::uint32_t second = upperZero + majority;
            working = {
                first + second,
                working[0],
                working[1],
                working[2],
                working[3] + first,
                working[4],
                working[5],
                working[6]};
        }
        for (std::size_t index = 0; index < state_.size(); ++index) {
            state_[index] += working[index];
        }
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19};
    std::array<std::uint8_t, 64> block_{};
    std::size_t blockSize_{};
    std::uint64_t totalBytes_{};
};

class CoreHandle {
public:
    ~CoreHandle() { ls_core_destroy(value); }
    ls_core_t *value{};
};

class SessionHandle {
public:
    ~SessionHandle() { ls_session_destroy(value); }
    ls_session_t *value{};
};

class BytesHandle {
public:
    ~BytesHandle() { ls_owned_bytes_destroy(value); }
    ls_owned_bytes_t *value{};
};

class JournalCleanup {
public:
    JournalCleanup(std::filesystem::path path, bool keep)
        : path_(std::move(path)),
          keep_(keep || std::filesystem::exists(path_))
    {
    }

    ~JournalCleanup()
    {
        if (keep_) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

private:
    std::filesystem::path path_;
    bool keep_{};
};

ls_utf8_view_v1 view(const std::string &value)
{
    return ls_utf8_view_v1{
        sizeof(ls_utf8_view_v1),
        LS_CORE_ABI_VERSION,
        reinterpret_cast<const std::uint8_t *>(value.data()),
        value.size()};
}

std::string errorText(const ls_error_v1 &error)
{
    return std::string(
        reinterpret_cast<const char *>(error.message),
        error.message_size);
}

void require(ls_status_code_t actual, ls_status_code_t expected, const char *op)
{
    if (actual != expected) {
        throw std::runtime_error(
            std::string(op) + " returned " + std::to_string(actual)
            + ", expected " + std::to_string(expected));
    }
}

std::size_t occurrences(std::string_view value, std::string_view needle)
{
    std::size_t result = 0;
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        ++result;
        position += needle.size();
    }
    return result;
}

void sourceEvent(
    ls_session_t *session,
    std::uint64_t source,
    ls_source_kind_t kind,
    ls_source_event_kind_t eventKind,
    ls_source_health_t health,
    std::int64_t startTimeNs,
    std::int64_t endTimeNs,
    const std::string &reason)
{
    ls_source_event_v1 event{};
    event.struct_size = sizeof(event);
    event.abi_version = LS_CORE_ABI_VERSION;
    event.source_id = source;
    event.source_kind = kind;
    event.event_kind = eventKind;
    event.health = health;
    event.flags = LS_SOURCE_EVENT_FLAG_TEST_INJECTED;
    event.start_time_ns = startTimeNs;
    event.end_time_ns = endTimeNs;
    event.reason = view(reason);
    require(
        ls_session_source_event_v1(session, &event),
        LS_OK,
        "source event");
}

struct JournalEvidence {
    std::uint64_t segmentRows{};
    std::uint64_t distinctSegmentIds{};
    std::uint64_t revisionTwoRows{};
    std::uint64_t receiptRows{};
    std::uint64_t receiptCheckpoint{};
    std::uint32_t receiptHighestRevision{};
    std::uint64_t sourceEventRows{};
    std::uint64_t sourceDiscontinuityRows{};
    std::uint64_t sourceUnavailableRows{};
    std::uint64_t sourceRecoveredRows{};
    std::uint64_t testInjectedSourceEventRows{};
    std::int64_t minimumRecoveredDurationNs{};
    std::uint64_t sourceAcceptedFrames{};
    std::uint64_t sourceDiscontinuities{};
    std::uint64_t terminalStateRows{};
    std::uint64_t checkpoint{};
    std::uint32_t highestRevision{};
    ls_phase_t phase{LS_PHASE_UNKNOWN};
    ls_finalize_reason_t finalizeReason{LS_FINALIZE_REASON_UNKNOWN};
};

JournalEvidence verifyJournal(
    const std::filesystem::path &path,
    const std::string &sessionId)
{
    sqlite3 *raw = nullptr;
    if (sqlite3_open_v2(
            path.string().c_str(),
            &raw,
            SQLITE_OPEN_READONLY,
            nullptr)
        != SQLITE_OK) {
        if (raw != nullptr) {
            sqlite3_close_v2(raw);
        }
        throw std::runtime_error("cannot open journal for verification");
    }
    const auto close = std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>(
        raw,
        sqlite3_close_v2);
    auto singleText = [&](const char *sql) {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(raw, sql, -1, &statement, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error("journal verification prepare failed");
        }
        const auto finalize =
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
                statement,
                sqlite3_finalize);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            throw std::runtime_error("journal verification query failed");
        }
        const auto *text = sqlite3_column_text(statement, 0);
        return std::string(
            text == nullptr
                ? ""
                : reinterpret_cast<const char *>(text));
    };
    if (singleText("PRAGMA quick_check") != "ok") {
        throw std::runtime_error("journal quick_check failed");
    }
    sqlite3_stmt *foreignKeys = nullptr;
    if (sqlite3_prepare_v2(
            raw,
            "PRAGMA foreign_key_check",
            -1,
            &foreignKeys,
            nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("foreign-key check prepare failed");
    }
    const auto foreignFinalize =
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
            foreignKeys,
            sqlite3_finalize);
    if (sqlite3_step(foreignKeys) != SQLITE_DONE) {
        throw std::runtime_error("journal foreign-key check failed");
    }

    JournalEvidence evidence;
    auto sessionQuery = [&](const char *sql, auto consume) {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(raw, sql, -1, &statement, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error("journal evidence prepare failed");
        }
        const auto finalize =
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
                statement,
                sqlite3_finalize);
        sqlite3_bind_text(
            statement,
            1,
            sessionId.data(),
            static_cast<int>(sessionId.size()),
            SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            throw std::runtime_error("journal evidence query failed");
        }
        consume(statement);
    };
    sessionQuery(
        "SELECT phase, journal_checkpoint, highest_segment_revision, "
        "finalize_reason "
        "FROM sessions WHERE session_id = ?",
        [&](sqlite3_stmt *row) {
            evidence.phase = sqlite3_column_int(row, 0);
            evidence.checkpoint = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 1));
            evidence.highestRevision = static_cast<std::uint32_t>(
                sqlite3_column_int64(row, 2));
            evidence.finalizeReason = sqlite3_column_int(row, 3);
        });
    sessionQuery(
        "SELECT COUNT(*), COUNT(DISTINCT stable_id), "
        "COALESCE(SUM(revision = 2), 0) "
        "FROM segments WHERE session_id = ?",
        [&](sqlite3_stmt *row) {
            evidence.segmentRows = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 0));
            evidence.distinctSegmentIds = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 1));
            evidence.revisionTwoRows = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 2));
        });
    sessionQuery(
        "SELECT COUNT(*), COALESCE(MAX(journal_checkpoint), 0), "
        "COALESCE(MAX(highest_segment_revision), 0) "
        "FROM publication_receipts WHERE session_id = ?",
        [&](sqlite3_stmt *row) {
            evidence.receiptRows = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 0));
            evidence.receiptCheckpoint = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 1));
            evidence.receiptHighestRevision =
                static_cast<std::uint32_t>(
                    sqlite3_column_int64(row, 2));
        });
    sessionQuery(
        "SELECT COALESCE(SUM(accepted_frames), 0), "
        "COALESCE(SUM(discontinuities), 0) "
        "FROM sources WHERE session_id = ?",
        [&](sqlite3_stmt *row) {
            evidence.sourceAcceptedFrames = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 0));
            evidence.sourceDiscontinuities = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 1));
        });
    const std::string sourceEventSummarySql =
        "SELECT COALESCE(SUM(test_injected), 0), "
        "COALESCE(MIN(CASE WHEN event_kind = "
        + std::to_string(LS_SOURCE_EVENT_RECOVERED)
        + " THEN end_time_ns - start_time_ns END), 0) "
          "FROM source_events WHERE session_id = ?";
    sessionQuery(
        sourceEventSummarySql.c_str(),
        [&](sqlite3_stmt *row) {
            evidence.testInjectedSourceEventRows =
                static_cast<std::uint64_t>(
                    sqlite3_column_int64(row, 0));
            evidence.minimumRecoveredDurationNs =
                sqlite3_column_int64(row, 1);
        });
    const std::string terminalStateSql =
        "SELECT COUNT(*) FROM state_events WHERE session_id = ? "
        "AND phase IN ("
        + std::to_string(LS_PHASE_COMPLETE) + ", "
        + std::to_string(LS_PHASE_INCOMPLETE_SOURCES) + ", "
        + std::to_string(LS_PHASE_INTERRUPTED) + ", "
        + std::to_string(LS_PHASE_FAILED_TO_START) + ")";
    sessionQuery(
        terminalStateSql.c_str(),
        [&](sqlite3_stmt *row) {
            evidence.terminalStateRows = static_cast<std::uint64_t>(
                sqlite3_column_int64(row, 0));
        });

    sqlite3_stmt *sourceEvents = nullptr;
    if (sqlite3_prepare_v2(
            raw,
            "SELECT event_kind, COUNT(*) FROM source_events "
            "WHERE session_id = ? GROUP BY event_kind",
            -1,
            &sourceEvents,
            nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("source-event evidence prepare failed");
    }
    const auto sourceEventsFinalize =
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
            sourceEvents,
            sqlite3_finalize);
    sqlite3_bind_text(
        sourceEvents,
        1,
        sessionId.data(),
        static_cast<int>(sessionId.size()),
        SQLITE_TRANSIENT);
    int sourceEventStep = SQLITE_ROW;
    while ((sourceEventStep = sqlite3_step(sourceEvents)) == SQLITE_ROW) {
        const auto kind = sqlite3_column_int(sourceEvents, 0);
        const auto count = static_cast<std::uint64_t>(
            sqlite3_column_int64(sourceEvents, 1));
        evidence.sourceEventRows += count;
        if (kind == LS_SOURCE_EVENT_DISCONTINUITY) {
            evidence.sourceDiscontinuityRows = count;
        } else if (kind == LS_SOURCE_EVENT_UNAVAILABLE) {
            evidence.sourceUnavailableRows = count;
        } else if (kind == LS_SOURCE_EVENT_RECOVERED) {
            evidence.sourceRecoveredRows = count;
        }
    }
    if (sourceEventStep != SQLITE_DONE) {
        throw std::runtime_error("source-event evidence query failed");
    }
    return evidence;
}

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--smoke") {
            options.durationSeconds = 12.0;
            options.speed = 10.0;
        } else if (argument == "--duration-seconds" && index + 1 < argc) {
            options.durationSeconds = std::stod(argv[++index]);
        } else if (argument == "--speed" && index + 1 < argc) {
            options.speed = std::stod(argv[++index]);
        } else if (argument == "--journal" && index + 1 < argc) {
            options.journalPath = argv[++index];
        } else if (argument == "--keep-journal") {
            options.keepJournal = true;
        } else if (argument == "--help") {
            std::cout
                << "Usage: LocalScribeCoreSoak [--smoke] "
                   "[--duration-seconds N] [--speed N] "
                   "[--journal PATH] [--keep-journal]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown or incomplete argument");
        }
    }
    if (!std::isfinite(options.durationSeconds)
        || options.durationSeconds < 1.0
        || !std::isfinite(options.speed) || options.speed <= 0.0) {
        throw std::runtime_error("duration and speed must be positive");
    }
    if (options.journalPath.empty()) {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        options.journalPath = std::filesystem::temp_directory_path()
            / ("localscribe-soak-" + std::to_string(stamp) + ".sqlite3");
    }
    return options;
}

int run(const Options &options)
{
    JournalCleanup cleanup(options.journalPath, options.keepJournal);
    Sha256 selfTest;
    static constexpr std::array<std::uint8_t, 3> kAbc{'a', 'b', 'c'};
    selfTest.update(kAbc.data(), kAbc.size());
    if (selfTest.finish()
        != "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad") {
        throw std::runtime_error("SHA-256 self-test failed");
    }
    const auto wallStarted = Clock::now();
    const std::string journalPath = options.journalPath.string();
    const std::string sessionId =
        "soak-" + std::to_string(
            wallStarted.time_since_epoch().count());
    const std::string sourceApp{"CoreSoak"};
    const std::string localSpeaker{"Me"};
    const std::string asrBackend{"fixture"};
    const std::string empty;
    const std::string diarizationBackend{"source-aware"};
    const std::string created{"2026-07-29T00:00:00Z"};

    ls_error_v1 error{};
    error.struct_size = sizeof(error);
    error.abi_version = LS_CORE_ABI_VERSION;
    CoreHandle core;
    ls_core_config_v1 coreConfig{};
    coreConfig.struct_size = sizeof(coreConfig);
    coreConfig.abi_version = LS_CORE_ABI_VERSION;
    coreConfig.flags = LS_CORE_CONFIG_ALLOW_TEST_BACKENDS;
    coreConfig.journal_path = view(journalPath);
    const auto createCore =
        ls_core_create_v1(&coreConfig, &core.value, &error);
    if (createCore != LS_OK) {
        throw std::runtime_error(
            "core create failed: " + errorText(error));
    }

    SessionHandle session;
    ls_session_config_v1 sessionConfig{};
    sessionConfig.struct_size = sizeof(sessionConfig);
    sessionConfig.abi_version = LS_CORE_ABI_VERSION;
    sessionConfig.session_id = view(sessionId);
    sessionConfig.journal_path = view(journalPath);
    sessionConfig.source_app = view(sourceApp);
    sessionConfig.local_speaker_name = view(localSpeaker);
    sessionConfig.asr_backend_id = view(asrBackend);
    sessionConfig.asr_model_path = view(empty);
    sessionConfig.diarization_backend_id = view(diarizationBackend);
    sessionConfig.created_at_iso8601 = view(created);
    sessionConfig.language_mode = LS_LANGUAGE_MODE_RUSSIAN_ENGLISH;
    sessionConfig.audio_queue_capacity_frames = 4096;
    sessionConfig.microphone_source_id = 1;
    sessionConfig.system_audio_source_id = 2;
    sessionConfig.required_source_mask =
        LS_REQUIRED_SOURCE_MICROPHONE | LS_REQUIRED_SOURCE_SYSTEM_AUDIO;
    const long double completenessThresholdNs = std::max<long double>(
        20'000'000.0L,
        static_cast<long double>(options.durationSeconds)
            * 0.04L * 1'000'000'000.0L);
    if (completenessThresholdNs
        > static_cast<long double>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("duration exceeds the soak timeline range");
    }
    sessionConfig.source_completeness_threshold_ns =
        static_cast<std::int64_t>(completenessThresholdNs);
    const auto createSession = ls_session_create_after_consent_v1(
        core.value,
        &sessionConfig,
        &session.value,
        &error);
    if (createSession != LS_OK) {
        throw std::runtime_error(
            "session create failed: " + errorText(error));
    }
    require(
        ls_session_mark_sources_ready_v1(session.value),
        LS_OK,
        "mark sources ready");

    constexpr std::uint32_t sampleRate = 48'000;
    constexpr std::uint32_t framesPerBuffer = 960;
    constexpr std::int64_t frameDurationNs = 20'000'000;
    const std::uint64_t totalTicks = static_cast<std::uint64_t>(
        std::ceil(options.durationSeconds * 50.0));
    const std::uint64_t cueEveryTicks =
        options.durationSeconds <= 60.0 ? 100u : 250u;
    const std::uint64_t pauseStart = totalTicks * 25u / 100u;
    const std::uint64_t pauseEnd =
        std::max<std::uint64_t>(pauseStart + 1u, totalTicks * 35u / 100u);
    const std::uint64_t micLoss = totalTicks * 50u / 100u;
    const std::uint64_t micRestore =
        std::max<std::uint64_t>(micLoss + 1u, totalTicks * 58u / 100u);
    const std::uint64_t systemLoss = totalTicks * 68u / 100u;
    const std::uint64_t systemRestore =
        std::max<std::uint64_t>(systemLoss + 1u, totalTicks * 76u / 100u);
    constexpr std::int64_t timelineBaseNs = 1'000'000'000;
    const auto timestampForTick = [](std::uint64_t tick) {
        return timelineBaseNs
            + static_cast<std::int64_t>(tick) * frameDurationNs;
    };
    const std::int64_t micLossTimestamp = timestampForTick(micLoss);
    const std::int64_t systemLossTimestamp = timestampForTick(systemLoss);

    std::array<std::uint64_t, 2> sequences{};
    std::array<bool, 2> available{true, true};
    std::array<bool, 2> discontinuity{true, true};
    std::vector<float> microphone(framesPerBuffer, 0.0F);
    std::vector<float> system(framesPerBuffer, 0.0F);
    bool paused = false;
    std::uint64_t logicalFrames = 0;
    std::uint64_t offeredByRunner = 0;
    std::uint64_t acceptedByRunner = 0;
    std::uint64_t backpressureRejections = 0;
    std::uint64_t pauseRejections = 0;
    std::uint64_t acceptedAudioDiscontinuities = 0;
    std::uint64_t acceptedCues = 0;
    std::map<
        std::pair<std::uint64_t, std::uint64_t>,
        std::uint32_t>
        expectedSegments;

    const std::string unavailableReason{"soak source unavailable"};
    const std::string recoveredReason{"soak source recovered"};
    constexpr std::uint32_t queuePacingDepth = 1'024;
    const auto waitForQueueRoom = [&] {
        for (;;) {
            ls_pipeline_metrics_v1 live{};
            live.struct_size = sizeof(live);
            live.abi_version = LS_CORE_ABI_VERSION;
            require(
                ls_session_copy_metrics_v1(session.value, &live),
                LS_OK,
                "copy live metrics");
            if (live.audio_queue_depth < queuePacingDepth) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    for (std::uint64_t tick = 0; tick < totalTicks; ++tick) {
        const std::int64_t timestamp = timestampForTick(tick);
        if (tick == pauseStart) {
            require(
                ls_session_pause_v1(session.value),
                LS_OK,
                "pause");
            paused = true;
        }
        if (tick == pauseEnd) {
            require(
                ls_session_resume_after_consent_v1(session.value),
                LS_OK,
                "resume");
            paused = false;
            discontinuity = {true, true};
        }
        if (tick == micLoss) {
            sourceEvent(
                session.value,
                1,
                LS_SOURCE_KIND_MICROPHONE,
                LS_SOURCE_EVENT_UNAVAILABLE,
                LS_SOURCE_HEALTH_TEMPORARILY_UNAVAILABLE,
                timestamp,
                timestamp,
                unavailableReason);
            available[0] = false;
        }
        if (tick == micRestore) {
            sourceEvent(
                session.value,
                1,
                LS_SOURCE_KIND_MICROPHONE,
                LS_SOURCE_EVENT_RECOVERED,
                LS_SOURCE_HEALTH_READY,
                micLossTimestamp,
                timestamp,
                recoveredReason);
            available[0] = true;
            discontinuity[0] = true;
        }
        if (tick == systemLoss) {
            sourceEvent(
                session.value,
                2,
                LS_SOURCE_KIND_SYSTEM_AUDIO,
                LS_SOURCE_EVENT_UNAVAILABLE,
                LS_SOURCE_HEALTH_TEMPORARILY_UNAVAILABLE,
                timestamp,
                timestamp,
                unavailableReason);
            available[1] = false;
        }
        if (tick == systemRestore) {
            sourceEvent(
                session.value,
                2,
                LS_SOURCE_KIND_SYSTEM_AUDIO,
                LS_SOURCE_EVENT_RECOVERED,
                LS_SOURCE_HEALTH_READY,
                systemLossTimestamp,
                timestamp,
                recoveredReason);
            available[1] = true;
            discontinuity[1] = true;
        }

        for (std::size_t sourceIndex = 0; sourceIndex < 2; ++sourceIndex) {
            /* One rejected probe per source proves pause gating; capture is
               otherwise suspended until explicit resume. */
            if (paused && tick != pauseStart) {
                continue;
            }
            if (!available[sourceIndex]) {
                continue;
            }
            auto &samples = sourceIndex == 0 ? microphone : system;
            std::fill(samples.begin(), samples.end(), 0.0F);
            const bool cue = tick % cueEveryTicks == 0;
            std::uint64_t cueId = 0;
            std::uint32_t cueRevision = 0;
            if (cue) {
                const std::uint64_t cueOrdinal = tick / cueEveryTicks;
                cueId = 10u + cueOrdinal / 2u;
                cueRevision = cueOrdinal % 2u == 0 ? 1u : 2u;
                const float encoded =
                    static_cast<float>(cueId) / 1'000.0F;
                samples.front() =
                    cueRevision == 1u ? encoded : -encoded;
            }
            ls_audio_frame_v1 frame{};
            frame.struct_size = sizeof(frame);
            frame.abi_version = LS_CORE_ABI_VERSION;
            frame.source_id = sourceIndex + 1u;
            frame.sequence_number = ++sequences[sourceIndex];
            frame.monotonic_time_ns = timestamp;
            frame.sample_rate_hz = sampleRate;
            frame.channel_count = 1;
            frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
            frame.frame_count = framesPerBuffer;
            frame.samples = samples.data();
            ++logicalFrames;

            if (paused) {
                ++offeredByRunner;
                const auto status =
                    ls_session_push_audio_v1(session.value, &frame);
                if (status != LS_INVALID_STATE) {
                    throw std::runtime_error(
                        "paused audio probe returned "
                        + std::to_string(status));
                }
                ++pauseRejections;
                discontinuity[sourceIndex] = true;
                continue;
            }

            std::uint32_t consecutiveBackpressure = 0;
            for (;;) {
                waitForQueueRoom();
                frame.flags = discontinuity[sourceIndex]
                    ? LS_AUDIO_FLAG_DISCONTINUITY
                    : 0;
                if (tick + 1 == totalTicks) {
                    frame.flags |= LS_AUDIO_FLAG_END_OF_STREAM;
                }
                ++offeredByRunner;
                const auto status =
                    ls_session_push_audio_v1(session.value, &frame);
                if (status == LS_OK) {
                    ++acceptedByRunner;
                    if ((frame.flags & LS_AUDIO_FLAG_DISCONTINUITY) != 0) {
                        ++acceptedAudioDiscontinuities;
                    }
                    discontinuity[sourceIndex] = false;
                    if (cue) {
                        ++acceptedCues;
                        auto &highest = expectedSegments[
                            {sourceIndex + 1u, cueId}];
                        highest = std::max(highest, cueRevision);
                    }
                    break;
                }
                if (status != LS_BACKPRESSURE) {
                    throw std::runtime_error(
                        "unexpected audio push status "
                        + std::to_string(status));
                }
                ++backpressureRejections;
                discontinuity[sourceIndex] = true;
                if (++consecutiveBackpressure > 10'000u) {
                    throw std::runtime_error(
                        "audio push made no progress under backpressure");
                }
                std::this_thread::sleep_for(
                    std::chrono::microseconds(50));
            }
        }

        const auto target = wallStarted
            + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(
                    static_cast<double>(tick + 1u) * 0.02
                    / options.speed));
        std::this_thread::sleep_until(target);
    }
    if (paused) {
        require(
            ls_session_resume_after_consent_v1(session.value),
            LS_OK,
            "final resume");
    }
    require(
        ls_session_finalize_v1(
            session.value,
            LS_FINALIZE_REASON_USER_STOP),
        LS_OK,
        "finalize");

    std::uint64_t finalEvents = 0;
    std::uint64_t sourceEvents = 0;
    std::uint64_t sourceDiscontinuityEvents = 0;
    std::uint64_t sourceUnavailableEvents = 0;
    std::uint64_t sourceRecoveredEvents = 0;
    std::uint64_t testInjectedSourceEvents = 0;
    std::uint64_t terminalEvents = 0;
    std::uint64_t errorEvents = 0;
    ls_state_event_copy_v1 terminalState{};
    terminalState.struct_size = sizeof(terminalState);
    terminalState.abi_version = LS_CORE_ABI_VERSION;
    for (;;) {
        ls_event_t *event = nullptr;
        const auto status =
            ls_session_next_event_v1(session.value, 0, &event);
        if (status == LS_TIMEOUT) {
            break;
        }
        require(status, LS_OK, "event poll");
        const auto kind = ls_event_kind(event);
        if (kind == LS_EVENT_FINAL_SEGMENT) {
            ++finalEvents;
        } else if (kind == LS_EVENT_SOURCE_CHANGED) {
            ls_source_event_copy_v1 source{};
            source.struct_size = sizeof(source);
            source.abi_version = LS_CORE_ABI_VERSION;
            require(
                ls_event_copy_source_v1(event, &source),
                LS_OK,
                "copy source event");
            ++sourceEvents;
            if ((source.flags & LS_SOURCE_EVENT_FLAG_TEST_INJECTED) != 0) {
                ++testInjectedSourceEvents;
            }
            if (source.event_kind == LS_SOURCE_EVENT_DISCONTINUITY) {
                ++sourceDiscontinuityEvents;
            } else if (source.event_kind == LS_SOURCE_EVENT_UNAVAILABLE) {
                ++sourceUnavailableEvents;
            } else if (source.event_kind == LS_SOURCE_EVENT_RECOVERED) {
                ++sourceRecoveredEvents;
            } else {
                throw std::runtime_error(
                    "unexpected source event kind "
                    + std::to_string(source.event_kind));
            }
        } else if (kind == LS_EVENT_TERMINAL) {
            ++terminalEvents;
            require(
                ls_event_copy_state_v1(event, &terminalState),
                LS_OK,
                "copy terminal state");
        } else if (kind == LS_EVENT_ERROR) {
            ++errorEvents;
        }
        ls_event_destroy(event);
    }

    ls_pipeline_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LS_CORE_ABI_VERSION;
    require(
        ls_session_copy_metrics_v1(session.value, &metrics),
        LS_OK,
        "copy metrics");
    ls_state_event_copy_v1 authoritativeState{};
    authoritativeState.struct_size = sizeof(authoritativeState);
    authoritativeState.abi_version = LS_CORE_ABI_VERSION;
    require(
        ls_session_copy_state_v1(session.value, &authoritativeState),
        LS_OK,
        "copy authoritative state");

    const std::uint64_t expectedRevisionTwo =
        static_cast<std::uint64_t>(std::count_if(
            expectedSegments.begin(),
            expectedSegments.end(),
            [](const auto &entry) { return entry.second == 2u; }));
    const std::uint64_t expectedSourceDiscontinuities =
        4u + acceptedAudioDiscontinuities;
    const std::uint64_t expectedSourceEvents =
        expectedSourceDiscontinuities + 4u;
    const std::uint64_t allowedBackpressure =
        std::max<std::uint64_t>(
            4u,
            (logicalFrames + 99u) / 100u);
    if (logicalFrames != acceptedByRunner + pauseRejections
        || pauseRejections != 2u
        || backpressureRejections > allowedBackpressure
        || metrics.frames_offered != offeredByRunner
        || metrics.frames_accepted != acceptedByRunner
        || metrics.frames_rejected
            != backpressureRejections + pauseRejections
        || metrics.frames_accepted + metrics.frames_rejected
            != metrics.frames_offered
        || metrics.final_segments_committed != acceptedCues
        || finalEvents != metrics.final_segments_committed
        || metrics.audio_queue_depth != 0
        || metrics.audio_queue_high_water > queuePacingDepth
        || metrics.highest_segment_revision != 2u
        || sourceEvents != expectedSourceEvents
        || sourceDiscontinuityEvents != expectedSourceDiscontinuities
        || sourceUnavailableEvents != 2u
        || sourceRecoveredEvents != 2u
        || testInjectedSourceEvents != 4u
        || terminalEvents != 1u
        || errorEvents != 0u
        || terminalState.phase != LS_PHASE_INCOMPLETE_SOURCES
        || terminalState.published_status
            != LS_PUBLISHED_STATUS_INCOMPLETE_SOURCES
        || terminalState.finalize_reason != LS_FINALIZE_REASON_USER_STOP
        || authoritativeState.phase != LS_PHASE_INCOMPLETE_SOURCES
        || authoritativeState.published_status
            != LS_PUBLISHED_STATUS_INCOMPLETE_SOURCES
        || authoritativeState.finalize_reason
            != LS_FINALIZE_REASON_USER_STOP
        || expectedSegments.empty()
        || expectedRevisionTwo == 0u) {
        throw std::runtime_error("pipeline reconciliation failed");
    }

    const std::string title{"Core soak"};
    const std::string ended{"2026-07-29T02:00:00Z"};
    ls_markdown_options_v1 markdownOptions{};
    markdownOptions.struct_size = sizeof(markdownOptions);
    markdownOptions.abi_version = LS_CORE_ABI_VERSION;
    markdownOptions.title = view(title);
    markdownOptions.created_at_iso8601 = view(created);
    markdownOptions.ended_at_iso8601 = view(ended);
    markdownOptions.duration_seconds =
        static_cast<std::int64_t>(options.durationSeconds);
    markdownOptions.microphone_captured = 1;
    markdownOptions.system_audio_captured = 1;
    BytesHandle markdown;
    ls_render_snapshot_v1 renderSnapshot{};
    renderSnapshot.struct_size = sizeof(renderSnapshot);
    renderSnapshot.abi_version = LS_CORE_ABI_VERSION;
    require(
        ls_session_render_markdown_with_snapshot_v1(
            session.value,
            &markdownOptions,
            &markdown.value,
            &renderSnapshot,
            &error),
        LS_OK,
        "render Markdown");
    const auto *markdownData = ls_owned_bytes_data(markdown.value);
    const std::size_t markdownSize =
        ls_owned_bytes_size(markdown.value);
    const std::string_view markdownView{
        reinterpret_cast<const char *>(markdownData),
        markdownSize};
    if (occurrences(markdownView, "<!-- transcript:start -->") != 1
        || occurrences(markdownView, "<!-- transcript:end -->") != 1
        || occurrences(markdownView, "<!-- capture-events:start -->") != 1
        || occurrences(markdownView, "<!-- capture-events:end -->") != 1
        || occurrences(markdownView, "fixture cue ")
            != expectedSegments.size()
        || occurrences(markdownView, " revised") != expectedRevisionTwo
        || occurrences(markdownView, "temporarily unavailable.") != 2u
        || occurrences(markdownView, "recovered.") != 2u
        || occurrences(markdownView, "discontinuity.")
            != expectedSourceDiscontinuities
        || markdownView.find("status: \"incomplete_sources\"")
            == std::string_view::npos
        || markdownView.find("summary") != std::string_view::npos
        || renderSnapshot.journal_checkpoint
            != metrics.journal_checkpoint
        || renderSnapshot.highest_segment_revision
            != metrics.highest_segment_revision) {
        throw std::runtime_error("Markdown reconciliation failed");
    }

    Sha256 hasher;
    hasher.update(markdownData, markdownSize);
    const std::string digest = hasher.finish();
    const std::string fileIdentity{"soak-memory-snapshot"};
    const auto publishedAt = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    ls_publication_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.abi_version = LS_CORE_ABI_VERSION;
    receipt.journal_checkpoint = renderSnapshot.journal_checkpoint;
    receipt.highest_segment_revision =
        renderSnapshot.highest_segment_revision;
    receipt.destination = LS_PUBLICATION_DESTINATION_STAGING;
    receipt.published_at_unix_ns = publishedAt;
    receipt.sha256_hex = view(digest);
    receipt.file_identity = view(fileIdentity);
    require(
        ls_session_ack_publication_v1(session.value, &receipt),
        LS_OK,
        "publication acknowledgement");

    const auto journal =
        verifyJournal(options.journalPath, sessionId);
    if (journal.phase != LS_PHASE_INCOMPLETE_SOURCES
        || journal.finalizeReason != LS_FINALIZE_REASON_USER_STOP
        || journal.terminalStateRows != 1u
        || journal.checkpoint != renderSnapshot.journal_checkpoint
        || journal.highestRevision != receipt.highest_segment_revision
        || journal.segmentRows != metrics.final_segments_committed
        || journal.distinctSegmentIds != expectedSegments.size()
        || journal.revisionTwoRows != expectedRevisionTwo
        || journal.receiptRows != 1u
        || journal.receiptCheckpoint != renderSnapshot.journal_checkpoint
        || journal.receiptHighestRevision
            != renderSnapshot.highest_segment_revision
        || journal.sourceEventRows != sourceEvents
        || journal.sourceDiscontinuityRows
            != sourceDiscontinuityEvents
        || journal.sourceUnavailableRows != sourceUnavailableEvents
        || journal.sourceRecoveredRows != sourceRecoveredEvents
        || journal.testInjectedSourceEventRows
            != testInjectedSourceEvents
        || journal.minimumRecoveredDurationNs
            <= sessionConfig.source_completeness_threshold_ns
        || journal.sourceAcceptedFrames != acceptedByRunner
        || journal.sourceDiscontinuities
            != journal.sourceDiscontinuityRows
                + journal.sourceUnavailableRows) {
        throw std::runtime_error("journal reconciliation failed");
    }

    const double wallSeconds =
        std::chrono::duration<double>(Clock::now() - wallStarted).count();
    std::cout << "LocalScribe core soak: PASS\n";
    std::cout << "simulated_seconds=" << options.durationSeconds << '\n';
    std::cout << "wall_seconds=" << std::fixed << std::setprecision(3)
              << wallSeconds << '\n';
    std::cout << "logical_frames=" << logicalFrames << '\n';
    std::cout << "frames_offered=" << metrics.frames_offered << '\n';
    std::cout << "frames_accepted=" << metrics.frames_accepted << '\n';
    std::cout << "frames_rejected=" << metrics.frames_rejected << '\n';
    std::cout << "backpressure_rejections="
              << backpressureRejections << '\n';
    std::cout << "allowed_backpressure=" << allowedBackpressure << '\n';
    std::cout << "pause_rejections=" << pauseRejections << '\n';
    std::cout << "queue_high_water="
              << metrics.audio_queue_high_water << '\n';
    std::cout << "discontinuities=" << metrics.discontinuities << '\n';
    std::cout << "final_segments=" << metrics.final_segments_committed << '\n';
    std::cout << "distinct_segments=" << journal.distinctSegmentIds
              << '\n';
    std::cout << "revision_two_segments=" << journal.revisionTwoRows
              << '\n';
    std::cout << "final_events=" << finalEvents << '\n';
    std::cout << "source_events=" << sourceEvents << '\n';
    std::cout << "terminal_phase=" << authoritativeState.phase << '\n';
    std::cout << "journal_checkpoint=" << metrics.journal_checkpoint << '\n';
    std::cout << "markdown_bytes=" << markdownSize << '\n';
    std::cout << "sha256=" << digest << '\n';
    std::cout << "journal_quick_check=ok\n";
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        return run(parseOptions(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << "LocalScribe core soak: FAIL: " << error.what() << '\n';
        return 1;
    }
}
