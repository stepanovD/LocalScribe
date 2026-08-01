#include <LocalScribeCore/LocalScribeCore.h>

#include "../common/Expected.hpp"
#include "../common/TranscriptLanguagePolicy.hpp"
#include "../common/Types.hpp"
#include "../inference/AsrBackend.hpp"
#include "../inference/DiarizationBackend.hpp"
#include "../output/MarkdownRenderer.hpp"
#include "../session/SessionStateMachine.hpp"
#include "../storage/RecoveryJournal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace localscribe {
namespace {

template <typename T>
Expected<void> validateStruct(const T *value)
{
    if (value == nullptr) {
        return Error{LS_INVALID_ARGUMENT, "required structure is null"};
    }
    if (value->abi_version != LS_CORE_ABI_VERSION) {
        return Error{LS_INVALID_ABI_VERSION, "unsupported ABI version"};
    }
    if (value->struct_size < sizeof(T)) {
        return Error{LS_INVALID_STRUCT_SIZE, "structure is undersized"};
    }
    return success();
}

Expected<std::string> copyUtf8(
    const ls_utf8_view_v1 &view,
    std::size_t maximumBytes,
    bool rejectEmbeddedNul)
{
    auto valid = validateStruct(&view);
    if (!valid) {
        return valid.error();
    }
    if (view.size > maximumBytes) {
        return Error{LS_INVALID_ARGUMENT, "UTF-8 value is too large"};
    }
    if (view.size != 0 && view.data == nullptr) {
        return Error{LS_INVALID_ARGUMENT, "UTF-8 data is null"};
    }
    std::string result;
    if (view.size != 0) {
        result.assign(
            reinterpret_cast<const char *>(view.data),
            view.size);
    }
    if (rejectEmbeddedNul && result.find('\0') != std::string::npos) {
        return Error{LS_INVALID_ARGUMENT, "UTF-8 value contains NUL"};
    }
    return result;
}

ls_utf8_view_v1 makeView(const std::string &value)
{
    return ls_utf8_view_v1{
        sizeof(ls_utf8_view_v1),
        LS_CORE_ABI_VERSION,
        reinterpret_cast<const std::uint8_t *>(value.data()),
        value.size()};
}

void copyStableId(const StableId &source, ls_uuid_v1 &destination)
{
    destination.struct_size = sizeof(ls_uuid_v1);
    destination.abi_version = LS_CORE_ABI_VERSION;
    std::copy(source.begin(), source.end(), destination.bytes);
}

void clearError(ls_error_v1 *error)
{
    if (error == nullptr
        || error->struct_size < offsetof(ls_error_v1, message)
        || error->abi_version != LS_CORE_ABI_VERSION) {
        return;
    }
    error->code = LS_OK;
    error->reserved = 0;
    error->message_size = 0;
    if (error->struct_size >= sizeof(ls_error_v1)) {
        std::fill(
            std::begin(error->message),
            std::end(error->message),
            std::uint8_t{0});
    }
}

ls_status_code_t report(Error error, ls_error_v1 *outError = nullptr)
{
    if (outError != nullptr
        && outError->abi_version == LS_CORE_ABI_VERSION
        && outError->struct_size >= sizeof(ls_error_v1)) {
        outError->code = error.code;
        outError->reserved = 0;
        const std::size_t length =
            std::min<std::size_t>(
                error.message.size(),
                LS_ERROR_MESSAGE_CAPACITY - 1u);
        std::copy_n(
            reinterpret_cast<const std::uint8_t *>(error.message.data()),
            length,
            outError->message);
        outError->message[length] = 0;
        outError->message_size = length;
    }
    return error.code;
}

ls_status_code_t reportUnknown(ls_error_v1 *outError = nullptr)
{
    return report(
        Error{LS_INTERNAL_ERROR, "unexpected core exception"},
        outError);
}

ls_source_kind_t sourceKind(
    std::uint64_t sourceId,
    std::uint64_t microphoneSourceId,
    std::uint64_t systemAudioSourceId)
{
    if (sourceId == microphoneSourceId) {
        return LS_SOURCE_KIND_MICROPHONE;
    }
    if (sourceId == systemAudioSourceId) {
        return LS_SOURCE_KIND_SYSTEM_AUDIO;
    }
    return LS_SOURCE_KIND_UNKNOWN;
}

struct EventData {
    ls_event_kind_t kind{LS_EVENT_UNKNOWN};
    TranscriptSegment segment;
    PipelineMetrics metrics;
    ls_phase_t phase{LS_PHASE_UNKNOWN};
    ls_finalize_reason_t finalizeReason{LS_FINALIZE_REASON_UNKNOWN};
    SourceGap source;
    Error error;
};

struct SessionStateSnapshot {
    ls_phase_t phase{LS_PHASE_UNKNOWN};
    ls_published_status_t publishedStatus{LS_PUBLISHED_STATUS_UNKNOWN};
    ls_finalize_reason_t finalizeReason{LS_FINALIZE_REASON_UNKNOWN};
};

class CoreRuntime {
public:
    CoreRuntime(std::uint32_t flags, std::string primaryPath)
        : flags_(flags), primaryPath_(std::move(primaryPath))
    {
    }

    [[nodiscard]] bool allowTestBackends() const noexcept
    {
        return (flags_ & LS_CORE_CONFIG_ALLOW_TEST_BACKENDS) != 0;
    }

    [[nodiscard]] Expected<std::shared_ptr<RecoveryJournal>>
    journalFor(const std::string &requestedPath)
    {
        const std::string path =
            requestedPath.empty() ? primaryPath_ : requestedPath;
        if (path.empty()) {
            return Error{
                LS_INVALID_ARGUMENT,
                "a journal path is required"};
        }

        std::lock_guard lock(mutex_);
        const auto found = journals_.find(path);
        if (found != journals_.end()) {
            return found->second;
        }
        auto opened = RecoveryJournal::open(path);
        if (!opened) {
            return opened.error();
        }
        auto recoverable = opened.value()->markAndListRecoverableSessions();
        if (!recoverable) {
            return recoverable.error();
        }
        journals_.emplace(path, opened.value());
        if (primaryPath_.empty()) {
            primaryPath_ = path;
        }
        return opened.takeValue();
    }

