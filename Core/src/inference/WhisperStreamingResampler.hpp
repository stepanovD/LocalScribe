#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace localscribe {

struct WhisperResamplerInputLimits {
    static constexpr std::uint32_t minimumSampleRate = 8'000;
    static constexpr std::uint32_t maximumSampleRate = 96'000;
    static constexpr std::uint64_t maximumCallbackSeconds = 10;

    [[nodiscard]] static constexpr bool supportsRate(
        std::uint32_t inputRate) noexcept
    {
        return inputRate >= minimumSampleRate
            && inputRate <= maximumSampleRate;
    }

    [[nodiscard]] static constexpr std::uint64_t projectedOutputFrames(
        std::uint32_t inputRate,
        std::uint32_t inputFrames,
        std::uint32_t outputRate) noexcept
    {
        return inputRate == 0
            ? std::numeric_limits<std::uint64_t>::max()
            : (static_cast<std::uint64_t>(inputFrames) * outputRate
               + inputRate - 1u)
                / inputRate;
    }

    [[nodiscard]] static constexpr bool callbackFits(
        std::uint32_t inputRate,
        std::uint32_t inputFrames,
        std::uint32_t outputRate) noexcept
    {
        return supportsRate(inputRate) && outputRate != 0
            && projectedOutputFrames(
                   inputRate,
                   inputFrames,
                   outputRate)
                <= static_cast<std::uint64_t>(outputRate)
                    * maximumCallbackSeconds;
    }
};

/*
 * Exact rational sample-count clock shared by the streaming resampler and its
 * long-duration tests. The remainder is deliberately retained across input
 * buffers; resetting it per callback is the source of multi-second drift.
 */
class StreamingSampleRateClock {
public:
    StreamingSampleRateClock(
        std::uint32_t inputRate = 1,
        std::uint32_t outputRate = 1)
        : inputRate_(std::max(inputRate, 1u)),
          outputRate_(std::max(outputRate, 1u))
    {
    }

    [[nodiscard]] std::uint64_t advance(std::uint64_t inputFrames)
    {
        const std::uint64_t wholeSeconds = inputFrames / inputRate_;
        const std::uint64_t remainingFrames = inputFrames % inputRate_;
        const std::uint64_t wholeOutput =
            wholeSeconds * static_cast<std::uint64_t>(outputRate_);
        const std::uint64_t scaledRemainder =
            remainder_
            + remainingFrames * static_cast<std::uint64_t>(outputRate_);
        const std::uint64_t output =
            wholeOutput + scaledRemainder / inputRate_;
        remainder_ = scaledRemainder % inputRate_;
        return output;
    }

    [[nodiscard]] std::uint64_t advanceOne()
    {
        return advance(1);
    }

    /*
     * A finite stream is rounded to the nearest output sample. The error is
     * therefore bounded by half an output period rather than accumulating at
     * every callback.
     */
    [[nodiscard]] bool finishRounded()
    {
        const bool emit =
            remainder_ >= (static_cast<std::uint64_t>(inputRate_) + 1u) / 2u;
        remainder_ = 0;
        return emit;
    }

    [[nodiscard]] std::uint64_t remainder() const noexcept
    {
        return remainder_;
    }

private:
    std::uint32_t inputRate_{1};
    std::uint32_t outputRate_{1};
    std::uint64_t remainder_{};
};

struct ResampledAudioBlock {
    std::uint64_t sourceId{};
    std::int64_t startTimeNs{};
    std::vector<float> samples;
    bool discontinuityBefore{};
    bool endOfStream{};
    bool dropShortBeforeDiscontinuity{};
};

/*
 * Source-isolated streaming conversion to whisper.cpp's mono sample rate.
 *
 * Downsampling uses a ratio-scaled Blackman-windowed sinc low-pass before
 * phase-aware rational conversion. Filter history and the exact rate-clock
 * remainder survive input callback boundaries. A discontinuity finishes the
 * old finite stream before resetting both history and phase, so no pre-gap
 * sample bleeds into post-gap audio. EOS and flush round only the one final
 * fractional output sample.
 */
class WhisperStreamingResampler {
public:
    explicit WhisperStreamingResampler(std::uint32_t outputRate)
        : outputRate_(std::max(outputRate, 1u))
    {
    }

