#pragma once

#include "../common/Expected.hpp"
#include "../common/Types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace localscribe {

struct MarkdownRenderOptions {
    std::string title;
    std::string createdAt;
    std::string endedAt;
    std::int64_t durationSeconds{-1};
    bool microphoneCaptured{};
    bool systemAudioCaptured{};
};

class MarkdownRenderer {
public:
    [[nodiscard]] static Expected<std::string>
    render(
        const JournalSnapshot &snapshot,
        const MarkdownRenderOptions &options);

    [[nodiscard]] static std::string
    normalizeFileName(std::string_view requestedName);

    [[nodiscard]] static std::string normalizeUtf8(std::string_view input);
    [[nodiscard]] static std::string yamlScalar(std::string_view input);

private:
    [[nodiscard]] static std::string
    markdownInline(std::string_view input);
    [[nodiscard]] static std::string
    transcriptInline(std::string_view input);
};

} // namespace localscribe
