#include "../Core/src/inference/WhisperCppBackend.hpp"

#include <LocalScribeCore/LocalScribeCore.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("truncated WAV integer");
    }
    return static_cast<std::uint16_t>(
        bytes[offset] | static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("truncated WAV integer");
    }
    return static_cast<std::uint32_t>(bytes[offset])
        | static_cast<std::uint32_t>(bytes[offset + 1]) << 8u
        | static_cast<std::uint32_t>(bytes[offset + 2]) << 16u
        | static_cast<std::uint32_t>(bytes[offset + 3]) << 24u;
}

bool tagEquals(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    const char (&tag)[5])
{
    return offset + 4 <= bytes.size()
        && std::equal(tag, tag + 4, bytes.begin() + offset);
}

struct Wave {
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::vector<float> samples;
};

ls_utf8_view_v1 view(const std::string &value)
{
    return ls_utf8_view_v1{
        sizeof(ls_utf8_view_v1),
        LS_CORE_ABI_VERSION,
        reinterpret_cast<const std::uint8_t *>(value.data()),
        value.size()};
}

std::string errorText(const ls_error_v1 &error)
{
    return std::string(
        reinterpret_cast<const char *>(error.message),
        std::min<std::size_t>(
            error.message_size,
            LS_ERROR_MESSAGE_CAPACITY));
}

void requireStatus(
    ls_status_code_t actual,
    ls_status_code_t expected,
    const char *operation)
{
    if (actual != expected) {
        throw std::runtime_error(
            std::string(operation) + " failed with status "
            + std::to_string(actual));
    }
}

Wave readWave(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open WAV file");
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || !tagEquals(bytes, 0, "RIFF")
        || !tagEquals(bytes, 8, "WAVE")) {
        throw std::runtime_error("input is not a RIFF/WAVE file");
    }

    std::uint16_t format{};
    std::uint16_t channels{};
    std::uint32_t sampleRate{};
    std::uint16_t bitsPerSample{};
    std::span<const std::uint8_t> audio;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const std::uint32_t size = readU32(bytes, offset + 4);
        const std::size_t payload = offset + 8;
        if (payload > bytes.size() || size > bytes.size() - payload) {
            throw std::runtime_error("truncated WAV chunk");
        }
        if (tagEquals(bytes, offset, "fmt ")) {
            if (size < 16) {
                throw std::runtime_error("invalid WAV format chunk");
            }
            format = readU16(bytes, payload);
            channels = readU16(bytes, payload + 2);
            sampleRate = readU32(bytes, payload + 4);
            bitsPerSample = readU16(bytes, payload + 14);
        } else if (tagEquals(bytes, offset, "data")) {
            audio = std::span(bytes).subspan(payload, size);
        }
        const std::size_t padded = static_cast<std::size_t>(size) + (size & 1u);
        if (padded > std::numeric_limits<std::size_t>::max() - payload) {
            throw std::runtime_error("invalid WAV chunk size");
        }
        offset = payload + padded;
    }

    if (channels == 0 || sampleRate == 0 || audio.empty()) {
        throw std::runtime_error("WAV has no supported audio stream");
    }
    const std::size_t bytesPerSample = bitsPerSample / 8u;
    if ((format != 1 || bitsPerSample != 16)
        && (format != 3 || bitsPerSample != 32)) {
        throw std::runtime_error(
            "only little-endian PCM16 and Float32 WAV are supported");
    }
    if (bytesPerSample == 0 || audio.size() % bytesPerSample != 0
        || audio.size() / bytesPerSample % channels != 0) {
        throw std::runtime_error("WAV sample payload is malformed");
    }

    Wave result;
    result.sampleRate = sampleRate;
    result.channels = channels;
    result.samples.reserve(audio.size() / bytesPerSample);
    for (std::size_t offset = 0; offset < audio.size();
         offset += bytesPerSample) {
        if (format == 1) {
            const auto encoded = readU16(audio, offset);
            const auto sample = static_cast<std::int16_t>(encoded);
            result.samples.push_back(
                std::clamp(
                    static_cast<float>(sample) / 32768.0F,
                    -1.0F,
                    1.0F));
        } else {
            const std::uint32_t encoded = readU32(audio, offset);
            float sample{};
            static_assert(sizeof(sample) == sizeof(encoded));
            std::memcpy(&sample, &encoded, sizeof(sample));
            result.samples.push_back(sample);
        }
    }
    return result;
}

