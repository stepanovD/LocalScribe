import AudioToolbox
import CoreMedia
import Foundation
import ScreenCaptureKit

actor ScreenCaptureKitAdapter: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .systemAudio

    private let sequenceGenerator = CaptureSequenceGenerator()
    private let bindingGate = ScreenCaptureBindingGate()
    private var stream: SCStream?
    private var output: ScreenAudioOutput?
    private var runGeneration: UInt64 = 0
    private var bindingGeneration: UInt64 = 0
    private var activeBindingGeneration: UInt64?

    init(sourceID: UInt64 = 2) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) async throws {
        guard stream == nil else {
            throw AudioCaptureError.alreadyRunning
        }
        runGeneration &+= 1
        let generation = runGeneration

        let content: SCShareableContent
        do {
            content = try await SCShareableContent.excludingDesktopWindows(
                false,
                onScreenWindowsOnly: true
            )
        } catch {
            throw AudioCaptureError.permissionDenied(.systemAudio)
        }
        try Task.checkCancellation()
        guard generation == runGeneration else {
            throw CancellationError()
        }

        guard let display = content.displays.first else {
            throw AudioCaptureError.noCaptureDevice(.systemAudio)
        }

        let ownPID = ProcessInfo.processInfo.processIdentifier
        let ownApplications = content.applications.filter {
            $0.processID == ownPID
        }
        let filter = SCContentFilter(
            display: display,
            excludingApplications: ownApplications,
            exceptingWindows: []
        )

        let configuration = SCStreamConfiguration()
        configuration.capturesAudio = true
        configuration.excludesCurrentProcessAudio = true
        configuration.sampleRate = 48_000
        configuration.channelCount = 2
        configuration.width = 2
        configuration.height = 2
        configuration.queueDepth = 3
        configuration.showsCursor = false
        configuration.minimumFrameInterval = CMTime(
            value: 1,
            timescale: 1
        )

        bindingGeneration &+= 1
        let newBindingGeneration = bindingGeneration
        let newOutput = ScreenAudioOutput(
            sourceID: sourceID,
            sequenceGenerator: sequenceGenerator,
            bindingGeneration: newBindingGeneration,
            bindingGate: bindingGate,
            frameHandler: frameHandler,
            eventHandler: eventHandler
        )
        let newStream = SCStream(
            filter: filter,
            configuration: configuration,
            delegate: newOutput
        )

        do {
            try newStream.addStreamOutput(
                newOutput,
                type: .audio,
                sampleHandlerQueue: newOutput.sampleQueue
            )
        } catch {
            throw AudioCaptureError.frameworkFailure(
                code: "system_audio_output"
            )
        }

        bindingGate.activate(newBindingGeneration)
        activeBindingGeneration = newBindingGeneration
        output = newOutput
        stream = newStream
        do {
            try await newStream.startCapture()
        } catch {
            newOutput.invalidateSilently()
            bindingGate.invalidate(newBindingGeneration)
            try? newStream.removeStreamOutput(newOutput, type: .audio)
            if stream === newStream {
                output = nil
                stream = nil
                activeBindingGeneration = nil
            }
            if generation != runGeneration || Task.isCancelled {
                throw CancellationError()
            }
            throw AudioCaptureError.frameworkFailure(code: "system_audio_start")
        }

        guard generation == runGeneration,
              stream === newStream,
              !Task.isCancelled,
              bindingGate.isActive(newBindingGeneration)
        else {
            newOutput.markStopped()
            try? await newStream.stopCapture()
            try? newStream.removeStreamOutput(newOutput, type: .audio)
            if stream === newStream {
                output = nil
                stream = nil
                activeBindingGeneration = nil
            }
            if generation != runGeneration || Task.isCancelled {
                throw CancellationError()
            }
            throw AudioCaptureError.frameworkFailure(
                code: "system_audio_stopped_during_start"
            )
        }
        newOutput.startWatchdog()
        newOutput.emitReady()
    }

    func stop() async {
        runGeneration &+= 1
        guard let stream else {
            if let activeBindingGeneration {
                bindingGate.invalidate(activeBindingGeneration)
                self.activeBindingGeneration = nil
            }
            return
        }

        let stoppedOutput = output
        let stoppedBindingGeneration = activeBindingGeneration
        self.output = nil
        self.stream = nil
        activeBindingGeneration = nil
        stoppedOutput?.markStopped()
        if let stoppedBindingGeneration {
            bindingGate.invalidate(stoppedBindingGeneration)
        }
        try? await stream.stopCapture()
        if let stoppedOutput {
            try? stream.removeStreamOutput(stoppedOutput, type: .audio)
        }
    }
}