    [[nodiscard]] std::vector<ResampledAudioBlock> accept(
        std::uint64_t sourceId,
        std::int64_t startTimeNs,
        std::uint32_t inputRate,
        std::span<const float> monoSamples,
        bool discontinuity,
        bool endOfStream,
        bool dropShortBeforeDiscontinuity = false)
    {
        inputRate = std::max(inputRate, 1u);
        std::vector<ResampledAudioBlock> blocks;
        auto found = states_.find(sourceId);
        const bool rateChanged =
            found != states_.end() && found->second.inputRate != inputRate;
        const bool boundary = discontinuity || rateChanged;

        if (found != states_.end() && boundary) {
            auto tail = finishBlock(sourceId, found->second, false);
            if (!tail.samples.empty()) {
                blocks.push_back(std::move(tail));
            }
            states_.erase(found);
            found = states_.end();
        }

        if (found == states_.end()) {
            found = states_.emplace(
                sourceId,
                makeState(inputRate, startTimeNs)).first;
        }
        auto &state = found->second;

        ResampledAudioBlock current;
        current.sourceId = sourceId;
        current.startTimeNs = outputTime(state);
        current.discontinuityBefore = boundary;
        current.endOfStream = endOfStream;
        current.dropShortBeforeDiscontinuity =
            boundary && dropShortBeforeDiscontinuity;
        current.samples.reserve(
            static_cast<std::size_t>(
                monoSamples.size()
                    * static_cast<std::uint64_t>(outputRate_)
                    / inputRate
                + 2u));

        for (const float sample : monoSamples) {
            const float filtered = filter(state, sample);
            const auto previousRemainder = state.clock.remainder();
            const auto produced = state.clock.advanceOne();
            for (std::uint64_t index = 0; index < produced; ++index) {
                float converted = filtered;
                if (state.hasPreviousFiltered) {
                    const double numerator =
                        static_cast<double>(state.inputRate)
                            * static_cast<double>(index + 1u)
                        - static_cast<double>(previousRemainder);
                    const double fraction = std::clamp(
                        numerator / static_cast<double>(outputRate_),
                        0.0,
                        1.0);
                    converted = static_cast<float>(
                        static_cast<double>(state.previousFiltered)
                            * (1.0 - fraction)
                        + static_cast<double>(filtered) * fraction);
                }
                current.samples.push_back(converted);
            }
            state.previousFiltered = filtered;
            state.hasPreviousFiltered = true;
            state.emittedSamples += produced;
        }

        if (endOfStream) {
            appendRoundedTail(state, current.samples);
        }
        if (!current.samples.empty() || boundary || endOfStream) {
            blocks.push_back(std::move(current));
        }
        if (endOfStream) {
            states_.erase(sourceId);
        }
        return blocks;
    }

    [[nodiscard]] std::vector<ResampledAudioBlock> flush()
    {
        std::vector<std::uint64_t> sourceIds;
        sourceIds.reserve(states_.size());
        for (const auto &[sourceId, state] : states_) {
            (void)state;
            sourceIds.push_back(sourceId);
        }
        std::sort(sourceIds.begin(), sourceIds.end());

        std::vector<ResampledAudioBlock> blocks;
        blocks.reserve(sourceIds.size());
        for (const auto sourceId : sourceIds) {
            blocks.push_back(
                finishBlock(sourceId, states_.at(sourceId), true));
        }
        states_.clear();
        return blocks;
    }

    void reset() { states_.clear(); }

private:
    static constexpr std::size_t kMinimumFilterTaps = 63;
    static constexpr double kTapsPerDownsampleRatio = 32.0;
    static constexpr double kPi = 3.14159265358979323846264338327950288;

    struct State {
        std::uint32_t inputRate{1};
        StreamingSampleRateClock clock;
        std::int64_t streamStartTimeNs{};
        std::uint64_t emittedSamples{};
        std::vector<float> coefficients;
        std::vector<float> history;
        std::size_t newestHistoryIndex{};
        float lastFiltered{};
        float previousFiltered{};
        bool historyPrimed{};
        bool hasPreviousFiltered{};
        bool hasInput{};
    };

    [[nodiscard]] State makeState(
        std::uint32_t inputRate,
        std::int64_t startTimeNs) const
    {
        State state;
        state.inputRate = inputRate;
        state.clock = StreamingSampleRateClock(inputRate, outputRate_);
        state.streamStartTimeNs = startTimeNs;
        if (inputRate > outputRate_) {
            state.coefficients = lowPassCoefficients(inputRate);
            state.history.resize(state.coefficients.size());
        }
        return state;
    }

