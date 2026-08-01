#include "WhisperCppBackend.hpp"
#include "SpeakerFeatureExtractor.hpp"
#include "WhisperChunker.hpp"
#include "WhisperSpeechGate.hpp"
#include "WhisperStreamingResampler.hpp"

#if defined(LOCALSCRIBE_ENABLE_WHISPER)
#include <whisper/whisper.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace localscribe {
namespace {

constexpr std::uint32_t kWhisperSampleRate = WHISPER_SAMPLE_RATE;
constexpr std::size_t kChunkSamples =
    static_cast<std::size_t>(kWhisperSampleRate) * 5u;
constexpr std::size_t kMinimumDiscontinuitySamples =
    static_cast<std::size_t>(kWhisperSampleRate);
constexpr std::int64_t kWhisperTimestampUnitNs = 10'000'000;

void discardWhisperLog(
    enum ggml_log_level,
    const char *,
    void *)
{
}

std::int64_t saturatedTimestamp(
    std::int64_t base,
    std::int64_t units)
{
    if (units <= 0) {
        return base;
    }
    if (units
        > (std::numeric_limits<std::int64_t>::max() - base)
            / kWhisperTimestampUnitNs) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return base + units * kWhisperTimestampUnitNs;
}

StableId stableId(
    std::uint64_t sourceId,
    std::uint64_t chunk,
    std::uint32_t segment)
{
    StableId result{};
    const std::uint64_t local =
        (chunk << 16u) ^ static_cast<std::uint64_t>(segment);
    for (std::size_t index = 0; index < 8; ++index) {
        result[index] = static_cast<std::uint8_t>(
            sourceId >> ((7u - index) * 8u));
        result[index + 8] = static_cast<std::uint8_t>(
            local >> ((7u - index) * 8u));
    }
    return result;
}

std::string trim(std::string value)
{
    const auto visible = [](unsigned char byte) {
        return std::isspace(byte) == 0;
    };
    const auto begin =
        std::find_if(value.begin(), value.end(), visible);
    const auto end =
        std::find_if(value.rbegin(), value.rend(), visible).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::vector<float> toMono(const AudioWindow &audio)
{
    if (audio.samples.empty() || audio.sampleRateHz == 0
        || audio.channelCount == 0 || audio.frameCount == 0) {
        return {};
    }
    const std::size_t availableFrames = std::min<std::size_t>(
        audio.frameCount,
        audio.samples.size() / audio.channelCount);
    std::vector<float> output(availableFrames);
    if (audio.channelCount == 1) {
        std::copy_n(
            audio.samples.begin(),
            static_cast<std::ptrdiff_t>(availableFrames),
            output.begin());
        return output;
    }
    for (std::size_t frame = 0; frame < availableFrames; ++frame) {
        long double total = 0;
        const std::size_t offset = frame * audio.channelCount;
        for (std::size_t channel = 0; channel < audio.channelCount;
             ++channel) {
            total += audio.samples[offset + channel];
        }
        output[frame] = static_cast<float>(
            total / static_cast<long double>(audio.channelCount));
    }
    return output;
}

#if defined(__APPLE__) && defined(__aarch64__)
bool metalProbeSucceeded(const std::string &modelPath)
{
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, bool> cache;
    {
        std::lock_guard lock(cacheMutex);
        if (const auto found = cache.find(modelPath);
            found != cache.end()) {
            return found->second;
        }
    }

    std::filesystem::path probePath;
    if (const char *configured =
            std::getenv("LOCALSCRIBE_WHISPER_METAL_PROBE_PATH");
        configured != nullptr && *configured != '\0') {
        probePath = configured;
    } else {
        std::uint32_t size = 0;
        char placeholder = '\0';
        (void)_NSGetExecutablePath(&placeholder, &size);
        std::vector<char> executable(size + 1u, '\0');
        if (_NSGetExecutablePath(executable.data(), &size) == 0) {
            probePath = std::filesystem::path(executable.data()).parent_path()
                / "LocalScribeMetalProbe";
        }
    }

    bool supported = false;
    const std::string probe = probePath.string();
    if (!probe.empty()
        && std::filesystem::is_regular_file(probePath)
        && access(probe.c_str(), X_OK) == 0) {
        std::array<char *, 3> arguments{
            const_cast<char *>(probe.c_str()),
            const_cast<char *>(modelPath.c_str()),
            nullptr};
        pid_t process = 0;
        const int spawned = posix_spawn(
            &process,
            probe.c_str(),
            nullptr,
            nullptr,
            arguments.data(),
            environ);
        if (spawned == 0) {
            int status = 0;
            pid_t waited = 0;
            do {
                waited = waitpid(process, &status, 0);
            } while (waited < 0 && errno == EINTR);
            supported = waited == process
                && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }

    {
        std::lock_guard lock(cacheMutex);
        cache[modelPath] = supported;
    }
    return supported;
}
#endif

} // namespace

class WhisperCppBackend::Impl {
public:
    Impl()
        : resampler(kWhisperSampleRate),
          chunker(
              kWhisperSampleRate,
              kChunkSamples,
              kMinimumDiscontinuitySamples)
    {
    }

    ~Impl()
    {
        if (context != nullptr) {
            whisper_free(context);
        }
    }

    [[nodiscard]] bool loadContext(bool useGpu)
    {
        auto parameters = whisper_context_default_params();
        parameters.use_gpu = useGpu;
        parameters.gpu_device = 0;
        whisper_context *replacement = whisper_init_from_file_with_params(
            modelPath.c_str(),
            parameters);
        if (replacement == nullptr) {
            return false;
        }
        if (context != nullptr) {
            whisper_free(context);
        }
        context = replacement;
        usingGpu = useGpu;
        return true;
    }

    Expected<std::vector<AsrHypothesis>> transcribe(
        std::uint64_t sourceId,
        std::int64_t startTimeNs,
        std::uint64_t chunkOrdinal,
        std::span<const float> samples)
    {
        if (context == nullptr) {
            return Error{LS_INVALID_STATE, "whisper.cpp is not prepared"};
        }
        if (abortRequested.load(std::memory_order_acquire)) {
            return Error{
                LS_TIMEOUT,
                "whisper.cpp inference cancelled for bounded finalization"};
        }
        if (!speechGate.shouldTranscribe(
                sourceId,
                samples,
                kWhisperSampleRate)) {
            return std::vector<AsrHypothesis>{};
        }
        const float peak = std::accumulate(
            samples.begin(),
            samples.end(),
            0.0F,
            [](float current, float sample) {
                return std::max(current, std::fabs(sample));
            });
        if (peak < 0.001F) {
            return std::vector<AsrHypothesis>{};
        }
        if (samples.size()
            > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Error{LS_BACKEND_FAILURE, "ASR chunk is too large"};
        }

        auto parameters =
            whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        parameters.n_threads = std::max(
            1,
            std::min(
                8,
                static_cast<int>(
                    std::thread::hardware_concurrency())));
        parameters.translate = false;
        parameters.no_context = true;
        parameters.no_timestamps = false;
        parameters.single_segment = false;
        parameters.print_special = false;
        parameters.print_progress = false;
        parameters.print_realtime = false;
        parameters.print_timestamps = false;
        parameters.token_timestamps = false;
        parameters.debug_mode = false;
        parameters.suppress_blank = true;
        parameters.suppress_nst = false;
        parameters.language = language.empty() ? nullptr : language.c_str();
        /*
         * A null language requests auto-detection and then transcription.
         * whisper.cpp's detect_language flag means "detect and return", so it
         * must remain false for the mixed RU/EN transcription mode.
         */
        parameters.detect_language = false;
        parameters.tdrz_enable = tinyDiarizationEnabled;
        parameters.abort_callback = [](void *data) {
            return static_cast<std::atomic<bool> *>(data)->load(
                std::memory_order_acquire);
        };
        parameters.abort_callback_user_data = &abortRequested;

        auto runInference = [&] {
            return whisper_full(
                context,
                parameters,
                samples.data(),
                static_cast<int>(samples.size()));
        };
        int inferenceStatus = runInference();
        if (inferenceStatus != 0
            && usingGpu
            && !abortRequested.load(std::memory_order_acquire)
            && loadContext(false)) {
            inferenceStatus = runInference();
        }
        if (inferenceStatus != 0) {
            if (abortRequested.load(std::memory_order_acquire)) {
                return Error{
                    LS_TIMEOUT,
                    "whisper.cpp inference cancelled for bounded finalization"};
            }
            return Error{LS_BACKEND_FAILURE, "whisper.cpp inference failed"};
        }

        std::string detectedLanguage{"und"};
        const int languageId = whisper_full_lang_id(context);
        if (const char *value = whisper_lang_str(languageId);
            value != nullptr) {
            detectedLanguage = value;
        }

        const int count = whisper_full_n_segments(context);
        std::vector<AsrHypothesis> result;
        result.reserve(count > 0 ? static_cast<std::size_t>(count) : 0u);
        for (int index = 0; index < count; ++index) {
            const char *raw = whisper_full_get_segment_text(context, index);
            std::string text = trim(raw == nullptr ? "" : raw);
            if (text.empty()) {
                continue;
            }
            float confidence = 0.0F;
            const int tokenCount = whisper_full_n_tokens(context, index);
            for (int token = 0; token < tokenCount; ++token) {
                confidence +=
                    whisper_full_get_token_p(context, index, token);
            }
            if (tokenCount > 0) {
                confidence /= static_cast<float>(tokenCount);
            }

            AsrHypothesis hypothesis;
            hypothesis.stableId = stableId(
                sourceId,
                chunkOrdinal,
                static_cast<std::uint32_t>(index));
            hypothesis.sourceId = sourceId;
            hypothesis.startTimeNs = saturatedTimestamp(
                startTimeNs,
                whisper_full_get_segment_t0(context, index));
            hypothesis.endTimeNs = saturatedTimestamp(
                startTimeNs,
                whisper_full_get_segment_t1(context, index));
            hypothesis.text = std::move(text);
            hypothesis.language = detectedLanguage;
            hypothesis.confidence =
                std::clamp(confidence, 0.0F, 1.0F);
            hypothesis.revision = 1;
            hypothesis.final = true;
            hypothesis.speakerTurnAfter =
                tinyDiarizationEnabled
                && whisper_full_get_segment_speaker_turn_next(
                    context,
                    index);
            const auto begin = std::min<std::size_t>(
                samples.size(),
                static_cast<std::size_t>(
                    std::max<std::int64_t>(
                        whisper_full_get_segment_t0(context, index),
                        0))
                    * kWhisperSampleRate / 100u);
            const auto end = std::min<std::size_t>(
                samples.size(),
                static_cast<std::size_t>(
                    std::max<std::int64_t>(
                        whisper_full_get_segment_t1(context, index),
                        0))
                    * kWhisperSampleRate / 100u);
            if (end > begin) {
                hypothesis.speakerEmbedding =
                    SpeakerFeatureExtractor::extract(
                        samples.subspan(begin, end - begin),
                        kWhisperSampleRate);
            }
            result.push_back(std::move(hypothesis));
        }
        return result;
    }

    whisper_context *context{};
    std::string version;
    std::string modelPath;
    std::string language;
    bool tinyDiarizationEnabled{};
    bool usingGpu{};
    std::atomic<bool> abortRequested{false};
    WhisperStreamingResampler resampler;
    WhisperChunker chunker;
    WhisperSpeechGate speechGate;
};

WhisperCppBackend::WhisperCppBackend() : impl_(std::make_unique<Impl>()) {}
WhisperCppBackend::~WhisperCppBackend() = default;

BackendInfo WhisperCppBackend::info() const
{
    return BackendInfo{
        "whisper.cpp",
        impl_->version.empty()
            ? "unknown"
            : impl_->version + (impl_->usingGpu ? "+metal" : "+cpu"),
        false};
}

Expected<void>
WhisperCppBackend::prepare(const AsrConfiguration &configuration)
{
    if (configuration.modelPath.empty()
        || configuration.modelPath.find('\0') != std::string::npos
        || !std::filesystem::is_regular_file(configuration.modelPath)) {
        return Error{
            LS_MODEL_UNAVAILABLE,
            "selected local whisper.cpp model is unavailable"};
    }
    whisper_log_set(discardWhisperLog, nullptr);
    impl_->modelPath = configuration.modelPath;
    impl_->abortRequested.store(false, std::memory_order_release);
#if defined(__APPLE__) && defined(__aarch64__)
    const bool preferMetal =
        std::getenv("LOCALSCRIBE_WHISPER_FORCE_CPU") == nullptr
        && metalProbeSucceeded(configuration.modelPath);
#else
    const bool preferMetal = false;
#endif
    if ((!preferMetal || !impl_->loadContext(true))
        && !impl_->loadContext(false)) {
        return Error{
            LS_MODEL_UNAVAILABLE,
            "selected local whisper.cpp model could not be loaded"};
    }
    impl_->version =
        whisper_version() == nullptr ? "unknown" : whisper_version();
    std::string modelFilename =
        std::filesystem::path(configuration.modelPath).filename().string();
    std::transform(
        modelFilename.begin(),
        modelFilename.end(),
        modelFilename.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    impl_->tinyDiarizationEnabled =
        modelFilename.find("-tdrz") != std::string::npos;
    switch (configuration.languageMode) {
    case LS_LANGUAGE_MODE_RUSSIAN:
        impl_->language = "ru";
        break;
    case LS_LANGUAGE_MODE_ENGLISH:
        impl_->language = "en";
        break;
    case LS_LANGUAGE_MODE_RUSSIAN_ENGLISH:
        impl_->language.clear();
        break;
    default:
        return Error{LS_INVALID_ARGUMENT, "unsupported ASR language mode"};
    }
    impl_->resampler.reset();
    impl_->chunker.reset();
    impl_->speechGate.reset();
    return success();
}

Expected<std::vector<AsrHypothesis>>
WhisperCppBackend::accept(const AudioWindow &audio)
{
    if (impl_->context == nullptr) {
        return Error{LS_INVALID_STATE, "whisper.cpp is not prepared"};
    }
    /*
     * Bound both the FIR ratio and the largest transient allocation before
     * the source's phase/history is touched. The public C ABI already checks
     * structural validity; this is the whisper.cpp adapter's narrower input
     * contract.
     */
    if (!WhisperResamplerInputLimits::supportsRate(
            audio.sampleRateHz)) {
        return Error{
            LS_AUDIO_FORMAT_ERROR,
            "whisper.cpp supports input rates from 8 kHz through 96 kHz"};
    }
    if (!WhisperResamplerInputLimits::callbackFits(
            audio.sampleRateHz,
            audio.frameCount,
            kWhisperSampleRate)) {
        return Error{
            LS_AUDIO_FORMAT_ERROR,
            "whisper.cpp audio callback exceeds ten seconds"};
    }
    auto mono = toMono(audio);
    auto blocks = impl_->resampler.accept(
        audio.sourceId,
        audio.monotonicTimeNs,
        audio.sampleRateHz,
        mono,
        (audio.flags & LS_AUDIO_FLAG_DISCONTINUITY) != 0,
        (audio.flags & LS_AUDIO_FLAG_END_OF_STREAM) != 0,
        audio.overloadGapBefore);
    std::vector<AsrHypothesis> result;
    for (const auto &block : blocks) {
        auto chunks = impl_->chunker.accept(
            block.sourceId,
            block.startTimeNs,
            block.samples,
            block.discontinuityBefore,
            block.endOfStream,
            block.dropShortBeforeDiscontinuity);
        for (const auto &chunk : chunks) {
            auto transcription = impl_->transcribe(
                chunk.sourceId,
                chunk.startTimeNs,
                chunk.ordinal,
                chunk.samples);
            if (!transcription) {
                return transcription.error();
            }
            result.insert(
                result.end(),
                std::make_move_iterator(transcription.value().begin()),
                std::make_move_iterator(transcription.value().end()));
        }
    }
    return result;
}

Expected<std::vector<AsrHypothesis>> WhisperCppBackend::flush()
{
    if (impl_->context == nullptr) {
        return Error{LS_INVALID_STATE, "whisper.cpp is not prepared"};
    }
    std::vector<AsrHypothesis> result;
    for (const auto &block : impl_->resampler.flush()) {
        auto chunks = impl_->chunker.accept(
            block.sourceId,
            block.startTimeNs,
            block.samples,
            block.discontinuityBefore,
            block.endOfStream,
            block.dropShortBeforeDiscontinuity);
        for (const auto &chunk : chunks) {
            auto transcription = impl_->transcribe(
                chunk.sourceId,
                chunk.startTimeNs,
                chunk.ordinal,
                chunk.samples);
            if (!transcription) {
                return transcription.error();
            }
            result.insert(
                result.end(),
                std::make_move_iterator(transcription.value().begin()),
                std::make_move_iterator(transcription.value().end()));
        }
    }
    for (const auto &chunk : impl_->chunker.flush()) {
        auto transcription = impl_->transcribe(
            chunk.sourceId,
            chunk.startTimeNs,
            chunk.ordinal,
            chunk.samples);
        if (!transcription) {
            return transcription.error();
        }
        result.insert(
            result.end(),
            std::make_move_iterator(transcription.value().begin()),
            std::make_move_iterator(transcription.value().end()));
    }
    return result;
}

void WhisperCppBackend::requestAbort() noexcept
{
    impl_->abortRequested.store(true, std::memory_order_release);
}

} // namespace localscribe

#endif /* LOCALSCRIBE_ENABLE_WHISPER */
