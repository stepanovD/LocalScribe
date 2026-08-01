#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace localscribe {

/*
 * Conservative, source-isolated speech gate for complete Whisper chunks.
 *
 * It never trims samples: a chunk containing speech is passed through in full,
 * which preserves the leading and trailing context. Long digital silence,
 * low-level room tone, high-frequency hiss, and other clearly non-voice
 * buffers are rejected before the expensive model invocation.
 */
class WhisperSpeechGate {
public:
    [[nodiscard]] bool shouldTranscribe(
        std::uint64_t sourceId,
        std::span<const float> samples,
        std::uint32_t sampleRate)
    {
        if (samples.empty() || sampleRate == 0) {
            return false;
        }

        auto &state = states_[sourceId];
        const std::size_t frameSamples =
            std::max<std::size_t>(sampleRate / 50u, 1u);
        std::size_t activeFrames = 0;
        std::size_t examinedFrames = 0;

        for (std::size_t begin = 0; begin < samples.size();
             begin += frameSamples) {
            const std::size_t end =
                std::min(samples.size(), begin + frameSamples);
            if (end <= begin) {
                continue;
            }

            long double energy = 0.0L;
            float peak = 0.0F;
            std::size_t crossings = 0;
            float previous = samples[begin];
            for (std::size_t index = begin; index < end; ++index) {
                const float sample = samples[index];
                energy += static_cast<long double>(sample) * sample;
                peak = std::max(peak, std::fabs(sample));
                if (index != begin
                    && ((sample >= 0.0F) != (previous >= 0.0F))) {
                    ++crossings;
                }
                previous = sample;
            }

            const float rms = static_cast<float>(
                std::sqrt(energy / static_cast<long double>(end - begin)));
            const float crossingRate = static_cast<float>(crossings)
                / static_cast<float>(end - begin);
            const float threshold =
                std::max(0.003F, state.noiseFloor * 2.5F);
            const bool voiceBandActivity =
                crossingRate >= 0.004F && crossingRate <= 0.38F;
            const bool active =
                rms >= threshold && peak >= 0.006F && voiceBandActivity;
            if (active) {
                ++activeFrames;
            } else if (rms < threshold) {
                state.noiseFloor = std::clamp(
                    state.noiseFloor * 0.98F + rms * 0.02F,
                    0.0005F,
                    0.02F);
            }
            ++examinedFrames;
        }

        const std::size_t requiredFrames =
            std::min<std::size_t>(3u, examinedFrames);
        return requiredFrames != 0 && activeFrames >= requiredFrames;
    }

    void reset() { states_.clear(); }

private:
    struct State {
        float noiseFloor{0.0015F};
    };

    std::unordered_map<std::uint64_t, State> states_;
};

} // namespace localscribe
