#include "RecoveryJournal.hpp"

#include "Migrations.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace localscribe {
namespace {

class Statement {
public:
    Statement() = default;
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    ~Statement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    sqlite3_stmt **out() { return &statement_; }
    sqlite3_stmt *get() const { return statement_; }

private:
    sqlite3_stmt *statement_{};
};

constexpr std::size_t kMaximumEmbeddingDimensions = 4'096;
constexpr std::size_t kMaximumProfilePrototypes = 6;
constexpr std::size_t kMaximumVoiceProfiles = 1'024;

std::vector<std::uint8_t> encodeEmbedding(std::span<const float> values)
{
    std::vector<std::uint8_t> result;
    result.reserve(values.size() * sizeof(float));
    for (const float value : values) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        result.push_back(static_cast<std::uint8_t>(bits));
        result.push_back(static_cast<std::uint8_t>(bits >> 8u));
        result.push_back(static_cast<std::uint8_t>(bits >> 16u));
        result.push_back(static_cast<std::uint8_t>(bits >> 24u));
    }
    return result;
}

Expected<std::vector<float>> decodeEmbedding(
    sqlite3_stmt *statement,
    int column,
    bool allowEmpty)
{
    const int byteCount = sqlite3_column_bytes(statement, column);
    const auto *bytes = static_cast<const std::uint8_t *>(
        sqlite3_column_blob(statement, column));
    if (byteCount == 0 && allowEmpty) {
        return std::vector<float>{};
    }
    if (bytes == nullptr || byteCount <= 0
        || byteCount % static_cast<int>(sizeof(float)) != 0
        || static_cast<std::size_t>(byteCount / sizeof(float))
            > kMaximumEmbeddingDimensions) {
        return Error{
            LS_RECOVERY_ERROR,
            "journal has an invalid speaker embedding"};
    }
    std::vector<float> result;
    result.reserve(static_cast<std::size_t>(byteCount) / sizeof(float));
    for (int offset = 0; offset < byteCount; offset += 4) {
        const std::uint32_t bits =
            static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u)
            | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
        const float value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) {
            return Error{
                LS_RECOVERY_ERROR,
                "journal speaker embedding contains a non-finite value"};
        }
        result.push_back(value);
    }
    return result;
}

void bindEmbedding(
    sqlite3_stmt *statement,
    int index,
    std::span<const float> values)
{
    const auto bytes = encodeEmbedding(values);
    if (bytes.empty()) {
        sqlite3_bind_zeroblob(statement, index, 0);
        return;
    }
    sqlite3_bind_blob(
        statement,
        index,
        bytes.data(),
        static_cast<int>(bytes.size()),
        SQLITE_TRANSIENT);
}

bool validEmbedding(
    const std::string &modelId,
    std::span<const float> values,
    bool allowEmpty)
{
    if (values.empty()) {
        return allowEmpty && modelId.empty();
    }
    return !modelId.empty() && modelId.size() <= 256
        && values.size() <= kMaximumEmbeddingDimensions
        && std::all_of(values.begin(), values.end(), [](float value) {
               return std::isfinite(value);
           });
}

bool isUnicodeWhitespace(std::uint32_t codePoint)
{
    return codePoint == 0x00A0u || codePoint == 0x1680u
        || (codePoint >= 0x2000u && codePoint <= 0x200Au)
        || codePoint == 0x2028u || codePoint == 0x2029u
        || codePoint == 0x202Fu || codePoint == 0x205Fu
        || codePoint == 0x3000u;
}

bool validProfileName(const std::string &value)
{
    if (value.empty() || value.size() > 256) {
        return false;
    }
    std::size_t index = 0;
    bool hasVisibleCharacter = false;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first < 0x80u) {
            if (first == 0 || first < 0x20u || first == 0x7Fu) {
                return false;
            }
            if (std::isspace(first) == 0) {
                hasVisibleCharacter = true;
            }
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2u && first <= 0xDFu) {
            length = 2;
            codePoint = first & 0x1Fu;
            minimum = 0x80u;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            length = 3;
            codePoint = first & 0x0Fu;
            minimum = 0x800u;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            length = 4;
            codePoint = first & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto byte =
                static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xC0u) != 0x80u) {
                return false;
            }
            codePoint = (codePoint << 6u) | (byte & 0x3Fu);
        }
        if (codePoint < minimum || codePoint > 0x10FFFFu
            || (codePoint >= 0xD800u && codePoint <= 0xDFFFu)
            || (codePoint >= 0x80u && codePoint <= 0x9Fu)
            || codePoint == 0x2028u || codePoint == 0x2029u) {
            return false;
        }
        if (!isUnicodeWhitespace(codePoint)) {
            hasVisibleCharacter = true;
        }
        index += length;
    }
    return hasVisibleCharacter;
}

bool equalNameCaseInsensitive(
    const std::string &left,
    const std::string &right)
{
    return sqlite3_stricmp(left.c_str(), right.c_str()) == 0;
}

bool normalizeEmbedding(std::vector<float> &values)
{
    long double magnitude = 0.0L;
    for (const float value : values) {
        magnitude += static_cast<long double>(value) * value;
    }
    if (!std::isfinite(magnitude) || magnitude <= 1.0e-12L) {
        return false;
    }
    const float scale =
        1.0F / static_cast<float>(std::sqrt(magnitude));
    for (float &value : values) {
        value *= scale;
    }
    return true;
}

float cosineSimilarity(
    std::span<const float> left,
    std::span<const float> right)
{
    if (left.empty() || left.size() != right.size()) {
        return -1.0F;
    }
    return std::inner_product(
        left.begin(),
        left.end(),
        right.begin(),
        0.0F);
}

std::int64_t unixTimeNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Error sqliteError(sqlite3 *database, std::string context)
{
    return Error{
        LS_SQLITE_ERROR,
        std::move(context) + ": " + sqlite3_errmsg(database)};
}

Expected<void> execute(sqlite3 *database, const char *sql)
{
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return sqliteError(database, "SQLite statement failed");
    }
    return success();
}

Expected<void> prepare(
    sqlite3 *database,
    const char *sql,
    Statement &statement)
{
    if (sqlite3_prepare_v2(database, sql, -1, statement.out(), nullptr)
        != SQLITE_OK) {
        return sqliteError(database, "cannot prepare SQLite statement");
    }
    return success();
}

Expected<void> stepDone(sqlite3 *database, sqlite3_stmt *statement)
{
    if (sqlite3_step(statement) != SQLITE_DONE) {
        return sqliteError(database, "cannot execute SQLite statement");
    }
    return success();
}

void bindText(sqlite3_stmt *statement, int index, const std::string &value)
{
    sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
}

std::string columnText(sqlite3_stmt *statement, int index)
{
    const auto *bytes = sqlite3_column_text(statement, index);
    const int length = sqlite3_column_bytes(statement, index);
    if (bytes == nullptr || length <= 0) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char *>(bytes),
        static_cast<std::size_t>(length));
}

class Transaction {
public:
    explicit Transaction(sqlite3 *database) : database_(database)
    {
        active_ = sqlite3_exec(
                      database_,
                      "BEGIN IMMEDIATE",
                      nullptr,
                      nullptr,
                      nullptr)
            == SQLITE_OK;
    }
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    ~Transaction()
    {
        if (active_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] Expected<void> commit()
    {
        if (!active_) {
            return sqliteError(database_, "transaction was not started");
        }
        if (sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr)
            != SQLITE_OK) {
            return sqliteError(database_, "cannot commit transaction");
        }
        active_ = false;
        return success();
    }

private:
    sqlite3 *database_{};
    bool active_{false};
};

bool validSessionId(const std::string &value)
{
    if (value.empty() || value.size() > 128) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte == 0 || byte < 0x20 || byte == 0x7F;
    });
}

bool sameSegment(sqlite3_stmt *row, const TranscriptSegment &segment)
{
    const auto *stable = static_cast<const std::uint8_t *>(
        sqlite3_column_blob(row, 0));
    if (stable == nullptr || sqlite3_column_bytes(row, 0) != 16
        || !std::equal(
            segment.stableId.begin(),
            segment.stableId.end(),
            stable)) {
        return false;
    }
    const auto encodedEmbedding = encodeEmbedding(segment.speakerEmbedding);
    const auto *storedEmbedding = static_cast<const std::uint8_t *>(
        sqlite3_column_blob(row, 12));
    const int storedEmbeddingSize = sqlite3_column_bytes(row, 12);
    const bool sameEmbedding = encodedEmbedding.empty()
        ? storedEmbeddingSize == 0
        : storedEmbedding != nullptr
            && storedEmbeddingSize
                == static_cast<int>(encodedEmbedding.size())
            && std::equal(
                encodedEmbedding.begin(),
                encodedEmbedding.end(),
                storedEmbedding);
    return static_cast<std::uint64_t>(sqlite3_column_int64(row, 1))
            == segment.sourceId
        && sqlite3_column_int64(row, 2) == segment.startTimeNs
        && sqlite3_column_int64(row, 3) == segment.endTimeNs
        && static_cast<std::uint64_t>(sqlite3_column_int64(row, 4))
            == segment.speakerId
        && columnText(row, 5) == segment.speakerLabel
        && columnText(row, 6) == segment.text
        && columnText(row, 7) == segment.language
        && static_cast<float>(sqlite3_column_double(row, 8))
            == segment.confidence
        && static_cast<std::uint32_t>(sqlite3_column_int64(row, 9))
            == segment.flags
        && columnText(row, 10) == segment.speakerEmbeddingModel
        && static_cast<std::size_t>(sqlite3_column_int64(row, 11))
            == segment.speakerEmbedding.size()
        && sameEmbedding;
}

bool validFinalSegment(const TranscriptSegment &segment)
{
    return (segment.flags & LS_SEGMENT_FLAG_FINAL) != 0
        && segment.revision != 0
        && segment.endTimeNs >= segment.startTimeNs
        && std::isfinite(segment.confidence)
        && segment.confidence >= 0.0F && segment.confidence <= 1.0F
        && validEmbedding(
            segment.speakerEmbeddingModel,
            segment.speakerEmbedding,
            true);
}

bool sameSegmentPayload(
    const TranscriptSegment &left,
    const TranscriptSegment &right)
{
    return left.stableId == right.stableId
        && left.revision == right.revision
        && left.sourceId == right.sourceId
        && left.startTimeNs == right.startTimeNs
        && left.endTimeNs == right.endTimeNs
        && left.speakerId == right.speakerId
        && left.speakerLabel == right.speakerLabel
        && left.text == right.text
        && left.language == right.language
        && left.confidence == right.confidence
        && left.flags == right.flags
        && left.speakerEmbeddingModel == right.speakerEmbeddingModel
        && left.speakerEmbedding == right.speakerEmbedding;
}

Expected<TranscriptSegment> decodeStoredSegment(sqlite3_stmt *row)
{
    TranscriptSegment segment;
    const auto *stable = static_cast<const std::uint8_t *>(
        sqlite3_column_blob(row, 0));
    if (stable == nullptr || sqlite3_column_bytes(row, 0) != 16) {
        return Error{LS_RECOVERY_ERROR, "journal has invalid stable ID"};
    }
    std::copy_n(stable, 16, segment.stableId.begin());
    segment.sourceId = static_cast<std::uint64_t>(
        sqlite3_column_int64(row, 1));
    segment.startTimeNs = sqlite3_column_int64(row, 2);
    segment.endTimeNs = sqlite3_column_int64(row, 3);
    segment.speakerId = static_cast<std::uint64_t>(
        sqlite3_column_int64(row, 4));
    segment.speakerLabel = columnText(row, 5);
    segment.text = columnText(row, 6);
    segment.language = columnText(row, 7);
    segment.confidence = static_cast<float>(sqlite3_column_double(row, 8));
    segment.flags = static_cast<std::uint32_t>(
        sqlite3_column_int64(row, 9));
    segment.speakerEmbeddingModel = columnText(row, 10);
    const auto embedding = decodeEmbedding(row, 12, true);
    segment.revision = static_cast<std::uint32_t>(
        sqlite3_column_int64(row, 13));
    segment.journalCheckpoint = static_cast<std::uint64_t>(
        sqlite3_column_int64(row, 14));
    if (!embedding
        || static_cast<std::size_t>(sqlite3_column_int64(row, 11))
            != embedding.value().size()) {
        return Error{
            LS_RECOVERY_ERROR,
            "journal has an invalid pending or visible segment"};
    }
    segment.speakerEmbedding = embedding.value();
    if (!validFinalSegment(segment)) {
        return Error{
            LS_RECOVERY_ERROR,
            "journal has an invalid pending or visible segment"};
    }
    return segment;
}

Expected<std::vector<TranscriptSegment>> loadPendingSegments(
    sqlite3 *database,
    const std::string &sessionId,
    std::uint64_t groupId)
{
    Statement statement;
    auto prepared = prepare(
        database,
        R"SQL(
SELECT stable_id, source_id, start_time_ns, end_time_ns, speaker_id,
       speaker_label, text, language, confidence, flags,
       speaker_embedding_model, speaker_embedding_dimension,
       speaker_embedding, revision, staged_checkpoint
FROM pending_speaker_segments
WHERE session_id = ? AND group_id = ?
ORDER BY start_time_ns, end_time_ns, source_id, hex(stable_id), revision
)SQL",
        statement);
    if (!prepared) {
        return prepared.error();
    }
    bindText(statement.get(), 1, sessionId);
    sqlite3_bind_int64(
        statement.get(),
        2,
        static_cast<sqlite3_int64>(groupId));
    std::vector<TranscriptSegment> result;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        auto segment = decodeStoredSegment(statement.get());
        if (!segment) {
            return segment.error();
        }
        result.push_back(segment.takeValue());
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database, "cannot load pending speaker group");
    }
    return result;
}

Expected<TranscriptSegment> applySpeakerEnrollment(
    sqlite3 *database,
    const std::string &sessionId,
    const TranscriptSegment &segment)
{
    TranscriptSegment effective = segment;
    Statement enrollment;
    auto prepared = prepare(
        database,
        R"SQL(
SELECT profile_id, display_name
FROM session_voice_profile_enrollments
WHERE session_id = ? AND original_speaker_id = ?
)SQL",
        enrollment);
    if (!prepared) {
        return prepared.error();
    }
    bindText(enrollment.get(), 1, sessionId);
    sqlite3_bind_int64(
        enrollment.get(),
        2,
        static_cast<sqlite3_int64>(segment.speakerId));
    const int step = sqlite3_step(enrollment.get());
    if (step == SQLITE_DONE) {
        return effective;
    }
    if (step != SQLITE_ROW) {
        return sqliteError(database, "cannot inspect speaker enrollment");
    }
    const auto profileId = static_cast<std::uint64_t>(
        sqlite3_column_int64(enrollment.get(), 0));
    const auto displayName = columnText(enrollment.get(), 1);
    if (profileId == 0 || profileId > kSpeakerIdPayloadMask
        || !validProfileName(displayName)) {
        return Error{
            LS_RECOVERY_ERROR,
            "journal has an invalid speaker enrollment"};
    }
    effective.speakerId = persistentSpeakerId(profileId);
    effective.speakerLabel = displayName;
    return effective;
}

