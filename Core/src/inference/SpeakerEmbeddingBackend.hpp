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
 * Backend-neutral seam for learned speaker encoders. The built-in offline
 * fallback currently persists versioned descriptors through the journal;
 * future encoders can implement the same embed/match boundary without
 * exposing third-party types to the session runtime.
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