private final class ScreenAudioOutput: NSObject, SCStreamOutput, SCStreamDelegate,
    @unchecked Sendable
{
    let sampleQueue = DispatchQueue(
        label: "app.localscribe.capture.system-audio",
        qos: .userInitiated
    )

    private let sourceID: UInt64
    private let bindingGeneration: UInt64
    private let bindingGate: ScreenCaptureBindingGate
    private let frameHandler: AudioFrameHandler
    private let eventHandler: CaptureEventHandler
    private let timeline: CaptureTimeline
    private let timeMapper = SampleTimeMapper()

    private let lock = NSLock()
    private var acceptsFrames = true
    private var rejectionEpisodeActive = false
    private var lastAudioCallbackNanoseconds =
        DispatchTime.now().uptimeNanoseconds
    private var watchdogTask: Task<Void, Never>?

    private static let watchdogTimeoutNanoseconds: UInt64 = 3_000_000_000
    private static let watchdogPollNanoseconds: UInt64 = 250_000_000

    init(
        sourceID: UInt64,
        sequenceGenerator: CaptureSequenceGenerator,
        bindingGeneration: UInt64,
        bindingGate: ScreenCaptureBindingGate,
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        self.sourceID = sourceID
        self.bindingGeneration = bindingGeneration
        self.bindingGate = bindingGate
        timeline = CaptureTimeline(sequenceGenerator: sequenceGenerator)
        self.frameHandler = frameHandler
        self.eventHandler = eventHandler
        super.init()
        timeline.resetEpoch()
    }

    func emitReady() {
        bindingGate.emit(
            .ready(.systemAudio),
            bindingGeneration: bindingGeneration,
            eventHandler: eventHandler
        )
    }

    func startWatchdog() {
        lock.lock()
        guard acceptsFrames, watchdogTask == nil else {
            lock.unlock()
            return
        }
        lastAudioCallbackNanoseconds = DispatchTime.now().uptimeNanoseconds
        let task = Task.detached(priority: .utility) { [weak self] in
            while !Task.isCancelled {
                do {
                    try await Task<Never, Never>.sleep(
                        nanoseconds: Self.watchdogPollNanoseconds
                    )
                } catch {
                    return
                }
                guard let self, self.watchdogIteration() else {
                    return
                }
            }
        }
        watchdogTask = task
        lock.unlock()
    }

    func markStopped() {
        bindingGate.close(
            bindingGeneration: bindingGeneration,
            events: [.stopped(.systemAudio)],
            eventHandler: eventHandler
        )
        _ = stopAccepting()
    }

    func invalidateSilently() {
        bindingGate.invalidate(bindingGeneration)
        _ = stopAccepting()
    }

    nonisolated func stream(
        _ stream: SCStream,
        didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
        of outputType: SCStreamOutputType
    ) {
        guard outputType == .audio else {
            return
        }

        lock.lock()
        let shouldProcess = acceptsFrames
        if shouldProcess {
            lastAudioCallbackNanoseconds = DispatchTime.now().uptimeNanoseconds
        }
        lock.unlock()
        guard shouldProcess else {
            return
        }

        guard sampleBuffer.isValid else {
            rejectMalformedFrame()
            return
        }

        let copied: CopiedPCM
        do {
            copied = try SystemAudioPCMConverter.copy(sampleBuffer)
        } catch {
            rejectMalformedFrame()
            return
        }

        let timestamp = timeMapper.monotonicNanoseconds(
            for: sampleBuffer.presentationTimeStamp
        )
        let stamp = timeline.next(
            timestampNanoseconds: timestamp,
            frameCount: copied.frameCount,
            sampleRateHz: copied.sampleRateHz,
            channelCount: copied.channelCount
        )

        let frame: CapturedAudioFrame
        do {
            frame = try CapturedAudioFrame(
                sourceID: sourceID,
                sequenceNumber: stamp.sequenceNumber,
                monotonicTimeNanoseconds: timestamp,
                sampleRateHz: copied.sampleRateHz,
                channelCount: copied.channelCount,
                frameCount: copied.frameCount,
                flags: stamp.flags,
                interleavedSamples: copied.samples
            )
        } catch {
            rejectMalformedFrame()
            return
        }

        guard let disposition = bindingGate.deliver(
            frame,
            bindingGeneration: bindingGeneration,
            frameHandler: frameHandler
        ) else {
            return
        }

        switch disposition {
        case .accepted:
            closeRejectionEpisode()
        case .backpressure:
            timeline.markDiscontinuity()
            emitRejectedFrameOnce()
        case .closed:
            markStopped()
        case .rejected:
            timeline.markDiscontinuity()
            emitRejectedFrameOnce()
        }
    }

    nonisolated func stream(_ stream: SCStream, didStopWithError error: Error) {
        bindingGate.close(
            bindingGeneration: bindingGeneration,
            events: [
                .stopped(.systemAudio),
                .failed(.systemAudio, code: "stream_stopped"),
            ],
            eventHandler: eventHandler
        )
        _ = stopAccepting()
    }

    private func rejectMalformedFrame() {
        timeline.markDiscontinuity()
        emitRejectedFrameOnce()
    }

    private func closeRejectionEpisode() {
        lock.lock()
        rejectionEpisodeActive = false
        lock.unlock()
    }

    private func emitRejectedFrameOnce() {
        lock.lock()
        let shouldEmit = !rejectionEpisodeActive
        rejectionEpisodeActive = true
        lock.unlock()
        if shouldEmit {
            bindingGate.emit(
                .frameRejected(.systemAudio),
                bindingGeneration: bindingGeneration,
                eventHandler: eventHandler
            )
        }
    }

    private func watchdogIteration() -> Bool {
        lock.lock()
        guard acceptsFrames, watchdogTask != nil else {
            lock.unlock()
            return false
        }

        let now = DispatchTime.now().uptimeNanoseconds
        let silence = now >= lastAudioCallbackNanoseconds
            ? now - lastAudioCallbackNanoseconds
            : 0
        guard silence >= Self.watchdogTimeoutNanoseconds else {
            lock.unlock()
            return true
        }

        acceptsFrames = false
        watchdogTask = nil
        lock.unlock()

        bindingGate.close(
            bindingGeneration: bindingGeneration,
            events: [
                .failed(.systemAudio, code: "system_audio_watchdog"),
            ],
            eventHandler: eventHandler
        )
        return false
    }

    @discardableResult
    private func stopAccepting() -> Bool {
        lock.lock()
        let wasAccepting = acceptsFrames
        acceptsFrames = false
        let task = watchdogTask
        watchdogTask = nil
        lock.unlock()
        task?.cancel()
        return wasAccepting
    }
}

