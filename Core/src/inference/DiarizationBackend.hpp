#pragma once

#include "../common/Expected.hpp"
#include "../common/Types.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace localscribe {

struct DiarizationConfiguration {
    std::uint64_t microphoneSourceId{1};
    std::uint64_t systemAudioSourceId{2};
    std::string localSpeakerName{"Me"};
    std::string remoteSpeakerName{"Speaker 1"};
    std::vector<VoiceProfile> voiceProfiles;
};

class IDiarizationBackend {
public:
    virtual ~IDiarizationBackend() = default;
    [[nodiscard]] virtual BackendInfo info() const = 0;
    [[nodiscard]] virtual Expected<void>
    prepare(const DiarizationConfiguration &configuration) = 0;
    [[nodiscard]] virtual Expected<std::vector<SpeakerTurn>>
    assign(
        const AudioWindow &audio,
        std::span<const AsrHypothesis> hypotheses) = 0;
    [[nodiscard]] virtual Expected<std::vector<SpeakerTurn>> flush() = 0;
};

[[nodiscard]] Expected<std::unique_ptr<IDiarizationBackend>>
createDiarizationBackend(std::string_view backendId);

} // namespace localscribe
