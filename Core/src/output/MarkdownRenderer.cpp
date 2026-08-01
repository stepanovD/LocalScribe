#include "MarkdownRenderer.hpp"

#include "../common/TranscriptLanguagePolicy.hpp"
#include "../session/SessionStateMachine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace localscribe {
namespace {

constexpr std::string_view kStartMarker = "<!-- transcript:start -->";
constexpr std::string_view kEndMarker = "<!-- transcript:end -->";
constexpr std::string_view kCaptureEventsStartMarker =
    "<!-- capture-events:start -->";
constexpr std::string_view kCaptureEventsEndMarker =
    "<!-- capture-events:end -->";
constexpr std::string_view kNeutralStart =
    "&lt;!-- transcript:start --&gt;";
constexpr std::string_view kNeutralEnd =
    "&lt;!-- transcript:end --&gt;";
constexpr std::string_view kNeutralCaptureEventsStart =
    "&lt;!-- capture-events:start --&gt;";
constexpr std::string_view kNeutralCaptureEventsEnd =
    "&lt;!-- capture-events:end --&gt;";

bool continuation(unsigned char value)
{
    return (value & 0xC0u) == 0x80u;
}

void appendReplacement(std::string &output)
{
    output.append("\xEF\xBF\xBD", 3);
}

void replaceAll(
    std::string &value,
    std::string_view needle,
    std::string_view replacement)
{
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

std::string phaseStatus(ls_phase_t phase)
{
    switch (SessionStateMachine::publishedStatus(phase)) {
    case LS_PUBLISHED_STATUS_RECORDING:
        return "recording";
    case LS_PUBLISHED_STATUS_COMPLETE:
        return "complete";
    case LS_PUBLISHED_STATUS_INTERRUPTED:
        return "interrupted";
    case LS_PUBLISHED_STATUS_INCOMPLETE_SOURCES:
        return "incomplete_sources";
    default:
        return {};
    }
}

std::string relativeTime(
    std::int64_t timestampNs,
    std::int64_t originNs)
{
    std::uint64_t seconds = 0;
    if (timestampNs > originNs) {
        const auto delta = static_cast<std::uint64_t>(timestampNs - originNs);
        seconds = delta / 1'000'000'000ULL;
    }
    const std::uint64_t hours = seconds / 3600u;
    const std::uint64_t minutes = (seconds / 60u) % 60u;
    const std::uint64_t remaining = seconds % 60u;
    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(2) << hours << ':'
           << std::setw(2) << minutes << ':' << std::setw(2) << remaining;
    return stream.str();
}

bool isMicrophone(const SourceRecord &source)
{
    return source.sourceKind == LS_SOURCE_KIND_MICROPHONE;
}

bool isSystemAudio(const SourceRecord &source)
{
    return source.sourceKind == LS_SOURCE_KIND_SYSTEM_AUDIO;
}

ls_source_kind_t sourceKind(
    const JournalSnapshot &snapshot,
    std::uint64_t sourceId)
{
    if (sourceId != 0
        && sourceId == snapshot.session.microphoneSourceId) {
        return LS_SOURCE_KIND_MICROPHONE;
    }
    if (sourceId != 0
        && sourceId == snapshot.session.systemAudioSourceId) {
        return LS_SOURCE_KIND_SYSTEM_AUDIO;
    }
    const auto source = std::find_if(
        snapshot.sources.begin(),
        snapshot.sources.end(),
        [&](const SourceRecord &candidate) {
            return candidate.sourceId == sourceId;
        });
    return source == snapshot.sources.end()
        ? LS_SOURCE_KIND_UNKNOWN
        : source->sourceKind;
}

std::string echoComparisonText(std::string_view input)
{
    const std::string normalized = MarkdownRenderer::normalizeUtf8(input);
    std::string result;
    result.reserve(normalized.size());
    for (const unsigned char value : normalized) {
        if (value >= 0x80u || std::isalnum(value) != 0) {
            result.push_back(
                value < 0x80u
                ? static_cast<char>(std::tolower(value))
                : static_cast<char>(value));
        }
    }
    return result;
}

float editSimilarity(std::string_view left, std::string_view right)
{
    if (left.empty() || right.empty()) {
        return 0.0F;
    }
    if (left == right) {
        return 1.0F;
    }
    if (left.size() > 1'024 || right.size() > 1'024) {
        return 0.0F;
    }

    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t index = 0; index <= right.size(); ++index) {
        previous[index] = index;
    }
    for (std::size_t leftIndex = 1;
         leftIndex <= left.size();
         ++leftIndex) {
        current[0] = leftIndex;
        for (std::size_t rightIndex = 1;
             rightIndex <= right.size();
             ++rightIndex) {
            const std::size_t substitution =
                left[leftIndex - 1] == right[rightIndex - 1] ? 0 : 1;
            current[rightIndex] = std::min(
                {
                    previous[rightIndex] + 1,
                    current[rightIndex - 1] + 1,
                    previous[rightIndex - 1] + substitution,
                });
        }
        std::swap(previous, current);
    }
    const float length =
        static_cast<float>(std::max(left.size(), right.size()));
    return 1.0F - static_cast<float>(previous.back()) / length;
}

bool timestampsCouldBeEcho(
    const TranscriptSegment &microphone,
    const TranscriptSegment &system)
{
    const auto distance = [](std::int64_t left, std::int64_t right) {
        return std::fabs(
            static_cast<long double>(left)
            - static_cast<long double>(right));
    };
    if (distance(microphone.startTimeNs, system.startTimeNs)
            <= 2'500'000'000.0L
        && distance(microphone.endTimeNs, system.endTimeNs)
            <= 3'500'000'000.0L) {
        return true;
    }

    const auto overlapStart =
        std::max(microphone.startTimeNs, system.startTimeNs);
    const auto overlapEnd =
        std::min(microphone.endTimeNs, system.endTimeNs);
    if (overlapEnd <= overlapStart) {
        return false;
    }
    const long double microphoneDuration = std::max(
        static_cast<long double>(microphone.endTimeNs)
            - static_cast<long double>(microphone.startTimeNs),
        1.0L);
    const long double systemDuration = std::max(
        static_cast<long double>(system.endTimeNs)
            - static_cast<long double>(system.startTimeNs),
        1.0L);
    return static_cast<long double>(overlapEnd)
            - static_cast<long double>(overlapStart)
        >= std::min(microphoneDuration, systemDuration) * 0.55L;
}

bool isMicrophoneEcho(
    const TranscriptSegment &microphone,
    const TranscriptSegment &system)
{
    if (!timestampsCouldBeEcho(microphone, system)) {
        return false;
    }
    const std::string microphoneText =
        echoComparisonText(microphone.text);
    const std::string systemText = echoComparisonText(system.text);
    if (microphoneText.size() < 8 || systemText.size() < 8) {
        return false;
    }
    const float similarity =
        editSimilarity(microphoneText, systemText);
    if (similarity >= 0.76F) {
        return true;
    }
    const auto &shorter = microphoneText.size() < systemText.size()
        ? microphoneText
        : systemText;
    const auto &longer = microphoneText.size() < systemText.size()
        ? systemText
        : microphoneText;
    return shorter.size() >= 12
        && static_cast<float>(shorter.size())
                / static_cast<float>(longer.size())
            >= 0.28F
        && longer.find(shorter) != std::string::npos;
}

TranscriptSegment mergeSegments(
    const std::vector<const TranscriptSegment *> &segments,
    std::size_t begin,
    std::size_t end)
{
    TranscriptSegment merged = *segments[begin];
    for (std::size_t index = begin + 1; index <= end; ++index) {
        merged.startTimeNs =
            std::min(merged.startTimeNs, segments[index]->startTimeNs);
        merged.endTimeNs =
            std::max(merged.endTimeNs, segments[index]->endTimeNs);
        if (!merged.text.empty() && !segments[index]->text.empty()) {
            merged.text.push_back(' ');
        }
        merged.text += segments[index]->text;
    }
    return merged;
}

bool canExtendEchoGroup(
    const TranscriptSegment &current,
    const TranscriptSegment &next)
{
    constexpr std::int64_t kMaximumFragmentGapNs = 2'500'000'000;
    return next.startTimeNs <= current.endTimeNs
        || next.startTimeNs - current.endTimeNs
            <= kMaximumFragmentGapNs;
}

std::set<const TranscriptSegment *> microphoneEchoes(
    std::vector<const TranscriptSegment *> microphoneSegments,
    std::vector<const TranscriptSegment *> systemSegments)
{
    const auto chronological = [](const auto *left, const auto *right) {
        if (left->startTimeNs != right->startTimeNs) {
            return left->startTimeNs < right->startTimeNs;
        }
        return left->endTimeNs < right->endTimeNs;
    };
    std::sort(
        microphoneSegments.begin(),
        microphoneSegments.end(),
        chronological);
    std::sort(
        systemSegments.begin(),
        systemSegments.end(),
        chronological);

    /*
     * The same acoustic turn is commonly split at different word boundaries
     * in the direct system stream and in its loudspeaker echo. Compare small
     * adjacent groups on both sides so 1:2, 2:1, and more fragmented matches
     * are still recognized.
     */
    constexpr std::size_t kMaximumGroupSegments = 4;
    constexpr std::int64_t kCandidateMarginNs = 4'000'000'000;
    std::set<const TranscriptSegment *> result;
    for (std::size_t microphoneBegin = 0;
         microphoneBegin < microphoneSegments.size();
         ++microphoneBegin) {
        TranscriptSegment microphone =
            *microphoneSegments[microphoneBegin];
        for (std::size_t microphoneEnd = microphoneBegin;
             microphoneEnd < microphoneSegments.size()
                 && microphoneEnd
                        < microphoneBegin + kMaximumGroupSegments;
             ++microphoneEnd) {
            if (microphoneEnd != microphoneBegin) {
                if (!canExtendEchoGroup(
                        microphone,
                        *microphoneSegments[microphoneEnd])) {
                    break;
                }
                microphone = mergeSegments(
                    microphoneSegments,
                    microphoneBegin,
                    microphoneEnd);
            }

            bool matched = false;
            for (std::size_t systemBegin = 0;
                 systemBegin < systemSegments.size() && !matched;
                 ++systemBegin) {
                const auto *firstSystem = systemSegments[systemBegin];
                if (firstSystem->endTimeNs + kCandidateMarginNs
                    < microphone.startTimeNs) {
                    continue;
                }
                if (firstSystem->startTimeNs
                    > microphone.endTimeNs + kCandidateMarginNs) {
                    break;
                }

                TranscriptSegment system = *firstSystem;
                for (std::size_t systemEnd = systemBegin;
                     systemEnd < systemSegments.size()
                         && systemEnd
                                < systemBegin + kMaximumGroupSegments;
                     ++systemEnd) {
                    if (systemEnd != systemBegin) {
                        if (!canExtendEchoGroup(
                                system,
                                *systemSegments[systemEnd])) {
                            break;
                        }
                        system = mergeSegments(
                            systemSegments,
                            systemBegin,
                            systemEnd);
                    }
                    if (isMicrophoneEcho(microphone, system)) {
                        matched = true;
                        for (std::size_t index = microphoneBegin;
                             index <= microphoneEnd;
                             ++index) {
                            result.insert(microphoneSegments[index]);
                        }
                        break;
                    }
                }
            }
            if (matched) {
                break;
            }
        }
    }
    return result;
}

std::vector<const TranscriptSegment *> visibleSegments(
    const JournalSnapshot &snapshot)
{
    if (snapshot.session.asrBackendId != "whisper.cpp") {
        std::vector<const TranscriptSegment *> allFinals;
        allFinals.reserve(snapshot.segments.size());
        for (const auto &segment : snapshot.segments) {
            if ((segment.flags & LS_SEGMENT_FLAG_FINAL) != 0) {
                allFinals.push_back(&segment);
            }
        }
        return allFinals;
    }

    std::vector<const TranscriptSegment *> microphoneSegments;
    std::vector<const TranscriptSegment *> systemSegments;
    for (const auto &segment : snapshot.segments) {
        if ((segment.flags & LS_SEGMENT_FLAG_FINAL) == 0) {
            continue;
        }
        const auto kind = sourceKind(snapshot, segment.sourceId);
        if (kind == LS_SOURCE_KIND_MICROPHONE) {
            microphoneSegments.push_back(&segment);
        } else if (kind == LS_SOURCE_KIND_SYSTEM_AUDIO) {
            systemSegments.push_back(&segment);
        }
    }
    const auto echoes = microphoneEchoes(
        std::move(microphoneSegments),
        std::move(systemSegments));

    std::vector<const TranscriptSegment *> result;
    result.reserve(snapshot.segments.size());
    for (const auto &segment : snapshot.segments) {
        if ((segment.flags & LS_SEGMENT_FLAG_FINAL) == 0) {
            continue;
        }
        if (!echoes.contains(&segment)) {
            result.push_back(&segment);
        }
    }
    return result;
}

std::string sourceName(ls_source_kind_t kind)
{
    switch (kind) {
    case LS_SOURCE_KIND_MICROPHONE:
        return "microphone";
    case LS_SOURCE_KIND_SYSTEM_AUDIO:
        return "system audio";
    default:
        return "audio source";
    }
}

std::string sourceEventName(ls_source_event_kind_t kind)
{
    switch (kind) {
    case LS_SOURCE_EVENT_UNAVAILABLE:
        return "temporarily unavailable";
    case LS_SOURCE_EVENT_RECOVERED:
        return "recovered";
    case LS_SOURCE_EVENT_PERMANENTLY_LOST:
        return "permanently lost";
    case LS_SOURCE_EVENT_DISCONTINUITY:
        return "discontinuity";
    case LS_SOURCE_EVENT_READY:
        return "ready";
    case LS_SOURCE_EVENT_ACTIVE:
        return "active";
    default:
        return "state changed";
    }
}

} // namespace

