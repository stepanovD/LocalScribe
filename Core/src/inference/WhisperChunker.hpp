#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace localscribe {

struct WhisperChunk {
    std::uint64_t sourceId{};
    std::int64_t startTimeNs{};
    std::uint64_t ordinal{};
    bool discontinuityBefore{};
    std::vector<float> samples;
};

/*
 * Backend-independent buffering policy for whisper.cpp. A discontinuity
 * closes the old timeline before any post-gap samples are appended.
 */
class WhisperChunker {
public:
    WhisperChunker(
        std::uint32_t sampleRate,
        std::size_t chunkSamples,
        std::size_t minimumDiscontinuitySamples = 0)
        : sampleRate_(sampleRate),
          chunkSamples_(std::max<std::size_t>(chunkSamples, 1u)),
          minimumDiscontinuitySamples_(minimumDiscontinuitySamples)
    {
    }

    [[nodiscard]] std::vector<WhisperChunk> accept(
        std::uint64_t sourceId,
        std::int64_t startTimeNs,
        std::span<const float> samples,
        bool discontinuity,
        bool endOfStream,
        bool dropShortBeforeDiscontinuity = false)
    {
        std::vector<WhisperChunk> ready;
        auto &pending = pending_[sourceId];
        if (discontinuity && !pending.samples.empty()) {
            if (!dropShortBeforeDiscontinuity
                || pending.samples.size() >= minimumDiscontinuitySamples_) {
                ready.push_back(takeAll(sourceId, pending));
            } else {
                pending = Pending{};
            }
        }
        if (discontinuity) {
            pending.discontinuityBefore = true;
        }
        if (pending.samples.empty() && !samples.empty()) {
            pending.startTimeNs = startTimeNs;
            pending.ordinal = nextOrdinal_++;
        }
        pending.samples.insert(
            pending.samples.end(),
            samples.begin(),
            samples.end());

        while (pending.samples.size() >= chunkSamples_) {
            ready.push_back(takePrefix(sourceId, pending, chunkSamples_));
        }
        if (endOfStream && !pending.samples.empty()) {
            ready.push_back(takeAll(sourceId, pending));
        }
        const bool emittedBoundary = std::any_of(
            ready.begin(),
            ready.end(),
            [](const WhisperChunk &chunk) {
                return chunk.discontinuityBefore;
            });
        if (discontinuity && !emittedBoundary) {
            WhisperChunk marker;
            marker.sourceId = sourceId;
            marker.startTimeNs = startTimeNs;
            marker.ordinal = pending.ordinal;
            marker.discontinuityBefore = true;
            ready.push_back(std::move(marker));
            /* The marker consumes the boundary before buffered post-gap
               samples are eventually decoded. */
            pending.discontinuityBefore = false;
        }
        if (endOfStream && pending.samples.empty()) {
            pending_.erase(sourceId);
        }
        return ready;
    }

    [[nodiscard]] std::vector<WhisperChunk> flush()
    {
        std::vector<std::uint64_t> sources;
        sources.reserve(pending_.size());
        for (const auto &[sourceId, pending] : pending_) {
            if (!pending.samples.empty()) {
                sources.push_back(sourceId);
            }
        }
        std::sort(sources.begin(), sources.end());

        std::vector<WhisperChunk> ready;
        ready.reserve(sources.size());
        for (const auto sourceId : sources) {
            ready.push_back(takeAll(sourceId, pending_.at(sourceId)));
        }
        pending_.clear();
        return ready;
    }

    void reset()
    {
        pending_.clear();
        nextOrdinal_ = 1;
    }

private:
    struct Pending {
        std::int64_t startTimeNs{};
        std::vector<float> samples;
        std::uint64_t ordinal{};
        bool discontinuityBefore{};
    };

    [[nodiscard]] WhisperChunk takePrefix(
        std::uint64_t sourceId,
        Pending &pending,
        std::size_t count)
    {
        WhisperChunk chunk;
        chunk.sourceId = sourceId;
        chunk.startTimeNs = pending.startTimeNs;
        chunk.ordinal = pending.ordinal;
        chunk.discontinuityBefore = pending.discontinuityBefore;
        chunk.samples.assign(
            pending.samples.begin(),
            pending.samples.begin() + static_cast<std::ptrdiff_t>(count));
        pending.samples.erase(
            pending.samples.begin(),
            pending.samples.begin() + static_cast<std::ptrdiff_t>(count));
        pending.startTimeNs += static_cast<std::int64_t>(
            count * 1'000'000'000ULL / sampleRate_);
        pending.ordinal = nextOrdinal_++;
        pending.discontinuityBefore = false;
        return chunk;
    }

    [[nodiscard]] WhisperChunk
    takeAll(std::uint64_t sourceId, Pending &pending)
    {
        WhisperChunk chunk;
        chunk.sourceId = sourceId;
        chunk.startTimeNs = pending.startTimeNs;
        chunk.ordinal = pending.ordinal;
        chunk.discontinuityBefore = pending.discontinuityBefore;
        chunk.samples = std::move(pending.samples);
        pending = Pending{};
        return chunk;
    }

    std::uint32_t sampleRate_{};
    std::size_t chunkSamples_{};
    std::size_t minimumDiscontinuitySamples_{};
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::uint64_t nextOrdinal_{1};
};

} // namespace localscribe
