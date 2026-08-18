#include "FixtureAsrBackend.hpp"

#if defined(LOCALSCRIBE_ENABLE_WHISPER)
#include "WhisperCppBackend.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace localscribe {
namespace {

StableId fixtureStableId(std::uint64_t sourceId, std::uint64_t cueId)
{
    StableId id{};
    for (std::size_t index = 0; index < 8; ++index) {
        id[index] = static_cast<std::uint8_t>(
            sourceId >> ((7u - index) * 8u));
        id[index + 8] = static_cast<std::uint8_t>(
            cueId >> ((7u - index) * 8u));
    }
    return id;
}

std::int64_t durationNs(const AudioWindow &audio)
{
    if (audio.sampleRateHz == 0) {
        return 0;
    }
    const long double duration = static_cast<long double>(audio.frameCount)
        * 1'000'000'000.0L / static_cast<long double>(audio.sampleRateHz);
    if (duration
        >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(duration);
}

std::int64_t durationNs(
    std::uint32_t frameCount,
    std::uint32_t sampleRateHz)
{
    if (sampleRateHz == 0) {
        return 0;
    }
    const long double duration = static_cast<long double>(frameCount)
        * 1'000'000'000.0L / static_cast<long double>(sampleRateHz);
    if (duration
        >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(duration);
}

std::int64_t saturatedAdd(std::int64_t left, std::int64_t right)
{
    if (right > 0
        && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return left + right;
}

struct FixtureCallbackSlice {
    std::size_t sampleOffset{};
    std::size_t sampleCount{};
    std::uint32_t frameOffset{};
    std::uint32_t frameCount{};
    std::uint64_t sequenceNumber{};
    std::int64_t startTimeNs{};
    std::int64_t endTimeNs{};
};

std::vector<FixtureCallbackSlice>
fixtureCallbackSlices(const AudioWindow &audio)
{
    const auto singleSlice = [&] {
        return std::vector<FixtureCallbackSlice>{FixtureCallbackSlice{
            0,
            audio.samples.size(),
            0,
            audio.frameCount,
            audio.sequenceNumber,
            audio.monotonicTimeNs,
            saturatedAdd(audio.monotonicTimeNs, durationNs(audio))}};
    };

    if (audio.callbackCount <= 1 || audio.channelCount == 0
        || audio.callbackCount > audio.frameCount
        || audio.frameCount % audio.callbackCount != 0) {
        return singleSlice();
    }

    const auto framesPerCallback = static_cast<std::uint32_t>(
        audio.frameCount / audio.callbackCount);
    const auto samplesPerCallback =
        static_cast<std::size_t>(framesPerCallback) * audio.channelCount;
    if (framesPerCallback == 0 || samplesPerCallback == 0
        || audio.callbackCount
            > std::numeric_limits<std::size_t>::max() / samplesPerCallback
        || static_cast<std::size_t>(audio.callbackCount)
                * samplesPerCallback
            != audio.samples.size()) {
        return singleSlice();
    }

    std::vector<FixtureCallbackSlice> slices;
    slices.reserve(static_cast<std::size_t>(audio.callbackCount));
    for (std::uint64_t index = 0; index < audio.callbackCount; ++index) {
        const auto frameOffset = static_cast<std::uint32_t>(
            index * framesPerCallback);
        const auto nextFrameOffset = static_cast<std::uint32_t>(
            (index + 1u) * framesPerCallback);
        const auto sequenceNumber =
            audio.sequenceNumber
                > std::numeric_limits<std::uint64_t>::max() - index
            ? std::numeric_limits<std::uint64_t>::max()
            : audio.sequenceNumber + index;
        slices.push_back(FixtureCallbackSlice{
            static_cast<std::size_t>(index) * samplesPerCallback,
            samplesPerCallback,
            frameOffset,
            framesPerCallback,
            sequenceNumber,
            saturatedAdd(
                audio.monotonicTimeNs,
                durationNs(frameOffset, audio.sampleRateHz)),
            saturatedAdd(
                audio.monotonicTimeNs,
                durationNs(nextFrameOffset, audio.sampleRateHz))});
    }
    return slices;
}

bool fixtureSliceHasSignal(
    const AudioWindow &audio,
    const FixtureCallbackSlice &slice)
{
    const auto begin = audio.samples.begin()
        + static_cast<std::ptrdiff_t>(slice.sampleOffset);
    const auto end = begin + static_cast<std::ptrdiff_t>(slice.sampleCount);
    const auto peak = std::max_element(
        begin,
        end,
        [](float left, float right) {
            return std::fabs(left) < std::fabs(right);
        });
    return peak != end && std::fabs(*peak) >= 0.001F;
}

class SlowFixtureAsrBackend final : public IAsrBackend {
public:
    explicit SlowFixtureAsrBackend(
        std::chrono::milliseconds delay = std::chrono::milliseconds(20))
        : delay_(delay)
    {
    }

    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{"fixture-slow", "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override
    {
        return fixture_.prepare(configuration);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) override
    {
        std::this_thread::sleep_for(delay_);
        return fixture_.accept(audio);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        return fixture_.flush();
    }

private:
    std::chrono::milliseconds delay_;
    FixtureAsrBackend fixture_;
};

class StallingFixtureAsrBackend final : public IAsrBackend {
public:
    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{"fixture-stall", "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override
    {
        aborted_.store(false, std::memory_order_relaxed);
        return fixture_.prepare(configuration);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) override
    {
        for (int attempt = 0; attempt < 1'000; ++attempt) {
            if (aborted_.load(std::memory_order_acquire)) {
                return Error{
                    LS_TIMEOUT,
                    "fixture inference aborted for bounded finalization"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return fixture_.accept(audio);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        return std::vector<AsrTimelineBatch>{};
    }

    void requestAbort() noexcept override
    {
        aborted_.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool> aborted_{false};
    FixtureAsrBackend fixture_;
};

class FlushStallingFixtureAsrBackend final : public IAsrBackend {
public:
    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{"fixture-flush-stall", "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override
    {
        aborted_.store(false, std::memory_order_relaxed);
        return fixture_.prepare(configuration);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &) override
    {
        return std::vector<AsrTimelineBatch>{};
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        for (int attempt = 0; attempt < 1'000; ++attempt) {
            if (aborted_.load(std::memory_order_acquire)) {
                return Error{
                    LS_TIMEOUT,
                    "fixture flush aborted for bounded finalization"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return fixture_.flush();
    }

    void requestAbort() noexcept override
    {
        aborted_.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool> aborted_{false};
    FixtureAsrBackend fixture_;
};

class SpeakerFixtureAsrBackend final : public IAsrBackend {
public:
    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{"fixture-speakers", "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override
    {
        return fixture_.prepare(configuration);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) override
    {
        auto accepted = fixture_.accept(audio);
        if (!accepted) {
            return accepted.error();
        }
        /*
         * Test-only speaker encoding, layered over the stable fixture cue:
         * samples 1...3 are the descriptor and sample 4 is a turn hint.
         */
        const auto slices = fixtureCallbackSlices(audio);
        std::size_t sliceIndex = 0;
        for (auto &batch : accepted.value()) {
            for (auto &hypothesis : batch.hypotheses) {
                while (sliceIndex < slices.size()
                    && !fixtureSliceHasSignal(
                        audio,
                        slices[sliceIndex])) {
                    ++sliceIndex;
                }
                if (sliceIndex == slices.size()) {
                    continue;
                }
                const auto &slice = slices[sliceIndex++];
                const auto descriptorOffset = slice.sampleOffset + 1u;
                if (slice.sampleCount >= 4) {
                    hypothesis.speakerEmbeddingModel =
                        "fixture-speaker-v1";
                    hypothesis.speakerEmbedding.assign(
                        audio.samples.begin()
                            + static_cast<std::ptrdiff_t>(descriptorOffset),
                        audio.samples.begin()
                            + static_cast<std::ptrdiff_t>(
                                descriptorOffset + 3u));
                }
                hypothesis.speakerTurnAfter =
                    slice.sampleCount >= 5
                    && audio.samples[slice.sampleOffset + 4u] > 0.5F;
            }
        }
        return accepted.takeValue();
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        return fixture_.flush();
    }

private:
    FixtureAsrBackend fixture_;
};

class BufferedSpeakerFixtureAsrBackend final : public IAsrBackend {
public:
    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{"fixture-speakers-buffered", "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override
    {
        acceptedFirstCue_ = false;
        buffered_.clear();
        return fixture_.prepare(configuration);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) override
    {
        auto accepted = fixture_.accept(audio);
        if (!accepted) {
            return accepted.error();
        }
        if (!acceptedFirstCue_) {
            acceptedFirstCue_ = true;
            return accepted.takeValue();
        }
        for (auto &batch : accepted.value()) {
            buffered_.push_back(std::move(batch));
        }
        return std::vector<AsrTimelineBatch>{};
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        auto tail = fixture_.flush();
        if (!tail) {
            return tail.error();
        }
        for (auto &batch : tail.value()) {
            buffered_.push_back(std::move(batch));
        }
        auto result = std::move(buffered_);
        buffered_.clear();
        return result;
    }

private:
    bool acceptedFirstCue_{};
    std::vector<AsrTimelineBatch> buffered_;
    SpeakerFixtureAsrBackend fixture_;
};

class WrongSourceFixtureAsrBackend final : public IAsrBackend {
public:
    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{"fixture-wrong-source", "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override
    {
        return fixture_.prepare(configuration);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) override
    {
        auto accepted = fixture_.accept(audio);
        if (!accepted) {
            return accepted.error();
        }
        const auto wrongSourceId = audio.sourceId == 1 ? 2 : 1;
        for (auto &batch : accepted.value()) {
            batch.sourceId = wrongSourceId;
            for (auto &hypothesis : batch.hypotheses) {
                hypothesis.sourceId = wrongSourceId;
            }
        }
        return accepted.takeValue();
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        return fixture_.flush();
    }

private:
    FixtureAsrBackend fixture_;
};

class OutOfRangeTimestampFixtureAsrBackend final : public IAsrBackend {
public:
    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{"fixture-out-of-range-timestamp", "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override
    {
        return fixture_.prepare(configuration);
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) override
    {
        auto accepted = fixture_.accept(audio);
        if (!accepted) {
            return accepted.error();
        }
        for (auto &batch : accepted.value()) {
            for (auto &hypothesis : batch.hypotheses) {
                hypothesis.endTimeNs = batch.finalizedThroughTimeNs
                        == std::numeric_limits<std::int64_t>::max()
                    ? batch.finalizedThroughTimeNs
                    : batch.finalizedThroughTimeNs + 1;
            }
        }
        return accepted.takeValue();
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        return fixture_.flush();
    }

private:
    FixtureAsrBackend fixture_;
};

class FailingFixtureAsrBackend final : public IAsrBackend {
public:
    FailingFixtureAsrBackend(std::string id, bool throws)
        : id_(std::move(id)), throws_(throws)
    {
    }

    [[nodiscard]] BackendInfo info() const override
    {
        return BackendInfo{id_, "1", true};
    }

    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &) override
    {
        prepared_ = true;
        return success();
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &) override
    {
        if (!prepared_) {
            return Error{LS_INVALID_STATE, "failing fixture is not prepared"};
        }
        if (throws_) {
            /*
             * Keep the first frame in flight long enough for a deterministic
             * queued-frame abandonment test.
             */
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            throw std::runtime_error("injected fixture exception");
        }
        return Error{LS_BACKEND_FAILURE, "injected fixture ASR failure"};
    }

    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override
    {
        if (!prepared_) {
            return Error{LS_INVALID_STATE, "failing fixture is not prepared"};
        }
        return std::vector<AsrTimelineBatch>{};
    }

private:
    std::string id_;
    bool throws_{};
    bool prepared_{};
};

} // namespace

BackendInfo FixtureAsrBackend::info() const
{
    return BackendInfo{"fixture", "1", true};
}

Expected<void>
FixtureAsrBackend::prepare(const AsrConfiguration &configuration)
{
    configuration_ = configuration;
    prepared_ = true;
    return success();
}

Expected<std::vector<AsrTimelineBatch>>
FixtureAsrBackend::accept(const AudioWindow &audio)
{
    if (!prepared_) {
        return Error{LS_INVALID_STATE, "fixture ASR is not prepared"};
    }
    AsrTimelineBatch batch;
    batch.sourceId = audio.sourceId;
    batch.processedStartTimeNs = audio.monotonicTimeNs;
    batch.finalizedThroughTimeNs = saturatedAdd(
        audio.monotonicTimeNs,
        durationNs(audio));
    batch.discontinuityBefore =
        (audio.flags & LS_AUDIO_FLAG_DISCONTINUITY) != 0;

    if (audio.samples.empty()) {
        return std::vector<AsrTimelineBatch>{std::move(batch)};
    }

    /*
     * Fixture encoding (test-only and deliberately simple):
     *   abs(first callback sample) * 1000 -> cue ID
     *   zero first sample                    -> callback sequence number
     *   negative first sample                -> revision 2
     * A callback produces at most one final. Runtime may aggregate equal-
     * sized callbacks, so the fixture preserves those logical boundaries.
     * No production backend can be selected through this path without the
     * explicit core test flag.
     */
    for (const auto &slice : fixtureCallbackSlices(audio)) {
        if (!fixtureSliceHasSignal(audio, slice)) {
            continue;
        }
        const float first = audio.samples[slice.sampleOffset];
        const auto encoded = static_cast<std::uint64_t>(
            std::llround(std::fabs(static_cast<double>(first)) * 1000.0));
        const std::uint64_t cueId =
            encoded == 0 ? slice.sequenceNumber : encoded;
        const std::uint32_t revision = first < 0.0F ? 2u : 1u;

        std::string language;
        switch (configuration_.languageMode) {
        case LS_LANGUAGE_MODE_RUSSIAN:
            language = "ru";
            break;
        case LS_LANGUAGE_MODE_ENGLISH:
            language = "en";
            break;
        case LS_LANGUAGE_MODE_RUSSIAN_ENGLISH:
            language = cueId % 2 == 0 ? "en" : "ru";
            break;
        default:
            language = "und";
            break;
        }

        AsrHypothesis hypothesis;
        hypothesis.stableId = fixtureStableId(audio.sourceId, cueId);
        hypothesis.sourceId = audio.sourceId;
        hypothesis.startTimeNs = slice.startTimeNs;
        hypothesis.endTimeNs = slice.endTimeNs;
        hypothesis.text = "fixture cue " + std::to_string(cueId);
        if (revision > 1) {
            hypothesis.text += " revised";
        }
        hypothesis.language = std::move(language);
        hypothesis.confidence = revision > 1 ? 0.99F : 0.95F;
        hypothesis.revision = revision;
        hypothesis.final = true;

        batch.hypotheses.push_back(std::move(hypothesis));
    }
    return std::vector<AsrTimelineBatch>{std::move(batch)};
}

Expected<std::vector<AsrTimelineBatch>> FixtureAsrBackend::flush()
{
    if (!prepared_) {
        return Error{LS_INVALID_STATE, "fixture ASR is not prepared"};
    }
    return std::vector<AsrTimelineBatch>{};
}

Expected<std::unique_ptr<IAsrBackend>>
createAsrBackend(std::string_view backendId, bool allowTestBackends)
{
    if (backendId == "fixture-speakers-buffered") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "buffered speaker fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<BufferedSpeakerFixtureAsrBackend>());
    }
    if (backendId == "fixture-wrong-source") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "wrong-source fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<WrongSourceFixtureAsrBackend>());
    }
    if (backendId == "fixture-out-of-range-timestamp") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "out-of-range timestamp fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<OutOfRangeTimestampFixtureAsrBackend>());
    }
    if (backendId == "fixture-speakers") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "speaker fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<SpeakerFixtureAsrBackend>());
    }
    if (backendId == "fixture") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<FixtureAsrBackend>());
    }
    if (backendId == "fixture-slow") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "slow fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<SlowFixtureAsrBackend>());
    }
    if (backendId == "fixture-overloaded") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "overloaded fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<SlowFixtureAsrBackend>(
                std::chrono::milliseconds(150)));
    }
    if (backendId == "fixture-stall") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "stalling fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<StallingFixtureAsrBackend>());
    }
    if (backendId == "fixture-flush-stall") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "flush-stalling fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<FlushStallingFixtureAsrBackend>());
    }
    if (backendId == "fixture-fail" || backendId == "fixture-throw") {
        if (!allowTestBackends) {
            return Error{
                LS_BACKEND_UNAVAILABLE,
                "failing fixture backend requires explicit test configuration"};
        }
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<FailingFixtureAsrBackend>(
                std::string{backendId},
                backendId == "fixture-throw"));
    }
#if defined(LOCALSCRIBE_ENABLE_WHISPER)
    if (backendId == "whisper.cpp" || backendId == "whisper") {
        return std::unique_ptr<IAsrBackend>(
            std::make_unique<WhisperCppBackend>());
    }
#endif
    return Error{
        LS_BACKEND_UNAVAILABLE,
        "requested local ASR backend is not available"};
}

} // namespace localscribe