std::string MarkdownRenderer::normalizeUtf8(std::string_view input)
{
    std::string output;
    output.reserve(input.size());
    std::size_t index = 0;
    if (input.size() >= 3
        && static_cast<unsigned char>(input[0]) == 0xEFu
        && static_cast<unsigned char>(input[1]) == 0xBBu
        && static_cast<unsigned char>(input[2]) == 0xBFu) {
        index = 3;
    }

    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first < 0x80u) {
            if (first == '\r') {
                output.push_back('\n');
                if (index + 1 < input.size() && input[index + 1] == '\n') {
                    ++index;
                }
            } else if (first == '\n') {
                output.push_back('\n');
            } else if (first == '\t') {
                output.push_back(' ');
            } else if (first >= 0x20u && first != 0x7Fu) {
                output.push_back(static_cast<char>(first));
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
            appendReplacement(output);
            ++index;
            continue;
        }

        if (index + length > input.size()) {
            appendReplacement(output);
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto byte =
                static_cast<unsigned char>(input[index + offset]);
            if (!continuation(byte)) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6u) | (byte & 0x3Fu);
        }
        if (!valid || codePoint < minimum || codePoint > 0x10FFFFu
            || (codePoint >= 0xD800u && codePoint <= 0xDFFFu)) {
            appendReplacement(output);
            ++index;
            continue;
        }
        output.append(input.substr(index, length));
        index += length;
    }
    return output;
}