void runCoreVertical(
    const std::string &modelPath,
    const Wave &wave,
    std::size_t expectedFinalSegments)
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path journalPath =
        std::filesystem::temp_directory_path()
        / ("localscribe-whisper-smoke-" + std::to_string(stamp) + ".sqlite3");
    const std::string journal = journalPath.string();
    const std::string sessionID{"00000000-0000-4000-8000-000000000001"};
    const std::string sourceApp{"WhisperSmoke"};
    const std::string speaker{"Me"};
    const std::string backend{"whisper.cpp"};
    const std::string diarization{"source-aware"};
    const std::string created{"2026-07-29T00:00:00Z"};
    const std::string ended{"2026-07-29T00:01:00Z"};
    const std::string title{"Local production ASR smoke"};

    ls_core_t *core{};
    ls_session_t *session{};
    ls_owned_bytes_t *markdown{};
    ls_error_v1 error{
        sizeof(ls_error_v1),
        LS_CORE_ABI_VERSION};
    auto cleanup = [&] {
        if (markdown != nullptr) {
            ls_owned_bytes_destroy(markdown);
            markdown = nullptr;
        }
        if (session != nullptr) {
            ls_session_destroy(session);
            session = nullptr;
        }
        if (core != nullptr) {
            ls_core_destroy(core);
            core = nullptr;
        }
        std::error_code ignored;
        std::filesystem::remove(journalPath, ignored);
        std::filesystem::remove(journal + "-wal", ignored);
        std::filesystem::remove(journal + "-shm", ignored);
    };

    try {
        ls_core_config_v1 coreConfig{};
        coreConfig.struct_size = sizeof(coreConfig);
        coreConfig.abi_version = LS_CORE_ABI_VERSION;
        coreConfig.journal_path = view(journal);
        const auto coreStatus =
            ls_core_create_v1(&coreConfig, &core, &error);
        if (coreStatus != LS_OK) {
            throw std::runtime_error(
                "core create failed: " + errorText(error));
        }

        ls_session_config_v1 sessionConfig{};
        sessionConfig.struct_size = sizeof(sessionConfig);
        sessionConfig.abi_version = LS_CORE_ABI_VERSION;
        sessionConfig.session_id = view(sessionID);
        sessionConfig.journal_path = view(journal);
        sessionConfig.source_app = view(sourceApp);
        sessionConfig.local_speaker_name = view(speaker);
        sessionConfig.asr_backend_id = view(backend);
        sessionConfig.asr_model_path = view(modelPath);
        sessionConfig.diarization_backend_id = view(diarization);
        sessionConfig.created_at_iso8601 = view(created);
        sessionConfig.language_mode = LS_LANGUAGE_MODE_RUSSIAN_ENGLISH;
        sessionConfig.audio_queue_capacity_frames = 24;
        sessionConfig.microphone_source_id = 1;
        sessionConfig.system_audio_source_id = 2;
        sessionConfig.required_source_mask = LS_REQUIRED_SOURCE_MICROPHONE;
        sessionConfig.source_completeness_threshold_ns = 10'000'000'000;
        const auto sessionCreateStarted = std::chrono::steady_clock::now();
        const auto sessionStatus = ls_session_create_after_consent_v1(
            core,
            &sessionConfig,
            &session,
            &error);
        const auto sessionCreateMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - sessionCreateStarted)
                .count();
        if (sessionStatus != LS_OK) {
            throw std::runtime_error(
                "session create failed: " + errorText(error));
        }
        requireStatus(
            ls_session_mark_sources_ready_v1(session),
            LS_OK,
            "mark sources ready");

        const std::size_t totalFrames = wave.samples.size() / wave.channels;
        /*
         * This is a functional production-backend smoke, not a capture-rate
         * load test. Feed bounded five-second blocks so the offline driver
         * does not enqueue an entire recording instantaneously while Whisper
         * is transcribing its first chunk.
         */
        const std::size_t framesPerPush =
            static_cast<std::size_t>(wave.sampleRate) * 5u;
        std::uint64_t sequence = 1;
        for (std::size_t offset = 0; offset < totalFrames;
             offset += framesPerPush, ++sequence) {
            const std::size_t frameCount =
                std::min(framesPerPush, totalFrames - offset);
            const auto first =
                wave.samples.begin() + static_cast<std::ptrdiff_t>(
                    offset * wave.channels);
            std::vector<float> samples(
                first,
                first + static_cast<std::ptrdiff_t>(
                    frameCount * wave.channels));

            ls_audio_frame_v1 frame{};
            frame.struct_size = sizeof(frame);
            frame.abi_version = LS_CORE_ABI_VERSION;
            frame.source_id = 1;
            frame.sequence_number = sequence;
            frame.monotonic_time_ns = static_cast<std::int64_t>(
                static_cast<long double>(offset) * 1'000'000'000.0L
                / wave.sampleRate);
            frame.sample_rate_hz = wave.sampleRate;
            frame.channel_count = wave.channels;
            frame.sample_format = LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED;
            frame.frame_count = static_cast<std::uint32_t>(frameCount);
            frame.flags =
                offset == 0 ? LS_AUDIO_FLAG_DISCONTINUITY : 0;
            if (offset + frameCount == totalFrames) {
                frame.flags |= LS_AUDIO_FLAG_END_OF_STREAM;
            }
            frame.samples = samples.data();

            ls_status_code_t status = LS_BACKPRESSURE;
            for (int attempt = 0;
                 attempt < 1'000 && status == LS_BACKPRESSURE;
                 ++attempt) {
                status = ls_session_push_audio_v1(session, &frame);
                if (status == LS_BACKPRESSURE) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
            requireStatus(status, LS_OK, "push production audio");
        }

        ls_pipeline_metrics_v1 metrics{};
        metrics.struct_size = sizeof(metrics);
        metrics.abi_version = LS_CORE_ABI_VERSION;
        const auto processingDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        do {
            requireStatus(
                ls_session_copy_metrics_v1(session, &metrics),
                LS_OK,
                "copy production metrics while draining");
            if (metrics.final_segments_committed
                >= expectedFinalSegments) {
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < processingDeadline);
        if (metrics.final_segments_committed < expectedFinalSegments) {
            throw std::runtime_error(
                "production core did not drain expected final segments");
        }

        requireStatus(
            ls_session_finalize_v1(
                session,
                LS_FINALIZE_REASON_USER_STOP),
            LS_OK,
            "finalize production session");

        requireStatus(
            ls_session_copy_metrics_v1(session, &metrics),
            LS_OK,
            "copy production metrics");
        if (metrics.final_segments_committed == 0) {
            throw std::runtime_error(
                "production core committed no final segments");
        }

        ls_markdown_options_v1 markdownOptions{};
        markdownOptions.struct_size = sizeof(markdownOptions);
        markdownOptions.abi_version = LS_CORE_ABI_VERSION;
        markdownOptions.title = view(title);
        markdownOptions.created_at_iso8601 = view(created);
        markdownOptions.ended_at_iso8601 = view(ended);
        markdownOptions.duration_seconds = static_cast<std::int64_t>(
            totalFrames / wave.sampleRate);
        markdownOptions.microphone_captured = 1;
        markdownOptions.system_audio_captured = 0;
        const auto renderStatus = ls_session_render_markdown_v1(
            session,
            &markdownOptions,
            &markdown,
            &error);
        if (renderStatus != LS_OK) {
            throw std::runtime_error(
                "Markdown render failed: " + errorText(error));
        }
        const auto *markdownData = ls_owned_bytes_data(markdown);
        const auto markdownSize = ls_owned_bytes_size(markdown);
        const std::string rendered(
            reinterpret_cast<const char *>(markdownData),
            markdownSize);
        if (rendered.find("status: \"complete\"") == std::string::npos
            || rendered.find("<!-- transcript:start -->")
                == std::string::npos
            || rendered.find("<!-- transcript:end -->")
                == std::string::npos) {
            throw std::runtime_error(
                "production core emitted invalid terminal Markdown");
        }

        std::cout << "core_final_segments="
                  << metrics.final_segments_committed
                  << " journal_checkpoint="
                  << metrics.journal_checkpoint
                  << " markdown_bytes=" << markdownSize
                  << " session_create_ms="
                  << sessionCreateMilliseconds << '\n';
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: WhisperSmoke MODEL.bin INPUT.wav\n";
        return 2;
    }

    try {
        const auto wave = readWave(argv[2]);
        localscribe::WhisperCppBackend backend;
        const auto backendPrepareStarted =
            std::chrono::steady_clock::now();
        auto prepared = backend.prepare(
            localscribe::AsrConfiguration{
                argv[1],
                LS_LANGUAGE_MODE_RUSSIAN_ENGLISH});
        const auto backendPrepareMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - backendPrepareStarted)
                .count();
        if (!prepared) {
            std::cerr << "prepare failed: "
                      << prepared.error().message << '\n';
            return 3;
        }

        const std::size_t frames = wave.samples.size() / wave.channels;
        if (frames > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("WAV is too long for this smoke tool");
        }
        localscribe::AudioWindow window;
        window.sourceId = 1;
        window.sourceKind = LS_SOURCE_KIND_MICROPHONE;
        window.sequenceNumber = 1;
        window.monotonicTimeNs = 0;
        window.sampleRateHz = wave.sampleRate;
        window.channelCount = wave.channels;
        window.frameCount = static_cast<std::uint32_t>(frames);
        window.samples = wave.samples;
        const auto peak = std::max_element(
            wave.samples.begin(),
            wave.samples.end(),
            [](float left, float right) {
                return std::abs(left) < std::abs(right);
            });
        std::cout << "input_frames=" << frames
                  << " peak="
                  << (peak == wave.samples.end() ? 0.0F : std::abs(*peak))
                  << '\n';

        const auto inferenceStarted = std::chrono::steady_clock::now();
        auto accepted = backend.accept(window);
        if (!accepted) {
            std::cerr << "inference failed: "
                      << accepted.error().message << '\n';
            return 4;
        }
        auto flushed = backend.flush();
        if (!flushed) {
            std::cerr << "flush failed: " << flushed.error().message << '\n';
            return 5;
        }
        accepted.value().insert(
            accepted.value().end(),
            std::make_move_iterator(flushed.value().begin()),
            std::make_move_iterator(flushed.value().end()));
        const auto inferenceSeconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - inferenceStarted)
                .count();
        const double audioSeconds =
            static_cast<double>(frames) / wave.sampleRate;
        if (accepted.value().empty()) {
            std::cerr << "ASR produced no final segments\n";
            return 6;
        }

        std::cout << "backend=" << backend.info().id
                  << " version=" << backend.info().version
                  << " prepare_ms=" << backendPrepareMilliseconds
                  << " sample_rate=" << wave.sampleRate
                  << " channels=" << wave.channels
                  << " inference_seconds=" << inferenceSeconds
                  << " real_time_factor="
                  << (audioSeconds == 0.0
                          ? 0.0
                          : inferenceSeconds / audioSeconds)
                  << '\n';
        for (const auto &segment : accepted.value()) {
            std::cout << '[' << segment.language << "] "
                      << segment.text << '\n';
        }
        std::cout << "final_segments=" << accepted.value().size() << '\n';
        if (std::getenv("LOCALSCRIBE_WHISPER_SMOKE_SKIP_VERTICAL")
            == nullptr) {
            runCoreVertical(
                argv[1],
                wave,
                accepted.value().size());
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 7;
    }
}