Expected<std::optional<TranscriptSegment>> loadLatestVisibleSegment(
    sqlite3 *database,
    const std::string &sessionId,
    const StableId &stableId)
{
    Statement statement;
    auto prepared = prepare(
        database,
        R"SQL(
SELECT stable_id, source_id, start_time_ns, end_time_ns, speaker_id,
       speaker_label, text, language, confidence, flags,
       speaker_embedding_model, speaker_embedding_dimension,
       speaker_embedding, revision, journal_checkpoint
FROM segments
WHERE session_id = ? AND stable_id = ?
ORDER BY revision DESC
LIMIT 1
)SQL",
        statement);
    if (!prepared) {
        return prepared.error();
    }
    bindText(statement.get(), 1, sessionId);
    sqlite3_bind_blob(
        statement.get(),
        2,
        stableId.data(),
        static_cast<int>(stableId.size()),
        SQLITE_TRANSIENT);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return std::optional<TranscriptSegment>{};
    }
    if (step != SQLITE_ROW) {
        return sqliteError(database, "cannot inspect visible segment");
    }
    auto segment = decodeStoredSegment(statement.get());
    if (!segment) {
        return segment.error();
    }
    return std::optional<TranscriptSegment>{segment.takeValue()};
}

Expected<std::optional<TranscriptSegment>> loadVisibleSegmentRevision(
    sqlite3 *database,
    const std::string &sessionId,
    const StableId &stableId,
    std::uint32_t revision)
{
    Statement statement;
    auto prepared = prepare(
        database,
        R"SQL(
SELECT stable_id, source_id, start_time_ns, end_time_ns, speaker_id,
       speaker_label, text, language, confidence, flags,
       speaker_embedding_model, speaker_embedding_dimension,
       speaker_embedding, revision, journal_checkpoint
FROM segments
WHERE session_id = ? AND stable_id = ? AND revision = ?
)SQL",
        statement);
    if (!prepared) {
        return prepared.error();
    }
    bindText(statement.get(), 1, sessionId);
    sqlite3_bind_blob(
        statement.get(),
        2,
        stableId.data(),
        static_cast<int>(stableId.size()),
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 3, revision);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return std::optional<TranscriptSegment>{};
    }
    if (step != SQLITE_ROW) {
        return sqliteError(database, "cannot inspect visible segment revision");
    }
    auto segment = decodeStoredSegment(statement.get());
    if (!segment) {
        return segment.error();
    }
    return std::optional<TranscriptSegment>{segment.takeValue()};
}

bool validSpeakerTurn(const SpeakerTurn &turn)
{
    return turn.revision != 0 && turn.endTimeNs >= turn.startTimeNs
        && turn.speakerId != 0 && !turn.speakerLabel.empty()
        && std::isfinite(turn.confidence)
        && turn.confidence >= 0.0F && turn.confidence <= 1.0F;
}

bool validRemoteSpeakerId(std::uint64_t speakerId)
{
    const auto namespaceBits =
        speakerId & (kAnonymousSpeakerFlag | kPersistentSpeakerFlag);
    const bool anonymous = namespaceBits == kAnonymousSpeakerFlag
        && (speakerId & kSpeakerIdPayloadMask) != 0;
    const bool persistent = isPersistentSpeakerId(speakerId)
        && profileIdFromSpeakerId(speakerId) != 0;
    return anonymous || persistent;
}

Expected<std::vector<TranscriptSegment>> resolvePendingPayload(
    sqlite3 *database,
    const std::string &sessionId,
    std::uint64_t groupId,
    std::span<const SpeakerTurn> attributions)
{
    auto pending = loadPendingSegments(database, sessionId, groupId);
    if (!pending) {
        return pending.error();
    }
    if (pending.value().empty()) {
        return Error{
            LS_RECOVERY_ERROR,
            "pending speaker group has no segments"};
    }
    if (!attributions.empty()
        && attributions.size() != pending.value().size()) {
        return Error{
            LS_CONFLICT,
            "speaker resolution does not cover the entire pending group"};
    }
    if (!attributions.empty()) {
        const auto targetSpeakerId = attributions.front().speakerId;
        const auto &targetSpeakerLabel =
            attributions.front().speakerLabel;
        if (!validRemoteSpeakerId(targetSpeakerId)
            || std::any_of(
                attributions.begin() + 1,
                attributions.end(),
                [&](const SpeakerTurn &turn) {
                    return turn.speakerId != targetSpeakerId
                        || turn.speakerLabel != targetSpeakerLabel;
                })) {
            return Error{
                LS_CONFLICT,
                "speaker resolution must name one remote speaker"};
        }
    }

    std::vector<TranscriptSegment> result;
    result.reserve(pending.value().size());
    for (const auto &fallback : pending.value()) {
        TranscriptSegment resolved = fallback;
        if (!attributions.empty()) {
            const auto match = std::find_if(
                attributions.begin(),
                attributions.end(),
                [&](const SpeakerTurn &turn) {
                    return turn.stableId == fallback.stableId
                        && turn.revision == fallback.revision;
                });
            if (match == attributions.end()
                || !validSpeakerTurn(*match)
                || match->sourceId != fallback.sourceId
                || match->startTimeNs != fallback.startTimeNs
                || match->endTimeNs != fallback.endTimeNs) {
                return Error{
                    LS_CONFLICT,
                    "speaker resolution does not match held payload"};
            }
            resolved.speakerId = match->speakerId;
            resolved.speakerLabel = match->speakerLabel;
        }
        auto effective = applySpeakerEnrollment(
            database,
            sessionId,
            resolved);
        if (!effective) {
            return effective.error();
        }
        result.push_back(effective.takeValue());
    }

    if (!attributions.empty()) {
        for (std::size_t index = 0; index < attributions.size(); ++index) {
            const auto duplicate = std::find_if(
                attributions.begin() + static_cast<std::ptrdiff_t>(index + 1),
                attributions.end(),
                [&](const SpeakerTurn &candidate) {
                    return candidate.stableId == attributions[index].stableId
                        && candidate.revision == attributions[index].revision;
                });
            if (duplicate != attributions.end()) {
                return Error{
                    LS_CONFLICT,
                    "speaker resolution repeats a held segment"};
            }
        }
    }
    return result;
}

Expected<void> insertVisibleSegment(
    sqlite3 *database,
    const std::string &sessionId,
    const TranscriptSegment &segment,
    std::uint64_t checkpoint)
{
    Statement insert;
    auto prepared = prepare(
        database,
        R"SQL(
INSERT INTO segments(
    session_id, stable_id, revision, source_id, start_time_ns, end_time_ns,
    speaker_id, speaker_label, text, language, confidence, flags,
    journal_checkpoint, speaker_embedding_model, speaker_embedding_dimension,
    speaker_embedding
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
        insert);
    if (!prepared) {
        return prepared.error();
    }
    bindText(insert.get(), 1, sessionId);
    sqlite3_bind_blob(
        insert.get(),
        2,
        segment.stableId.data(),
        static_cast<int>(segment.stableId.size()),
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 3, segment.revision);
    sqlite3_bind_int64(
        insert.get(),
        4,
        static_cast<sqlite3_int64>(segment.sourceId));
    sqlite3_bind_int64(insert.get(), 5, segment.startTimeNs);
    sqlite3_bind_int64(insert.get(), 6, segment.endTimeNs);
    sqlite3_bind_int64(
        insert.get(),
        7,
        static_cast<sqlite3_int64>(segment.speakerId));
    bindText(insert.get(), 8, segment.speakerLabel);
    bindText(insert.get(), 9, segment.text);
    bindText(insert.get(), 10, segment.language);
    sqlite3_bind_double(insert.get(), 11, segment.confidence);
    sqlite3_bind_int64(insert.get(), 12, segment.flags);
    sqlite3_bind_int64(
        insert.get(),
        13,
        static_cast<sqlite3_int64>(checkpoint));
    bindText(insert.get(), 14, segment.speakerEmbeddingModel);
    sqlite3_bind_int64(
        insert.get(),
        15,
        static_cast<sqlite3_int64>(segment.speakerEmbedding.size()));
    bindEmbedding(insert.get(), 16, segment.speakerEmbedding);
    return stepDone(database, insert.get());
}

bool validDigest(const std::string &digest)
{
    return digest.size() == 64
        && std::all_of(
            digest.begin(),
            digest.end(),
            [](unsigned char value) { return std::isxdigit(value) != 0; });
}

bool publicationMustMatchCurrentCheckpoint(ls_phase_t phase)
{
    return phase == LS_PHASE_RECOVERY_REQUIRED
        || phase == LS_PHASE_COMPLETE
        || phase == LS_PHASE_INCOMPLETE_SOURCES
        || phase == LS_PHASE_INTERRUPTED
        || phase == LS_PHASE_FAILED_TO_START;
}

bool isTerminalPhase(ls_phase_t phase)
{
    return phase == LS_PHASE_COMPLETE
        || phase == LS_PHASE_INCOMPLETE_SOURCES
        || phase == LS_PHASE_INTERRUPTED
        || phase == LS_PHASE_FAILED_TO_START;
}

} // namespace

RecoveryJournal::RecoveryJournal(sqlite3 *database, std::string path)
    : database_(database), path_(std::move(path))
{
}

RecoveryJournal::~RecoveryJournal()
{
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
    }
}

Expected<std::shared_ptr<RecoveryJournal>>
RecoveryJournal::open(const std::string &path)
{
    if (path.empty() || path.find('\0') != std::string::npos) {
        return Error{LS_INVALID_ARGUMENT, "journal path is empty or invalid"};
    }

    sqlite3 *database = nullptr;
    const int result = sqlite3_open_v2(
        path.c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK) {
        const std::string detail =
            database == nullptr ? "unknown SQLite open error"
                                : sqlite3_errmsg(database);
        if (database != nullptr) {
            sqlite3_close_v2(database);
        }
        return Error{LS_SQLITE_ERROR, "cannot open journal: " + detail};
    }

    auto migration = applyMigrations(database);
    if (!migration) {
        sqlite3_close_v2(database);
        return migration.error();
    }
    return std::shared_ptr<RecoveryJournal>(
        new RecoveryJournal(database, path));
}

Expected<void> RecoveryJournal::createSession(
    const SessionRecord &session,
    std::span<const SourceRecord> sources)
{
    std::lock_guard lock(mutex_);
    if (!validSessionId(session.sessionId)) {
        return Error{LS_INVALID_ARGUMENT, "session ID is invalid"};
    }
    if (session.phase != LS_PHASE_PREPARING) {
        return Error{
            LS_INVALID_STATE,
            "new journal session must start in preparing"};
    }
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin session transaction");
    }

    Statement statement;
    auto prepared = prepare(
        database_,
        R"SQL(
INSERT INTO sessions(
    session_id, phase, created_at, ended_at, source_app,
    local_speaker_name, asr_backend_id, asr_backend_version,
    diarization_backend_id, diarization_backend_version, language_mode,
    microphone_source_id, system_audio_source_id, required_source_mask,
    completeness_threshold_ns, journal_checkpoint
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
)SQL",
        statement);
    if (!prepared) {
        return prepared;
    }
    bindText(statement.get(), 1, session.sessionId);
    sqlite3_bind_int(statement.get(), 2, session.phase);
    bindText(statement.get(), 3, session.createdAt);
    bindText(statement.get(), 4, session.endedAt);
    bindText(statement.get(), 5, session.sourceApp);
    bindText(statement.get(), 6, session.localSpeakerName);
    bindText(statement.get(), 7, session.asrBackendId);
    bindText(statement.get(), 8, session.asrBackendVersion);
    bindText(statement.get(), 9, session.diarizationBackendId);
    bindText(statement.get(), 10, session.diarizationBackendVersion);
    sqlite3_bind_int(statement.get(), 11, session.languageMode);
    sqlite3_bind_int64(
        statement.get(),
        12,
        static_cast<sqlite3_int64>(session.microphoneSourceId));
    sqlite3_bind_int64(
        statement.get(),
        13,
        static_cast<sqlite3_int64>(session.systemAudioSourceId));
    sqlite3_bind_int64(statement.get(), 14, session.requiredSourceMask);
    sqlite3_bind_int64(
        statement.get(),
        15,
        session.completenessThresholdNs);
    if (auto stepped = stepDone(database_, statement.get()); !stepped) {
        return stepped;
    }

    Statement event;
    prepared = prepare(
        database_,
        "INSERT INTO state_events(session_id, event_sequence, phase, reason) "
        "VALUES (?, 1, ?, 0)",
        event);
    if (!prepared) {
        return prepared;
    }
    bindText(event.get(), 1, session.sessionId);
    sqlite3_bind_int(event.get(), 2, session.phase);
    if (auto stepped = stepDone(database_, event.get()); !stepped) {
        return stepped;
    }

    for (const auto &source : sources) {
        Statement sourceStatement;
        prepared = prepare(
            database_,
            R"SQL(
INSERT INTO sources(
    session_id, source_id, source_kind, required, health
) VALUES (?, ?, ?, ?, ?)
)SQL",
            sourceStatement);
        if (!prepared) {
            return prepared;
        }
        bindText(sourceStatement.get(), 1, session.sessionId);
        sqlite3_bind_int64(
            sourceStatement.get(),
            2,
            static_cast<sqlite3_int64>(source.sourceId));
        sqlite3_bind_int(sourceStatement.get(), 3, source.sourceKind);
        sqlite3_bind_int(sourceStatement.get(), 4, source.required ? 1 : 0);
        sqlite3_bind_int(sourceStatement.get(), 5, source.health);
        if (auto stepped = stepDone(database_, sourceStatement.get());
            !stepped) {
            return stepped;
        }
    }
    return transaction.commit();
}

Expected<SessionRecord>
RecoveryJournal::loadSessionLocked(const std::string &sessionId)
{
    Statement statement;
    auto prepared = prepare(
        database_,
        R"SQL(
SELECT
    session_id, phase, created_at, ended_at, source_app,
    local_speaker_name, asr_backend_id, asr_backend_version,
    diarization_backend_id, diarization_backend_version, language_mode,
    microphone_source_id, system_audio_source_id, required_source_mask,
    completeness_threshold_ns, timeline_origin_ns, journal_checkpoint,
    highest_segment_revision, finalize_reason
FROM sessions
WHERE session_id = ?
)SQL",
        statement);
    if (!prepared) {
        return prepared.error();
    }
    bindText(statement.get(), 1, sessionId);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Error{LS_NOT_FOUND, "journal session was not found"};
    }
    if (step != SQLITE_ROW) {
        return sqliteError(database_, "cannot load journal session");
    }

    SessionRecord record;
    record.sessionId = columnText(statement.get(), 0);
    record.phase = sqlite3_column_int(statement.get(), 1);
    record.createdAt = columnText(statement.get(), 2);
    record.endedAt = columnText(statement.get(), 3);
    record.sourceApp = columnText(statement.get(), 4);
    record.localSpeakerName = columnText(statement.get(), 5);
    record.asrBackendId = columnText(statement.get(), 6);
    record.asrBackendVersion = columnText(statement.get(), 7);
    record.diarizationBackendId = columnText(statement.get(), 8);
    record.diarizationBackendVersion = columnText(statement.get(), 9);
    record.languageMode = sqlite3_column_int(statement.get(), 10);
    record.microphoneSourceId =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 11));
    record.systemAudioSourceId =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 12));
    record.requiredSourceMask =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 13));
    record.completenessThresholdNs = sqlite3_column_int64(statement.get(), 14);
    record.timelineOriginNs = sqlite3_column_int64(statement.get(), 15);
    record.journalCheckpoint =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 16));
    record.highestSegmentRevision =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 17));
    record.finalizeReason = sqlite3_column_int(statement.get(), 18);
    return record;
}