std::string MarkdownRenderer::yamlScalar(std::string_view input)
{
    std::string normalized = normalizeUtf8(input);
    replaceAll(normalized, kStartMarker, kNeutralStart);
    replaceAll(normalized, kEndMarker, kNeutralEnd);
    std::string output{"\""};
    output.reserve(normalized.size() + 2);
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        const unsigned char value =
            static_cast<unsigned char>(normalized[index]);
        switch (value) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        default:
            if (index + 2 < normalized.size() && value == 0xE2u
                && static_cast<unsigned char>(normalized[index + 1]) == 0x80u
                && (static_cast<unsigned char>(normalized[index + 2]) == 0xA8u
                    || static_cast<unsigned char>(
                           normalized[index + 2])
                        == 0xA9u)) {
                output +=
                    static_cast<unsigned char>(normalized[index + 2]) == 0xA8u
                    ? "\\u2028"
                    : "\\u2029";
                index += 2;
            } else {
                output.push_back(static_cast<char>(value));
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::string MarkdownRenderer::markdownInline(std::string_view input)
{
    std::string normalized = normalizeUtf8(input);
    replaceAll(normalized, kStartMarker, kNeutralStart);
    replaceAll(normalized, kEndMarker, kNeutralEnd);
    replaceAll(normalized, "&", "&amp;");
    replaceAll(normalized, "<", "&lt;");
    replaceAll(normalized, ">", "&gt;");

    std::string output;
    output.reserve(normalized.size() * 2);
    static constexpr std::string_view kMarkdownPunctuation =
        "\\`*_{}[]()#+-.!|";
    for (char value : normalized) {
        if (value == '\n') {
            output.push_back(' ');
        } else {
            if (kMarkdownPunctuation.find(value) != std::string_view::npos) {
                output.push_back('\\');
            }
            output.push_back(value);
        }
    }
    return output;
}

std::string MarkdownRenderer::transcriptInline(std::string_view input)
{
    std::string normalized = normalizeUtf8(input);
    replaceAll(normalized, "\xC2\x85", "\n");
    replaceAll(normalized, "\xE2\x80\xA8", "\n");
    replaceAll(normalized, "\xE2\x80\xA9", "\n");

    std::string output;
    output.reserve(normalized.size() * 2);
    static constexpr std::string_view kLiteralInlinePunctuation =
        "\\`*_{}[]!~=$#|%";
    bool pendingSpace = false;
    for (char value : normalized) {
        if (value == ' ' || value == '\n') {
            pendingSpace = !output.empty();
            continue;
        }
        if (pendingSpace) {
            output.push_back(' ');
            pendingSpace = false;
        }
        switch (value) {
        case '&':
            output += "&amp;";
            break;
        case '<':
            output += "&lt;";
            break;
        case '>':
            output += "&gt;";
            break;
        default:
            if (kLiteralInlinePunctuation.find(value)
                != std::string_view::npos) {
                output.push_back('\\');
            }
            output.push_back(value);
            break;
        }
    }
    return output;
}

Expected<std::string> MarkdownRenderer::render(
    const JournalSnapshot &snapshot,
    const MarkdownRenderOptions &options)
{
    const std::string status = phaseStatus(snapshot.session.phase);
    if (status.empty()) {
        return Error{
            LS_INVALID_STATE,
            "session phase is not publishable as Markdown"};
    }

    const std::string created =
        options.createdAt.empty() ? snapshot.session.createdAt
                                  : options.createdAt;
    const std::string ended =
        options.endedAt.empty() ? snapshot.session.endedAt : options.endedAt;
    const auto segments = visibleSegments(snapshot);

    bool microphoneCaptured = options.microphoneCaptured;
    bool systemCaptured = options.systemAudioCaptured;
    for (const auto &source : snapshot.sources) {
        if (isMicrophone(source) && source.acceptedFrames > 0) {
            microphoneCaptured = true;
        }
        if (isSystemAudio(source) && source.acceptedFrames > 0) {
            systemCaptured = true;
        }
    }

    std::vector<std::string> languages;
    std::vector<std::string> participants;
    std::set<std::string> seenLanguages;
    std::set<std::string> seenParticipants;
    std::unordered_map<std::uint64_t, std::string>
        previousLanguageBySource;
    for (const auto *segment : segments) {
        const auto previous =
            previousLanguageBySource.find(segment->sourceId);
        const std::string language =
            TranscriptLanguagePolicy::select(
                snapshot.session.languageMode,
                segment->text,
                segment->language,
                previous == previousLanguageBySource.end()
                    ? std::string_view{}
                    : std::string_view(previous->second));
        previousLanguageBySource[segment->sourceId] = language;
        if (!language.empty() && language != "und"
            && seenLanguages.insert(language).second) {
            languages.push_back(language);
        }
        const std::string speaker = normalizeUtf8(segment->speakerLabel);
        if (!speaker.empty() && seenParticipants.insert(speaker).second) {
            participants.push_back(speaker);
        }
    }

    std::int64_t duration = options.durationSeconds;
    if (duration < 0) {
        duration = 0;
        if (!segments.empty()) {
            const auto end = std::max_element(
                segments.begin(),
                segments.end(),
                [](const auto &left, const auto &right) {
                    return left->endTimeNs < right->endTimeNs;
                });
            const auto origin = snapshot.session.timelineOriginNs;
            if ((*end)->endTimeNs > origin && origin != 0) {
                duration =
                    ((*end)->endTimeNs - origin) / 1'000'000'000;
            }
        }
    }

    std::ostringstream output;
    output << "---\n";
    output << "type: " << yamlScalar("call-transcript") << '\n';
    output << "schema_version: 2\n";
    output << "status: " << yamlScalar(status) << '\n';
    output << "session_id: " << yamlScalar(snapshot.session.sessionId) << '\n';
    if (!created.empty()) {
        output << "created: " << yamlScalar(created) << '\n';
    }
    if (!ended.empty()) {
        output << "ended: " << yamlScalar(ended) << '\n';
    }
    output << "duration_seconds: " << std::max<std::int64_t>(duration, 0)
           << '\n';
    if (!snapshot.session.sourceApp.empty()) {
        output << "source_app: " << yamlScalar(snapshot.session.sourceApp)
               << '\n';
    }
    output << "capture:\n";
    output << "  microphone: "
           << (microphoneCaptured ? "true" : "false") << '\n';
    output << "  system_audio: "
           << (systemCaptured ? "true" : "false") << '\n';
    if (!languages.empty()) {
        output << "languages:\n";
        for (const auto &language : languages) {
            output << "  - " << yamlScalar(language) << '\n';
        }
    }
    if (!participants.empty()) {
        output << "participants:\n";
        for (const auto &participant : participants) {
            output << "  - " << yamlScalar(participant) << '\n';
        }
    }
    output << "tags:\n";
    output << "  - " << yamlScalar("transcript") << '\n';
    output << "  - " << yamlScalar("call") << '\n';
    output << "---\n\n";

    const std::string title =
        options.title.empty() ? "Call" : options.title;
    output << "# " << markdownInline(title) << "\n\n";
    output << kStartMarker << "\n\n";

    std::int64_t origin = snapshot.session.timelineOriginNs;
    if (origin == 0 && !segments.empty()) {
        origin = segments.front()->startTimeNs;
    }
    for (const auto *segment : segments) {
        output << "**" << relativeTime(segment->startTimeNs, origin)
               << " — " << markdownInline(segment->speakerLabel) << " :**";
        const std::string text = transcriptInline(segment->text);
        if (!text.empty()) {
            output << ' ' << text;
        }
        output << '\n';
    }
    if (!segments.empty()) {
        output << '\n';
    }
    output << kEndMarker << "\n\n";
    output << kCaptureEventsStartMarker << '\n';

    if (!snapshot.gaps.empty()) {
        output << "\n## Capture events\n\n";
        for (const auto &gap : snapshot.gaps) {
            output << "- " << relativeTime(gap.startTimeNs, origin) << " — "
                   << sourceName(gap.sourceKind) << ": "
                   << sourceEventName(gap.eventKind) << ".\n";
        }
        output << '\n';
    }
    output << kCaptureEventsEndMarker << '\n';
    return output.str();
}

std::string
MarkdownRenderer::normalizeFileName(std::string_view requestedName)
{
    std::string value = normalizeUtf8(requestedName);
    std::string sanitized;
    sanitized.reserve(value.size());
    bool previousSpace = false;
    for (unsigned char byte : value) {
        if (byte == '/' || byte == '\\' || byte == ':' || byte < 0x20u
            || byte == 0x7Fu) {
            if (!previousSpace) {
                sanitized.push_back(' ');
                previousSpace = true;
            }
        } else {
            sanitized.push_back(static_cast<char>(byte));
            previousSpace = byte == ' ';
        }
    }
    while (!sanitized.empty()
           && (sanitized.front() == ' ' || sanitized.front() == '.')) {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty()
           && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.pop_back();
    }
    if (sanitized.empty() || sanitized == "." || sanitized == "..") {
        sanitized = "Call";
    }

    constexpr std::size_t kMaxBaseBytes = 116;
    if (sanitized.size() > kMaxBaseBytes) {
        std::size_t length = kMaxBaseBytes;
        while (length > 0
               && (static_cast<unsigned char>(sanitized[length]) & 0xC0u)
                   == 0x80u) {
            --length;
        }
        sanitized.resize(length);
        while (!sanitized.empty() && sanitized.back() == ' ') {
            sanitized.pop_back();
        }
    }
    const bool hasMarkdownExtension =
        sanitized.size() >= 3
        && sanitized.substr(sanitized.size() - 3) == ".md";
    if (!hasMarkdownExtension) {
        sanitized += ".md";
    }
    return sanitized;
}

} // namespace localscribe
