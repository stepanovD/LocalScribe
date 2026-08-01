#pragma once

#include "../common/Expected.hpp"
#include "../common/Types.hpp"

#include <span>
#include <string>
#include <vector>

namespace localscribe {

struct SpeakerEmbedding {
    std::string modelId;
    std::vector<float> values;
};

struct SpeakerMatch {
    std::uint64_t speakerId{};
    float confidence{};
};

/*
 * Stage 0 does not persist voice profiles, but this backend-neutral seam keeps
 * future profile implementations out of session, Swift, and SQLite types.
 */
class ISpeakerEmbeddingBackend {
public:
    virtual ~ISpeakerEmbeddingBackend() = default;
    [[nodiscard]] virtual BackendInfo info() const = 0;
    [[nodiscard]] virtual Expected<SpeakerEmbedding>
    embed(const AudioWindow &audio) = 0;
    [[nodiscard]] virtual Expected<SpeakerMatch>
    match(const SpeakerEmbedding &embedding) = 0;
};

} // namespace localscribe