Expected<SessionRecord>
RecoveryJournal::loadSession(const std::string &sessionId)
{
    std::lock_guard lock(mutex_);
    return loadSessionLocked(sessionId);
}

Expected<std::uint64_t> RecoveryJournal::transition(
    const std::string &sessionId,
    ls_phase_t expected,
    ls_phase_t next,
    ls_finalize_reason_t reason)
{
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin phase transaction");
    }

    auto current = loadSessionLocked(sessionId);
    if (!current) {
        return current.error();
    }
    if (current.value().phase != expected) {
        return Error{
            LS_INVALID_STATE,
            "journal phase changed before transition"};
    }
    if (isTerminalPhase(next)) {
        Statement pending;
        auto prepared = prepare(
            database_,
            "SELECT 1 FROM pending_speaker_groups "
            "WHERE session_id = ? AND resolved_checkpoint IS NULL LIMIT 1",
            pending);
        if (!prepared) {
            return prepared.error();
        }
        bindText(pending.get(), 1, sessionId);
        const int pendingStep = sqlite3_step(pending.get());
        if (pendingStep == SQLITE_ROW) {
            return Error{
                LS_INVALID_STATE,
                "pending speaker groups must resolve before terminal phase"};
        }
        if (pendingStep != SQLITE_DONE) {
            return sqliteError(
                database_,
                "cannot inspect pending speaker groups before transition");
        }
    }

    Statement sequence;
    auto prepared = prepare(
        database_,
        "SELECT COALESCE(MAX(event_sequence), 0) + 1 "
        "FROM state_events WHERE session_id = ?",
        sequence);
    if (!prepared) {
        return prepared.error();
    }
    bindText(sequence.get(), 1, sessionId);
    if (sqlite3_step(sequence.get()) != SQLITE_ROW) {
        return sqliteError(database_, "cannot allocate state event sequence");
    }
    const sqlite3_int64 nextSequence = sqlite3_column_int64(sequence.get(), 0);
    const std::uint64_t checkpoint =
        current.value().journalCheckpoint + 1u;

    Statement update;
    prepared = prepare(
        database_,
        "UPDATE sessions SET phase = ?, finalize_reason = ?, "
        "journal_checkpoint = ? WHERE session_id = ? AND phase = ?",
        update);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(update.get(), 1, next);
    sqlite3_bind_int(update.get(), 2, reason);
    sqlite3_bind_int64(
        update.get(),
        3,
        static_cast<sqlite3_int64>(checkpoint));
    bindText(update.get(), 4, sessionId);
    sqlite3_bind_int(update.get(), 5, expected);
    if (auto stepped = stepDone(database_, update.get()); !stepped) {
        return stepped.error();
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{LS_CONFLICT, "session phase update lost a race"};
    }

    Statement event;
    prepared = prepare(
        database_,
        "INSERT INTO state_events(session_id, event_sequence, phase, reason) "
        "VALUES (?, ?, ?, ?)",
        event);
    if (!prepared) {
        return prepared.error();
    }
    bindText(event.get(), 1, sessionId);
    sqlite3_bind_int64(event.get(), 2, nextSequence);
    sqlite3_bind_int(event.get(), 3, next);
    sqlite3_bind_int(event.get(), 4, reason);
    if (auto stepped = stepDone(database_, event.get()); !stepped) {
        return stepped.error();
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }
    return checkpoint;
}

Expected<std::uint64_t> RecoveryJournal::appendFinalSegment(
    const std::string &sessionId,
    const TranscriptSegment &segment)
{
    auto copy = segment;
    return appendFinalSegment(sessionId, copy);
}

Expected<std::uint64_t> RecoveryJournal::appendFinalSegment(
    const std::string &sessionId,
    TranscriptSegment &segment)
{
    if (!validFinalSegment(segment)) {
        return Error{LS_INVALID_ARGUMENT, "final segment is invalid"};
    }

    TranscriptSegment effectiveSegment = segment;

    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin segment transaction");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }

    Statement enrollment;
    auto prepared = prepare(
        database_,
        R"SQL(
SELECT e.profile_id, e.display_name
FROM session_voice_profile_enrollments AS e
WHERE e.session_id = ? AND e.original_speaker_id = ?
)SQL",
        enrollment);
    if (!prepared) {
        return prepared.error();
    }
    bindText(enrollment.get(), 1, sessionId);
    sqlite3_bind_int64(
        enrollment.get(),
        2,
        static_cast<sqlite3_int64>(segment.speakerId));
    const int enrollmentStep = sqlite3_step(enrollment.get());
    if (enrollmentStep == SQLITE_ROW) {
        const auto profileId = static_cast<std::uint64_t>(
            sqlite3_column_int64(enrollment.get(), 0));
        const auto displayName = columnText(enrollment.get(), 1);
        if (profileId == 0 || profileId > kSpeakerIdPayloadMask
            || !validProfileName(displayName)) {
            return Error{
                LS_RECOVERY_ERROR,
                "journal has an invalid speaker enrollment"};
        }
        effectiveSegment.speakerId = persistentSpeakerId(profileId);
        effectiveSegment.speakerLabel = displayName;
    } else if (enrollmentStep != SQLITE_DONE) {
        return sqliteError(database_, "cannot inspect speaker enrollment");
    }

    Statement existing;
    prepared = prepare(
        database_,
        R"SQL(
SELECT stable_id, source_id, start_time_ns, end_time_ns, speaker_id,
       speaker_label, text, language, confidence, flags,
       speaker_embedding_model, speaker_embedding_dimension,
       speaker_embedding,
       revision
FROM segments
WHERE session_id = ? AND stable_id = ?
ORDER BY revision DESC
LIMIT 1
)SQL",
        existing);
    if (!prepared) {
        return prepared.error();
    }
    bindText(existing.get(), 1, sessionId);
    sqlite3_bind_blob(
        existing.get(),
        2,
        effectiveSegment.stableId.data(),
        static_cast<int>(effectiveSegment.stableId.size()),
        SQLITE_TRANSIENT);
    const int existingStep = sqlite3_step(existing.get());
    if (existingStep != SQLITE_ROW && existingStep != SQLITE_DONE) {
        return sqliteError(database_, "cannot inspect segment revision");
    }
    if (existingStep == SQLITE_ROW) {
        const auto currentRevision =
            static_cast<std::uint32_t>(sqlite3_column_int64(existing.get(), 13));
        if (effectiveSegment.revision < currentRevision) {
            return Error{
                LS_CONFLICT,
                "segment revision would move backwards"};
        }
        if (effectiveSegment.revision == currentRevision) {
            if (!sameSegment(existing.get(), segment)
                && !sameSegment(existing.get(), effectiveSegment)) {
                return Error{
                    LS_CONFLICT,
                    "same segment revision has different content"};
            }
            effectiveSegment.journalCheckpoint =
                session.value().journalCheckpoint;
            segment = std::move(effectiveSegment);
            return session.value().journalCheckpoint;
        }
    }

    const std::uint64_t checkpoint =
        session.value().journalCheckpoint + 1u;
    Statement insert;
    prepared = prepare(
        database_,
        R"SQL(
INSERT INTO segments(
    session_id, stable_id, revision, source_id, start_time_ns, end_time_ns,
    speaker_id, speaker_label, text, language, confidence, flags,
    journal_checkpoint, speaker_embedding_model, speaker_embedding_dimension,
    speaker_embedding
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
        insert);
    if (!prepared) {
        return prepared.error();
    }
    bindText(insert.get(), 1, sessionId);
    sqlite3_bind_blob(
        insert.get(),
        2,
        effectiveSegment.stableId.data(),
        static_cast<int>(effectiveSegment.stableId.size()),
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 3, effectiveSegment.revision);
    sqlite3_bind_int64(
        insert.get(),
        4,
        static_cast<sqlite3_int64>(effectiveSegment.sourceId));
    sqlite3_bind_int64(insert.get(), 5, effectiveSegment.startTimeNs);
    sqlite3_bind_int64(insert.get(), 6, effectiveSegment.endTimeNs);
    sqlite3_bind_int64(
        insert.get(),
        7,
        static_cast<sqlite3_int64>(effectiveSegment.speakerId));
    bindText(insert.get(), 8, effectiveSegment.speakerLabel);
    bindText(insert.get(), 9, effectiveSegment.text);
    bindText(insert.get(), 10, effectiveSegment.language);
    sqlite3_bind_double(insert.get(), 11, effectiveSegment.confidence);
    sqlite3_bind_int64(insert.get(), 12, effectiveSegment.flags);
    sqlite3_bind_int64(
        insert.get(),
        13,
        static_cast<sqlite3_int64>(checkpoint));
    bindText(insert.get(), 14, effectiveSegment.speakerEmbeddingModel);
    sqlite3_bind_int64(
        insert.get(),
        15,
        static_cast<sqlite3_int64>(effectiveSegment.speakerEmbedding.size()));
    bindEmbedding(insert.get(), 16, effectiveSegment.speakerEmbedding);
    if (auto stepped = stepDone(database_, insert.get()); !stepped) {
        return stepped.error();
    }

    Statement update;
    prepared = prepare(
        database_,
        "UPDATE sessions SET journal_checkpoint = ?, "
        "highest_segment_revision = MAX(highest_segment_revision, ?), "
        "timeline_origin_ns = CASE "
        "WHEN timeline_origin_ns = 0 OR timeline_origin_ns > ? THEN ? "
        "ELSE timeline_origin_ns END "
        "WHERE session_id = ?",
        update);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        update.get(),
        1,
        static_cast<sqlite3_int64>(checkpoint));
    sqlite3_bind_int64(update.get(), 2, effectiveSegment.revision);
    sqlite3_bind_int64(update.get(), 3, effectiveSegment.startTimeNs);
    sqlite3_bind_int64(update.get(), 4, effectiveSegment.startTimeNs);
    bindText(update.get(), 5, sessionId);
    if (auto stepped = stepDone(database_, update.get()); !stepped) {
        return stepped.error();
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }
    effectiveSegment.journalCheckpoint = checkpoint;
    segment = std::move(effectiveSegment);
    return checkpoint;
}

Expected<DiarizationJournalBatchResult>
RecoveryJournal::applyDiarizationBatch(
    const std::string &sessionId,
    const DiarizationJournalBatch &batch)
{
    return applyDiarizationBatchImpl(sessionId, batch, false);
}