/// Serializes the final generation check with delivery. Once a binding is
/// closed or superseded, callbacks already queued by ScreenCaptureKit cannot
/// deliver frames or events into a later capture epoch.
final class ScreenCaptureBindingGate: @unchecked Sendable {
    private let lock = NSLock()
    private var activeBindingGeneration: UInt64?

    func activate(_ bindingGeneration: UInt64) {
        lock.lock()
        activeBindingGeneration = bindingGeneration
        lock.unlock()
    }

    func invalidate(_ bindingGeneration: UInt64) {
        lock.lock()
        if activeBindingGeneration == bindingGeneration {
            activeBindingGeneration = nil
        }
        lock.unlock()
    }

    func isActive(_ bindingGeneration: UInt64) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return activeBindingGeneration == bindingGeneration
    }

    @discardableResult
    func emit(
        _ event: CaptureSourceEvent,
        bindingGeneration: UInt64,
        eventHandler: CaptureEventHandler
    ) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard activeBindingGeneration == bindingGeneration else {
            return false
        }
        eventHandler(event)
        return true
    }

    func deliver(
        _ frame: CapturedAudioFrame,
        bindingGeneration: UInt64,
        frameHandler: AudioFrameHandler
    ) -> AudioFrameDisposition? {
        lock.lock()
        defer { lock.unlock() }
        guard activeBindingGeneration == bindingGeneration else {
            return nil
        }
        return frameHandler(frame)
    }

    @discardableResult
    func close(
        bindingGeneration: UInt64,
        events: [CaptureSourceEvent],
        eventHandler: CaptureEventHandler
    ) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard activeBindingGeneration == bindingGeneration else {
            return false
        }
        activeBindingGeneration = nil
        for event in events {
            eventHandler(event)
        }
        return true
    }
}