    [[nodiscard]] std::vector<float>
    lowPassCoefficients(std::uint32_t inputRate) const
    {
        /*
         * Preserve most of whisper's 0-8 kHz band while leaving transition
         * room below the new Nyquist limit.
         */
        const double cutoff =
            0.45 * static_cast<double>(outputRate_)
            / static_cast<double>(inputRate);
        std::size_t filterTaps = std::max<std::size_t>(
            kMinimumFilterTaps,
            static_cast<std::size_t>(
                std::ceil(
                    kTapsPerDownsampleRatio
                    * static_cast<double>(inputRate)
                    / static_cast<double>(outputRate_))));
        if (filterTaps % 2u == 0) {
            ++filterTaps;
        }
        std::vector<float> coefficients(filterTaps);
        const double center =
            static_cast<double>(filterTaps - 1u) / 2.0;
        double sum = 0.0;
        for (std::size_t index = 0; index < filterTaps; ++index) {
            const double offset = static_cast<double>(index) - center;
            const double sinc =
                offset == 0.0
                ? 2.0 * cutoff
                : std::sin(2.0 * kPi * cutoff * offset)
                    / (kPi * offset);
            const double position =
                static_cast<double>(index)
                / static_cast<double>(filterTaps - 1u);
            const double window =
                0.42 - 0.5 * std::cos(2.0 * kPi * position)
                + 0.08 * std::cos(4.0 * kPi * position);
            coefficients[index] =
                static_cast<float>(sinc * window);
            sum += coefficients[index];
        }
        if (sum != 0.0) {
            for (auto &coefficient : coefficients) {
                coefficient =
                    static_cast<float>(coefficient / sum);
            }
        }
        return coefficients;
    }

    [[nodiscard]] static float filter(State &state, float sample)
    {
        state.hasInput = true;
        if (state.coefficients.empty()) {
            state.lastFiltered = sample;
            return sample;
        }
        if (!state.historyPrimed) {
            std::fill(state.history.begin(), state.history.end(), sample);
            state.newestHistoryIndex = 0;
            state.historyPrimed = true;
        } else {
            state.newestHistoryIndex =
                (state.newestHistoryIndex + 1u) % state.history.size();
            state.history[state.newestHistoryIndex] = sample;
        }

        double filtered = 0.0;
        for (std::size_t tap = 0; tap < state.coefficients.size(); ++tap) {
            const std::size_t historyIndex =
                (state.newestHistoryIndex + state.history.size() - tap)
                % state.history.size();
            filtered +=
                static_cast<double>(state.coefficients[tap])
                * state.history[historyIndex];
        }
        state.lastFiltered = static_cast<float>(filtered);
        return state.lastFiltered;
    }

    void appendRoundedTail(
        State &state,
        std::vector<float> &samples) const
    {
        if (state.hasInput && state.clock.finishRounded()) {
            samples.push_back(state.lastFiltered);
            ++state.emittedSamples;
        }
    }

    [[nodiscard]] ResampledAudioBlock finishBlock(
        std::uint64_t sourceId,
        State &state,
        bool endOfStream) const
    {
        ResampledAudioBlock block;
        block.sourceId = sourceId;
        block.startTimeNs = outputTime(state);
        block.endOfStream = endOfStream;
        appendRoundedTail(state, block.samples);
        return block;
    }

    [[nodiscard]] std::int64_t outputTime(const State &state) const
    {
        const std::uint64_t seconds =
            state.emittedSamples / outputRate_;
        const std::uint64_t remainder =
            state.emittedSamples % outputRate_;
        constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
        if (seconds
            > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())
                / kNanosecondsPerSecond) {
            return std::numeric_limits<std::int64_t>::max();
        }
        const std::uint64_t wholeNanoseconds =
            seconds * kNanosecondsPerSecond;
        const std::uint64_t fractionalNanoseconds =
            remainder * kNanosecondsPerSecond / outputRate_;
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        if (wholeNanoseconds
            > static_cast<std::uint64_t>(maximum)
                - fractionalNanoseconds) {
            return maximum;
        }
        const std::uint64_t delta =
            wholeNanoseconds + fractionalNanoseconds;
        if (state.streamStartTimeNs >= 0
            && delta
                > static_cast<std::uint64_t>(
                    maximum - state.streamStartTimeNs)) {
            return maximum;
        }
        return state.streamStartTimeNs
            + static_cast<std::int64_t>(delta);
    }

    std::uint32_t outputRate_{};
    std::unordered_map<std::uint64_t, State> states_;
};

} // namespace localscribe