Expected<DiarizationJournalBatchResult>
RecoveryJournal::applyDiarizationBatchImpl(
    const std::string &sessionId,
    const DiarizationJournalBatch &batch,
    bool resolveAllPendingToFallback)
{
    std::vector<PendingSpeakerGroupStage> holds;
    for (const auto &hold : batch.holds) {
        if (hold.groupId == 0
            || hold.groupId
                > static_cast<std::uint64_t>(
                    std::numeric_limits<sqlite3_int64>::max())
            || hold.deadlineMonotonicNs < 0
            || hold.fallbackSegments.empty()
            || std::any_of(
                hold.fallbackSegments.begin(),
                hold.fallbackSegments.end(),
                [](const TranscriptSegment &segment) {
                    return !validFinalSegment(segment);
                })) {
            return Error{
                LS_INVALID_ARGUMENT,
                "pending speaker group is invalid"};
        }
        auto existingGroup = std::find_if(
            holds.begin(),
            holds.end(),
            [&](const PendingSpeakerGroupStage &candidate) {
                return candidate.groupId == hold.groupId;
            });
        if (existingGroup == holds.end()) {
            holds.push_back(hold);
            continue;
        }
        if (existingGroup->deadlineMonotonicNs
            != hold.deadlineMonotonicNs) {
            return Error{
                LS_CONFLICT,
                "pending speaker group deadline changed"};
        }
        for (const auto &segment : hold.fallbackSegments) {
            const auto duplicate = std::find_if(
                existingGroup->fallbackSegments.begin(),
                existingGroup->fallbackSegments.end(),
                [&](const TranscriptSegment &candidate) {
                    return candidate.stableId == segment.stableId
                        && candidate.revision == segment.revision;
                });
            if (duplicate == existingGroup->fallbackSegments.end()) {
                existingGroup->fallbackSegments.push_back(segment);
            } else if (!sameSegmentPayload(*duplicate, segment)) {
                return Error{
                    LS_CONFLICT,
                    "same pending segment revision has different content"};
            }
        }
    }
    std::vector<PendingSpeakerGroupResolution> resolutions;
    for (const auto &resolution : batch.resolutions) {
        if (resolution.groupId == 0
            || resolution.groupId
                > static_cast<std::uint64_t>(
                    std::numeric_limits<sqlite3_int64>::max())) {
            return Error{
                LS_INVALID_ARGUMENT,
                "pending speaker resolution is invalid"};
        }
        if (std::any_of(
                resolution.attributions.begin(),
                resolution.attributions.end(),
                [](const SpeakerTurn &turn) {
                    return !validSpeakerTurn(turn);
                })) {
            return Error{
                LS_INVALID_ARGUMENT,
                "pending speaker attribution is invalid"};
        }
        if (std::any_of(
                resolutions.begin(),
                resolutions.end(),
                [&](const PendingSpeakerGroupResolution &candidate) {
                    return candidate.groupId == resolution.groupId;
                })) {
            return Error{
                LS_CONFLICT,
                "pending speaker group is resolved twice in one batch"};
        }
        resolutions.push_back(resolution);
    }
    std::vector<TranscriptSegment> commits;
    for (const auto &commit : batch.commits) {
        if (!validFinalSegment(commit)) {
            return Error{
                LS_INVALID_ARGUMENT,
                "committed diarization segment is invalid"};
        }
        const auto duplicate = std::find_if(
            commits.begin(),
            commits.end(),
            [&](const TranscriptSegment &candidate) {
                return candidate.stableId == commit.stableId
                    && candidate.revision == commit.revision;
            });
        if (duplicate == commits.end()) {
            commits.push_back(commit);
        } else if (!sameSegmentPayload(*duplicate, commit)) {
            return Error{
                LS_CONFLICT,
                "same committed segment revision has different content"};
        }
    }

    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin diarization transaction");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }

    DiarizationJournalBatchResult result;
    result.journalCheckpoint = session.value().journalCheckpoint;
    result.highestSegmentRevision =
        session.value().highestSegmentRevision;

    if (resolveAllPendingToFallback) {
        Statement unresolved;
        auto prepared = prepare(
            database_,
            "SELECT group_id FROM pending_speaker_groups "
            "WHERE session_id = ? AND resolved_checkpoint IS NULL "
            "ORDER BY deadline_monotonic_ns, group_id",
            unresolved);
        if (!prepared) {
            return prepared.error();
        }
        bindText(unresolved.get(), 1, sessionId);
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(unresolved.get())) == SQLITE_ROW) {
            const auto groupId = static_cast<std::uint64_t>(
                sqlite3_column_int64(unresolved.get(), 0));
            if (groupId == 0
                || groupId
                    > static_cast<std::uint64_t>(
                        std::numeric_limits<sqlite3_int64>::max())) {
                return Error{
                    LS_RECOVERY_ERROR,
                    "journal has an invalid pending speaker group"};
            }
            const auto duplicate = std::find_if(
                resolutions.begin(),
                resolutions.end(),
                [&](const PendingSpeakerGroupResolution &resolution) {
                    return resolution.groupId == groupId;
                });
            if (duplicate == resolutions.end()) {
                resolutions.push_back(PendingSpeakerGroupResolution{
                    groupId,
                    {}});
            }
        }
        if (step != SQLITE_DONE) {
            return sqliteError(
                database_,
                "cannot discover pending speaker groups");
        }
    }

    struct HoldWork {
        const PendingSpeakerGroupStage *hold{};
        bool insertGroup{};
        std::vector<const TranscriptSegment *> insertSegments;
    };
    std::vector<HoldWork> work;
    bool changed = false;
    for (const auto &hold : holds) {
        HoldWork item;
        item.hold = &hold;
        Statement group;
        auto prepared = prepare(
            database_,
            R"SQL(
SELECT deadline_monotonic_ns, resolved_checkpoint
FROM pending_speaker_groups
WHERE session_id = ? AND group_id = ?
)SQL",
            group);
        if (!prepared) {
            return prepared.error();
        }
        bindText(group.get(), 1, sessionId);
        sqlite3_bind_int64(
            group.get(),
            2,
            static_cast<sqlite3_int64>(hold.groupId));
        const int groupStep = sqlite3_step(group.get());
        if (groupStep != SQLITE_ROW && groupStep != SQLITE_DONE) {
            return sqliteError(database_, "cannot inspect pending group");
        }
        const bool groupExists = groupStep == SQLITE_ROW;
        const bool groupResolved = groupExists
            && sqlite3_column_type(group.get(), 1) != SQLITE_NULL;
        if (groupExists
            && sqlite3_column_int64(group.get(), 0)
                != hold.deadlineMonotonicNs) {
            return Error{
                LS_CONFLICT,
                "pending speaker group deadline changed"};
        }
        item.insertGroup = !groupExists;
        changed = changed || item.insertGroup;

        for (const auto &segment : hold.fallbackSegments) {
            Statement existing;
            prepared = prepare(
                database_,
                R"SQL(
SELECT stable_id, source_id, start_time_ns, end_time_ns, speaker_id,
       speaker_label, text, language, confidence, flags,
       speaker_embedding_model, speaker_embedding_dimension,
       speaker_embedding
FROM pending_speaker_segments
WHERE session_id = ? AND group_id = ? AND stable_id = ? AND revision = ?
)SQL",
                existing);
            if (!prepared) {
                return prepared.error();
            }
            bindText(existing.get(), 1, sessionId);
            sqlite3_bind_int64(
                existing.get(),
                2,
                static_cast<sqlite3_int64>(hold.groupId));
            sqlite3_bind_blob(
                existing.get(),
                3,
                segment.stableId.data(),
                static_cast<int>(segment.stableId.size()),
                SQLITE_TRANSIENT);
            sqlite3_bind_int64(existing.get(), 4, segment.revision);
            const int existingStep = sqlite3_step(existing.get());
            if (existingStep != SQLITE_ROW && existingStep != SQLITE_DONE) {
                return sqliteError(
                    database_,
                    "cannot inspect pending segment");
            }
            if (existingStep == SQLITE_ROW) {
                if (!sameSegment(existing.get(), segment)) {
                    return Error{
                        LS_CONFLICT,
                        "same pending segment revision has different content"};
                }
                continue;
            }
            if (groupResolved) {
                return Error{
                    LS_CONFLICT,
                    "resolved pending speaker group cannot be extended"};
            }
            item.insertSegments.push_back(&segment);
            changed = true;
        }
        work.push_back(std::move(item));
    }

    struct ResolutionWork {
        const PendingSpeakerGroupResolution *resolution{};
        bool unresolved{};
    };
    std::vector<ResolutionWork> resolutionWork;
    for (const auto &resolution : resolutions) {
        Statement group;
        auto prepared = prepare(
            database_,
            "SELECT resolved_checkpoint FROM pending_speaker_groups "
            "WHERE session_id = ? AND group_id = ?",
            group);
        if (!prepared) {
            return prepared.error();
        }
        bindText(group.get(), 1, sessionId);
        sqlite3_bind_int64(
            group.get(),
            2,
            static_cast<sqlite3_int64>(resolution.groupId));
        const int groupStep = sqlite3_step(group.get());
        if (groupStep != SQLITE_ROW && groupStep != SQLITE_DONE) {
            return sqliteError(
                database_,
                "cannot inspect pending group resolution");
        }
        const bool exists = groupStep == SQLITE_ROW;
        const bool anticipated = std::any_of(
            holds.begin(),
            holds.end(),
            [&](const PendingSpeakerGroupStage &hold) {
                return hold.groupId == resolution.groupId;
            });
        if (!exists && !anticipated) {
            return Error{
                LS_NOT_FOUND,
                "pending speaker group was not found"};
        }
        const bool unresolved = !exists
            || sqlite3_column_type(group.get(), 0) == SQLITE_NULL;
        if (!unresolved) {
            auto expected = resolvePendingPayload(
                database_,
                sessionId,
                resolution.groupId,
                resolution.attributions);
            if (!expected) {
                return expected.error();
            }
            for (const auto &segment : expected.value()) {
                auto visible = loadVisibleSegmentRevision(
                    database_,
                    sessionId,
                    segment.stableId,
                    segment.revision);
                if (!visible) {
                    return visible.error();
                }
                if (!visible.value().has_value()) {
                    return Error{
                        LS_RECOVERY_ERROR,
                        "resolved pending group is not fully visible"};
                }
                auto effectiveVisible = applySpeakerEnrollment(
                    database_,
                    sessionId,
                    *visible.value());
                if (!effectiveVisible) {
                    return effectiveVisible.error();
                }
                if (!sameSegmentPayload(
                        effectiveVisible.value(),
                        segment)) {
                    return Error{
                        LS_CONFLICT,
                        "speaker group resolution differs from prior result"};
                }
            }
        } else {
            changed = true;
        }
        resolutionWork.push_back(ResolutionWork{&resolution, unresolved});
    }

    std::vector<TranscriptSegment> commitWork;
    for (const auto &commit : commits) {
        auto effective = applySpeakerEnrollment(database_, sessionId, commit);
        if (!effective) {
            return effective.error();
        }
        auto latest = loadLatestVisibleSegment(
            database_,
            sessionId,
            effective.value().stableId);
        if (!latest) {
            return latest.error();
        }
        if (latest.value().has_value()) {
            if (latest.value()->revision > effective.value().revision) {
                return Error{
                    LS_CONFLICT,
                    "segment revision would move backwards"};
            }
            if (latest.value()->revision == effective.value().revision) {
                auto effectiveLatest = applySpeakerEnrollment(
                    database_,
                    sessionId,
                    *latest.value());
                if (!effectiveLatest) {
                    return effectiveLatest.error();
                }
                if (!sameSegmentPayload(commit, *latest.value())
                    && !sameSegmentPayload(
                        effective.value(),
                        effectiveLatest.value())) {
                    return Error{
                        LS_CONFLICT,
                        "same segment revision has different content"};
                }
                continue;
            }
        }
        commitWork.push_back(effective.takeValue());
        changed = true;
    }

    if (!changed) {
        if (auto committed = transaction.commit(); !committed) {
            return committed.error();
        }
        return result;
    }
    if (session.value().journalCheckpoint
        == std::numeric_limits<std::uint64_t>::max()) {
        return Error{LS_CONFLICT, "journal checkpoint space is exhausted"};
    }
    const std::uint64_t checkpoint =
        session.value().journalCheckpoint + 1u;

    for (const auto &item : work) {
        const auto &hold = *item.hold;
        if (item.insertGroup) {
            Statement group;
            auto prepared = prepare(
                database_,
                R"SQL(
INSERT INTO pending_speaker_groups(
    session_id, group_id, deadline_monotonic_ns, created_checkpoint
) VALUES (?, ?, ?, ?)
)SQL",
                group);
            if (!prepared) {
                return prepared.error();
            }
            bindText(group.get(), 1, sessionId);
            sqlite3_bind_int64(
                group.get(),
                2,
                static_cast<sqlite3_int64>(hold.groupId));
            sqlite3_bind_int64(group.get(), 3, hold.deadlineMonotonicNs);
            sqlite3_bind_int64(
                group.get(),
                4,
                static_cast<sqlite3_int64>(checkpoint));
            if (auto inserted = stepDone(database_, group.get()); !inserted) {
                return inserted.error();
            }
        }

        for (const auto *segment : item.insertSegments) {
            Statement insert;
            auto prepared = prepare(
                database_,
                R"SQL(
INSERT INTO pending_speaker_segments(
    session_id, group_id, stable_id, revision, source_id, start_time_ns,
    end_time_ns, speaker_id, speaker_label, text, language, confidence,
    flags, staged_checkpoint, speaker_embedding_model,
    speaker_embedding_dimension, speaker_embedding
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
                insert);
            if (!prepared) {
                return prepared.error();
            }
            bindText(insert.get(), 1, sessionId);
            sqlite3_bind_int64(
                insert.get(),
                2,
                static_cast<sqlite3_int64>(hold.groupId));
            sqlite3_bind_blob(
                insert.get(),
                3,
                segment->stableId.data(),
                static_cast<int>(segment->stableId.size()),
                SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert.get(), 4, segment->revision);
            sqlite3_bind_int64(
                insert.get(),
                5,
                static_cast<sqlite3_int64>(segment->sourceId));
            sqlite3_bind_int64(insert.get(), 6, segment->startTimeNs);
            sqlite3_bind_int64(insert.get(), 7, segment->endTimeNs);
            sqlite3_bind_int64(
                insert.get(),
                8,
                static_cast<sqlite3_int64>(segment->speakerId));
            bindText(insert.get(), 9, segment->speakerLabel);
            bindText(insert.get(), 10, segment->text);
            bindText(insert.get(), 11, segment->language);
            sqlite3_bind_double(insert.get(), 12, segment->confidence);
            sqlite3_bind_int64(insert.get(), 13, segment->flags);
            sqlite3_bind_int64(
                insert.get(),
                14,
                static_cast<sqlite3_int64>(checkpoint));
            bindText(insert.get(), 15, segment->speakerEmbeddingModel);
            sqlite3_bind_int64(
                insert.get(),
                16,
                static_cast<sqlite3_int64>(
                    segment->speakerEmbedding.size()));
            bindEmbedding(insert.get(), 17, segment->speakerEmbedding);
            if (auto inserted = stepDone(database_, insert.get()); !inserted) {
                return inserted.error();
            }
        }
    }

    std::vector<TranscriptSegment> visibleCandidates;
    std::vector<std::uint64_t> groupsToResolve;
    for (const auto &item : resolutionWork) {
        if (!item.unresolved) {
            continue;
        }
        auto resolved = resolvePendingPayload(
            database_,
            sessionId,
            item.resolution->groupId,
            item.resolution->attributions);
        if (!resolved) {
            return resolved.error();
        }
        for (auto &segment : resolved.value()) {
            segment.journalCheckpoint = checkpoint;
            visibleCandidates.push_back(std::move(segment));
        }
        groupsToResolve.push_back(item.resolution->groupId);
    }
    for (auto &commit : commitWork) {
        commit.journalCheckpoint = checkpoint;
        visibleCandidates.push_back(std::move(commit));
    }

    std::vector<TranscriptSegment> normalizedVisible;
    for (auto &candidate : visibleCandidates) {
        const auto duplicate = std::find_if(
            normalizedVisible.begin(),
            normalizedVisible.end(),
            [&](const TranscriptSegment &existing) {
                return existing.stableId == candidate.stableId
                    && existing.revision == candidate.revision;
            });
        if (duplicate == normalizedVisible.end()) {
            normalizedVisible.push_back(std::move(candidate));
        } else if (!sameSegmentPayload(*duplicate, candidate)) {
            return Error{
                LS_CONFLICT,
                "diarization batch publishes conflicting segment content"};
        }
    }
    std::sort(
        normalizedVisible.begin(),
        normalizedVisible.end(),
        [](const TranscriptSegment &left, const TranscriptSegment &right) {
            if (left.stableId != right.stableId) {
                return left.stableId < right.stableId;
            }
            return left.revision < right.revision;
        });

    for (auto &candidate : normalizedVisible) {
        auto exact = loadVisibleSegmentRevision(
            database_,
            sessionId,
            candidate.stableId,
            candidate.revision);
        if (!exact) {
            return exact.error();
        }
        if (exact.value().has_value()) {
            auto effectiveExisting = applySpeakerEnrollment(
                database_,
                sessionId,
                *exact.value());
            if (!effectiveExisting) {
                return effectiveExisting.error();
            }
            if (!sameSegmentPayload(
                    effectiveExisting.value(),
                    candidate)) {
                return Error{
                    LS_CONFLICT,
                    "same segment revision has different content"};
            }
            continue;
        }
        auto latest = loadLatestVisibleSegment(
            database_,
            sessionId,
            candidate.stableId);
        if (!latest) {
            return latest.error();
        }
        if (latest.value().has_value()
            && latest.value()->revision > candidate.revision) {
            return Error{
                LS_CONFLICT,
                "segment revision would move backwards"};
        }
        if (auto inserted = insertVisibleSegment(
                database_,
                sessionId,
                candidate,
                checkpoint);
            !inserted) {
            return inserted.error();
        }
        result.highestSegmentRevision = std::max(
            result.highestSegmentRevision,
            candidate.revision);
        result.visibleSegments.push_back(candidate);
    }

    for (const auto groupId : groupsToResolve) {
        Statement resolve;
        auto prepared = prepare(
            database_,
            "UPDATE pending_speaker_groups SET resolved_checkpoint = ? "
            "WHERE session_id = ? AND group_id = ? "
            "AND resolved_checkpoint IS NULL",
            resolve);
        if (!prepared) {
            return prepared.error();
        }
        sqlite3_bind_int64(
            resolve.get(),
            1,
            static_cast<sqlite3_int64>(checkpoint));
        bindText(resolve.get(), 2, sessionId);
        sqlite3_bind_int64(
            resolve.get(),
            3,
            static_cast<sqlite3_int64>(groupId));
        if (auto resolved = stepDone(database_, resolve.get()); !resolved) {
            return resolved.error();
        }
        if (sqlite3_changes(database_) != 1) {
            return Error{
                LS_CONFLICT,
                "pending speaker group resolution lost a race"};
        }
    }

    Statement update;
    const bool published = !result.visibleSegments.empty();
    auto prepared = published
        ? prepare(
              database_,
              "UPDATE sessions SET journal_checkpoint = ?, "
              "highest_segment_revision = MAX(highest_segment_revision, ?), "
              "timeline_origin_ns = CASE "
              "WHEN timeline_origin_ns = 0 OR timeline_origin_ns > ? THEN ? "
              "ELSE timeline_origin_ns END WHERE session_id = ?",
              update)
        : prepare(
              database_,
              "UPDATE sessions SET journal_checkpoint = ? "
              "WHERE session_id = ?",
              update);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        update.get(),
        1,
        static_cast<sqlite3_int64>(checkpoint));
    if (published) {
        const auto timelineOrigin = std::min_element(
            result.visibleSegments.begin(),
            result.visibleSegments.end(),
            [](const TranscriptSegment &left, const TranscriptSegment &right) {
                return left.startTimeNs < right.startTimeNs;
            })->startTimeNs;
        sqlite3_bind_int64(
            update.get(),
            2,
            result.highestSegmentRevision);
        sqlite3_bind_int64(update.get(), 3, timelineOrigin);
        sqlite3_bind_int64(update.get(), 4, timelineOrigin);
        bindText(update.get(), 5, sessionId);
    } else {
        bindText(update.get(), 2, sessionId);
    }
    if (auto updated = stepDone(database_, update.get()); !updated) {
        return updated.error();
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }
    result.journalCheckpoint = checkpoint;
    result.wasChanged = true;
    std::sort(
        result.visibleSegments.begin(),
        result.visibleSegments.end(),
        [](const TranscriptSegment &left, const TranscriptSegment &right) {
            if (left.startTimeNs != right.startTimeNs) {
                return left.startTimeNs < right.startTimeNs;
            }
            if (left.endTimeNs != right.endTimeNs) {
                return left.endTimeNs < right.endTimeNs;
            }
            if (left.sourceId != right.sourceId) {
                return left.sourceId < right.sourceId;
            }
            if (left.stableId != right.stableId) {
                return left.stableId < right.stableId;
            }
            return left.revision < right.revision;
        });
    return result;
}

