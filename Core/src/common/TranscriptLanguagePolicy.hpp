#pragma once

#include <LocalScribeCore/LocalScribeCore.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace localscribe {

/*
 * LocalScribe deliberately exposes only the two product languages. Whisper
 * may auto-detect a neighboring or unrelated language on short/noisy chunks;
 * those labels are reduced deterministically from the transcript script.
 */
class TranscriptLanguagePolicy {
public:
    [[nodiscard]] static std::string select(
        ls_language_mode_t mode,
        std::string_view text,
        std::string_view detected,
        std::string_view previous = {})
    {
        if (mode == LS_LANGUAGE_MODE_RUSSIAN) {
            return "ru";
        }
        if (mode == LS_LANGUAGE_MODE_ENGLISH) {
            return "en";
        }
        if (detected == "ru" || detected == "en") {
            return std::string(detected);
        }

        const auto [cyrillic, latin] = scriptCounts(text);
        if (cyrillic > latin) {
            return "ru";
        }
        if (latin > cyrillic) {
            return "en";
        }
        if (previous == "ru" || previous == "en") {
            return std::string(previous);
        }
        return cyrillic != 0 ? "ru" : "en";
    }

private:
    struct Counts {
        std::size_t cyrillic{};
        std::size_t latin{};
    };

    [[nodiscard]] static Counts scriptCounts(std::string_view text)
    {
        Counts counts;
        for (std::size_t offset = 0; offset < text.size();) {
            const auto first =
                static_cast<std::uint8_t>(text[offset]);
            std::uint32_t codepoint = 0;
            std::size_t length = 1;
            if (first < 0x80u) {
                codepoint = first;
            } else if ((first & 0xE0u) == 0xC0u) {
                codepoint = first & 0x1Fu;
                length = 2;
            } else if ((first & 0xF0u) == 0xE0u) {
                codepoint = first & 0x0Fu;
                length = 3;
            } else if ((first & 0xF8u) == 0xF0u) {
                codepoint = first & 0x07u;
                length = 4;
            } else {
                ++offset;
                continue;
            }
            if (offset + length > text.size()) {
                break;
            }
            bool valid = true;
            for (std::size_t index = 1; index < length; ++index) {
                const auto continuation =
                    static_cast<std::uint8_t>(text[offset + index]);
                if ((continuation & 0xC0u) != 0x80u) {
                    valid = false;
                    break;
                }
                codepoint =
                    (codepoint << 6u) | (continuation & 0x3Fu);
            }
            if (!valid) {
                ++offset;
                continue;
            }
            if ((codepoint >= U'A' && codepoint <= U'Z')
                || (codepoint >= U'a' && codepoint <= U'z')
                || (codepoint >= 0x00C0u && codepoint <= 0x024Fu)) {
                ++counts.latin;
            } else if (
                (codepoint >= 0x0400u && codepoint <= 0x052Fu)
                || (codepoint >= 0x2DE0u && codepoint <= 0x2DFFu)
                || (codepoint >= 0xA640u && codepoint <= 0xA69Fu)) {
                ++counts.cyrillic;
            }
            offset += length;
        }
        return counts;
    }
};

} // namespace localscribe