    [[nodiscard]] Expected<std::shared_ptr<RecoveryJournal>> primaryJournal()
    {
        if (primaryPath_.empty()) {
            return Error{
                LS_INVALID_STATE,
                "core has no configured recovery journal"};
        }
        return journalFor(primaryPath_);
    }

private:
    std::uint32_t flags_{};
    std::string primaryPath_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<RecoveryJournal>> journals_;
};

class SessionRuntime
    : public std::enable_shared_from_this<SessionRuntime> {
public:
    static Expected<std::shared_ptr<SessionRuntime>> create(
        std::shared_ptr<RecoveryJournal> journal,
        SessionRecord record,
        std::unique_ptr<IAsrBackend> asr,
        std::unique_ptr<IDiarizationBackend> diarization,
        std::uint32_t queueCapacity)
    {
        std::vector<SourceRecord> sources;
        sources.push_back(SourceRecord{
            record.microphoneSourceId,
            LS_SOURCE_KIND_MICROPHONE,
            (record.requiredSourceMask & LS_REQUIRED_SOURCE_MICROPHONE) != 0,
            LS_SOURCE_HEALTH_READY});
        sources.push_back(SourceRecord{
            record.systemAudioSourceId,
            LS_SOURCE_KIND_SYSTEM_AUDIO,
            (record.requiredSourceMask & LS_REQUIRED_SOURCE_SYSTEM_AUDIO) != 0,
            LS_SOURCE_HEALTH_READY});
        auto created = journal->createSession(record, sources);
        if (!created) {
            return created.error();
        }

        auto runtime = std::shared_ptr<SessionRuntime>(
            new SessionRuntime(
                std::move(journal),
                std::move(record),
                std::move(asr),
                std::move(diarization),
                queueCapacity));
        runtime->enqueueStateEvent(
            LS_EVENT_STATE_CHANGED,
            LS_PHASE_PREPARING,
            LS_FINALIZE_REASON_UNKNOWN);
        runtime->startWorker();
        return runtime;
    }

    static Expected<std::shared_ptr<SessionRuntime>> recover(
        std::shared_ptr<RecoveryJournal> journal,
        SessionRecord record)
    {
        if (record.phase != LS_PHASE_RECOVERY_REQUIRED
            && record.phase != LS_PHASE_COMPLETE
            && record.phase != LS_PHASE_INCOMPLETE_SOURCES
            && record.phase != LS_PHASE_INTERRUPTED) {
            return Error{
                LS_INVALID_STATE,
                "session does not require recovery"};
        }
        auto runtime = std::shared_ptr<SessionRuntime>(
            new SessionRuntime(
                std::move(journal),
                std::move(record),
                nullptr,
                nullptr,
                1));
        runtime->journalCheckpoint_.store(
            runtime->record_.journalCheckpoint,
            std::memory_order_relaxed);
        runtime->enqueueStateEvent(
            LS_EVENT_STATE_CHANGED,
            runtime->record_.phase,
            LS_FINALIZE_REASON_PROCESS_INTERRUPTED);
        runtime->startWorker();
        return runtime;
    }

    ~SessionRuntime() { close(); }

    ls_status_code_t markSourcesReady()
    {
        std::lock_guard lifecycle(lifecycleMutex_);
        if (auto fatal = fatalErrorCopy(); fatal.has_value()) {
            return fatal->code;
        }
        return persistTransition(
            LS_PHASE_PREPARING,
            LS_PHASE_RECORDING,
            LS_FINALIZE_REASON_UNKNOWN,
            true,
            LS_EVENT_STATE_CHANGED);
    }

    ls_status_code_t pause()
    {
        std::lock_guard lifecycle(lifecycleMutex_);
        if (auto fatal = fatalErrorCopy(); fatal.has_value()) {
            return fatal->code;
        }
        const auto status = persistTransition(
            LS_PHASE_RECORDING,
            LS_PHASE_PAUSED,
            LS_FINALIZE_REASON_UNKNOWN,
            false,
            LS_EVENT_STATE_CHANGED);
        if (status != LS_OK) {
            return status;
        }
        waitUntilDrained();
        if (auto fatal = fatalErrorCopy(); fatal.has_value()) {
            return fatal->code;
        }
        auto recorded = recordLifecycleDiscontinuities("pause");
        return recorded ? LS_OK : recorded.error().code;
    }

    ls_status_code_t resumeAfterConsent()
    {
        std::lock_guard lifecycle(lifecycleMutex_);
        {
            std::lock_guard lock(stateMutex_);
            if (stateMachine_.phase() == LS_PHASE_RECOVERY_REQUIRED) {
                return LS_BACKEND_UNAVAILABLE;
            }
            if (fatalError_.has_value()) {
                return fatalError_->code;
            }
        }
        const auto status = persistTransition(
            LS_PHASE_PAUSED,
            LS_PHASE_RECORDING,
            LS_FINALIZE_REASON_UNKNOWN,
            true,
            LS_EVENT_STATE_CHANGED);
        if (status != LS_OK) {
            return status;
        }
        {
            std::lock_guard lock(stateMutex_);
            lastSequence_.clear();
            lastTimestamp_.clear();
            forceDiscontinuityBySource_.insert(record_.microphoneSourceId);
            forceDiscontinuityBySource_.insert(record_.systemAudioSourceId);
        }
        auto recorded = recordLifecycleDiscontinuities("resume");
        return recorded ? LS_OK : recorded.error().code;
    }

    ls_status_code_t push(const ls_audio_frame_v1 &frame)
    {
        framesOffered_.fetch_add(1, std::memory_order_relaxed);
        auto rejected = [this](ls_status_code_t status) {
            framesRejected_.fetch_add(1, std::memory_order_relaxed);
            return status;
        };

        if (frame.source_id == 0 || frame.sample_rate_hz == 0
            || frame.channel_count == 0 || frame.channel_count > 32
            || frame.sample_format != LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED
            || frame.frame_count == 0 || frame.samples == nullptr
            || (frame.flags
                & ~(LS_AUDIO_FLAG_DISCONTINUITY
                    | LS_AUDIO_FLAG_END_OF_STREAM))
                != 0) {
            return rejected(LS_AUDIO_FORMAT_ERROR);
        }
        if (frame.monotonic_time_ns < 0) {
            return rejected(LS_AUDIO_FORMAT_ERROR);
        }
        constexpr std::size_t kMaxSamplesPerFrame = 16u * 1024u * 1024u;
        if (frame.frame_count
            > kMaxSamplesPerFrame / frame.channel_count) {
            return rejected(LS_AUDIO_FORMAT_ERROR);
        }
        const std::size_t sampleCount =
            static_cast<std::size_t>(frame.frame_count)
            * frame.channel_count;
        for (std::size_t index = 0; index < sampleCount; ++index) {
            if (!std::isfinite(frame.samples[index])) {
                return rejected(LS_AUDIO_FORMAT_ERROR);
            }
        }
        const auto kind = sourceKind(
            frame.source_id,
            record_.microphoneSourceId,
            record_.systemAudioSourceId);
        if (kind == LS_SOURCE_KIND_UNKNOWN) {
            return rejected(LS_INVALID_ARGUMENT);
        }

        std::unique_lock lock(stateMutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            noteAtomicBackpressure(
                frame.source_id,
                frame.monotonic_time_ns);
            return LS_BACKPRESSURE;
        }
        absorbAtomicBackpressureLocked(frame.source_id);
        if (fatalError_.has_value()) {
            return rejected(fatalError_->code);
        }
        if (closed_ || stateMachine_.phase() != LS_PHASE_RECORDING
            || !acceptingAudio_) {
            return rejected(closed_ ? LS_CLOSED : LS_INVALID_STATE);
        }
        const auto sequence = lastSequence_.find(frame.source_id);
        if (sequence != lastSequence_.end()
            && frame.sequence_number <= sequence->second) {
            return rejected(LS_AUDIO_SEQUENCE_ERROR);
        }
        const auto timestamp = lastTimestamp_.find(frame.source_id);
        if (timestamp != lastTimestamp_.end()
            && frame.monotonic_time_ns < timestamp->second
            && (frame.flags & LS_AUDIO_FLAG_DISCONTINUITY) == 0) {
            return rejected(LS_AUDIO_SEQUENCE_ERROR);
        }
        const bool sequenceDiscontinuity =
            sequence != lastSequence_.end()
            && frame.sequence_number != sequence->second + 1u;
        const bool forcedDiscontinuity =
            forceDiscontinuityBySource_.erase(frame.source_id) != 0;
        const bool explicitDiscontinuity =
            (frame.flags & LS_AUDIO_FLAG_DISCONTINUITY) != 0;
        auto &overload = overloadBySource_[frame.source_id];
        const bool boundary = sequenceDiscontinuity
            || forcedDiscontinuity || explicitDiscontinuity
            || overload.active;

        const auto maximumAggregateFrames =
            std::max<std::uint64_t>(
                static_cast<std::uint64_t>(frame.sample_rate_hz) / 4u,
                frame.frame_count);
        AudioWindow *aggregate = nullptr;
        if (!boundary
            && (frame.flags & LS_AUDIO_FLAG_END_OF_STREAM) == 0) {
            const auto found = std::find_if(
                audioQueue_.rbegin(),
                audioQueue_.rend(),
                [&](const AudioWindow &candidate) {
                    return candidate.sourceId == frame.source_id;
                });
            if (found != audioQueue_.rend()
                && found->sampleRateHz == frame.sample_rate_hz
                && found->channelCount == frame.channel_count
                && (found->flags & LS_AUDIO_FLAG_END_OF_STREAM) == 0
                && static_cast<std::uint64_t>(found->frameCount)
                        + frame.frame_count
                    <= maximumAggregateFrames
                && queuedSamplesBySource_[frame.source_id] + sampleCount
                    <= kMaximumQueuedSamplesPerSource) {
                aggregate = &*found;
            }
        }

        if (aggregate == nullptr
            && (queueDepthBySource_[frame.source_id] >= queueCapacity_
                || queuedSamplesBySource_[frame.source_id] + sampleCount
                    > kMaximumQueuedSamplesPerSource)) {
            noteBackpressureLocked(
                frame.source_id,
                frame.monotonic_time_ns);
            workCondition_.notify_one();
            return LS_BACKPRESSURE;
        }

        if (aggregate != nullptr) {
            aggregate->samples.insert(
                aggregate->samples.end(),
                frame.samples,
                frame.samples + sampleCount);
            aggregate->frameCount += frame.frame_count;
            ++aggregate->callbackCount;
            queuedSamplesBySource_[frame.source_id] += sampleCount;
            lastSequence_[frame.source_id] = frame.sequence_number;
            lastTimestamp_[frame.source_id] = frame.monotonic_time_ns;
            latestMonotonicTimeNs_ =
                std::max(latestMonotonicTimeNs_, frame.monotonic_time_ns);
            framesAccepted_.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            workCondition_.notify_one();
            return LS_OK;
        }

        AudioWindow window;
        window.sourceId = frame.source_id;
        window.sourceKind = kind;
        window.sequenceNumber = frame.sequence_number;
        window.monotonicTimeNs = frame.monotonic_time_ns;
        window.sampleRateHz = frame.sample_rate_hz;
        window.channelCount = frame.channel_count;
        window.frameCount = frame.frame_count;
        window.flags = frame.flags;
        window.samples.assign(frame.samples, frame.samples + sampleCount);
        window.callbackCount = 1;

        if (boundary) {
            window.flags |= LS_AUDIO_FLAG_DISCONTINUITY;
            discontinuities_.fetch_add(1, std::memory_order_relaxed);
        }
        if (overload.active) {
            window.overloadGapBefore = true;
            window.overloadGapStartTimeNs = overload.startTimeNs;
            window.overloadGapEndTimeNs = std::max(
                overload.startTimeNs,
                overload.endTimeNs);
            window.rejectedCallbacksBefore = overload.rejectedCallbacks;
            overload = OverloadEpisode{};
        }
        lastSequence_[frame.source_id] = frame.sequence_number;
        lastTimestamp_[frame.source_id] = frame.monotonic_time_ns;
        latestMonotonicTimeNs_ =
            std::max(latestMonotonicTimeNs_, frame.monotonic_time_ns);
        audioQueue_.push_back(std::move(window));
        auto &depth = queueDepthBySource_[frame.source_id];
        ++depth;
        queuedSamplesBySource_[frame.source_id] += sampleCount;
        queueHighWater_ = std::max<std::uint32_t>(
            queueHighWater_,
            static_cast<std::uint32_t>(audioQueue_.size()));
        framesAccepted_.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        workCondition_.notify_one();
        return LS_OK;
    }

    ls_status_code_t sourceEvent(SourceGap event)
    {
        std::lock_guard lifecycle(lifecycleMutex_);
        {
            std::lock_guard lock(stateMutex_);
            if (closed_ || SessionStateMachine::isTerminal(
                               stateMachine_.phase())) {
                return closed_ ? LS_CLOSED : LS_INVALID_STATE;
            }
            if (fatalError_.has_value()) {
                return fatalError_->code;
            }
            if (sourceKind(
                    event.sourceId,
                    record_.microphoneSourceId,
                    record_.systemAudioSourceId)
                == LS_SOURCE_KIND_UNKNOWN) {
                return LS_INVALID_ARGUMENT;
            }
        }

        auto recorded =
            journal_->recordSourceEvent(record_.sessionId, event);
        if (!recorded) {
            latchFatal(recorded.error(), event.sourceId);
            return recorded.error().code;
        }
        journalCheckpoint_.store(
            recorded.value(),
            std::memory_order_relaxed);

        {
            std::lock_guard lock(stateMutex_);
            const bool required =
                (event.sourceKind == LS_SOURCE_KIND_MICROPHONE
                 && (record_.requiredSourceMask
                     & LS_REQUIRED_SOURCE_MICROPHONE)
                     != 0)
                || (event.sourceKind == LS_SOURCE_KIND_SYSTEM_AUDIO
                    && (record_.requiredSourceMask
                        & LS_REQUIRED_SOURCE_SYSTEM_AUDIO)
                        != 0);
            const bool thresholdExceeded =
                event.endTimeNs >= event.startTimeNs
                && event.endTimeNs - event.startTimeNs
                    > record_.completenessThresholdNs;
            if (required
                && (event.health == LS_SOURCE_HEALTH_PERMANENTLY_LOST
                    || thresholdExceeded)) {
                incompleteRequiredSource_ = true;
            }
        }
        enqueueSourceEvent(std::move(event));
        return LS_OK;
    }

    ls_status_code_t finalize(ls_finalize_reason_t reason)
    {
        if (reason < LS_FINALIZE_REASON_USER_STOP
            || reason > LS_FINALIZE_REASON_PROCESS_INTERRUPTED) {
            return LS_INVALID_ARGUMENT;
        }
        std::lock_guard lifecycle(lifecycleMutex_);
        ls_phase_t phase;
        {
            std::lock_guard lock(stateMutex_);
            phase = stateMachine_.phase();
            if (SessionStateMachine::isTerminal(phase)) {
                return LS_OK;
            }
            if (phase == LS_PHASE_PREPARING) {
                if (reason != LS_FINALIZE_REASON_CANCELLED) {
                    return LS_INVALID_STATE;
                }
            } else if (
                phase != LS_PHASE_RECORDING && phase != LS_PHASE_PAUSED
                && phase != LS_PHASE_RECOVERY_REQUIRED) {
                return LS_INVALID_STATE;
            }
        }
        if (phase == LS_PHASE_PREPARING) {
            return persistTransition(
                LS_PHASE_PREPARING,
                LS_PHASE_FAILED_TO_START,
                LS_FINALIZE_REASON_CANCELLED,
                false,
                LS_EVENT_TERMINAL);
        }
        const auto toFinalizing = persistTransition(
            phase,
            LS_PHASE_FINALIZING,
            reason,
            false,
            LS_EVENT_STATE_CHANGED);
        if (toFinalizing != LS_OK) {
            return toFinalizing;
        }
        bool abandonedForDeadline = false;
        if (!waitUntilDrainedFor(std::chrono::seconds(2))) {
            abandonedForDeadline = abandonForFinalizeDeadline();
            if (asr_ != nullptr) {
                asr_->requestAbort();
            }
            workCondition_.notify_all();
            (void)waitUntilDrainedFor(std::chrono::seconds(1));
        } else if (auto sealed = sealOpenOverloadEpisodes(); !sealed) {
            latchFatal(sealed.error());
        }

        if (!abandonedForDeadline && !fatalErrorCopy().has_value()) {
            auto flushed = flushAsrBounded();
            if (flushed.timedOut) {
                abandonedForDeadline = true;
                markAsrFlushDeadlineIncomplete();
            } else if (flushed.error.has_value()) {
                latchFatal(std::move(*flushed.error));
            } else {
                for (auto &hypothesis : flushed.hypotheses) {
                    AudioWindow source;
                    source.sourceId = hypothesis.sourceId;
                    source.sourceKind = sourceKind(
                        hypothesis.sourceId,
                        record_.microphoneSourceId,
                        record_.systemAudioSourceId);
                    if (!processHypotheses(
                            source,
                            {std::move(hypothesis)})) {
                        break;
                    }
                }
            }
        }
        if (!abandonedForDeadline && !fatalErrorCopy().has_value()) {
            try {
                if (diarization_ != nullptr) {
                    auto flushed = diarization_->flush();
                    if (!flushed) {
                        latchFatal(flushed.error());
                    }
                }
            } catch (const std::exception &) {
                latchFatal(
                    Error{
                        LS_INTERNAL_ERROR,
                        "diarization flush raised an exception"});
            } catch (...) {
                latchFatal(
                    Error{
                        LS_INTERNAL_ERROR,
                        "diarization flush raised an exception"});
            }
        }

        if (fatalErrorCopy().has_value()) {
            reason = LS_FINALIZE_REASON_PROCESS_INTERRUPTED;
            /*
             * A fatal latch asks the idle worker to exit. Waiting for the
             * explicit exit acknowledgement prevents backend destruction or
             * terminal publication racing a worker that is unwinding.
             */
            if (asr_ != nullptr) {
                asr_->requestAbort();
            }
            (void)waitUntilDrainedFor(std::chrono::seconds(1));
        }

        ls_phase_t terminal = LS_PHASE_COMPLETE;
        if (phase == LS_PHASE_RECOVERY_REQUIRED
            || reason == LS_FINALIZE_REASON_RECOVERY
            || reason == LS_FINALIZE_REASON_CANCELLED
            || reason == LS_FINALIZE_REASON_PROCESS_INTERRUPTED) {
            terminal = LS_PHASE_INTERRUPTED;
        } else {
            std::lock_guard lock(stateMutex_);
            if (incompleteRequiredSource_) {
                terminal = LS_PHASE_INCOMPLETE_SOURCES;
            }
        }
        return persistTransition(
            LS_PHASE_FINALIZING,
            terminal,
            reason,
            false,
            LS_EVENT_TERMINAL);
    }

    Expected<std::unique_ptr<EventData>> poll(std::uint32_t timeoutMs)
    {
        std::unique_lock lock(eventMutex_);
        const auto ready = [this] {
            return !events_.empty() || closedAtomic_.load();
        };
        if (timeoutMs == 0) {
            if (!ready()) {
                return Error{LS_TIMEOUT, "no event is available"};
            }
        } else if (!eventCondition_.wait_for(
                       lock,
                       std::chrono::milliseconds(timeoutMs),
                       ready)) {
            return Error{LS_TIMEOUT, "event poll timed out"};
        }
        if (!events_.empty()) {
            auto event = std::move(events_.front());
            events_.pop_front();
            return event;
        }
        return Error{LS_CLOSED, "session is closed"};
    }

    PipelineMetrics metrics() const
    {
        PipelineMetrics result;
        result.framesOffered =
            framesOffered_.load(std::memory_order_relaxed);
        result.framesAccepted =
            framesAccepted_.load(std::memory_order_relaxed);
        result.framesRejected =
            framesRejected_.load(std::memory_order_relaxed);
        result.discontinuities =
            discontinuities_.load(std::memory_order_relaxed);
        result.finalSegmentsCommitted =
            finalSegments_.load(std::memory_order_relaxed);
        result.partialEventsCoalesced =
            partialEventsCoalesced_.load(std::memory_order_relaxed);
        result.journalCheckpoint =
            journalCheckpoint_.load(std::memory_order_relaxed);
        result.highestSegmentRevision =
            highestSegmentRevision_.load(std::memory_order_relaxed);
        std::lock_guard lock(stateMutex_);
        result.audioQueueDepth =
            static_cast<std::uint32_t>(audioQueue_.size());
        result.audioQueueHighWater = queueHighWater_;
        return result;
    }

    SessionStateSnapshot state() const
    {
        std::lock_guard lock(stateMutex_);
        const auto phase = stateMachine_.phase();
        return SessionStateSnapshot{
            phase,
            SessionStateMachine::publishedStatus(phase),
            record_.finalizeReason};
    }

    Expected<RenderedMarkdown>
    render(const MarkdownRenderOptions &options)
    {
        {
            std::lock_guard lock(stateMutex_);
            if (fatalError_.has_value()
                && !SessionStateMachine::isTerminal(
                    stateMachine_.phase())) {
                return *fatalError_;
            }
        }
        /*
         * Live publication is a projection of committed state, not a barrier
         * on accepted inference work. Pause/finalize own their explicit drain;
         * recording renders must stay bounded while producers continue.
         */
        auto snapshot = journal_->snapshot(record_.sessionId);
        if (!snapshot) {
            return snapshot.error();
        }
        auto markdown = MarkdownRenderer::render(snapshot.value(), options);
        if (!markdown) {
            return markdown.error();
        }
        RenderedMarkdown result;
        result.bytes = markdown.takeValue();
        result.snapshot.journalCheckpoint =
            snapshot.value().session.journalCheckpoint;
        result.snapshot.highestSegmentRevision =
            snapshot.value().session.highestSegmentRevision;
        return result;
    }

    Expected<void>
    acknowledge(const PublicationReceipt &receipt)
    {
        return journal_->acknowledgePublication(
            record_.sessionId,
            receipt);
    }

    void close()
    {
        bool expected = false;
        if (!closedAtomic_.compare_exchange_strong(expected, true)) {
            return;
        }
        {
            std::lock_guard lock(stateMutex_);
            closed_ = true;
            acceptingAudio_ = false;
            stopWorker_ = true;
        }
        workCondition_.notify_all();
        drainedCondition_.notify_all();
        eventCondition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    static constexpr std::uint32_t kMaximumQueuedBlocksPerSource = 24;
    static constexpr std::size_t kMaximumQueuedSamplesPerSource =
        2u * 1024u * 1024u;

    struct OverloadEpisode {
        std::uint64_t rejectedCallbacks{};
        std::int64_t startTimeNs{};
        std::int64_t endTimeNs{};
        bool active{};
    };

    struct AtomicOverloadEpisode {
        std::atomic<std::uint64_t> rejectedCallbacks{0};
        std::atomic<std::int64_t> startTimeNs{0};
        std::atomic<std::int64_t> endTimeNs{0};
    };

    struct ProcessingWindowInfo {
        std::uint64_t sourceId{};
        ls_source_kind_t sourceKind{LS_SOURCE_KIND_UNKNOWN};
        std::uint64_t callbackCount{};
        std::int64_t startTimeNs{};
        std::int64_t endTimeNs{};
    };

    struct AsrFlushSharedState {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<Expected<std::vector<AsrHypothesis>>> result;
    };

    struct AsrFlushOutcome {
        std::vector<AsrHypothesis> hypotheses;
        std::optional<Error> error;
        bool timedOut{};
    };

    SessionRuntime(
        std::shared_ptr<RecoveryJournal> journal,
        SessionRecord record,
        std::unique_ptr<IAsrBackend> asr,
        std::unique_ptr<IDiarizationBackend> diarization,
        std::uint32_t queueCapacity)
        : journal_(std::move(journal)),
          record_(std::move(record)),
          stateMachine_(record_.phase),
          asr_(std::move(asr)),
          diarization_(std::move(diarization)),
          queueCapacity_(
              std::clamp<std::uint32_t>(
                  queueCapacity,
                  1u,
                  kMaximumQueuedBlocksPerSource))
    {
        journalCheckpoint_.store(
            record_.journalCheckpoint == 0 ? 1 : record_.journalCheckpoint,
            std::memory_order_relaxed);
        highestSegmentRevision_.store(
            record_.highestSegmentRevision,
            std::memory_order_relaxed);
    }

    void startWorker()
    {
        worker_ = std::thread([self = shared_from_this()] {
            self->workerLoop();
        });
    }

    [[nodiscard]] AtomicOverloadEpisode &
    atomicOverloadFor(std::uint64_t sourceId) noexcept
    {
        return sourceId == record_.microphoneSourceId
            ? atomicOverload_[0]
            : atomicOverload_[1];
    }

    void noteAtomicBackpressure(
        std::uint64_t sourceId,
        std::int64_t timeNs) noexcept
    {
        framesRejected_.fetch_add(1, std::memory_order_relaxed);
        auto &episode = atomicOverloadFor(sourceId);
        const auto previous = episode.rejectedCallbacks.fetch_add(
            1,
            std::memory_order_acq_rel);
        if (previous == 0) {
            episode.startTimeNs.store(timeNs, std::memory_order_release);
        }
        episode.endTimeNs.store(timeNs, std::memory_order_release);
    }

    void absorbAtomicBackpressureLocked(std::uint64_t sourceId)
    {
        auto &pending = atomicOverloadFor(sourceId);
        const auto count = pending.rejectedCallbacks.exchange(
            0,
            std::memory_order_acq_rel);
        if (count == 0) {
            return;
        }
        const auto start =
            pending.startTimeNs.load(std::memory_order_acquire);
        const auto end =
            pending.endTimeNs.load(std::memory_order_acquire);
        auto &episode = overloadBySource_[sourceId];
        if (!episode.active) {
            episode.active = true;
            episode.startTimeNs = start;
            episode.endTimeNs = end;
        } else {
            episode.startTimeNs = std::min(episode.startTimeNs, start);
            episode.endTimeNs = std::max(episode.endTimeNs, end);
        }
        episode.rejectedCallbacks += count;
    }

    void noteBackpressureLocked(
        std::uint64_t sourceId,
        std::int64_t timeNs)
    {
        framesRejected_.fetch_add(1, std::memory_order_relaxed);
        auto &episode = overloadBySource_[sourceId];
        if (!episode.active) {
            episode.active = true;
            episode.startTimeNs = timeNs;
        }
        episode.endTimeNs = timeNs;
        ++episode.rejectedCallbacks;
    }

    [[nodiscard]] std::optional<Error> fatalErrorCopy() const
    {
        std::lock_guard lock(stateMutex_);
        return fatalError_;
    }

    [[nodiscard]] bool hasFatalError() const
    {
        std::lock_guard lock(stateMutex_);
        return fatalError_.has_value();
    }

    /*
     * First fatal data-path failure wins. Frames still waiting in the queue
     * are reclassified as rejected, and their durable counters are updated
     * in one bounded write per source when the journal is still usable.
     * A source discontinuity makes the data-loss boundary visible in a
     * recovery snapshot. All persistence here is best effort: a journal
     * failure is itself a valid reason for this latch.
     */
    void latchFatal(
        Error error,
        std::uint64_t failureSourceId = 0,
        std::uint64_t provisionalSourceId = 0) noexcept
    {
        std::uint64_t microphoneAbandoned = 0;
        std::uint64_t systemAbandoned = 0;
        std::uint64_t microphonePendingRejected = 0;
        std::uint64_t systemPendingRejected = 0;
        std::int64_t failureTimeNs = 0;
        bool firstFailure = false;
        {
            std::lock_guard lock(stateMutex_);
            if (fatalError_.has_value()) {
                return;
            }
            fatalError_.emplace(std::move(error));
            firstFailure = true;
            acceptingAudio_ = false;
            stopWorker_ = true;
            failureTimeNs = latestMonotonicTimeNs_;
            absorbAtomicBackpressureLocked(record_.microphoneSourceId);
            absorbAtomicBackpressureLocked(record_.systemAudioSourceId);

            const auto countAbandoned =
                [&](std::uint64_t sourceId, std::uint64_t count) {
                if (sourceId == record_.microphoneSourceId) {
                    microphoneAbandoned += count;
                } else if (sourceId == record_.systemAudioSourceId) {
                    systemAbandoned += count;
                }
            };
            for (const auto &window : audioQueue_) {
                countAbandoned(window.sourceId, window.callbackCount);
                if (window.sourceId == record_.microphoneSourceId) {
                    microphonePendingRejected +=
                        window.rejectedCallbacksBefore;
                } else if (
                    window.sourceId == record_.systemAudioSourceId) {
                    systemPendingRejected +=
                        window.rejectedCallbacksBefore;
                }
            }
            if (provisionalSourceId != 0) {
                countAbandoned(provisionalSourceId, 1);
            }
            if (const auto found = overloadBySource_.find(
                    record_.microphoneSourceId);
                found != overloadBySource_.end() && found->second.active) {
                microphonePendingRejected +=
                    found->second.rejectedCallbacks;
            }
            if (const auto found = overloadBySource_.find(
                    record_.systemAudioSourceId);
                found != overloadBySource_.end() && found->second.active) {
                systemPendingRejected += found->second.rejectedCallbacks;
            }

            audioQueue_.clear();
            queueDepthBySource_.clear();
            queuedSamplesBySource_.clear();
            overloadBySource_.clear();
        }

        if (!firstFailure) {
            return;
        }

        const auto abandoned =
            microphoneAbandoned + systemAbandoned;
        if (abandoned != 0) {
            framesAccepted_.fetch_sub(
                abandoned,
                std::memory_order_relaxed);
            framesRejected_.fetch_add(
                abandoned,
                std::memory_order_relaxed);
        }

        workCondition_.notify_all();
        drainedCondition_.notify_all();

        std::optional<Error> fatal;
        try {
            fatal = fatalErrorCopy();
            if (fatal.has_value()) {
                enqueueError(*fatal);
            }
        } catch (...) {
            /* Error delivery is best effort; the latch remains authoritative. */
        }

        const auto persistRejected =
            [&](std::uint64_t sourceId, std::uint64_t count) {
                if (count == 0) {
                    return;
                }
                try {
                    (void)journal_->recordFramesRejected(
                        record_.sessionId,
                        sourceId,
                        count,
                        true);
                } catch (...) {
                }
            };
        persistRejected(
            record_.microphoneSourceId,
            microphoneAbandoned + microphonePendingRejected);
        persistRejected(
            record_.systemAudioSourceId,
            systemAbandoned + systemPendingRejected);

        const bool allSources = failureSourceId == 0;
        const bool microphoneAffected =
            allSources
            || failureSourceId == record_.microphoneSourceId
            || microphoneAbandoned != 0
            || microphonePendingRejected != 0;
        const bool systemAffected =
            allSources
            || failureSourceId == record_.systemAudioSourceId
            || systemAbandoned != 0
            || systemPendingRejected != 0;

        const auto persistFatalGap =
            [&](std::uint64_t sourceId, ls_source_kind_t kind) {
                SourceGap gap;
                gap.sourceId = sourceId;
                gap.sourceKind = kind;
                gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
                gap.health = LS_SOURCE_HEALTH_ACTIVE;
                gap.startTimeNs = failureTimeNs;
                gap.endTimeNs = failureTimeNs;
                try {
                    gap.reason = "fatal pipeline failure";
                    if (fatal.has_value() && !fatal->message.empty()) {
                        gap.reason += ": " + fatal->message;
                    }
                    auto checkpoint = journal_->recordSourceEvent(
                        record_.sessionId,
                        gap);
                    if (checkpoint) {
                        journalCheckpoint_.store(
                            checkpoint.value(),
                            std::memory_order_relaxed);
                        enqueueSourceEvent(std::move(gap));
                    }
                } catch (...) {
                }
            };
        if (microphoneAffected) {
            persistFatalGap(
                record_.microphoneSourceId,
                LS_SOURCE_KIND_MICROPHONE);
        }
        if (systemAffected) {
            persistFatalGap(
                record_.systemAudioSourceId,
                LS_SOURCE_KIND_SYSTEM_AUDIO);
        }
    }

    void workerLoop() noexcept
    {
        try {
            for (;;) {
                {
                    std::unique_lock lock(stateMutex_);
                    workCondition_.wait(lock, [this] {
                        return stopWorker_ || fatalError_.has_value()
                            || !audioQueue_.empty();
                    });
                    if ((stopWorker_ || fatalError_.has_value())
                        && audioQueue_.empty()) {
                        break;
                    }
                }

                bool failed = false;
                AudioWindow window;
                bool hasWindow = false;
                {
                    std::lock_guard lock(stateMutex_);
                    if (fatalError_.has_value()) {
                        break;
                    }
                    if (!audioQueue_.empty()) {
                        window = std::move(audioQueue_.front());
                        audioQueue_.pop_front();
                        auto &depth = queueDepthBySource_[window.sourceId];
                        if (depth > 0) {
                            --depth;
                        }
                        auto &queuedSamples =
                            queuedSamplesBySource_[window.sourceId];
                        queuedSamples = queuedSamples >= window.samples.size()
                            ? queuedSamples - window.samples.size()
                            : 0;
                        processing_ = true;
                        ProcessingWindowInfo info;
                        info.sourceId = window.sourceId;
                        info.sourceKind = window.sourceKind;
                        info.callbackCount = window.callbackCount;
                        info.startTimeNs = window.monotonicTimeNs;
                        const auto durationNs =
                            window.sampleRateHz == 0
                            ? 0
                            : static_cast<std::int64_t>(
                                  static_cast<long double>(window.frameCount)
                                  * 1'000'000'000.0L
                                  / window.sampleRateHz);
                        info.endTimeNs = window.monotonicTimeNs + durationNs;
                        processingWindow_ = info;
                        hasWindow = true;
                    }
                }

                if (hasWindow) {
                    auto accepted = journal_->recordFramesAccepted(
                        record_.sessionId,
                        window.sourceId,
                        window.callbackCount);
                    if (!accepted) {
                        latchFatal(
                            accepted.error(),
                            window.sourceId,
                            window.sourceId);
                        failed = true;
                    }

                    if (!failed && window.rejectedCallbacksBefore != 0) {
                        auto recorded = journal_->recordFramesRejected(
                            record_.sessionId,
                            window.sourceId,
                            window.rejectedCallbacksBefore,
                            false);
                        if (!recorded) {
                            latchFatal(recorded.error(), window.sourceId);
                            failed = true;
                        }
                    }

                    if (!failed
                        && (window.flags & LS_AUDIO_FLAG_DISCONTINUITY) != 0) {
                        SourceGap gap;
                        gap.sourceId = window.sourceId;
                        gap.sourceKind = window.sourceKind;
                        gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
                        gap.health = LS_SOURCE_HEALTH_ACTIVE;
                        gap.startTimeNs = window.overloadGapBefore
                            ? window.overloadGapStartTimeNs
                            : window.monotonicTimeNs;
                        gap.endTimeNs = window.overloadGapBefore
                            ? window.overloadGapEndTimeNs
                            : window.monotonicTimeNs;
                        gap.reason = window.overloadGapBefore
                            ? "backpressure overload episode"
                            : "audio sequence discontinuity";
                        auto checkpoint = journal_->recordSourceEvent(
                            record_.sessionId,
                            gap);
                        if (checkpoint) {
                            journalCheckpoint_.store(
                                checkpoint.value(),
                                std::memory_order_relaxed);
                            enqueueSourceEvent(std::move(gap));
                        } else {
                            latchFatal(
                                checkpoint.error(),
                                window.sourceId);
                            failed = true;
                        }
                    }
                    if (!failed) {
                        failed = !processWindow(window);
                    }
                    {
                        std::lock_guard lock(stateMutex_);
                        processing_ = false;
                        processingWindow_.reset();
                        drainedCondition_.notify_all();
                    }
                }
                enqueueMetrics();
                if (failed) {
                    break;
                }
            }
        } catch (const std::exception &) {
            latchFatal(
                Error{LS_INTERNAL_ERROR, "inference worker exception"});
        } catch (...) {
            latchFatal(
                Error{LS_INTERNAL_ERROR, "inference worker exception"});
        }
        {
            std::lock_guard lock(stateMutex_);
            audioQueue_.clear();
            queueDepthBySource_.clear();
            queuedSamplesBySource_.clear();
            processing_ = false;
            processingWindow_.reset();
            workerExited_ = true;
        }
        drainedCondition_.notify_all();
        workCondition_.notify_all();
        eventCondition_.notify_all();
    }

    [[nodiscard]] bool processWindow(const AudioWindow &window)
    {
        if (asr_ == nullptr || diarization_ == nullptr) {
            return true;
        }
        auto hypotheses = asr_->accept(window);
        if (!hypotheses) {
            if (shouldDiscardProcessing()) {
                return true;
            }
            latchFatal(hypotheses.error(), window.sourceId);
            return false;
        }
        if (shouldDiscardProcessing()) {
            return true;
        }
        if (hasFatalError()) {
            return false;
        }
        return processHypotheses(
            window,
            std::move(hypotheses.value()));
    }

    [[nodiscard]] bool shouldDiscardProcessing() const
    {
        std::lock_guard lock(stateMutex_);
        return discardProcessingResults_;
    }

    [[nodiscard]] bool processHypotheses(
        const AudioWindow &window,
        std::vector<AsrHypothesis> hypotheses)
    {
        if (hypotheses.empty()) {
            return true;
        }
        if (diarization_ == nullptr) {
            latchFatal(
                Error{
                    LS_BACKEND_UNAVAILABLE,
                    "diarization unavailable"},
                window.sourceId);
            return false;
        }
        if (hasFatalError()) {
            return false;
        }
        auto turns = diarization_->assign(window, hypotheses);
        if (!turns) {
            latchFatal(turns.error(), window.sourceId);
            return false;
        }
        if (hasFatalError()) {
            return false;
        }
        for (const auto &hypothesis : hypotheses) {
            if (hasFatalError()) {
                return false;
            }
            if (!hypothesis.final) {
                partialEventsCoalesced_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                continue;
            }
            const auto turn = std::find_if(
                turns.value().begin(),
                turns.value().end(),
                [&](const SpeakerTurn &candidate) {
                    return candidate.stableId == hypothesis.stableId
                        && candidate.revision == hypothesis.revision;
                });
            if (turn == turns.value().end()) {
                latchFatal(
                    Error{
                        LS_BACKEND_FAILURE,
                        "diarization omitted a final hypothesis"},
                    hypothesis.sourceId);
                return false;
            }
            TranscriptSegment segment;
            segment.stableId = hypothesis.stableId;
            segment.sourceId = hypothesis.sourceId;
            segment.startTimeNs = hypothesis.startTimeNs;
            segment.endTimeNs = hypothesis.endTimeNs;
            const auto segmentSourceKind = sourceKind(
                hypothesis.sourceId,
                record_.microphoneSourceId,
                record_.systemAudioSourceId);
            if (segmentSourceKind == LS_SOURCE_KIND_MICROPHONE) {
                /*
                 * Source ownership is a product invariant, not a clustering
                 * suggestion: all microphone speech belongs to the configured
                 * local participant.
                 */
                segment.speakerId = 1;
                segment.speakerLabel = record_.localSpeakerName;
            } else if (
                segmentSourceKind == LS_SOURCE_KIND_SYSTEM_AUDIO) {
                /*
                 * Reserve speaker ID 1 and the local label for microphone
                 * audio even if a future diarization backend misbehaves.
                 */
                segment.speakerId =
                    turn->speakerId == 1
                    ? (0x8000000000000000ULL | hypothesis.sourceId)
                    : turn->speakerId;
                segment.speakerLabel =
                    turn->speakerLabel.empty()
                        || turn->speakerLabel
                            == record_.localSpeakerName
                    ? (record_.localSpeakerName == "Speaker 1"
                           ? "Remote Speaker 1"
                           : "Speaker 1")
                    : turn->speakerLabel;
            } else {
                latchFatal(
                    Error{
                        LS_BACKEND_FAILURE,
                        "ASR produced a final for an unknown source"},
                    hypothesis.sourceId);
                return false;
            }
            segment.text = hypothesis.text;
            const auto previousLanguage =
                lastLanguageBySource_.find(hypothesis.sourceId);
            segment.language = TranscriptLanguagePolicy::select(
                record_.languageMode,
                segment.text,
                hypothesis.language,
                previousLanguage == lastLanguageBySource_.end()
                    ? std::string_view{}
                    : std::string_view(previousLanguage->second));
            lastLanguageBySource_[hypothesis.sourceId] =
                segment.language;
            segment.confidence =
                std::clamp(
                    hypothesis.confidence * turn->confidence,
                    0.0F,
                    1.0F);
            segment.revision = hypothesis.revision;
            segment.flags = LS_SEGMENT_FLAG_FINAL
                | (hypothesis.unintelligible
                       ? LS_SEGMENT_FLAG_UNINTELLIGIBLE
                       : 0u);
            auto committed = journal_->appendFinalSegment(
                record_.sessionId,
                segment);
            if (!committed) {
                latchFatal(committed.error(), segment.sourceId);
                return false;
            }
            segment.journalCheckpoint = committed.value();
            journalCheckpoint_.store(
                committed.value(),
                std::memory_order_relaxed);
            auto observed =
                highestSegmentRevision_.load(std::memory_order_relaxed);
            while (observed < segment.revision
                   && !highestSegmentRevision_.compare_exchange_weak(
                       observed,
                       segment.revision,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
            finalSegments_.fetch_add(1, std::memory_order_relaxed);
            enqueueSegmentEvent(std::move(segment));
        }
        return true;
    }

    ls_status_code_t persistTransition(
        ls_phase_t expected,
        ls_phase_t next,
        ls_finalize_reason_t reason,
        bool accepting,
        ls_event_kind_t eventKind)
    {
        std::unique_lock lock(stateMutex_);
        if (closed_) {
            return LS_CLOSED;
        }
        if (stateMachine_.phase() != expected) {
            return LS_INVALID_STATE;
        }
        if (!SessionStateMachine::isLegal(expected, next)) {
            return LS_INVALID_STATE;
        }
        if (accepting && fatalError_.has_value()) {
            return fatalError_->code;
        }
        acceptingAudio_ = false;
        auto checkpoint = journal_->transition(
            record_.sessionId,
            expected,
            next,
            reason);
        if (!checkpoint) {
            return checkpoint.error().code;
        }
        auto transitioned = stateMachine_.transition(next);
        if (!transitioned) {
            return transitioned.error().code;
        }
        record_.phase = next;
        record_.finalizeReason = reason;
        record_.journalCheckpoint = checkpoint.value();
        journalCheckpoint_.store(
            checkpoint.value(),
            std::memory_order_relaxed);
        acceptingAudio_ = accepting;
        lock.unlock();
        enqueueStateEvent(eventKind, next, reason);
        return LS_OK;
    }

    void waitUntilDrained()
    {
        std::unique_lock lock(stateMutex_);
        drainedCondition_.wait(lock, [this] {
            if (fatalError_.has_value()) {
                return workerExited_;
            }
            return audioQueue_.empty() && !processing_;
        });
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool waitUntilDrainedFor(
        const std::chrono::duration<Rep, Period> &timeout)
    {
        std::unique_lock lock(stateMutex_);
        return drainedCondition_.wait_for(lock, timeout, [this] {
            if (fatalError_.has_value()) {
                return workerExited_;
            }
            return audioQueue_.empty() && !processing_;
        });
    }

    [[nodiscard]] AsrFlushOutcome flushAsrBounded()
    {
        AsrFlushOutcome outcome;
        if (asr_ == nullptr) {
            return outcome;
        }

        auto state = std::make_shared<AsrFlushSharedState>();
        auto self = shared_from_this();
        std::thread([self = std::move(self), state] {
            Expected<std::vector<AsrHypothesis>> result =
                Error{LS_INTERNAL_ERROR, "ASR flush did not run"};
            try {
                result = self->asr_->flush();
            } catch (const std::exception &) {
                result = Error{
                    LS_INTERNAL_ERROR,
                    "ASR flush raised an exception"};
            } catch (...) {
                result = Error{
                    LS_INTERNAL_ERROR,
                    "ASR flush raised an exception"};
            }
            {
                std::lock_guard lock(state->mutex);
                state->result.emplace(std::move(result));
            }
            state->condition.notify_all();
        }).detach();

        std::unique_lock lock(state->mutex);
        const auto completed = [&] {
            return state->result.has_value();
        };
        if (!state->condition.wait_for(
                lock,
                std::chrono::seconds(2),
                completed)) {
            outcome.timedOut = true;
            lock.unlock();
            asr_->requestAbort();
            lock.lock();
            (void)state->condition.wait_for(
                lock,
                std::chrono::seconds(1),
                completed);
        }
        if (outcome.timedOut || !state->result.has_value()) {
            return outcome;
        }

        auto result = std::move(*state->result);
        if (!result) {
            outcome.error = result.error();
        } else {
            outcome.hypotheses = result.takeValue();
        }
        return outcome;
    }

    void markAsrFlushDeadlineIncomplete()
    {
        std::int64_t boundaryTimeNs = 0;
        {
            std::lock_guard lock(stateMutex_);
            boundaryTimeNs = latestMonotonicTimeNs_;
            incompleteRequiredSource_ = true;
            discardProcessingResults_ = true;
            stopWorker_ = true;
        }
        workCondition_.notify_all();

        const std::array<std::pair<std::uint64_t, ls_source_kind_t>, 2>
            sources{
                std::pair{
                    record_.microphoneSourceId,
                    LS_SOURCE_KIND_MICROPHONE},
                std::pair{
                    record_.systemAudioSourceId,
                    LS_SOURCE_KIND_SYSTEM_AUDIO}};
        for (const auto &[sourceId, kind] : sources) {
            SourceGap gap;
            gap.sourceId = sourceId;
            gap.sourceKind = kind;
            gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
            gap.health = LS_SOURCE_HEALTH_ACTIVE;
            gap.startTimeNs = boundaryTimeNs;
            gap.endTimeNs = boundaryTimeNs;
            gap.reason =
                "finalization deadline: ASR tail was not flushed";
            auto persisted = persistInternalGap(std::move(gap));
            if (!persisted) {
                latchFatal(persisted.error(), sourceId);
            } else {
                discontinuities_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] Expected<void> persistInternalGap(SourceGap gap)
    {
        auto checkpoint =
            journal_->recordSourceEvent(record_.sessionId, gap);
        if (!checkpoint) {
            return checkpoint.error();
        }
        journalCheckpoint_.store(
            checkpoint.value(),
            std::memory_order_relaxed);
        enqueueSourceEvent(std::move(gap));
        return success();
    }

    [[nodiscard]] Expected<void> sealOpenOverloadEpisodes()
    {
        std::array<OverloadEpisode, 2> episodes;
        const std::array<std::uint64_t, 2> sourceIds{
            record_.microphoneSourceId,
            record_.systemAudioSourceId};
        {
            std::lock_guard lock(stateMutex_);
            absorbAtomicBackpressureLocked(sourceIds[0]);
            absorbAtomicBackpressureLocked(sourceIds[1]);
            for (std::size_t index = 0; index < sourceIds.size(); ++index) {
                auto &episode = overloadBySource_[sourceIds[index]];
                episodes[index] = episode;
                episode = OverloadEpisode{};
            }
        }

        for (std::size_t index = 0; index < sourceIds.size(); ++index) {
            const auto &episode = episodes[index];
            if (!episode.active || episode.rejectedCallbacks == 0) {
                continue;
            }
            auto recorded = journal_->recordFramesRejected(
                record_.sessionId,
                sourceIds[index],
                episode.rejectedCallbacks,
                false);
            if (!recorded) {
                return recorded.error();
            }
            SourceGap gap;
            gap.sourceId = sourceIds[index];
            gap.sourceKind = index == 0
                ? LS_SOURCE_KIND_MICROPHONE
                : LS_SOURCE_KIND_SYSTEM_AUDIO;
            gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
            gap.health = LS_SOURCE_HEALTH_ACTIVE;
            gap.startTimeNs = episode.startTimeNs;
            gap.endTimeNs =
                std::max(episode.startTimeNs, episode.endTimeNs);
            gap.reason = "backpressure overload episode";
            auto persisted = persistInternalGap(std::move(gap));
            if (!persisted) {
                return persisted.error();
            }
            discontinuities_.fetch_add(1, std::memory_order_relaxed);
        }
        return success();
    }

    [[nodiscard]] bool abandonForFinalizeDeadline()
    {
        struct SourceLoss {
            std::uint64_t rejectedCallbacks{};
            std::int64_t startTimeNs{
                std::numeric_limits<std::int64_t>::max()};
            std::int64_t endTimeNs{};
            bool hasFinalizationLoss{};
        };

        const std::array<std::uint64_t, 2> sourceIds{
            record_.microphoneSourceId,
            record_.systemAudioSourceId};
        std::array<SourceLoss, 2> losses;
        std::vector<SourceGap> overloadGaps;
        std::uint64_t abandonedCallbacks = 0;
        std::uint64_t newDiscontinuities = 0;

        const auto sourceIndex = [&](std::uint64_t sourceId) {
            return sourceId == sourceIds[0] ? std::size_t{0}
                                           : std::size_t{1};
        };
        const auto kindForIndex = [](std::size_t index) {
            return index == 0 ? LS_SOURCE_KIND_MICROPHONE
                              : LS_SOURCE_KIND_SYSTEM_AUDIO;
        };
        const auto includeFinalizationLoss =
            [&](std::size_t index, std::int64_t start, std::int64_t end) {
                auto &loss = losses[index];
                loss.hasFinalizationLoss = true;
                loss.startTimeNs = std::min(loss.startTimeNs, start);
                loss.endTimeNs = std::max(loss.endTimeNs, std::max(start, end));
            };

        {
            std::lock_guard lock(stateMutex_);
            absorbAtomicBackpressureLocked(sourceIds[0]);
            absorbAtomicBackpressureLocked(sourceIds[1]);

            for (const auto &window : audioQueue_) {
                const auto index = sourceIndex(window.sourceId);
                abandonedCallbacks += window.callbackCount;
                losses[index].rejectedCallbacks +=
                    window.callbackCount + window.rejectedCallbacksBefore;
                includeFinalizationLoss(
                    index,
                    window.monotonicTimeNs,
                    window.monotonicTimeNs);
                if (window.overloadGapBefore) {
                    SourceGap gap;
                    gap.sourceId = window.sourceId;
                    gap.sourceKind = window.sourceKind;
                    gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
                    gap.health = LS_SOURCE_HEALTH_ACTIVE;
                    gap.startTimeNs = window.overloadGapStartTimeNs;
                    gap.endTimeNs = std::max(
                        window.overloadGapStartTimeNs,
                        window.overloadGapEndTimeNs);
                    gap.reason = "backpressure overload episode";
                    overloadGaps.push_back(std::move(gap));
                }
            }

            for (std::size_t index = 0; index < sourceIds.size(); ++index) {
                const auto found = overloadBySource_.find(sourceIds[index]);
                if (found == overloadBySource_.end()
                    || !found->second.active) {
                    continue;
                }
                const auto &episode = found->second;
                losses[index].rejectedCallbacks +=
                    episode.rejectedCallbacks;
                SourceGap gap;
                gap.sourceId = sourceIds[index];
                gap.sourceKind = kindForIndex(index);
                gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
                gap.health = LS_SOURCE_HEALTH_ACTIVE;
                gap.startTimeNs = episode.startTimeNs;
                gap.endTimeNs =
                    std::max(episode.startTimeNs, episode.endTimeNs);
                gap.reason = "backpressure overload episode";
                overloadGaps.push_back(std::move(gap));
                ++newDiscontinuities;
            }
            if (processingWindow_.has_value()) {
                const auto index =
                    sourceIndex(processingWindow_->sourceId);
                includeFinalizationLoss(
                    index,
                    processingWindow_->startTimeNs,
                    processingWindow_->endTimeNs);
            }

            audioQueue_.clear();
            queueDepthBySource_.clear();
            queuedSamplesBySource_.clear();
            overloadBySource_.clear();
            discardProcessingResults_ = true;
            stopWorker_ = true;
            incompleteRequiredSource_ = true;
        }

        if (abandonedCallbacks != 0) {
            framesAccepted_.fetch_sub(
                abandonedCallbacks,
                std::memory_order_relaxed);
            framesRejected_.fetch_add(
                abandonedCallbacks,
                std::memory_order_relaxed);
        }

        for (std::size_t index = 0; index < sourceIds.size(); ++index) {
            if (losses[index].rejectedCallbacks != 0) {
                auto recorded = journal_->recordFramesRejected(
                    record_.sessionId,
                    sourceIds[index],
                    losses[index].rejectedCallbacks,
                    false);
                if (!recorded) {
                    latchFatal(recorded.error(), sourceIds[index]);
                    continue;
                }
            }
        }
        for (auto &gap : overloadGaps) {
            auto persisted = persistInternalGap(std::move(gap));
            if (!persisted) {
                latchFatal(persisted.error());
            }
        }

        for (std::size_t index = 0; index < sourceIds.size(); ++index) {
            if (!losses[index].hasFinalizationLoss) {
                continue;
            }
            SourceGap gap;
            gap.sourceId = sourceIds[index];
            gap.sourceKind = kindForIndex(index);
            gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
            gap.health = LS_SOURCE_HEALTH_ACTIVE;
            gap.startTimeNs = losses[index].startTimeNs;
            gap.endTimeNs = std::max(
                losses[index].startTimeNs,
                losses[index].endTimeNs);
            gap.reason =
                "finalization deadline: accepted audio was not transcribed";
            auto persisted = persistInternalGap(std::move(gap));
            if (!persisted) {
                latchFatal(persisted.error(), sourceIds[index]);
            }
            ++newDiscontinuities;
        }
        if (newDiscontinuities != 0) {
            discontinuities_.fetch_add(
                newDiscontinuities,
                std::memory_order_relaxed);
        }
        drainedCondition_.notify_all();
        return true;
    }

    [[nodiscard]] Expected<void>
    recordLifecycleDiscontinuities(std::string reason)
    {
        std::int64_t boundaryTimeNs = 0;
        {
            std::lock_guard lock(stateMutex_);
            boundaryTimeNs = latestMonotonicTimeNs_;
        }
        std::array<std::pair<std::uint64_t, ls_source_kind_t>, 2> sources{
            std::pair{
                record_.microphoneSourceId,
                LS_SOURCE_KIND_MICROPHONE},
            std::pair{
                record_.systemAudioSourceId,
                LS_SOURCE_KIND_SYSTEM_AUDIO}};
        for (const auto &[sourceId, kind] : sources) {
            SourceGap gap;
            gap.sourceId = sourceId;
            gap.sourceKind = kind;
            gap.eventKind = LS_SOURCE_EVENT_DISCONTINUITY;
            gap.health = LS_SOURCE_HEALTH_READY;
            gap.startTimeNs = boundaryTimeNs;
            gap.endTimeNs = boundaryTimeNs;
            gap.reason = reason;
            auto checkpoint =
                journal_->recordSourceEvent(record_.sessionId, gap);
            if (checkpoint) {
                journalCheckpoint_.store(
                    checkpoint.value(),
                    std::memory_order_relaxed);
                enqueueSourceEvent(std::move(gap));
            } else {
                latchFatal(checkpoint.error(), sourceId);
                return checkpoint.error();
            }
        }
        return success();
    }

    void enqueue(std::unique_ptr<EventData> event)
    {
        {
            std::lock_guard lock(eventMutex_);
            if (event->kind == LS_EVENT_METRICS && !events_.empty()
                && events_.back()->kind == LS_EVENT_METRICS) {
                events_.back() = std::move(event);
            } else {
                events_.push_back(std::move(event));
            }
        }
        eventCondition_.notify_one();
    }

    void enqueueStateEvent(
        ls_event_kind_t kind,
        ls_phase_t phase,
        ls_finalize_reason_t reason)
    {
        auto event = std::make_unique<EventData>();
        event->kind = kind;
        event->phase = phase;
        event->finalizeReason = reason;
        enqueue(std::move(event));
    }

    void enqueueSegmentEvent(TranscriptSegment segment)
    {
        auto event = std::make_unique<EventData>();
        event->kind = LS_EVENT_FINAL_SEGMENT;
        event->segment = std::move(segment);
        enqueue(std::move(event));
    }

    void enqueueSourceEvent(SourceGap source)
    {
        auto event = std::make_unique<EventData>();
        event->kind = LS_EVENT_SOURCE_CHANGED;
        event->source = std::move(source);
        enqueue(std::move(event));
    }

    void enqueueMetrics()
    {
        auto event = std::make_unique<EventData>();
        event->kind = LS_EVENT_METRICS;
        event->metrics = metrics();
        enqueue(std::move(event));
    }

    void enqueueError(Error error)
    {
        auto event = std::make_unique<EventData>();
        event->kind = LS_EVENT_ERROR;
        event->error = std::move(error);
        enqueue(std::move(event));
    }

    std::shared_ptr<RecoveryJournal> journal_;
    SessionRecord record_;
    SessionStateMachine stateMachine_;
    std::unique_ptr<IAsrBackend> asr_;
    std::unique_ptr<IDiarizationBackend> diarization_;
    std::uint32_t queueCapacity_{};

    mutable std::mutex stateMutex_;
    std::mutex lifecycleMutex_;
    std::condition_variable workCondition_;
    std::condition_variable drainedCondition_;
    std::deque<AudioWindow> audioQueue_;
    std::unordered_map<std::uint64_t, std::size_t> queueDepthBySource_;
    std::unordered_map<std::uint64_t, std::size_t> queuedSamplesBySource_;
    std::unordered_map<std::uint64_t, OverloadEpisode> overloadBySource_;
    std::unordered_map<std::uint64_t, std::uint64_t> lastSequence_;
    std::unordered_map<std::uint64_t, std::int64_t> lastTimestamp_;
    std::unordered_map<std::uint64_t, std::string> lastLanguageBySource_;
    std::unordered_set<std::uint64_t> forceDiscontinuityBySource_;
    std::array<AtomicOverloadEpisode, 2> atomicOverload_;
    std::optional<ProcessingWindowInfo> processingWindow_;
    std::uint32_t queueHighWater_{};
    bool acceptingAudio_{false};
    bool processing_{false};
    bool stopWorker_{false};
    bool workerExited_{false};
    bool closed_{false};
    bool discardProcessingResults_{false};
    bool incompleteRequiredSource_{false};
    std::optional<Error> fatalError_;
    std::int64_t latestMonotonicTimeNs_{};
    std::thread worker_;

    std::mutex eventMutex_;
    std::condition_variable eventCondition_;
    std::deque<std::unique_ptr<EventData>> events_;

    std::atomic<bool> closedAtomic_{false};
    std::atomic<std::uint64_t> framesOffered_{0};
    std::atomic<std::uint64_t> framesAccepted_{0};
    std::atomic<std::uint64_t> framesRejected_{0};
    std::atomic<std::uint64_t> discontinuities_{0};
    std::atomic<std::uint64_t> finalSegments_{0};
    std::atomic<std::uint64_t> partialEventsCoalesced_{0};
    std::atomic<std::uint64_t> journalCheckpoint_{0};
    std::atomic<std::uint32_t> highestSegmentRevision_{0};
};

void copyMetrics(
    const PipelineMetrics &source,
    ls_pipeline_metrics_v1 &destination)
{
    destination.frames_offered = source.framesOffered;
    destination.frames_accepted = source.framesAccepted;
    destination.frames_rejected = source.framesRejected;
    destination.discontinuities = source.discontinuities;
    destination.final_segments_committed = source.finalSegmentsCommitted;
    destination.partial_events_coalesced = source.partialEventsCoalesced;
    destination.audio_queue_depth = source.audioQueueDepth;
    destination.audio_queue_high_water = source.audioQueueHighWater;
    destination.journal_checkpoint = source.journalCheckpoint;
    destination.highest_segment_revision =
        source.highestSegmentRevision;
    destination.reserved = 0;
}

} // namespace
} // namespace localscribe

struct ls_core {
    std::shared_ptr<localscribe::CoreRuntime> runtime;
};

struct ls_session {
    std::shared_ptr<localscribe::SessionRuntime> runtime;
};

struct ls_event {
    std::unique_ptr<localscribe::EventData> data;
};

struct ls_owned_bytes {
    std::vector<std::uint8_t> data;
};

struct ls_recovery_list {
    std::vector<std::string> sessionIds;
};

using namespace localscribe;

extern "C" {

ls_status_code_t ls_core_create_v1(
    const ls_core_config_v1 *config,
    ls_core_t **out_core,
    ls_error_v1 *out_error)
{
    if (out_core != nullptr) {
        *out_core = nullptr;
    }
    clearError(out_error);
    try {
        auto valid = validateStruct(config);
        if (!valid) {
            return report(valid.error(), out_error);
        }
        if (out_core == nullptr) {
            return report(
                Error{LS_INVALID_ARGUMENT, "out_core is null"},
                out_error);
        }
        if ((config->flags & ~LS_CORE_CONFIG_ALLOW_TEST_BACKENDS) != 0) {
            return report(
                Error{LS_INVALID_ARGUMENT, "unknown core configuration flag"},
                out_error);
        }
        auto path = copyUtf8(config->journal_path, 32u * 1024u, true);
        if (!path) {
            return report(path.error(), out_error);
        }
        auto runtime =
            std::make_shared<CoreRuntime>(config->flags, path.takeValue());
        if (config->journal_path.size != 0) {
            auto journal = runtime->primaryJournal();
            if (!journal) {
                return report(journal.error(), out_error);
            }
        }
        auto core = std::make_unique<ls_core>();
        core->runtime = std::move(runtime);
        *out_core = core.release();
        return LS_OK;
    } catch (...) {
        return reportUnknown(out_error);
    }
}

void ls_core_destroy(ls_core_t *core)
{
    try {
        delete core;
    } catch (...) {
    }
}

ls_status_code_t ls_session_create_after_consent_v1(
    ls_core_t *core,
    const ls_session_config_v1 *config,
    ls_session_t **out_session,
    ls_error_v1 *out_error)
{
    if (out_session != nullptr) {
        *out_session = nullptr;
    }
    clearError(out_error);
    try {
        if (core == nullptr || core->runtime == nullptr
            || out_session == nullptr) {
            return report(
                Error{LS_INVALID_ARGUMENT, "core or output session is null"},
                out_error);
        }
        auto valid = validateStruct(config);
        if (!valid) {
            return report(valid.error(), out_error);
        }
        auto sessionId = copyUtf8(config->session_id, 128, true);
        auto journalPath =
            copyUtf8(config->journal_path, 32u * 1024u, true);
        auto sourceApp = copyUtf8(config->source_app, 4u * 1024u, true);
        auto localSpeaker =
            copyUtf8(config->local_speaker_name, 4u * 1024u, true);
        auto asrId = copyUtf8(config->asr_backend_id, 256, true);
        auto modelPath =
            copyUtf8(config->asr_model_path, 32u * 1024u, true);
        auto diarizationId =
            copyUtf8(config->diarization_backend_id, 256, true);
        auto createdAt =
            copyUtf8(config->created_at_iso8601, 256, true);
        for (const auto *result :
             {&sessionId,
              &journalPath,
              &sourceApp,
              &localSpeaker,
              &asrId,
              &modelPath,
              &diarizationId,
              &createdAt}) {
            if (!*result) {
                return report(result->error(), out_error);
            }
        }
        if (sessionId.value().empty() || asrId.value().empty()) {
            return report(
                Error{
                    LS_INVALID_ARGUMENT,
                    "session ID and ASR backend ID are required"},
                out_error);
        }
        if (config->language_mode < LS_LANGUAGE_MODE_RUSSIAN
            || config->language_mode > LS_LANGUAGE_MODE_RUSSIAN_ENGLISH) {
            return report(
                Error{LS_INVALID_ARGUMENT, "language mode is invalid"},
                out_error);
        }
        const std::uint64_t microphoneId =
            config->microphone_source_id == 0
            ? 1
            : config->microphone_source_id;
        const std::uint64_t systemId =
            config->system_audio_source_id == 0
            ? 2
            : config->system_audio_source_id;
        if (microphoneId == systemId) {
            return report(
                Error{LS_INVALID_ARGUMENT, "audio source IDs must be distinct"},
                out_error);
        }
        const std::uint32_t requiredMask =
            config->required_source_mask == 0
            ? LS_REQUIRED_SOURCE_MICROPHONE
                | LS_REQUIRED_SOURCE_SYSTEM_AUDIO
            : config->required_source_mask;
        if ((requiredMask
             & ~(LS_REQUIRED_SOURCE_MICROPHONE
                 | LS_REQUIRED_SOURCE_SYSTEM_AUDIO))
            != 0) {
            return report(
                Error{LS_INVALID_ARGUMENT, "required source mask is invalid"},
                out_error);
        }

        auto asr = createAsrBackend(
            asrId.value(),
            core->runtime->allowTestBackends());
        if (!asr) {
            return report(asr.error(), out_error);
        }
        AsrConfiguration asrConfiguration{
            modelPath.value(),
            static_cast<ls_language_mode_t>(config->language_mode)};
        auto asrPrepared = asr.value()->prepare(asrConfiguration);
        if (!asrPrepared) {
            return report(asrPrepared.error(), out_error);
        }

        auto diarization =
            createDiarizationBackend(diarizationId.value());
        if (!diarization) {
            return report(diarization.error(), out_error);
        }
        DiarizationConfiguration diarizationConfiguration;
        diarizationConfiguration.microphoneSourceId = microphoneId;
        diarizationConfiguration.systemAudioSourceId = systemId;
        diarizationConfiguration.localSpeakerName =
            localSpeaker.value().empty() ? "Me" : localSpeaker.value();
        auto diarizationPrepared =
            diarization.value()->prepare(diarizationConfiguration);
        if (!diarizationPrepared) {
            return report(diarizationPrepared.error(), out_error);
        }

        auto journal = core->runtime->journalFor(journalPath.value());
        if (!journal) {
            return report(journal.error(), out_error);
        }
        SessionRecord record;
        record.sessionId = sessionId.takeValue();
        record.phase = LS_PHASE_PREPARING;
        record.createdAt = createdAt.takeValue();
        record.sourceApp = sourceApp.takeValue();
        record.localSpeakerName =
            localSpeaker.value().empty() ? "Me" : localSpeaker.takeValue();
        record.asrBackendId = asr.value()->info().id;
        record.asrBackendVersion = asr.value()->info().version;
        record.diarizationBackendId = diarization.value()->info().id;
        record.diarizationBackendVersion =
            diarization.value()->info().version;
        record.languageMode = config->language_mode;
        record.microphoneSourceId = microphoneId;
        record.systemAudioSourceId = systemId;
        record.requiredSourceMask = requiredMask;
        record.completenessThresholdNs =
            config->source_completeness_threshold_ns <= 0
            ? 30'000'000'000LL
            : config->source_completeness_threshold_ns;

        std::uint32_t queueCapacity =
            config->audio_queue_capacity_frames == 0
            ? 64
            : config->audio_queue_capacity_frames;
        if (queueCapacity > 65'536) {
            return report(
                Error{LS_INVALID_ARGUMENT, "audio queue capacity is too large"},
                out_error);
        }
        auto runtime = SessionRuntime::create(
            journal.takeValue(),
            std::move(record),
            asr.takeValue(),
            diarization.takeValue(),
            queueCapacity);
        if (!runtime) {
            return report(runtime.error(), out_error);
        }
        auto handle = std::make_unique<ls_session>();
        handle->runtime = runtime.takeValue();
        *out_session = handle.release();
        return LS_OK;
    } catch (...) {
        return reportUnknown(out_error);
    }
}

ls_status_code_t
ls_session_mark_sources_ready_v1(ls_session_t *session)
{
    try {
        return session == nullptr || session->runtime == nullptr
            ? LS_INVALID_ARGUMENT
            : session->runtime->markSourcesReady();
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_session_push_audio_v1(
    ls_session_t *session,
    const ls_audio_frame_v1 *frame)
{
    try {
        if (session == nullptr || session->runtime == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        auto valid = validateStruct(frame);
        return valid ? session->runtime->push(*frame)
                     : valid.error().code;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_session_pause_v1(ls_session_t *session)
{
    try {
        return session == nullptr || session->runtime == nullptr
            ? LS_INVALID_ARGUMENT
            : session->runtime->pause();
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t
ls_session_resume_after_consent_v1(ls_session_t *session)
{
    try {
        return session == nullptr || session->runtime == nullptr
            ? LS_INVALID_ARGUMENT
            : session->runtime->resumeAfterConsent();
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_session_source_event_v1(
    ls_session_t *session,
    const ls_source_event_v1 *event)
{
    try {
        if (session == nullptr || session->runtime == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        auto valid = validateStruct(event);
        if (!valid) {
            return valid.error().code;
        }
        auto reason = copyUtf8(event->reason, 4u * 1024u, true);
        if (!reason) {
            return reason.error().code;
        }
        if (event->source_id == 0
            || event->source_kind < LS_SOURCE_KIND_MICROPHONE
            || event->source_kind > LS_SOURCE_KIND_SYSTEM_AUDIO
            || event->event_kind < LS_SOURCE_EVENT_READY
            || event->event_kind > LS_SOURCE_EVENT_DISCONTINUITY
            || event->health < LS_SOURCE_HEALTH_READY
            || event->health > LS_SOURCE_HEALTH_PERMANENTLY_LOST
            || event->start_time_ns < 0
            || event->end_time_ns < event->start_time_ns
            || (event->flags & ~LS_SOURCE_EVENT_FLAG_TEST_INJECTED) != 0) {
            return LS_INVALID_ARGUMENT;
        }
        SourceGap gap;
        gap.sourceId = event->source_id;
        gap.sourceKind = event->source_kind;
        gap.eventKind = event->event_kind;
        gap.health = event->health;
        gap.startTimeNs = event->start_time_ns;
        gap.endTimeNs = event->end_time_ns;
        gap.reason = reason.takeValue();
        gap.testInjected =
            (event->flags & LS_SOURCE_EVENT_FLAG_TEST_INJECTED) != 0;
        return session->runtime->sourceEvent(std::move(gap));
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_session_finalize_v1(
    ls_session_t *session,
    ls_finalize_reason_t reason)
{
    try {
        return session == nullptr || session->runtime == nullptr
            ? LS_INVALID_ARGUMENT
            : session->runtime->finalize(reason);
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

void ls_session_destroy(ls_session_t *session)
{
    try {
        if (session != nullptr && session->runtime != nullptr) {
            session->runtime->close();
        }
        delete session;
    } catch (...) {
    }
}

ls_status_code_t ls_session_next_event_v1(
    ls_session_t *session,
    uint32_t timeout_ms,
    ls_event_t **out_event)
{
    if (out_event != nullptr) {
        *out_event = nullptr;
    }
    try {
        if (session == nullptr || session->runtime == nullptr
            || out_event == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        auto event = session->runtime->poll(timeout_ms);
        if (!event) {
            return event.error().code;
        }
        auto handle = std::make_unique<ls_event>();
        handle->data = event.takeValue();
        *out_event = handle.release();
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_event_kind_t ls_event_kind(const ls_event_t *event)
{
    try {
        return event == nullptr || event->data == nullptr
            ? LS_EVENT_UNKNOWN
            : event->data->kind;
    } catch (...) {
        return LS_EVENT_UNKNOWN;
    }
}

ls_status_code_t ls_event_copy_segment_v1(
    const ls_event_t *event,
    ls_transcript_segment_copy_v1 *out_segment)
{
    try {
        auto valid = validateStruct(out_segment);
        if (!valid) {
            return valid.error().code;
        }
        if (event == nullptr || event->data == nullptr
            || event->data->kind != LS_EVENT_FINAL_SEGMENT) {
            return LS_INVALID_ARGUMENT;
        }
        const auto &segment = event->data->segment;
        copyStableId(segment.stableId, out_segment->stable_id);
        out_segment->source_id = segment.sourceId;
        out_segment->start_time_ns = segment.startTimeNs;
        out_segment->end_time_ns = segment.endTimeNs;
        out_segment->speaker_id = segment.speakerId;
        out_segment->speaker_label = makeView(segment.speakerLabel);
        out_segment->text = makeView(segment.text);
        out_segment->language = makeView(segment.language);
        out_segment->confidence = segment.confidence;
        out_segment->revision = segment.revision;
        out_segment->flags = segment.flags;
        out_segment->reserved = 0;
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_event_copy_metrics_v1(
    const ls_event_t *event,
    ls_pipeline_metrics_v1 *out_metrics)
{
    try {
        auto valid = validateStruct(out_metrics);
        if (!valid) {
            return valid.error().code;
        }
        if (event == nullptr || event->data == nullptr
            || event->data->kind != LS_EVENT_METRICS) {
            return LS_INVALID_ARGUMENT;
        }
        copyMetrics(event->data->metrics, *out_metrics);
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_event_copy_state_v1(
    const ls_event_t *event,
    ls_state_event_copy_v1 *out_state)
{
    try {
        auto valid = validateStruct(out_state);
        if (!valid) {
            return valid.error().code;
        }
        if (event == nullptr || event->data == nullptr
            || (event->data->kind != LS_EVENT_STATE_CHANGED
                && event->data->kind != LS_EVENT_TERMINAL)) {
            return LS_INVALID_ARGUMENT;
        }
        out_state->phase = event->data->phase;
        out_state->published_status =
            SessionStateMachine::publishedStatus(event->data->phase);
        out_state->finalize_reason = event->data->finalizeReason;
        out_state->reserved = 0;
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_event_copy_source_v1(
    const ls_event_t *event,
    ls_source_event_copy_v1 *out_source)
{
    try {
        auto valid = validateStruct(out_source);
        if (!valid) {
            return valid.error().code;
        }
        if (event == nullptr || event->data == nullptr
            || event->data->kind != LS_EVENT_SOURCE_CHANGED) {
            return LS_INVALID_ARGUMENT;
        }
        const auto &source = event->data->source;
        out_source->source_id = source.sourceId;
        out_source->source_kind = source.sourceKind;
        out_source->event_kind = source.eventKind;
        out_source->health = source.health;
        out_source->flags =
            source.testInjected ? LS_SOURCE_EVENT_FLAG_TEST_INJECTED : 0;
        out_source->start_time_ns = source.startTimeNs;
        out_source->end_time_ns = source.endTimeNs;
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

void ls_event_destroy(ls_event_t *event)
{
    try {
        delete event;
    } catch (...) {
    }
}

ls_status_code_t ls_session_copy_metrics_v1(
    const ls_session_t *session,
    ls_pipeline_metrics_v1 *out_metrics)
{
    try {
        auto valid = validateStruct(out_metrics);
        if (!valid) {
            return valid.error().code;
        }
        if (session == nullptr || session->runtime == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        copyMetrics(session->runtime->metrics(), *out_metrics);
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_session_copy_state_v1(
    const ls_session_t *session,
    ls_state_event_copy_v1 *out_state)
{
    try {
        auto valid = validateStruct(out_state);
        if (!valid) {
            return valid.error().code;
        }
        if (session == nullptr || session->runtime == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        const auto state = session->runtime->state();
        out_state->phase = state.phase;
        out_state->published_status = state.publishedStatus;
        out_state->finalize_reason = state.finalizeReason;
        out_state->reserved = 0;
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_session_render_markdown_v1(
    ls_session_t *session,
    const ls_markdown_options_v1 *options,
    ls_owned_bytes_t **out_markdown,
    ls_error_v1 *out_error)
{
    ls_render_snapshot_v1 ignoredSnapshot{};
    ignoredSnapshot.struct_size = sizeof(ignoredSnapshot);
    ignoredSnapshot.abi_version = LS_CORE_ABI_VERSION;
    return ls_session_render_markdown_with_snapshot_v1(
        session,
        options,
        out_markdown,
        &ignoredSnapshot,
        out_error);
}

ls_status_code_t ls_session_render_markdown_with_snapshot_v1(
    ls_session_t *session,
    const ls_markdown_options_v1 *options,
    ls_owned_bytes_t **out_markdown,
    ls_render_snapshot_v1 *out_snapshot,
    ls_error_v1 *out_error)
{
    if (out_markdown != nullptr) {
        *out_markdown = nullptr;
    }
    clearError(out_error);
    try {
        if (session == nullptr || session->runtime == nullptr
            || out_markdown == nullptr || out_snapshot == nullptr) {
            return report(
                Error{LS_INVALID_ARGUMENT, "session or render output is null"},
                out_error);
        }
        auto valid = validateStruct(options);
        if (!valid) {
            return report(valid.error(), out_error);
        }
        valid = validateStruct(out_snapshot);
        if (!valid) {
            return report(valid.error(), out_error);
        }
        out_snapshot->journal_checkpoint = 0;
        out_snapshot->highest_segment_revision = 0;
        out_snapshot->reserved = 0;
        auto title = copyUtf8(options->title, 64u * 1024u, false);
        auto created =
            copyUtf8(options->created_at_iso8601, 256, true);
        auto ended = copyUtf8(options->ended_at_iso8601, 256, true);
        if (!title) {
            return report(title.error(), out_error);
        }
        if (!created) {
            return report(created.error(), out_error);
        }
        if (!ended) {
            return report(ended.error(), out_error);
        }
        MarkdownRenderOptions renderOptions;
        renderOptions.title = title.takeValue();
        renderOptions.createdAt = created.takeValue();
        renderOptions.endedAt = ended.takeValue();
        renderOptions.durationSeconds = options->duration_seconds;
        renderOptions.microphoneCaptured =
            options->microphone_captured != 0;
        renderOptions.systemAudioCaptured =
            options->system_audio_captured != 0;
        auto rendered = session->runtime->render(renderOptions);
        if (!rendered) {
            return report(rendered.error(), out_error);
        }
        auto bytes = std::make_unique<ls_owned_bytes>();
        bytes->data.assign(
            rendered.value().bytes.begin(),
            rendered.value().bytes.end());
        out_snapshot->journal_checkpoint =
            rendered.value().snapshot.journalCheckpoint;
        out_snapshot->highest_segment_revision =
            rendered.value().snapshot.highestSegmentRevision;
        *out_markdown = bytes.release();
        return LS_OK;
    } catch (...) {
        return reportUnknown(out_error);
    }
}

const uint8_t *ls_owned_bytes_data(const ls_owned_bytes_t *bytes)
{
    try {
        return bytes == nullptr || bytes->data.empty()
            ? nullptr
            : bytes->data.data();
    } catch (...) {
        return nullptr;
    }
}

size_t ls_owned_bytes_size(const ls_owned_bytes_t *bytes)
{
    try {
        return bytes == nullptr ? 0 : bytes->data.size();
    } catch (...) {
        return 0;
    }
}

void ls_owned_bytes_destroy(ls_owned_bytes_t *bytes)
{
    try {
        delete bytes;
    } catch (...) {
    }
}

ls_status_code_t ls_session_ack_publication_v1(
    ls_session_t *session,
    const ls_publication_receipt_v1 *receipt)
{
    try {
        if (session == nullptr || session->runtime == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        auto valid = validateStruct(receipt);
        if (!valid) {
            return valid.error().code;
        }
        auto digest = copyUtf8(receipt->sha256_hex, 64, true);
        auto identity =
            copyUtf8(receipt->file_identity, 4u * 1024u, true);
        if (!digest) {
            return digest.error().code;
        }
        if (!identity) {
            return identity.error().code;
        }
        PublicationReceipt copy;
        copy.journalCheckpoint = receipt->journal_checkpoint;
        copy.highestSegmentRevision = receipt->highest_segment_revision;
        copy.destination = receipt->destination;
        copy.publishedAtUnixNs = receipt->published_at_unix_ns;
        copy.sha256Hex = digest.takeValue();
        copy.fileIdentity = identity.takeValue();
        auto acknowledged = session->runtime->acknowledge(copy);
        return acknowledged ? LS_OK : acknowledged.error().code;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

ls_status_code_t ls_core_list_recoverable_sessions_v1(
    ls_core_t *core,
    ls_recovery_list_t **out_list)
{
    if (out_list != nullptr) {
        *out_list = nullptr;
    }
    try {
        if (core == nullptr || core->runtime == nullptr || out_list == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        auto journal = core->runtime->primaryJournal();
        if (!journal) {
            return journal.error().code;
        }
        auto ids = journal.value()->listRecoverableSessions();
        if (!ids) {
            return ids.error().code;
        }
        auto list = std::make_unique<ls_recovery_list>();
        list->sessionIds = ids.takeValue();
        *out_list = list.release();
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

size_t ls_recovery_list_count(const ls_recovery_list_t *list)
{
    try {
        return list == nullptr ? 0 : list->sessionIds.size();
    } catch (...) {
        return 0;
    }
}

ls_status_code_t ls_recovery_list_session_id_v1(
    const ls_recovery_list_t *list,
    size_t index,
    ls_utf8_view_v1 *out_session_id)
{
    try {
        auto valid = validateStruct(out_session_id);
        if (!valid) {
            return valid.error().code;
        }
        if (list == nullptr || index >= list->sessionIds.size()) {
            return LS_INVALID_ARGUMENT;
        }
        *out_session_id = makeView(list->sessionIds[index]);
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

void ls_recovery_list_destroy(ls_recovery_list_t *list)
{
    try {
        delete list;
    } catch (...) {
    }
}

ls_status_code_t ls_core_open_recoverable_session_v1(
    ls_core_t *core,
    ls_utf8_view_v1 session_id,
    ls_session_t **out_session)
{
    if (out_session != nullptr) {
        *out_session = nullptr;
    }
    try {
        if (core == nullptr || core->runtime == nullptr
            || out_session == nullptr) {
            return LS_INVALID_ARGUMENT;
        }
        auto id = copyUtf8(session_id, 128, true);
        if (!id) {
            return id.error().code;
        }
        auto journal = core->runtime->primaryJournal();
        if (!journal) {
            return journal.error().code;
        }
        auto record = journal.value()->loadSession(id.value());
        if (!record) {
            return record.error().code;
        }
        auto runtime = SessionRuntime::recover(
            journal.takeValue(),
            record.takeValue());
        if (!runtime) {
            return runtime.error().code;
        }
        auto session = std::make_unique<ls_session>();
        session->runtime = runtime.takeValue();
        *out_session = session.release();
        return LS_OK;
    } catch (...) {
        return LS_INTERNAL_ERROR;
    }
}

} // extern "C"