Expected<DiarizationJournalBatchResult>
RecoveryJournal::stagePendingSegments(
    const std::string &sessionId,
    std::uint64_t groupId,
    std::int64_t deadlineMonotonicNs,
    std::span<const TranscriptSegment> fallbackSegments)
{
    DiarizationJournalBatch batch;
    batch.holds.push_back(PendingSpeakerGroupStage{
        groupId,
        deadlineMonotonicNs,
        std::vector<TranscriptSegment>(
            fallbackSegments.begin(),
            fallbackSegments.end())});
    return applyDiarizationBatch(sessionId, batch);
}

Expected<DiarizationJournalBatchResult>
RecoveryJournal::resolvePendingSpeakerGroup(
    const std::string &sessionId,
    std::uint64_t groupId,
    std::span<const SpeakerTurn> attributions)
{
    DiarizationJournalBatch batch;
    batch.resolutions.push_back(PendingSpeakerGroupResolution{
        groupId,
        std::vector<SpeakerTurn>(attributions.begin(), attributions.end())});
    return applyDiarizationBatch(sessionId, batch);
}

Expected<DiarizationJournalBatchResult>
RecoveryJournal::resolveAllPendingSpeakerGroupsToFallback(
    const std::string &sessionId)
{
    return applyDiarizationBatchImpl(sessionId, {}, true);
}

Expected<std::uint64_t> RecoveryJournal::recordSourceEvent(
    const std::string &sessionId,
    const SourceGap &event)
{
    if (event.sourceId == 0 || event.endTimeNs < event.startTimeNs) {
        return Error{LS_INVALID_ARGUMENT, "source event is invalid"};
    }
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin source event transaction");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }
    const std::uint64_t checkpoint =
        session.value().journalCheckpoint + 1u;

    Statement insert;
    auto prepared = prepare(
        database_,
        R"SQL(
INSERT INTO source_events(
    session_id, source_id, source_kind, event_kind, health,
    start_time_ns, end_time_ns, reason, test_injected
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
        insert);
    if (!prepared) {
        return prepared.error();
    }
    bindText(insert.get(), 1, sessionId);
    sqlite3_bind_int64(
        insert.get(),
        2,
        static_cast<sqlite3_int64>(event.sourceId));
    sqlite3_bind_int(insert.get(), 3, event.sourceKind);
    sqlite3_bind_int(insert.get(), 4, event.eventKind);
    sqlite3_bind_int(insert.get(), 5, event.health);
    sqlite3_bind_int64(insert.get(), 6, event.startTimeNs);
    sqlite3_bind_int64(insert.get(), 7, event.endTimeNs);
    bindText(insert.get(), 8, event.reason);
    sqlite3_bind_int(insert.get(), 9, event.testInjected ? 1 : 0);
    if (auto stepped = stepDone(database_, insert.get()); !stepped) {
        return stepped.error();
    }

    Statement updateSource;
    const bool changesHealth =
        event.eventKind == LS_SOURCE_EVENT_READY
        || event.eventKind == LS_SOURCE_EVENT_ACTIVE
        || event.eventKind == LS_SOURCE_EVENT_UNAVAILABLE
        || event.eventKind == LS_SOURCE_EVENT_RECOVERED
        || event.eventKind == LS_SOURCE_EVENT_PERMANENTLY_LOST;
    prepared = prepare(
        database_,
        "UPDATE sources SET health = CASE WHEN ? != 0 THEN ? ELSE health END, "
        "discontinuities = "
        "discontinuities + CASE WHEN ? IN (?, ?, ?) THEN 1 ELSE 0 END "
        "WHERE session_id = ? AND source_id = ?",
        updateSource);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(updateSource.get(), 1, changesHealth ? 1 : 0);
    sqlite3_bind_int(updateSource.get(), 2, event.health);
    sqlite3_bind_int(updateSource.get(), 3, event.eventKind);
    sqlite3_bind_int(
        updateSource.get(),
        4,
        LS_SOURCE_EVENT_DISCONTINUITY);
    sqlite3_bind_int(updateSource.get(), 5, LS_SOURCE_EVENT_UNAVAILABLE);
    sqlite3_bind_int(
        updateSource.get(),
        6,
        LS_SOURCE_EVENT_PERMANENTLY_LOST);
    bindText(updateSource.get(), 7, sessionId);
    sqlite3_bind_int64(
        updateSource.get(),
        8,
        static_cast<sqlite3_int64>(event.sourceId));
    if (auto stepped = stepDone(database_, updateSource.get()); !stepped) {
        return stepped.error();
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{LS_INVALID_ARGUMENT, "source is not configured"};
    }

    Statement updateSession;
    prepared = prepare(
        database_,
        "UPDATE sessions SET journal_checkpoint = ? WHERE session_id = ?",
        updateSession);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        updateSession.get(),
        1,
        static_cast<sqlite3_int64>(checkpoint));
    bindText(updateSession.get(), 2, sessionId);
    if (auto stepped = stepDone(database_, updateSession.get()); !stepped) {
        return stepped.error();
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }
    return checkpoint;
}

Expected<void> RecoveryJournal::recordFrameAccepted(
    const std::string &sessionId,
    std::uint64_t sourceId)
{
    return recordFramesAccepted(sessionId, sourceId, 1);
}

Expected<void> RecoveryJournal::recordFramesAccepted(
    const std::string &sessionId,
    std::uint64_t sourceId,
    std::uint64_t count)
{
    if (count == 0
        || count
            > static_cast<std::uint64_t>(
                std::numeric_limits<sqlite3_int64>::max())) {
        return Error{LS_INVALID_ARGUMENT, "accepted frame count is invalid"};
    }
    std::lock_guard lock(mutex_);
    Statement statement;
    auto prepared = prepare(
        database_,
        "UPDATE sources SET accepted_frames = accepted_frames + ? "
        "WHERE session_id = ? AND source_id = ?",
        statement);
    if (!prepared) {
        return prepared;
    }
    sqlite3_bind_int64(
        statement.get(),
        1,
        static_cast<sqlite3_int64>(count));
    bindText(statement.get(), 2, sessionId);
    sqlite3_bind_int64(
        statement.get(),
        3,
        static_cast<sqlite3_int64>(sourceId));
    if (auto stepped = stepDone(database_, statement.get()); !stepped) {
        return stepped;
    }
    return sqlite3_changes(database_) == 1
        ? success()
        : Expected<void>{
              Error{LS_INVALID_ARGUMENT, "source is not configured"}};
}

Expected<void> RecoveryJournal::recordFrameRejected(
    const std::string &sessionId,
    std::uint64_t sourceId,
    bool discontinuity)
{
    return recordFramesRejected(
        sessionId,
        sourceId,
        1,
        discontinuity);
}

Expected<void> RecoveryJournal::recordFramesRejected(
    const std::string &sessionId,
    std::uint64_t sourceId,
    std::uint64_t count,
    bool discontinuity)
{
    if (count == 0
        || count
            > static_cast<std::uint64_t>(
                std::numeric_limits<sqlite3_int64>::max())) {
        return Error{LS_INVALID_ARGUMENT, "rejected frame count is invalid"};
    }
    std::lock_guard lock(mutex_);
    Statement statement;
    auto prepared = prepare(
        database_,
        "UPDATE sources SET rejected_frames = rejected_frames + ?, "
        "discontinuities = discontinuities + ? "
        "WHERE session_id = ? AND source_id = ?",
        statement);
    if (!prepared) {
        return prepared;
    }
    sqlite3_bind_int64(
        statement.get(),
        1,
        static_cast<sqlite3_int64>(count));
    sqlite3_bind_int64(
        statement.get(),
        2,
        discontinuity
            ? static_cast<sqlite3_int64>(count)
            : sqlite3_int64{0});
    bindText(statement.get(), 3, sessionId);
    sqlite3_bind_int64(
        statement.get(),
        4,
        static_cast<sqlite3_int64>(sourceId));
    if (auto stepped = stepDone(database_, statement.get()); !stepped) {
        return stepped;
    }
    return sqlite3_changes(database_) == 1
        ? success()
        : Expected<void>{
              Error{LS_INVALID_ARGUMENT, "source is not configured"}};
}

Expected<JournalSnapshot>
RecoveryJournal::snapshot(const std::string &sessionId)
{
    std::lock_guard lock(mutex_);
    if (auto begun = execute(database_, "BEGIN"); !begun) {
        return begun.error();
    }
    bool active = true;
    auto rollback = [&]() {
        if (active) {
            (void)execute(database_, "ROLLBACK");
        }
    };

    auto session = loadSessionLocked(sessionId);
    if (!session) {
        rollback();
        return session.error();
    }
    JournalSnapshot result;
    result.session = session.takeValue();

    Statement sources;
    auto prepared = prepare(
        database_,
        "SELECT source_id, source_kind, required, health, accepted_frames, "
        "rejected_frames, discontinuities FROM sources "
        "WHERE session_id = ? ORDER BY source_kind, source_id",
        sources);
    if (!prepared) {
        rollback();
        return prepared.error();
    }
    bindText(sources.get(), 1, sessionId);
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(sources.get())) == SQLITE_ROW) {
        SourceRecord source;
        source.sourceId = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 0));
        source.sourceKind = sqlite3_column_int(sources.get(), 1);
        source.required = sqlite3_column_int(sources.get(), 2) != 0;
        source.health = sqlite3_column_int(sources.get(), 3);
        source.acceptedFrames = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 4));
        source.rejectedFrames = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 5));
        source.discontinuities = static_cast<std::uint64_t>(
            sqlite3_column_int64(sources.get(), 6));
        result.sources.push_back(std::move(source));
    }
    if (step != SQLITE_DONE) {
        rollback();
        return sqliteError(database_, "cannot read source snapshot");
    }

    Statement gaps;
    prepared = prepare(
        database_,
        "SELECT source_id, source_kind, event_kind, health, start_time_ns, "
        "end_time_ns, reason, test_injected FROM source_events "
        "WHERE session_id = ? ORDER BY start_time_ns, event_id",
        gaps);
    if (!prepared) {
        rollback();
        return prepared.error();
    }
    bindText(gaps.get(), 1, sessionId);
    while ((step = sqlite3_step(gaps.get())) == SQLITE_ROW) {
        SourceGap gap;
        gap.sourceId = static_cast<std::uint64_t>(
            sqlite3_column_int64(gaps.get(), 0));
        gap.sourceKind = sqlite3_column_int(gaps.get(), 1);
        gap.eventKind = sqlite3_column_int(gaps.get(), 2);
        gap.health = sqlite3_column_int(gaps.get(), 3);
        gap.startTimeNs = sqlite3_column_int64(gaps.get(), 4);
        gap.endTimeNs = sqlite3_column_int64(gaps.get(), 5);
        gap.reason = columnText(gaps.get(), 6);
        gap.testInjected = sqlite3_column_int(gaps.get(), 7) != 0;
        result.gaps.push_back(std::move(gap));
    }
    if (step != SQLITE_DONE) {
        rollback();
        return sqliteError(database_, "cannot read source event snapshot");
    }

    Statement segments;
    prepared = prepare(
        database_,
        R"SQL(
SELECT
    s.stable_id, s.source_id, s.start_time_ns, s.end_time_ns, s.speaker_id,
    s.speaker_label, s.text, s.language, s.confidence, s.revision, s.flags,
    s.journal_checkpoint, s.speaker_embedding_model,
    s.speaker_embedding_dimension, s.speaker_embedding,
    enrollment.profile_id, enrollment.display_name
FROM segments AS s
LEFT JOIN session_voice_profile_enrollments AS enrollment
  ON enrollment.session_id = s.session_id
 AND enrollment.original_speaker_id = s.speaker_id
WHERE s.session_id = ?
  AND (s.flags & ?) != 0
  AND NOT EXISTS (
      SELECT 1 FROM segments AS newer
      WHERE newer.session_id = s.session_id
        AND newer.stable_id = s.stable_id
        AND newer.revision > s.revision
  )
ORDER BY s.start_time_ns, s.end_time_ns, s.source_id, hex(s.stable_id)
)SQL",
        segments);
    if (!prepared) {
        rollback();
        return prepared.error();
    }
    bindText(segments.get(), 1, sessionId);
    sqlite3_bind_int(segments.get(), 2, LS_SEGMENT_FLAG_FINAL);
    while ((step = sqlite3_step(segments.get())) == SQLITE_ROW) {
        TranscriptSegment segment;
        const auto *stable = static_cast<const std::uint8_t *>(
            sqlite3_column_blob(segments.get(), 0));
        if (stable == nullptr || sqlite3_column_bytes(segments.get(), 0) != 16) {
            rollback();
            return Error{LS_RECOVERY_ERROR, "journal has invalid stable ID"};
        }
        std::copy_n(stable, 16, segment.stableId.begin());
        segment.sourceId = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 1));
        segment.startTimeNs = sqlite3_column_int64(segments.get(), 2);
        segment.endTimeNs = sqlite3_column_int64(segments.get(), 3);
        segment.speakerId = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 4));
        segment.speakerLabel = columnText(segments.get(), 5);
        segment.text = columnText(segments.get(), 6);
        segment.language = columnText(segments.get(), 7);
        segment.confidence =
            static_cast<float>(sqlite3_column_double(segments.get(), 8));
        segment.revision = static_cast<std::uint32_t>(
            sqlite3_column_int64(segments.get(), 9));
        segment.flags = static_cast<std::uint32_t>(
            sqlite3_column_int64(segments.get(), 10));
        segment.journalCheckpoint = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 11));
        segment.speakerEmbeddingModel = columnText(segments.get(), 12);
        const auto embedding = decodeEmbedding(segments.get(), 14, true);
        if (!embedding
            || static_cast<std::size_t>(
                   sqlite3_column_int64(segments.get(), 13))
                != embedding.value().size()
            || !validEmbedding(
                segment.speakerEmbeddingModel,
                embedding.value(),
                true)) {
            rollback();
            return Error{
                LS_RECOVERY_ERROR,
                "journal has inconsistent segment speaker evidence"};
        }
        segment.speakerEmbedding = embedding.value();
        if (sqlite3_column_type(segments.get(), 15) != SQLITE_NULL) {
            const auto profileId = static_cast<std::uint64_t>(
                sqlite3_column_int64(segments.get(), 15));
            const auto displayName = columnText(segments.get(), 16);
            if (profileId == 0 || profileId > kSpeakerIdPayloadMask
                || !validProfileName(displayName)) {
                rollback();
                return Error{
                    LS_RECOVERY_ERROR,
                    "journal has an invalid speaker enrollment"};
            }
            segment.speakerId = persistentSpeakerId(profileId);
            segment.speakerLabel = displayName;
        }
        result.segments.push_back(std::move(segment));
    }
    if (step != SQLITE_DONE) {
        rollback();
        return sqliteError(database_, "cannot read segment snapshot");
    }

    if (auto committed = execute(database_, "COMMIT"); !committed) {
        rollback();
        return committed.error();
    }
    active = false;
    return result;
}

