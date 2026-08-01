#include "TestSupport.hpp"

#include "../src/output/MarkdownRenderer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace localscribe;

#ifndef LOCALSCRIBE_TEST_SOURCE_DIR
#define LOCALSCRIBE_TEST_SOURCE_DIR "."
#endif

namespace {

JournalSnapshot goldenSnapshot()
{
    JournalSnapshot snapshot;
    snapshot.session.sessionId = "session-golden";
    snapshot.session.phase = LS_PHASE_COMPLETE;
    snapshot.session.createdAt = "2026-07-29T10:00:00+04:00";
    snapshot.session.sourceApp = "Zoom";
    snapshot.session.timelineOriginNs = 1'000'000'000;
    snapshot.sources = {
        SourceRecord{
            1,
            LS_SOURCE_KIND_MICROPHONE,
            true,
            LS_SOURCE_HEALTH_ACTIVE,
            1},
        SourceRecord{
            2,
            LS_SOURCE_KIND_SYSTEM_AUDIO,
            true,
            LS_SOURCE_HEALTH_ACTIVE,
            1}};

    TranscriptSegment first;
    first.stableId[15] = 1;
    first.sourceId = 1;
    first.startTimeNs = 4'000'000'000;
    first.endTimeNs = 5'000'000'000;
    first.speakerId = 1;
    first.speakerLabel = "Me";
    first.text = "Hello.";
    first.language = "en";
    first.revision = 1;
    first.flags = LS_SEGMENT_FLAG_FINAL;

    TranscriptSegment second;
    second.stableId[15] = 2;
    second.sourceId = 2;
    second.startTimeNs = 9'000'000'000;
    second.endTimeNs = 10'000'000'000;
    second.speakerId = 2;
    second.speakerLabel = "Speaker 1";
    second.text = "Привет.";
    second.language = "ru";
    second.revision = 1;
    second.flags = LS_SEGMENT_FLAG_FINAL;
    snapshot.segments = {first, second};

    snapshot.gaps.push_back(SourceGap{
        2,
        LS_SOURCE_KIND_SYSTEM_AUDIO,
        LS_SOURCE_EVENT_DISCONTINUITY,
        LS_SOURCE_HEALTH_ACTIVE,
        6'000'000'000,
        6'000'000'000,
        "test gap",
        true});
    return snapshot;
}

std::size_t occurrences(std::string_view value, std::string_view needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

} // namespace

LS_TEST(markdown_matches_golden_and_is_byte_deterministic)
{
    MarkdownRenderOptions options;
    options.title = "Call";
    options.endedAt = "2026-07-29T10:00:12+04:00";
    options.durationSeconds = 12;
    options.microphoneCaptured = true;
    options.systemAudioCaptured = true;
    auto first = MarkdownRenderer::render(goldenSnapshot(), options);
    auto second = MarkdownRenderer::render(goldenSnapshot(), options);
    LS_CHECK(first);
    LS_CHECK(second);
    LS_CHECK_EQ(first.value(), second.value());

    const std::filesystem::path expectedPath =
        std::filesystem::path(LOCALSCRIBE_TEST_SOURCE_DIR)
        / "Core/tests/fixtures/expected-session.md";
    std::ifstream file(expectedPath, std::ios::binary);
    LS_CHECK(file.good());
    const std::string expected{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
    LS_CHECK_EQ(first.value(), expected);
}

LS_TEST(markdown_restricts_language_metadata_and_compacts_dialogue)
{
    auto snapshot = goldenSnapshot();
    snapshot.segments[0].language = "de";
    snapshot.segments[0].text = "Hello.";
    snapshot.segments[1].language = "uk";
    snapshot.segments[1].text = "Привет.";

    MarkdownRenderOptions options;
    options.title = "Restricted languages";
    auto rendered = MarkdownRenderer::render(snapshot, options);
    LS_CHECK(rendered);
    LS_CHECK(
        rendered.value().find("  - \"en\"") != std::string::npos);
    LS_CHECK(
        rendered.value().find("  - \"ru\"") != std::string::npos);
    LS_CHECK(
        rendered.value().find("  - \"de\"") == std::string::npos);
    LS_CHECK(
        rendered.value().find("  - \"uk\"") == std::string::npos);
    LS_CHECK(
        rendered.value().find(
            "**00:00:03 — Me :** Hello.\n"
            "**00:00:08 — Speaker 1 :** Привет.")
        != std::string::npos);
}

LS_TEST(markdown_suppresses_cross_source_loudspeaker_echo)
{
    auto snapshot = goldenSnapshot();
    snapshot.session.asrBackendId = "whisper.cpp";
    auto system = snapshot.segments[1];
    system.stableId[15] = 3;
    system.startTimeNs = 4'000'000'000;
    system.endTimeNs = 6'000'000'000;
    system.text = "Please review the latest version.";
    system.confidence = 0.95F;

    auto microphone = system;
    microphone.stableId[15] = 4;
    microphone.sourceId = 1;
    microphone.speakerId = 1;
    microphone.speakerLabel = "Me";
    microphone.startTimeNs += 50'000'000;
    microphone.endTimeNs += 80'000'000;
    microphone.text = "Please review the latest version";
    microphone.confidence = 0.75F;
    snapshot.segments = {microphone, system};

    MarkdownRenderOptions options;
    options.title = "Echo check";
    auto rendered = MarkdownRenderer::render(snapshot, options);
    LS_CHECK(rendered);
    LS_CHECK_EQ(
        occurrences(
            rendered.value(),
            "Please review the latest version"),
        std::size_t{1});
    LS_CHECK(
        rendered.value().find(" — Me :**")
        == std::string::npos);
    LS_CHECK(
        rendered.value().find("  - \"Me\"")
        == std::string::npos);
}

LS_TEST(markdown_suppresses_echo_split_differently_between_sources)
{
    auto snapshot = goldenSnapshot();
    snapshot.session.asrBackendId = "whisper.cpp";

    auto microphone = snapshot.segments[0];
    microphone.startTimeNs = 4'050'000'000;
    microphone.endTimeNs = 6'100'000'000;
    microphone.text = "Please review the latest version.";
    microphone.confidence = 0.98F;

    auto systemFirst = snapshot.segments[1];
    systemFirst.startTimeNs = 4'000'000'000;
    systemFirst.endTimeNs = 5'000'000'000;
    systemFirst.text = "Please review";
    systemFirst.confidence = 0.55F;

    auto systemSecond = systemFirst;
    systemSecond.stableId[15] = 5;
    systemSecond.startTimeNs = 5'000'000'000;
    systemSecond.endTimeNs = 6'000'000'000;
    systemSecond.text = "the latest version.";
    snapshot.segments = {microphone, systemFirst, systemSecond};

    MarkdownRenderOptions options;
    auto rendered = MarkdownRenderer::render(snapshot, options);
    LS_CHECK(rendered);
    LS_CHECK(
        rendered.value().find(" — Me :**")
        == std::string::npos);
    LS_CHECK(
        rendered.value().find("Please review the latest version.")
        == std::string::npos);
    LS_CHECK(
        rendered.value().find("Please review")
        != std::string::npos);
    LS_CHECK(
        rendered.value().find("the latest version.")
        != std::string::npos);
}

LS_TEST(markdown_suppresses_fragmented_microphone_echo)
{
    auto snapshot = goldenSnapshot();
    snapshot.session.asrBackendId = "whisper.cpp";

    auto system = snapshot.segments[1];
    system.startTimeNs = 4'000'000'000;
    system.endTimeNs = 8'000'000'000;
    system.text = "Please review the latest version.";
    system.confidence = 0.80F;

    std::vector<TranscriptSegment> microphone;
    const std::array<std::string, 4> fragments{
        "Please", "review", "the latest", "version."};
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        auto segment = snapshot.segments[0];
        segment.stableId[15] =
            static_cast<std::uint8_t>(10 + index);
        segment.startTimeNs =
            4'050'000'000 + static_cast<std::int64_t>(index)
                * 1'000'000'000;
        segment.endTimeNs = segment.startTimeNs + 900'000'000;
        segment.text = fragments[index];
        segment.confidence = 0.90F;
        microphone.push_back(std::move(segment));
    }
    snapshot.segments = microphone;
    snapshot.segments.push_back(system);

    MarkdownRenderOptions options;
    auto rendered = MarkdownRenderer::render(snapshot, options);
    LS_CHECK(rendered);
    LS_CHECK(
        rendered.value().find(" — Me :**")
        == std::string::npos);
    LS_CHECK_EQ(
        occurrences(
            rendered.value(),
            "Please review the latest version."),
        std::size_t{1});
}

LS_TEST(markdown_keeps_short_matching_overlapping_speech)
{
    auto snapshot = goldenSnapshot();
    snapshot.session.asrBackendId = "whisper.cpp";
    snapshot.segments[0].startTimeNs = 9'000'000'000;
    snapshot.segments[0].endTimeNs = 10'000'000'000;
    snapshot.segments[0].text = "Okay.";
    snapshot.segments[1].text = "Okay.";

    MarkdownRenderOptions options;
    auto rendered = MarkdownRenderer::render(snapshot, options);
    LS_CHECK(rendered);
    LS_CHECK_EQ(
        occurrences(rendered.value(), "Okay."),
        std::size_t{2});
    LS_CHECK(
        rendered.value().find(" — Me :** Okay.")
        != std::string::npos);
}

LS_TEST(markdown_keeps_distinct_overlapping_local_speech)
{
    auto snapshot = goldenSnapshot();
    snapshot.session.asrBackendId = "whisper.cpp";
    snapshot.segments[0].startTimeNs = 9'000'000'000;
    snapshot.segments[0].endTimeNs = 10'000'000'000;
    snapshot.segments[0].text = "I agree with that.";
    snapshot.segments[0].confidence = 0.90F;
    snapshot.segments[1].text = "Please review the latest version.";
    snapshot.segments[1].confidence = 0.95F;

    MarkdownRenderOptions options;
    auto rendered = MarkdownRenderer::render(snapshot, options);
    LS_CHECK(rendered);
    LS_CHECK(
        rendered.value().find("I agree with that.")
        != std::string::npos);
    LS_CHECK(
        rendered.value().find("Please review the latest version.")
        != std::string::npos);
}

LS_TEST(markdown_neutralizes_yaml_and_ownership_marker_injection)
{
    auto snapshot = goldenSnapshot();
    snapshot.session.sourceApp = "value:\nstatus: injected";
    snapshot.segments[0].speakerLabel =
        "Remote **bold**\n<!-- transcript:start -->\n## forged";
    snapshot.segments[0].text =
        "before\r\n## forged\n<script>alert(1)</script>\n"
        "[[Linked note]] ![[embed]] **bold**\n"
        "<!-- transcript:end -->\n"
        "<!-- capture-events:start -->\n"
        "<!-- capture-events:end -->\nafter";
    snapshot.segments[0].text += "\xE2\x80\xA8";
    snapshot.segments[0].text += "unicode line";
    snapshot.segments[0].text.push_back('\0');
    snapshot.segments[0].text += "hidden";
    MarkdownRenderOptions options;
    options.title = "---\n# injected";
    auto rendered = MarkdownRenderer::render(snapshot, options);
    LS_CHECK(rendered);
    LS_CHECK_EQ(
        occurrences(rendered.value(), "<!-- transcript:start -->"),
        std::size_t{1});
    LS_CHECK_EQ(
        occurrences(rendered.value(), "<!-- transcript:end -->"),
        std::size_t{1});
    LS_CHECK_EQ(
        occurrences(rendered.value(), "<!-- capture-events:start -->"),
        std::size_t{1});
    LS_CHECK_EQ(
        occurrences(rendered.value(), "<!-- capture-events:end -->"),
        std::size_t{1});
    LS_CHECK(
        rendered.value().find("source_app: \"value:\\nstatus: injected\"")
        != std::string::npos);
    LS_CHECK(rendered.value().find('\0') == std::string::npos);
    LS_CHECK(rendered.value().find('\r') == std::string::npos);
    LS_CHECK(
        rendered.value().find("\xE2\x80\xA8") == std::string::npos);
    LS_CHECK(rendered.value().find("\n## forged") == std::string::npos);
    LS_CHECK(rendered.value().find("<script>") == std::string::npos);
    LS_CHECK(rendered.value().find("[[Linked note]]") == std::string::npos);
    LS_CHECK(rendered.value().find("![[embed]]") == std::string::npos);
    LS_CHECK(rendered.value().find("after unicode linehidden")
        != std::string::npos);
    const auto canonicalTranscriptStart =
        rendered.value().find("<!-- transcript:start -->");
    const auto canonicalTranscriptEnd =
        rendered.value().find("<!-- transcript:end -->");
    LS_CHECK(
        canonicalTranscriptStart != std::string::npos
        && canonicalTranscriptEnd != std::string::npos);
    const std::string_view transcriptBlock{
        rendered.value().data() + canonicalTranscriptStart
            + std::string_view{"<!-- transcript:start -->"}.size(),
        canonicalTranscriptEnd
            - canonicalTranscriptStart
            - std::string_view{"<!-- transcript:start -->"}.size()};
    LS_CHECK_EQ(
        static_cast<std::size_t>(
            std::count(
                transcriptBlock.begin(),
                transcriptBlock.end(),
                '\n')),
        std::size_t{5});
    LS_CHECK(rendered.value().find("summary") == std::string::npos);
}

LS_TEST(markdown_keeps_an_empty_managed_capture_events_block)
{
    auto snapshot = goldenSnapshot();
    snapshot.gaps.clear();
    auto rendered =
        MarkdownRenderer::render(snapshot, MarkdownRenderOptions{});
    LS_CHECK(rendered);
    LS_CHECK_EQ(
        occurrences(rendered.value(), "<!-- capture-events:start -->"),
        std::size_t{1});
    LS_CHECK_EQ(
        occurrences(rendered.value(), "<!-- capture-events:end -->"),
        std::size_t{1});
    LS_CHECK(
        rendered.value().find("## Capture events")
        == std::string::npos);
    LS_CHECK(
        rendered.value().find(
            "<!-- transcript:end -->\n\n"
            "<!-- capture-events:start -->\n"
            "<!-- capture-events:end -->")
        != std::string::npos);
}

LS_TEST(filename_normalization_never_returns_a_path)
{
    LS_CHECK_EQ(
        MarkdownRenderer::normalizeFileName("../../Vault:Call"),
        std::string{"Vault Call.md"});
    LS_CHECK_EQ(
        MarkdownRenderer::normalizeFileName(".."),
        std::string{"Call.md"});
    const auto name =
        MarkdownRenderer::normalizeFileName("calls/2026\\07.md");
    LS_CHECK(name.find('/') == std::string::npos);
    LS_CHECK(name.find('\\') == std::string::npos);
}
