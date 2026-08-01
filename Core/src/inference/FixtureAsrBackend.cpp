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

std::int64_t saturatedAdd(std::int64_t left, std::int64_t right)
{
    if (right > 0
        && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return left + right;
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

    [[nodiscard]] Expected<std::vector<AsrHypothesis>>
    accept(const AudioWindow &audio) override
    {
        std::this_thread::sleep_for(delay_);
        return fixture_.accept(audio);
    }

    [[nodiscard]] Expected<std::vector<AsrHypothesis>> flush() override
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

    [[nodiscard]] Expected<std::vector<AsrHypothesis>>
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

    [[nodiscard]] Expected<std::vector<AsrHypothesis>> flush() override
    {
        return std::vector<AsrHypothesis>{};
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

    [[nodiscard]] Expected<std::vector<AsrHypothesis>>
    accept(const AudioWindow &) override
    {
        return std::vector<AsrHypothesis>{};
    }

    [[nodiscard]] Expected<std::vector<AsrHypothesis>> flush() override
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

    [[nodiscard]] Expected<std::vector<AsrHypothesis>>
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

    [[nodiscard]] Expected<std::vector<AsrHypothesis>> flush() override
    {
        if (!prepared_) {
            return Error{LS_INVALID_STATE, "failing fixture is not prepared"};
        }
        return std::vector<AsrHypothesis>{};
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

Expected<std::vector<AsrHypothesis>>
FixtureAsrBackend::accept(const AudioWindow &audio)
{
    if (!prepared_) {
        return Error{LS_INVALID_STATE, "fixture ASR is not prepared"};
    }
    if (audio.samples.empty()) {
        return std::vector<AsrHypothesis>{};
    }

    const auto peak = std::max_element(
        audio.samples.begin(),
        audio.samples.end(),
        [](float left, float right) {
            return std::fabs(left) < std::fabs(right);
        });
    if (peak == audio.samples.end() || std::fabs(*peak) < 0.001F) {
        return std::vector<AsrHypothesis>{};
    }

    /*
     * Fixture encoding (test-only and deliberately simple):
     *   abs(first sample) * 1000 -> cue ID, zero -> sequence number
     *   negative first sample   -> revision 2
     * A frame produces at most one final. No production backend can be
     * selected through this path without the explicit core test flag.
     */
    const float first = audio.samples.front();
    const auto encoded = static_cast<std::uint64_t>(
        std::llround(std::fabs(static_cast<double>(first)) * 1000.0));
    const std::uint64_t cueId =
        encoded == 0 ? audio.sequenceNumber : encoded;
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
    hypothesis.startTimeNs = audio.monotonicTimeNs;
    hypothesis.endTimeNs =
        saturatedAdd(audio.monotonicTimeNs, durationNs(audio));
    hypothesis.text = "fixture cue " + std::to_string(cueId);
    if (revision > 1) {
        hypothesis.text += " revised";
    }
    hypothesis.language = std::move(language);
    hypothesis.confidence = revision > 1 ? 0.99F : 0.95F;
    hypothesis.revision = revision;
    hypothesis.final = true;

    return std::vector<AsrHypothesis>{std::move(hypothesis)};
}

Expected<std::vector<AsrHypothesis>> FixtureAsrBackend::flush()
{
    if (!prepared_) {
        return Error{LS_INVALID_STATE, "fixture ASR is not prepared"};
    }
    return std::vector<AsrHypothesis>{};
}

Expected<std::unique_ptr<IAsrBackend>>
createAsrBackend(std::string_view backendId, bool allowTestBackends)
{
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