Expected<VoiceProfile>
RecoveryJournal::loadVoiceProfileLocked(std::uint64_t profileId)
{
    if (profileId == 0 || profileId > kSpeakerIdPayloadMask) {
        return Error{LS_INVALID_ARGUMENT, "voice profile ID is invalid"};
    }

    Statement statement;
    auto prepared = prepare(
        database_,
        R"SQL(
SELECT profile_id, display_name, embedding_model_id, embedding_dimension,
       centroid, observation_count, created_at_unix_ns, updated_at_unix_ns
FROM voice_profiles
WHERE profile_id = ?
)SQL",
        statement);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        statement.get(),
        1,
        static_cast<sqlite3_int64>(profileId));
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Error{LS_NOT_FOUND, "voice profile was not found"};
    }
    if (step != SQLITE_ROW) {
        return sqliteError(database_, "cannot load voice profile");
    }

    VoiceProfile profile;
    profile.profileId = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement.get(), 0));
    profile.displayName = columnText(statement.get(), 1);
    profile.embeddingModelId = columnText(statement.get(), 2);
    const auto dimension = static_cast<std::size_t>(
        sqlite3_column_int64(statement.get(), 3));
    auto centroid = decodeEmbedding(statement.get(), 4, false);
    profile.observationCount = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement.get(), 5));
    profile.createdAtUnixNs = sqlite3_column_int64(statement.get(), 6);
    profile.updatedAtUnixNs = sqlite3_column_int64(statement.get(), 7);
    if (!centroid || profile.profileId != profileId
        || !validProfileName(profile.displayName)
        || profile.embeddingModelId.empty()
        || profile.embeddingModelId.size() > 256
        || dimension == 0 || dimension > kMaximumEmbeddingDimensions
        || centroid.value().size() != dimension
        || profile.observationCount == 0
        || profile.createdAtUnixNs < 0
        || profile.updatedAtUnixNs < profile.createdAtUnixNs
        || !normalizeEmbedding(centroid.value())) {
        return Error{LS_RECOVERY_ERROR, "voice profile is invalid"};
    }
    profile.centroid = centroid.takeValue();

    Statement prototypes;
    prepared = prepare(
        database_,
        R"SQL(
SELECT embedding_dimension, embedding
FROM voice_profile_prototypes
WHERE profile_id = ?
ORDER BY prototype_index
)SQL",
        prototypes);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        prototypes.get(),
        1,
        static_cast<sqlite3_int64>(profileId));
    int prototypeStep = SQLITE_ROW;
    while ((prototypeStep = sqlite3_step(prototypes.get())) == SQLITE_ROW) {
        if (profile.prototypes.size() >= kMaximumProfilePrototypes
            || static_cast<std::size_t>(
                   sqlite3_column_int64(prototypes.get(), 0))
                != dimension) {
            return Error{
                LS_RECOVERY_ERROR,
                "voice profile prototype is invalid"};
        }
        auto prototype = decodeEmbedding(prototypes.get(), 1, false);
        if (!prototype || prototype.value().size() != dimension
            || !normalizeEmbedding(prototype.value())) {
            return Error{
                LS_RECOVERY_ERROR,
                "voice profile prototype is invalid"};
        }
        profile.prototypes.push_back(prototype.takeValue());
    }
    if (prototypeStep != SQLITE_DONE) {
        return sqliteError(database_, "cannot load voice profile prototypes");
    }
    if (profile.prototypes.empty()) {
        profile.prototypes.push_back(profile.centroid);
    }
    return profile;
}

Expected<std::vector<VoiceProfile>> RecoveryJournal::listVoiceProfiles()
{
    std::lock_guard lock(mutex_);
    Statement statement;
    auto prepared = prepare(
        database_,
        "SELECT profile_id FROM voice_profiles ORDER BY profile_id LIMIT 1025",
        statement);
    if (!prepared) {
        return prepared.error();
    }

    std::vector<VoiceProfile> result;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        if (result.size() >= kMaximumVoiceProfiles) {
            break;
        }
        const auto profileId = static_cast<std::uint64_t>(
            sqlite3_column_int64(statement.get(), 0));
        auto profile = loadVoiceProfileLocked(profileId);
        if (profile) {
            result.push_back(profile.takeValue());
        } else if (profile.error().code != LS_RECOVERY_ERROR) {
            return profile.error();
        }
    }
    if (step != SQLITE_DONE && result.size() < kMaximumVoiceProfiles) {
        return sqliteError(database_, "cannot list voice profiles");
    }
    return result;
}