private struct CopiedPCM {
    let samples: [Float]
    let frameCount: UInt32
    let sampleRateHz: UInt32
    let channelCount: UInt16
}

private enum SystemAudioPCMConverter {
    static func copy(_ sampleBuffer: CMSampleBuffer) throws -> CopiedPCM {
        guard let description = sampleBuffer.formatDescription,
              let formatPointer = CMAudioFormatDescriptionGetStreamBasicDescription(
                  description
              )
        else {
            throw AudioCaptureError.invalidFormat
        }

        let format = formatPointer.pointee
        guard format.mFormatID == kAudioFormatLinearPCM,
              format.mSampleRate > 0,
              format.mSampleRate <= Double(UInt32.max),
              format.mChannelsPerFrame > 0,
              format.mChannelsPerFrame <= UInt32(UInt16.max)
        else {
            throw AudioCaptureError.unsupportedPCMFormat
        }

        let frameCount = CMSampleBufferGetNumSamples(sampleBuffer)
        guard frameCount > 0, frameCount <= Int(UInt32.max) else {
            throw AudioCaptureError.invalidFormat
        }

        var requiredSize = 0
        var retainedBlockBuffer: CMBlockBuffer?
        let sizingStatus = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: &requiredSize,
            bufferListOut: nil,
            bufferListSize: 0,
            blockBufferAllocator: nil,
            blockBufferMemoryAllocator: nil,
            flags: UInt32(kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment),
            blockBufferOut: &retainedBlockBuffer
        )
        guard sizingStatus == noErr, requiredSize >= MemoryLayout<AudioBufferList>.size else {
            throw AudioCaptureError.frameworkFailure(code: "audio_buffer_size")
        }

        let rawList = UnsafeMutableRawPointer.allocate(
            byteCount: requiredSize,
            alignment: max(16, MemoryLayout<AudioBufferList>.alignment)
        )
        defer { rawList.deallocate() }
        let list = rawList.bindMemory(to: AudioBufferList.self, capacity: 1)