Expected<VoiceProfileEnrollment> RecoveryJournal::enrollVoiceProfile(
    const std::string &sessionId,
    std::uint64_t speakerId,
    const std::string &displayName)
{
    const auto namespaceBits =
        speakerId & (kAnonymousSpeakerFlag | kPersistentSpeakerFlag);
    const bool anonymous = namespaceBits == kAnonymousSpeakerFlag
        && (speakerId & kSpeakerIdPayloadMask) != 0;
    const bool persistent = isPersistentSpeakerId(speakerId)
        && profileIdFromSpeakerId(speakerId) != 0;
    if (!validSessionId(sessionId) || !validProfileName(displayName)
        || speakerId == 0 || speakerId == 1
        || (!anonymous && !persistent)) {
        return Error{
            LS_INVALID_ARGUMENT,
            "voice profile enrollment is invalid"};
    }

    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin voice profile enrollment");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }
    if (equalNameCaseInsensitive(displayName, "Me")
        || equalNameCaseInsensitive(
            displayName,
            session.value().localSpeakerName)) {
        return Error{
            LS_INVALID_ARGUMENT,
            "voice profile name is reserved for the local speaker"};
    }

    std::optional<std::uint64_t> mappedProfileId;
    std::optional<std::string> mappedDisplayName;
    Statement mapped;
    auto prepared = prepare(
        database_,
        "SELECT profile_id, display_name "
        "FROM session_voice_profile_enrollments "
        "WHERE session_id = ? AND original_speaker_id = ?",
        mapped);
    if (!prepared) {
        return prepared.error();
    }
    bindText(mapped.get(), 1, sessionId);
    sqlite3_bind_int64(
        mapped.get(),
        2,
        static_cast<sqlite3_int64>(speakerId));
    int step = sqlite3_step(mapped.get());
    if (step == SQLITE_ROW) {
        mappedProfileId = static_cast<std::uint64_t>(
            sqlite3_column_int64(mapped.get(), 0));
        mappedDisplayName = columnText(mapped.get(), 1);
        if (*mappedProfileId == 0
            || *mappedProfileId > kSpeakerIdPayloadMask
            || !validProfileName(*mappedDisplayName)) {
            return Error{
                LS_RECOVERY_ERROR,
                "journal has an invalid speaker enrollment"};
        }
    } else if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot inspect voice profile enrollment");
    }

    std::optional<VoiceProfile> existingProfile;
    if (isPersistentSpeakerId(speakerId)) {
        const auto profileId = profileIdFromSpeakerId(speakerId);
        if (mappedProfileId.has_value()
            && *mappedProfileId != profileId) {
            return Error{
                LS_CONFLICT,
                "session speaker is enrolled into another profile"};
        }
        auto loaded = loadVoiceProfileLocked(profileId);
        if (!loaded) {
            return loaded.error();
        }
        existingProfile = loaded.takeValue();
    } else if (mappedProfileId.has_value()) {
        auto loaded = loadVoiceProfileLocked(*mappedProfileId);
        if (!loaded) {
            return loaded.error();
        }
        existingProfile = loaded.takeValue();
    } else {
        Statement byName;
        prepared = prepare(
            database_,
            "SELECT profile_id FROM voice_profiles "
            "WHERE display_name = ? COLLATE NOCASE",
            byName);
        if (!prepared) {
            return prepared.error();
        }
        bindText(byName.get(), 1, displayName);
        step = sqlite3_step(byName.get());
        if (step == SQLITE_ROW) {
            const auto profileId = static_cast<std::uint64_t>(
                sqlite3_column_int64(byName.get(), 0));
            auto loaded = loadVoiceProfileLocked(profileId);
            if (!loaded) {
                return loaded.error();
            }
            existingProfile = loaded.takeValue();
        } else if (step != SQLITE_DONE) {
            return sqliteError(database_, "cannot find voice profile by name");
        }
    }
    if (existingProfile.has_value()
        && !equalNameCaseInsensitive(
            existingProfile->displayName,
            displayName)) {
        return Error{
            LS_CONFLICT,
            "speaker ID belongs to a differently named voice profile"};
    }

    const std::uint64_t effectiveSpeakerId = existingProfile.has_value()
        ? persistentSpeakerId(existingProfile->profileId)
        : 0;
    Statement segments;
    prepared = prepare(
        database_,
        R"SQL(
SELECT
    s.stable_id, s.source_id, s.start_time_ns, s.end_time_ns, s.speaker_id,
    s.speaker_label, s.text, s.language, s.confidence, s.revision, s.flags,
    s.journal_checkpoint, s.speaker_embedding_model,
    s.speaker_embedding_dimension, s.speaker_embedding
FROM segments AS s
WHERE s.session_id = ?
  AND (s.speaker_id = ? OR (? != 0 AND s.speaker_id = ?))
  AND (s.flags & ?) != 0
  AND NOT EXISTS (
      SELECT 1 FROM segments AS newer
      WHERE newer.session_id = s.session_id
        AND newer.stable_id = s.stable_id
        AND newer.revision > s.revision
  )
ORDER BY s.start_time_ns, s.end_time_ns, hex(s.stable_id)
)SQL",
        segments);
    if (!prepared) {
        return prepared.error();
    }
    bindText(segments.get(), 1, sessionId);
    sqlite3_bind_int64(
        segments.get(),
        2,
        static_cast<sqlite3_int64>(speakerId));
    sqlite3_bind_int64(
        segments.get(),
        3,
        static_cast<sqlite3_int64>(effectiveSpeakerId));
    sqlite3_bind_int64(
        segments.get(),
        4,
        static_cast<sqlite3_int64>(effectiveSpeakerId));
    sqlite3_bind_int(segments.get(), 5, LS_SEGMENT_FLAG_FINAL);

    std::vector<TranscriptSegment> sourceSegments;
    while ((step = sqlite3_step(segments.get())) == SQLITE_ROW) {
        TranscriptSegment segment;
        const auto *stable = static_cast<const std::uint8_t *>(
            sqlite3_column_blob(segments.get(), 0));
        if (stable == nullptr || sqlite3_column_bytes(segments.get(), 0) != 16) {
            return Error{LS_RECOVERY_ERROR, "journal has invalid stable ID"};
        }
        std::copy_n(stable, 16, segment.stableId.begin());
        segment.sourceId = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 1));
        segment.startTimeNs = sqlite3_column_int64(segments.get(), 2);
        segment.endTimeNs = sqlite3_column_int64(segments.get(), 3);
        segment.speakerId = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 4));
        segment.speakerLabel = columnText(segments.get(), 5);
        segment.text = columnText(segments.get(), 6);
        segment.language = columnText(segments.get(), 7);
        segment.confidence = static_cast<float>(
            sqlite3_column_double(segments.get(), 8));
        segment.revision = static_cast<std::uint32_t>(
            sqlite3_column_int64(segments.get(), 9));
        segment.flags = static_cast<std::uint32_t>(
            sqlite3_column_int64(segments.get(), 10));
        segment.journalCheckpoint = static_cast<std::uint64_t>(
            sqlite3_column_int64(segments.get(), 11));
        segment.speakerEmbeddingModel = columnText(segments.get(), 12);
        auto embedding = decodeEmbedding(segments.get(), 14, true);
        const auto dimension = static_cast<std::size_t>(
            sqlite3_column_int64(segments.get(), 13));
        if (!embedding || embedding.value().size() != dimension
            || !validEmbedding(
                segment.speakerEmbeddingModel,
                embedding.value(),
                true)) {
            return Error{
                LS_RECOVERY_ERROR,
                "journal has inconsistent segment speaker evidence"};
        }
        if (segment.sourceId != session.value().systemAudioSourceId) {
            return Error{
                LS_INVALID_ARGUMENT,
                "only remote system-audio speakers can be enrolled"};
        }
        segment.speakerEmbedding = embedding.takeValue();
        sourceSegments.push_back(std::move(segment));
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot load speaker enrollment evidence");
    }
    if (sourceSegments.empty()) {
        return Error{LS_NOT_FOUND, "session speaker was not found"};
    }

    std::string embeddingModelId;
    std::size_t embeddingDimension = 0;
    for (const auto &segment : sourceSegments) {
        if (segment.speakerEmbedding.empty()) {
            continue;
        }
        if (embeddingModelId.empty()) {
            embeddingModelId = segment.speakerEmbeddingModel;
            embeddingDimension = segment.speakerEmbedding.size();
        } else if (embeddingModelId != segment.speakerEmbeddingModel
                   || embeddingDimension != segment.speakerEmbedding.size()) {
            return Error{
                LS_CONFLICT,
                "session speaker has incompatible embedding models"};
        }
    }
    if (embeddingModelId.empty()) {
        return Error{
            LS_NOT_FOUND,
            "session speaker has no stable voice evidence"};
    }
    if (existingProfile.has_value()
        && (existingProfile->embeddingModelId != embeddingModelId
            || existingProfile->centroid.size() != embeddingDimension)) {
        return Error{
            LS_CONFLICT,
            "voice profile uses an incompatible embedding model"};
    }

    std::vector<std::pair<StableId, std::vector<float>>> newEvidence;
    for (const auto &segment : sourceSegments) {
        if (segment.speakerEmbedding.empty()) {
            continue;
        }
        if (existingProfile.has_value()) {
            Statement observed;
            prepared = prepare(
                database_,
                "SELECT 1 FROM voice_profile_observations "
                "WHERE profile_id = ? AND session_id = ? AND stable_id = ?",
                observed);
            if (!prepared) {
                return prepared.error();
            }
            sqlite3_bind_int64(
                observed.get(),
                1,
                static_cast<sqlite3_int64>(existingProfile->profileId));
            bindText(observed.get(), 2, sessionId);
            sqlite3_bind_blob(
                observed.get(),
                3,
                segment.stableId.data(),
                static_cast<int>(segment.stableId.size()),
                SQLITE_TRANSIENT);
            const int observedStep = sqlite3_step(observed.get());
            if (observedStep == SQLITE_ROW) {
                continue;
            }
            if (observedStep != SQLITE_DONE) {
                return sqliteError(
                    database_,
                    "cannot inspect voice profile observation");
            }
        }
        auto normalized = segment.speakerEmbedding;
        if (!normalizeEmbedding(normalized)) {
            return Error{
                LS_INVALID_ARGUMENT,
                "session speaker has unusable voice evidence"};
        }
        newEvidence.emplace_back(segment.stableId, std::move(normalized));
    }
    if (!existingProfile.has_value() && newEvidence.empty()) {
        return Error{
            LS_NOT_FOUND,
            "session speaker has no new voice evidence"};
    }

    VoiceProfile profile;
    bool profileNameChanged = false;
    if (existingProfile.has_value()) {
        profile = *existingProfile;
        profileNameChanged = profile.displayName != displayName;
        profile.displayName = displayName;
    } else {
        profile.displayName = displayName;
        profile.embeddingModelId = embeddingModelId;
        profile.centroid.assign(embeddingDimension, 0.0F);
        profile.createdAtUnixNs = unixTimeNs();
        profile.updatedAtUnixNs = profile.createdAtUnixNs;
    }

    if (!newEvidence.empty()) {
        std::vector<long double> aggregate(embeddingDimension, 0.0L);
        if (profile.observationCount != 0) {
            for (std::size_t index = 0; index < embeddingDimension; ++index) {
                aggregate[index] = static_cast<long double>(
                    profile.centroid[index])
                    * static_cast<long double>(profile.observationCount);
            }
        }
        for (const auto &[stableId, values] : newEvidence) {
            (void)stableId;
            for (std::size_t index = 0; index < values.size(); ++index) {
                aggregate[index] += values[index];
            }
            const bool distinct = std::all_of(
                profile.prototypes.begin(),
                profile.prototypes.end(),
                [&](const std::vector<float> &prototype) {
                    return cosineSimilarity(values, prototype) < 0.995F;
                });
            if (distinct
                && profile.prototypes.size() < kMaximumProfilePrototypes) {
                profile.prototypes.push_back(values);
            }
        }
        profile.centroid.resize(embeddingDimension);
        for (std::size_t index = 0; index < embeddingDimension; ++index) {
            profile.centroid[index] = static_cast<float>(aggregate[index]);
        }
        if (!normalizeEmbedding(profile.centroid)) {
            return Error{
                LS_INVALID_ARGUMENT,
                "voice evidence cannot form a stable profile"};
        }
        if (profile.prototypes.empty()) {
            profile.prototypes.push_back(profile.centroid);
        }
        constexpr auto kMaximumObservationCount =
            static_cast<std::uint64_t>(
                std::numeric_limits<sqlite3_int64>::max());
        if (newEvidence.size() > kMaximumObservationCount
            || profile.observationCount
                > kMaximumObservationCount - newEvidence.size()) {
            return Error{LS_CONFLICT, "voice profile observation count overflow"};
        }
        profile.observationCount += newEvidence.size();
        profile.updatedAtUnixNs = std::max(
            unixTimeNs(),
            profile.createdAtUnixNs);
    }

    if (!existingProfile.has_value()) {
        Statement insertProfile;
        prepared = prepare(
            database_,
            R"SQL(
INSERT INTO voice_profiles(
    display_name, embedding_model_id, embedding_dimension, centroid,
    observation_count, created_at_unix_ns, updated_at_unix_ns
) VALUES (?, ?, ?, ?, ?, ?, ?)
)SQL",
            insertProfile);
        if (!prepared) {
            return prepared.error();
        }
        bindText(insertProfile.get(), 1, profile.displayName);
        bindText(insertProfile.get(), 2, profile.embeddingModelId);
        sqlite3_bind_int64(
            insertProfile.get(),
            3,
            static_cast<sqlite3_int64>(profile.centroid.size()));
        bindEmbedding(insertProfile.get(), 4, profile.centroid);
        sqlite3_bind_int64(
            insertProfile.get(),
            5,
            static_cast<sqlite3_int64>(profile.observationCount));
        sqlite3_bind_int64(insertProfile.get(), 6, profile.createdAtUnixNs);
        sqlite3_bind_int64(insertProfile.get(), 7, profile.updatedAtUnixNs);
        const int inserted = sqlite3_step(insertProfile.get());
        if (inserted == SQLITE_CONSTRAINT) {
            return Error{LS_CONFLICT, "voice profile name already exists"};
        }
        if (inserted != SQLITE_DONE) {
            return sqliteError(database_, "cannot create voice profile");
        }
        profile.profileId = static_cast<std::uint64_t>(
            sqlite3_last_insert_rowid(database_));
        if (profile.profileId == 0
            || profile.profileId > kSpeakerIdPayloadMask) {
            return Error{LS_CONFLICT, "voice profile ID space is exhausted"};
        }
    } else if (!newEvidence.empty() || profileNameChanged) {
        if (profileNameChanged && newEvidence.empty()) {
            profile.updatedAtUnixNs = std::max(
                unixTimeNs(),
                profile.createdAtUnixNs);
        }
        Statement updateProfile;
        prepared = prepare(
            database_,
            R"SQL(
UPDATE voice_profiles
SET display_name = ?, centroid = ?, observation_count = ?,
    updated_at_unix_ns = ?
WHERE profile_id = ?
)SQL",
            updateProfile);
        if (!prepared) {
            return prepared.error();
        }
        bindText(updateProfile.get(), 1, profile.displayName);
        bindEmbedding(updateProfile.get(), 2, profile.centroid);
        sqlite3_bind_int64(
            updateProfile.get(),
            3,
            static_cast<sqlite3_int64>(profile.observationCount));
        sqlite3_bind_int64(
            updateProfile.get(),
            4,
            profile.updatedAtUnixNs);
        sqlite3_bind_int64(
            updateProfile.get(),
            5,
            static_cast<sqlite3_int64>(profile.profileId));
        const int updated = sqlite3_step(updateProfile.get());
        if (updated == SQLITE_CONSTRAINT) {
            return Error{LS_CONFLICT, "voice profile name already exists"};
        }
        if (updated != SQLITE_DONE) {
            return sqliteError(database_, "cannot update voice profile");
        }
    }

    if (!newEvidence.empty()) {
        Statement clearPrototypes;
        prepared = prepare(
            database_,
            "DELETE FROM voice_profile_prototypes WHERE profile_id = ?",
            clearPrototypes);
        if (!prepared) {
            return prepared.error();
        }
        sqlite3_bind_int64(
            clearPrototypes.get(),
            1,
            static_cast<sqlite3_int64>(profile.profileId));
        if (auto cleared = stepDone(database_, clearPrototypes.get()); !cleared) {
            return cleared.error();
        }
        for (std::size_t index = 0; index < profile.prototypes.size(); ++index) {
            Statement insertPrototype;
            prepared = prepare(
                database_,
                R"SQL(
INSERT INTO voice_profile_prototypes(
    profile_id, prototype_index, embedding_dimension, embedding
) VALUES (?, ?, ?, ?)
)SQL",
                insertPrototype);
            if (!prepared) {
                return prepared.error();
            }
            sqlite3_bind_int64(
                insertPrototype.get(),
                1,
                static_cast<sqlite3_int64>(profile.profileId));
            sqlite3_bind_int64(
                insertPrototype.get(),
                2,
                static_cast<sqlite3_int64>(index));
            sqlite3_bind_int64(
                insertPrototype.get(),
                3,
                static_cast<sqlite3_int64>(embeddingDimension));
            bindEmbedding(
                insertPrototype.get(),
                4,
                profile.prototypes[index]);
            if (auto inserted = stepDone(database_, insertPrototype.get());
                !inserted) {
                return inserted.error();
            }
        }

        for (const auto &[stableId, values] : newEvidence) {
            (void)values;
            Statement observation;
            prepared = prepare(
                database_,
                R"SQL(
INSERT OR IGNORE INTO voice_profile_observations(
    profile_id, session_id, stable_id
) VALUES (?, ?, ?)
)SQL",
                observation);
            if (!prepared) {
                return prepared.error();
            }
            sqlite3_bind_int64(
                observation.get(),
                1,
                static_cast<sqlite3_int64>(profile.profileId));
            bindText(observation.get(), 2, sessionId);
            sqlite3_bind_blob(
                observation.get(),
                3,
                stableId.data(),
                static_cast<int>(stableId.size()),
                SQLITE_TRANSIENT);
            if (auto inserted = stepDone(database_, observation.get());
                !inserted) {
                return inserted.error();
            }
        }
    }

    bool insertedMapping = false;
    if (!mappedProfileId.has_value()) {
        Statement insertMapping;
        prepared = prepare(
            database_,
            R"SQL(
INSERT INTO session_voice_profile_enrollments(
    session_id, original_speaker_id, profile_id, display_name
) VALUES (?, ?, ?, ?)
)SQL",
            insertMapping);
        if (!prepared) {
            return prepared.error();
        }
        bindText(insertMapping.get(), 1, sessionId);
        sqlite3_bind_int64(
            insertMapping.get(),
            2,
            static_cast<sqlite3_int64>(speakerId));
        sqlite3_bind_int64(
            insertMapping.get(),
            3,
            static_cast<sqlite3_int64>(profile.profileId));
        bindText(insertMapping.get(), 4, profile.displayName);
        if (auto inserted = stepDone(database_, insertMapping.get()); !inserted) {
            return inserted.error();
        }
        insertedMapping = true;
    } else if (*mappedProfileId != profile.profileId) {
        return Error{
            LS_CONFLICT,
            "session speaker is enrolled into another voice profile"};
    }

    const std::uint64_t persistedSpeakerId =
        persistentSpeakerId(profile.profileId);
    std::uint32_t relabeledSegments = 0;
    if (insertedMapping) {
        for (const auto &source : sourceSegments) {
            if (source.speakerId == persistedSpeakerId
                && source.speakerLabel == profile.displayName) {
                continue;
            }
            if (relabeledSegments
                == std::numeric_limits<std::uint32_t>::max()) {
                return Error{
                    LS_CONFLICT,
                    "too many segments are affected by voice enrollment"};
            }
            ++relabeledSegments;
        }
    }

    if (insertedMapping
        && session.value().journalCheckpoint
            == std::numeric_limits<std::uint64_t>::max()) {
        return Error{LS_CONFLICT, "journal checkpoint space is exhausted"};
    }
    const std::uint64_t checkpoint = insertedMapping
        ? session.value().journalCheckpoint + 1u
        : session.value().journalCheckpoint;
    if (insertedMapping) {
        Statement updateSession;
        prepared = prepare(
            database_,
            "UPDATE sessions SET journal_checkpoint = ? WHERE session_id = ?",
            updateSession);
        if (!prepared) {
            return prepared.error();
        }
        sqlite3_bind_int64(
            updateSession.get(),
            1,
            static_cast<sqlite3_int64>(checkpoint));
        bindText(updateSession.get(), 2, sessionId);
        if (auto updated = stepDone(database_, updateSession.get()); !updated) {
            return updated.error();
        }
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }

    VoiceProfileEnrollment enrollment;
    enrollment.profile = std::move(profile);
    enrollment.speakerId = persistedSpeakerId;
    enrollment.relabeledSegments = relabeledSegments;
    enrollment.journalCheckpoint = checkpoint;
    enrollment.highestSegmentRevision =
        session.value().highestSegmentRevision;
    return enrollment;
}

Expected<void> RecoveryJournal::renameVoiceProfile(
    std::uint64_t profileId,
    const std::string &displayName)
{
    if (profileId == 0 || profileId > kSpeakerIdPayloadMask
        || !validProfileName(displayName)
        || equalNameCaseInsensitive(displayName, "Me")) {
        return Error{LS_INVALID_ARGUMENT, "voice profile name or ID is invalid"};
    }
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin voice profile rename");
    }

    Statement update;
    auto prepared = prepare(
        database_,
        "UPDATE voice_profiles SET display_name = ?, updated_at_unix_ns = ? "
        "WHERE profile_id = ?",
        update);
    if (!prepared) {
        return prepared.error();
    }
    bindText(update.get(), 1, displayName);
    sqlite3_bind_int64(update.get(), 2, unixTimeNs());
    sqlite3_bind_int64(
        update.get(),
        3,
        static_cast<sqlite3_int64>(profileId));
    const int updated = sqlite3_step(update.get());
    if (updated == SQLITE_CONSTRAINT) {
        return Error{LS_CONFLICT, "voice profile name already exists"};
    }
    if (updated != SQLITE_DONE) {
        return sqliteError(database_, "cannot rename voice profile");
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{LS_NOT_FOUND, "voice profile was not found"};
    }
    return transaction.commit();
}

Expected<void> RecoveryJournal::deleteVoiceProfile(std::uint64_t profileId)
{
    if (profileId == 0 || profileId > kSpeakerIdPayloadMask) {
        return Error{LS_INVALID_ARGUMENT, "voice profile ID is invalid"};
    }
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin voice profile deletion");
    }
    Statement statement;
    auto prepared = prepare(
        database_,
        "DELETE FROM voice_profiles WHERE profile_id = ?",
        statement);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int64(
        statement.get(),
        1,
        static_cast<sqlite3_int64>(profileId));
    if (auto stepped = stepDone(database_, statement.get()); !stepped) {
        return stepped.error();
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{LS_NOT_FOUND, "voice profile was not found"};
    }
    return transaction.commit();
}