        let copyStatus = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: nil,
            bufferListOut: list,
            bufferListSize: requiredSize,
            blockBufferAllocator: nil,
            blockBufferMemoryAllocator: nil,
            flags: UInt32(kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment),
            blockBufferOut: &retainedBlockBuffer
        )
        guard copyStatus == noErr else {
            throw AudioCaptureError.frameworkFailure(code: "audio_buffer_copy")
        }

        let buffers = UnsafeMutableAudioBufferListPointer(list)
        let channelCount = Int(format.mChannelsPerFrame)
        let isFloat = format.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let isSignedInteger = format.mFormatFlags & kAudioFormatFlagIsSignedInteger != 0
        let isNonInterleaved = format.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0

        let samples: [Float]
        if isFloat && format.mBitsPerChannel == 32 {
            samples = try copyFloat32(
                buffers: buffers,
                frames: frameCount,
                channels: channelCount,
                nonInterleaved: isNonInterleaved
            )
        } else if isSignedInteger && format.mBitsPerChannel == 16 {
            samples = try copyInt16(
                buffers: buffers,
                frames: frameCount,
                channels: channelCount,
                nonInterleaved: isNonInterleaved
            )
        } else {
            throw AudioCaptureError.unsupportedPCMFormat
        }

        return CopiedPCM(
            samples: samples,
            frameCount: UInt32(frameCount),
            sampleRateHz: UInt32(format.mSampleRate.rounded()),
            channelCount: UInt16(channelCount)
        )
    }

    private static func copyFloat32(
        buffers: UnsafeMutableAudioBufferListPointer,
        frames: Int,
        channels: Int,
        nonInterleaved: Bool
    ) throws -> [Float] {
        var output = [Float](repeating: 0, count: frames * channels)
        if nonInterleaved {
            guard buffers.count >= channels else {
                throw AudioCaptureError.invalidFormat
            }
            for channel in 0..<channels {
                let buffer = buffers[channel]
                guard let data = buffer.mData,
                      Int(buffer.mDataByteSize) >= frames * MemoryLayout<Float>.size
                else {
                    throw AudioCaptureError.invalidFormat
                }
                let source = data.assumingMemoryBound(to: Float.self)
                for frame in 0..<frames {
                    output[frame * channels + channel] = source[frame]
                }
            }
        } else {
            guard let first = buffers.first,
                  let data = first.mData,
                  Int(first.mDataByteSize)
                    >= output.count * MemoryLayout<Float>.size
            else {
                throw AudioCaptureError.invalidFormat
            }
            let sampleCount = output.count
            output.withUnsafeMutableBufferPointer { destination in
                destination.baseAddress?.update(
                    from: data.assumingMemoryBound(to: Float.self),
                    count: sampleCount
                )
            }
        }
        return output
    }

    private static func copyInt16(
        buffers: UnsafeMutableAudioBufferListPointer,
        frames: Int,
        channels: Int,
        nonInterleaved: Bool
    ) throws -> [Float] {
        let scale = Float(1.0 / 32_768.0)
        var output = [Float](repeating: 0, count: frames * channels)
        if nonInterleaved {
            guard buffers.count >= channels else {
                throw AudioCaptureError.invalidFormat
            }
            for channel in 0..<channels {
                let buffer = buffers[channel]
                guard let data = buffer.mData,
                      Int(buffer.mDataByteSize) >= frames * MemoryLayout<Int16>.size
                else {
                    throw AudioCaptureError.invalidFormat
                }
                let source = data.assumingMemoryBound(to: Int16.self)
                for frame in 0..<frames {
                    output[frame * channels + channel] = Float(source[frame]) * scale
                }
            }
        } else {
            guard let first = buffers.first,
                  let data = first.mData,
                  Int(first.mDataByteSize)
                    >= output.count * MemoryLayout<Int16>.size
            else {
                throw AudioCaptureError.invalidFormat
            }
            let source = data.assumingMemoryBound(to: Int16.self)
            for index in output.indices {
                output[index] = Float(source[index]) * scale
            }
        }
        return output
    }
}

private final class SampleTimeMapper: @unchecked Sendable {
    private let lock = NSLock()
    private var originPresentationSeconds: Double?
    private var originHostNanoseconds: Int64?

    func monotonicNanoseconds(for presentationTime: CMTime) -> Int64 {
        lock.lock()
        defer { lock.unlock() }

        let host = DispatchTime.now().uptimeNanoseconds
        let clampedHost = host > UInt64(Int64.max) ? Int64.max : Int64(host)
        let seconds = CMTimeGetSeconds(presentationTime)
        guard seconds.isFinite else {
            return clampedHost
        }

        if originPresentationSeconds == nil {
            originPresentationSeconds = seconds
            originHostNanoseconds = clampedHost
            return clampedHost
        }

        guard let originPresentationSeconds, let originHostNanoseconds else {
            return clampedHost
        }
        let delta = (seconds - originPresentationSeconds) * 1_000_000_000
        guard delta.isFinite,
              delta >= Double(Int64.min),
              delta <= Double(Int64.max)
        else {
            return clampedHost
        }
        return originHostNanoseconds &+ Int64(delta.rounded())
    }
}