Expected<void> RecoveryJournal::acknowledgePublication(
    const std::string &sessionId,
    const PublicationReceipt &receipt)
{
    if (receipt.destination < LS_PUBLICATION_DESTINATION_VAULT
        || receipt.destination > LS_PUBLICATION_DESTINATION_RECOVERY_COPY
        || receipt.publishedAtUnixNs < 0 || !validDigest(receipt.sha256Hex)
        || receipt.fileIdentity.find('\0') != std::string::npos) {
        return Error{LS_INVALID_ARGUMENT, "publication receipt is invalid"};
    }
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin receipt transaction");
    }
    auto session = loadSessionLocked(sessionId);
    if (!session) {
        return session.error();
    }
    if (receipt.journalCheckpoint == 0
        || receipt.journalCheckpoint > session.value().journalCheckpoint) {
        return Error{
            LS_CONFLICT,
            "publication receipt checkpoint is in the future"};
    }
    if (publicationMustMatchCurrentCheckpoint(session.value().phase)
        && receipt.journalCheckpoint
            != session.value().journalCheckpoint) {
        return Error{
            LS_CONFLICT,
            "terminal or recovery publication must match current checkpoint"};
    }

    Statement expectedRevision;
    auto prepared = prepare(
        database_,
        "SELECT COALESCE(MAX(revision), 0) FROM segments "
        "WHERE session_id = ? AND journal_checkpoint <= ?",
        expectedRevision);
    if (!prepared) {
        return prepared;
    }
    bindText(expectedRevision.get(), 1, sessionId);
    sqlite3_bind_int64(
        expectedRevision.get(),
        2,
        static_cast<sqlite3_int64>(receipt.journalCheckpoint));
    if (sqlite3_step(expectedRevision.get()) != SQLITE_ROW) {
        return sqliteError(
            database_,
            "cannot inspect historical segment revision");
    }
    const auto revisionAtCheckpoint = static_cast<std::uint32_t>(
        sqlite3_column_int64(expectedRevision.get(), 0));
    if (receipt.highestSegmentRevision != revisionAtCheckpoint) {
        return Error{
            LS_CONFLICT,
            "publication receipt revision does not match its checkpoint"};
    }

    Statement latest;
    prepared = prepare(
        database_,
        "SELECT COALESCE(MAX(journal_checkpoint), 0) "
        "FROM publication_receipts WHERE session_id = ?",
        latest);
    if (!prepared) {
        return prepared;
    }
    bindText(latest.get(), 1, sessionId);
    if (sqlite3_step(latest.get()) != SQLITE_ROW) {
        return sqliteError(database_, "cannot inspect publication receipts");
    }
    const auto previous =
        static_cast<std::uint64_t>(sqlite3_column_int64(latest.get(), 0));
    if (previous > receipt.journalCheckpoint) {
        return Error{LS_CONFLICT, "publication checkpoint regressed"};
    }

    Statement insert;
    prepared = prepare(
        database_,
        R"SQL(
INSERT INTO publication_receipts(
    session_id, journal_checkpoint, highest_segment_revision, destination,
    published_at_unix_ns, sha256_hex, file_identity
) VALUES (?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(session_id, journal_checkpoint) DO UPDATE SET
    highest_segment_revision = excluded.highest_segment_revision,
    destination = excluded.destination,
    published_at_unix_ns = excluded.published_at_unix_ns,
    sha256_hex = excluded.sha256_hex,
    file_identity = excluded.file_identity
WHERE publication_receipts.highest_segment_revision =
          excluded.highest_segment_revision
  AND publication_receipts.destination = excluded.destination
  AND publication_receipts.sha256_hex = excluded.sha256_hex
  AND publication_receipts.file_identity = excluded.file_identity
)SQL",
        insert);
    if (!prepared) {
        return prepared;
    }
    bindText(insert.get(), 1, sessionId);
    sqlite3_bind_int64(
        insert.get(),
        2,
        static_cast<sqlite3_int64>(receipt.journalCheckpoint));
    sqlite3_bind_int64(insert.get(), 3, receipt.highestSegmentRevision);
    sqlite3_bind_int(insert.get(), 4, receipt.destination);
    sqlite3_bind_int64(insert.get(), 5, receipt.publishedAtUnixNs);
    bindText(insert.get(), 6, receipt.sha256Hex);
    bindText(insert.get(), 7, receipt.fileIdentity);
    if (auto stepped = stepDone(database_, insert.get()); !stepped) {
        return stepped;
    }
    if (sqlite3_changes(database_) != 1) {
        return Error{
            LS_CONFLICT,
            "publication receipt conflicts with an existing receipt"};
    }
    return transaction.commit();
}

Expected<std::vector<std::string>>
RecoveryJournal::markAndListRecoverableSessions()
{
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    if (!transaction.active()) {
        return sqliteError(database_, "cannot begin recovery transaction");
    }

    Statement candidates;
    auto prepared = prepare(
        database_,
        "SELECT session_id, phase, journal_checkpoint, "
        "highest_segment_revision, timeline_origin_ns FROM sessions "
        "WHERE phase IN (?, ?, ?, ?) ORDER BY session_id",
        candidates);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(candidates.get(), 1, LS_PHASE_PREPARING);
    sqlite3_bind_int(candidates.get(), 2, LS_PHASE_RECORDING);
    sqlite3_bind_int(candidates.get(), 3, LS_PHASE_PAUSED);
    sqlite3_bind_int(candidates.get(), 4, LS_PHASE_FINALIZING);

    struct Candidate {
        std::string id;
        ls_phase_t phase{};
        std::uint64_t checkpoint{};
        std::uint32_t highestSegmentRevision{};
        std::int64_t timelineOriginNs{};
    };
    std::vector<Candidate> rows;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(candidates.get())) == SQLITE_ROW) {
        rows.push_back(Candidate{
            columnText(candidates.get(), 0),
            sqlite3_column_int(candidates.get(), 1),
            static_cast<std::uint64_t>(
                sqlite3_column_int64(candidates.get(), 2)),
            static_cast<std::uint32_t>(
                sqlite3_column_int64(candidates.get(), 3)),
            sqlite3_column_int64(candidates.get(), 4)});
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot discover recoverable sessions");
    }

    for (const auto &candidate : rows) {
        if (candidate.checkpoint
            == std::numeric_limits<std::uint64_t>::max()) {
            return Error{
                LS_CONFLICT,
                "journal checkpoint space is exhausted during recovery"};
        }
        const std::uint64_t recoveryCheckpoint = candidate.checkpoint + 1u;

        Statement unresolvedGroups;
        prepared = prepare(
            database_,
            "SELECT group_id FROM pending_speaker_groups "
            "WHERE session_id = ? AND resolved_checkpoint IS NULL "
            "ORDER BY deadline_monotonic_ns, group_id",
            unresolvedGroups);
        if (!prepared) {
            return prepared.error();
        }
        bindText(unresolvedGroups.get(), 1, candidate.id);
        std::vector<std::uint64_t> groupIds;
        int groupStep = SQLITE_ROW;
        while ((groupStep = sqlite3_step(unresolvedGroups.get()))
               == SQLITE_ROW) {
            const auto groupId = static_cast<std::uint64_t>(
                sqlite3_column_int64(unresolvedGroups.get(), 0));
            if (groupId == 0
                || groupId
                    > static_cast<std::uint64_t>(
                        std::numeric_limits<sqlite3_int64>::max())) {
                return Error{
                    LS_RECOVERY_ERROR,
                    "journal has an invalid pending speaker group"};
            }
            groupIds.push_back(groupId);
        }
        if (groupStep != SQLITE_DONE) {
            return sqliteError(
                database_,
                "cannot discover pending recovery groups");
        }

        std::vector<TranscriptSegment> fallbackSegments;
        for (const auto groupId : groupIds) {
            auto fallback = resolvePendingPayload(
                database_,
                candidate.id,
                groupId,
                {});
            if (!fallback) {
                return fallback.error();
            }
            for (auto &segment : fallback.value()) {
                segment.journalCheckpoint = recoveryCheckpoint;
                const auto duplicate = std::find_if(
                    fallbackSegments.begin(),
                    fallbackSegments.end(),
                    [&](const TranscriptSegment &existing) {
                        return existing.stableId == segment.stableId
                            && existing.revision == segment.revision;
                    });
                if (duplicate == fallbackSegments.end()) {
                    fallbackSegments.push_back(std::move(segment));
                } else if (!sameSegmentPayload(*duplicate, segment)) {
                    return Error{
                        LS_RECOVERY_ERROR,
                        "pending recovery groups have conflicting payload"};
                }
            }
        }
        std::sort(
            fallbackSegments.begin(),
            fallbackSegments.end(),
            [](const TranscriptSegment &left,
               const TranscriptSegment &right) {
                if (left.stableId != right.stableId) {
                    return left.stableId < right.stableId;
                }
                return left.revision < right.revision;
            });

        std::uint32_t highestRevision =
            candidate.highestSegmentRevision;
        std::int64_t timelineOrigin = candidate.timelineOriginNs;
        bool insertedFallback = false;
        for (const auto &fallback : fallbackSegments) {
            auto exact = loadVisibleSegmentRevision(
                database_,
                candidate.id,
                fallback.stableId,
                fallback.revision);
            if (!exact) {
                return exact.error();
            }
            if (exact.value().has_value()) {
                auto effectiveExisting = applySpeakerEnrollment(
                    database_,
                    candidate.id,
                    *exact.value());
                if (!effectiveExisting) {
                    return effectiveExisting.error();
                }
                if (!sameSegmentPayload(
                        effectiveExisting.value(),
                        fallback)) {
                    return Error{
                        LS_RECOVERY_ERROR,
                        "pending fallback conflicts with visible segment"};
                }
                continue;
            }
            auto latest = loadLatestVisibleSegment(
                database_,
                candidate.id,
                fallback.stableId);
            if (!latest) {
                return latest.error();
            }
            if (latest.value().has_value()
                && latest.value()->revision > fallback.revision) {
                return Error{
                    LS_RECOVERY_ERROR,
                    "pending fallback revision is older than visible segment"};
            }
            if (auto inserted = insertVisibleSegment(
                    database_,
                    candidate.id,
                    fallback,
                    recoveryCheckpoint);
                !inserted) {
                return inserted.error();
            }
            insertedFallback = true;
            highestRevision = std::max(
                highestRevision,
                fallback.revision);
            if (timelineOrigin == 0
                || timelineOrigin > fallback.startTimeNs) {
                timelineOrigin = fallback.startTimeNs;
            }
        }

        if (!groupIds.empty()) {
            Statement resolveGroups;
            prepared = prepare(
                database_,
                "UPDATE pending_speaker_groups "
                "SET resolved_checkpoint = ? "
                "WHERE session_id = ? AND resolved_checkpoint IS NULL",
                resolveGroups);
            if (!prepared) {
                return prepared.error();
            }
            sqlite3_bind_int64(
                resolveGroups.get(),
                1,
                static_cast<sqlite3_int64>(recoveryCheckpoint));
            bindText(resolveGroups.get(), 2, candidate.id);
            if (auto resolved = stepDone(
                    database_,
                    resolveGroups.get());
                !resolved) {
                return resolved.error();
            }
            if (sqlite3_changes(database_)
                != static_cast<int>(groupIds.size())) {
                return Error{
                    LS_CONFLICT,
                    "pending recovery group resolution lost a race"};
            }
        }

        Statement sequence;
        prepared = prepare(
            database_,
            "SELECT COALESCE(MAX(event_sequence), 0) + 1 "
            "FROM state_events WHERE session_id = ?",
            sequence);
        if (!prepared) {
            return prepared.error();
        }
        bindText(sequence.get(), 1, candidate.id);
        if (sqlite3_step(sequence.get()) != SQLITE_ROW) {
            return sqliteError(database_, "cannot allocate recovery event");
        }
        const auto eventSequence = sqlite3_column_int64(sequence.get(), 0);

        Statement update;
        prepared = insertedFallback
            ? prepare(
                  database_,
                  "UPDATE sessions SET phase = ?, recovery_marked = 1, "
                  "journal_checkpoint = ?, highest_segment_revision = ?, "
                  "timeline_origin_ns = ? "
                  "WHERE session_id = ? AND phase = ?",
                  update)
            : prepare(
                  database_,
                  "UPDATE sessions SET phase = ?, recovery_marked = 1, "
                  "journal_checkpoint = ? "
                  "WHERE session_id = ? AND phase = ?",
                  update);
        if (!prepared) {
            return prepared.error();
        }
        sqlite3_bind_int(update.get(), 1, LS_PHASE_RECOVERY_REQUIRED);
        sqlite3_bind_int64(
            update.get(),
            2,
            static_cast<sqlite3_int64>(recoveryCheckpoint));
        if (insertedFallback) {
            sqlite3_bind_int64(update.get(), 3, highestRevision);
            sqlite3_bind_int64(update.get(), 4, timelineOrigin);
            bindText(update.get(), 5, candidate.id);
            sqlite3_bind_int(update.get(), 6, candidate.phase);
        } else {
            bindText(update.get(), 3, candidate.id);
            sqlite3_bind_int(update.get(), 4, candidate.phase);
        }
        if (auto stepped = stepDone(database_, update.get()); !stepped) {
            return stepped.error();
        }
        if (sqlite3_changes(database_) == 0) {
            continue;
        }

        Statement event;
        prepared = prepare(
            database_,
            "INSERT INTO state_events(session_id, event_sequence, phase, "
            "reason) VALUES (?, ?, ?, ?)",
            event);
        if (!prepared) {
            return prepared.error();
        }
        bindText(event.get(), 1, candidate.id);
        sqlite3_bind_int64(event.get(), 2, eventSequence);
        sqlite3_bind_int(event.get(), 3, LS_PHASE_RECOVERY_REQUIRED);
        sqlite3_bind_int(
            event.get(),
            4,
            LS_FINALIZE_REASON_PROCESS_INTERRUPTED);
        if (auto stepped = stepDone(database_, event.get()); !stepped) {
            return stepped.error();
        }
    }
    if (auto committed = transaction.commit(); !committed) {
        return committed.error();
    }

    /* Re-enter through the public query after releasing no lock would deadlock;
       query the now-canonical set directly under this lock instead. */
    Statement list;
    prepared = prepare(
        database_,
        R"SQL(
SELECT s.session_id
FROM sessions AS s
WHERE s.phase = ?
   OR (
       s.phase IN (?, ?, ?)
       AND NOT EXISTS (
           SELECT 1
           FROM publication_receipts AS p
           WHERE p.session_id = s.session_id
             AND p.journal_checkpoint = s.journal_checkpoint
             AND p.highest_segment_revision =
                   s.highest_segment_revision
       )
   )
ORDER BY s.session_id
)SQL",
        list);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(list.get(), 1, LS_PHASE_RECOVERY_REQUIRED);
    sqlite3_bind_int(list.get(), 2, LS_PHASE_COMPLETE);
    sqlite3_bind_int(list.get(), 3, LS_PHASE_INCOMPLETE_SOURCES);
    sqlite3_bind_int(list.get(), 4, LS_PHASE_INTERRUPTED);
    std::vector<std::string> result;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW) {
        result.push_back(columnText(list.get(), 0));
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot list recoverable sessions");
    }
    return result;
}

Expected<std::vector<std::string>>
RecoveryJournal::listRecoverableSessions()
{
    std::lock_guard lock(mutex_);
    Statement list;
    auto prepared = prepare(
        database_,
        R"SQL(
SELECT s.session_id
FROM sessions AS s
WHERE s.phase = ?
   OR (
       s.phase IN (?, ?, ?)
       AND NOT EXISTS (
           SELECT 1
           FROM publication_receipts AS p
           WHERE p.session_id = s.session_id
             AND p.journal_checkpoint = s.journal_checkpoint
             AND p.highest_segment_revision =
                   s.highest_segment_revision
       )
   )
ORDER BY s.session_id
)SQL",
        list);
    if (!prepared) {
        return prepared.error();
    }
    sqlite3_bind_int(list.get(), 1, LS_PHASE_RECOVERY_REQUIRED);
    sqlite3_bind_int(list.get(), 2, LS_PHASE_COMPLETE);
    sqlite3_bind_int(list.get(), 3, LS_PHASE_INCOMPLETE_SOURCES);
    sqlite3_bind_int(list.get(), 4, LS_PHASE_INTERRUPTED);
    std::vector<std::string> result;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(list.get())) == SQLITE_ROW) {
        result.push_back(columnText(list.get(), 0));
    }
    if (step != SQLITE_DONE) {
        return sqliteError(database_, "cannot list recoverable sessions");
    }
    return result;
}

Expected<void> RecoveryJournal::quickCheck()
{
    std::lock_guard lock(mutex_);
    for (const char *sql :
         {"PRAGMA quick_check", "PRAGMA foreign_key_check"}) {
        Statement statement;
        auto prepared = prepare(database_, sql, statement);
        if (!prepared) {
            return prepared;
        }
        int step = sqlite3_step(statement.get());
        if (std::strcmp(sql, "PRAGMA quick_check") == 0) {
            if (step != SQLITE_ROW || columnText(statement.get(), 0) != "ok") {
                return Error{LS_RECOVERY_ERROR, "SQLite quick_check failed"};
            }
            step = sqlite3_step(statement.get());
        }
        if (step != SQLITE_DONE) {
            return Error{
                LS_RECOVERY_ERROR,
                "SQLite foreign-key or integrity check failed"};
        }
    }
    return success();
}

} // namespace localscribe
