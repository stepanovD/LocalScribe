import AVFoundation
import CoreAudio
import Foundation

actor MicrophoneCaptureAdapter: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .microphone

    private enum Lifecycle {
        case idle
        case starting
        case running
        case recovering
        case unavailable
    }

    private static let watchdogTimeoutNanoseconds: UInt64 = 3_000_000_000
    private static let watchdogPollNanoseconds: UInt64 = 250_000_000
    private static let rebindBackoffsNanoseconds: [UInt64] = [
        200_000_000,
        500_000_000,
        1_000_000_000,
        2_000_000_000,
        4_000_000_000,
        8_000_000_000,
        8_000_000_000,
        8_000_000_000,
    ]

    private let sequenceGenerator = CaptureSequenceGenerator()

    private var lifecycle: Lifecycle = .idle
    private var runGeneration: UInt64 = 0
    private var bindingGeneration: UInt64 = 0

    private var engine: AVAudioEngine?
    private var processor: MicrophoneTapProcessor?
    private var configurationObserver: (any NSObjectProtocol)?
    private var watchdogTask: Task<Void, Never>?
    private var recoveryTask: Task<Void, Never>?

    private var frameHandler: AudioFrameHandler?
    private var eventHandler: CaptureEventHandler?

    init(sourceID: UInt64 = 1) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) async throws {
        guard lifecycle == .idle else {
            throw AudioCaptureError.alreadyRunning
        }

        guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else {
            throw AudioCaptureError.permissionDenied(.microphone)
        }

        runGeneration &+= 1
        let generation = runGeneration
        lifecycle = .starting
        self.frameHandler = frameHandler
        self.eventHandler = eventHandler

        do {
            try installBinding(
                frameHandler: frameHandler,
                eventHandler: eventHandler,
                runGeneration: generation
            )
        } catch {
            tearDownCurrentBinding()
            self.frameHandler = nil
            self.eventHandler = nil
            lifecycle = .idle
            throw error
        }

        lifecycle = .running
        eventHandler(.ready(.microphone))
    }

    func stop() async {
        guard lifecycle != .idle else {
            return
        }

        lifecycle = .idle
        runGeneration &+= 1
        recoveryTask?.cancel()
        recoveryTask = nil

        let hadBinding = processor != nil
        let processorWasAccepting = processor?.deactivate() ?? false
        tearDownCurrentBinding(processorAlreadyDeactivated: true)

        let stoppedHandler = eventHandler
        frameHandler = nil
        eventHandler = nil

        // A processor that observed `.closed` already emitted `.stopped`.
        // A source between rebind attempts has no processor, but still needs a
        // terminal event when its owner explicitly stops it.
        if processorWasAccepting || !hadBinding {
            stoppedHandler?(.stopped(.microphone))
        }
    }

    private func installBinding(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler,
        runGeneration: UInt64
    ) throws {
        let binding: (
            engine: AVAudioEngine,
            processor: MicrophoneTapProcessor
        )
        do {
            binding = try makeBinding(
                frameHandler: frameHandler,
                eventHandler: eventHandler,
                enableVoiceProcessing: true
            )
        } catch {
            // Voice I/O can be unavailable for aggregate, virtual, or
            // mismatched input/output routes. Retry with the raw microphone
            // so echo cancellation never turns into total source loss.
            binding = try makeBinding(
                frameHandler: frameHandler,
                eventHandler: eventHandler,
                enableVoiceProcessing: false
            )
        }

        let newEngine = binding.engine
        let newProcessor = binding.processor
        bindingGeneration &+= 1
        let newBindingGeneration = bindingGeneration
        engine = newEngine
        processor = newProcessor
        configurationObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange,
            object: newEngine,
            queue: nil
        ) { [weak self] _ in
            self?.enqueueConfigurationChange(
                runGeneration: runGeneration,
                bindingGeneration: newBindingGeneration
            )
        }
        armWatchdog(
            processor: newProcessor,
            runGeneration: runGeneration,
            bindingGeneration: newBindingGeneration
        )
    }

    private func makeBinding(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler,
        enableVoiceProcessing: Bool
    ) throws -> (
        engine: AVAudioEngine,
        processor: MicrophoneTapProcessor
    ) {
        let newEngine = AVAudioEngine()
        let input = newEngine.inputNode
        if enableVoiceProcessing {
            try input.setVoiceProcessingEnabled(true)
            input.isVoiceProcessingBypassed = false
            input.isVoiceProcessingAGCEnabled = true
        }
        let hardwareFormat = input.outputFormat(forBus: 0)
        guard hardwareFormat.sampleRate > 0,
              hardwareFormat.channelCount > 0,
              let tapFormat = AVAudioFormat(
                  commonFormat: .pcmFormatFloat32,
                  sampleRate: hardwareFormat.sampleRate,
                  channels: hardwareFormat.channelCount,
                  interleaved: false
              )
        else {
            throw AudioCaptureError.noCaptureDevice(.microphone)
        }

        let newProcessor = MicrophoneTapProcessor(
            sourceID: sourceID,
            sequenceGenerator: sequenceGenerator,
            frameHandler: frameHandler,
            eventHandler: eventHandler
        )
        newProcessor.resetEpoch()

        input.installTap(
            onBus: 0,
            bufferSize: 1_024,
            format: tapFormat
        ) { buffer, time in
            newProcessor.process(buffer: buffer, time: time)
        }

        do {
            newEngine.prepare()
            try newEngine.start()
        } catch {
            newProcessor.deactivate()
            input.removeTap(onBus: 0)
            newEngine.stop()
            throw AudioCaptureError.frameworkFailure(code: "microphone_start")
        }

        return (newEngine, newProcessor)
    }

    private nonisolated func enqueueConfigurationChange(
        runGeneration: UInt64,
        bindingGeneration: UInt64
    ) {
        Task { [weak self] in
            await self?.configurationDidChange(
                runGeneration: runGeneration,
                bindingGeneration: bindingGeneration
            )
        }
    }

    private func configurationDidChange(
        runGeneration: UInt64,
        bindingGeneration: UInt64
    ) {
        guard lifecycle == .running,
              self.runGeneration == runGeneration,
              self.bindingGeneration == bindingGeneration,
              processor?.isAccepting == true
        else {
            return
        }

        beginRecovery(
            code: "microphone_configuration_changed",
            runGeneration: runGeneration
        )
    }

    private func armWatchdog(
        processor: MicrophoneTapProcessor,
        runGeneration: UInt64,
        bindingGeneration: UInt64
    ) {
        watchdogTask?.cancel()
        watchdogTask = Task.detached(priority: .utility) { [weak self, processor] in
            while !Task.isCancelled {
                do {
                    try await Task<Never, Never>.sleep(
                        nanoseconds: Self.watchdogPollNanoseconds
                    )
                } catch {
                    return
                }

                guard processor.callbackSilenceNanoseconds()
                    >= Self.watchdogTimeoutNanoseconds
                else {
                    continue
                }

                await self?.watchdogExpired(
                    processor: processor,
                    runGeneration: runGeneration,
                    bindingGeneration: bindingGeneration
                )
                return
            }
        }
    }

    private func watchdogExpired(
        processor: MicrophoneTapProcessor,
        runGeneration: UInt64,
        bindingGeneration: UInt64
    ) {
        guard lifecycle == .running,
              self.runGeneration == runGeneration,
              self.bindingGeneration == bindingGeneration,
              self.processor === processor,
              processor.isAccepting
        else {
            return
        }

        beginRecovery(
            code: "microphone_callback_timeout",
            runGeneration: runGeneration
        )
    }

    private func beginRecovery(
        code: String,
        runGeneration: UInt64
    ) {
        guard lifecycle == .running,
              self.runGeneration == runGeneration
        else {
            return
        }

        lifecycle = .recovering
        eventHandler?(.failed(.microphone, code: code))
        tearDownCurrentBinding()

        recoveryTask?.cancel()
        recoveryTask = Task { [weak self] in
            await self?.performRecovery(runGeneration: runGeneration)
        }
    }

    private func performRecovery(runGeneration: UInt64) async {
        for delay in Self.rebindBackoffsNanoseconds {
            do {
                try await Task<Never, Never>.sleep(nanoseconds: delay)
            } catch {
                return
            }

            guard !Task.isCancelled,
                  lifecycle == .recovering,
                  self.runGeneration == runGeneration,
                  let frameHandler,
                  let eventHandler
            else {
                return
            }

            do {
                try installBinding(
                    frameHandler: frameHandler,
                    eventHandler: eventHandler,
                    runGeneration: runGeneration
                )
            } catch {
                continue
            }

            guard lifecycle == .recovering,
                  self.runGeneration == runGeneration
            else {
                tearDownCurrentBinding()
                return
            }

            lifecycle = .running
            recoveryTask = nil
            eventHandler(.ready(.microphone))
            return
        }

        guard lifecycle == .recovering,
              self.runGeneration == runGeneration
        else {
            return
        }

        lifecycle = .unavailable
        recoveryTask = nil
        eventHandler?(
            .failed(.microphone, code: "microphone_rebind_exhausted")
        )
    }

    private func tearDownCurrentBinding(
        processorAlreadyDeactivated: Bool = false
    ) {
        watchdogTask?.cancel()
        watchdogTask = nil

        if let configurationObserver {
            NotificationCenter.default.removeObserver(configurationObserver)
            self.configurationObserver = nil
        }

        let oldProcessor = processor
        let oldEngine = engine
        processor = nil
        engine = nil

        if !processorAlreadyDeactivated {
            oldProcessor?.deactivate()
        }
        if let oldEngine {
            oldEngine.inputNode.removeTap(onBus: 0)
            oldEngine.stop()
        }
    }
}

private final class MicrophoneTapProcessor: @unchecked Sendable {
    private let sourceID: UInt64
    private let frameHandler: AudioFrameHandler
    private let eventHandler: CaptureEventHandler
    private let timeline: CaptureTimeline

    private let lock = NSLock()
    private var acceptsFrames = true
    private var rejectionEpisodeActive = false
    private var lastCallbackNanoseconds = DispatchTime.now().uptimeNanoseconds

    init(
        sourceID: UInt64,
        sequenceGenerator: CaptureSequenceGenerator,
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        self.sourceID = sourceID
        timeline = CaptureTimeline(sequenceGenerator: sequenceGenerator)
        self.frameHandler = frameHandler
        self.eventHandler = eventHandler
    }

    var isAccepting: Bool {
        lock.lock()
        defer { lock.unlock() }
        return acceptsFrames
    }

    func resetEpoch() {
        lock.lock()
        acceptsFrames = true
        rejectionEpisodeActive = false
        lastCallbackNanoseconds = DispatchTime.now().uptimeNanoseconds
        lock.unlock()
        timeline.resetEpoch()
    }

    @discardableResult
    func deactivate() -> Bool {
        lock.lock()
        let wasAccepting = acceptsFrames
        acceptsFrames = false
        lock.unlock()
        return wasAccepting
    }

    func callbackSilenceNanoseconds() -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        guard acceptsFrames else {
            return 0
        }

        let now = DispatchTime.now().uptimeNanoseconds
        return now >= lastCallbackNanoseconds
            ? now - lastCallbackNanoseconds
            : 0
    }

    func process(buffer: AVAudioPCMBuffer, time: AVAudioTime) {
        lock.lock()
        let shouldProcess = acceptsFrames
        if shouldProcess {
            lastCallbackNanoseconds = DispatchTime.now().uptimeNanoseconds
        }
        lock.unlock()
        guard shouldProcess else {
            return
        }

        let frameCount = buffer.frameLength
        let channelCount = buffer.format.channelCount
        let sampleRate = buffer.format.sampleRate
        guard frameCount > 0,
              channelCount > 0,
              sampleRate > 0,
              sampleRate <= Double(UInt32.max),
              let channels = buffer.floatChannelData
        else {
            reportRejectedFrame()
            return
        }

        let frames = Int(frameCount)
        let channelTotal = Int(channelCount)
        var samples = [Float](repeating: 0, count: frames * channelTotal)
        for frameIndex in 0..<frames {
            for channelIndex in 0..<channelTotal {
                samples[frameIndex * channelTotal + channelIndex] =
                    channels[channelIndex][frameIndex]
            }
        }

        let timestamp: Int64
        if time.isHostTimeValid {
            let nanos = AudioConvertHostTimeToNanos(time.hostTime)
            timestamp = nanos > UInt64(Int64.max)
                ? Int64.max
                : Int64(nanos)
        } else {
            let nanos = DispatchTime.now().uptimeNanoseconds
            timestamp = nanos > UInt64(Int64.max)
                ? Int64.max
                : Int64(nanos)
        }

        let sampleRateHz = UInt32(sampleRate.rounded())
        let stamp = timeline.next(
            timestampNanoseconds: timestamp,
            frameCount: frameCount,
            sampleRateHz: sampleRateHz,
            channelCount: UInt16(channelCount)
        )

        let frame: CapturedAudioFrame
        do {
            frame = try CapturedAudioFrame(
                sourceID: sourceID,
                sequenceNumber: stamp.sequenceNumber,
                monotonicTimeNanoseconds: timestamp,
                sampleRateHz: sampleRateHz,
                channelCount: UInt16(channelCount),
                frameCount: frameCount,
                flags: stamp.flags,
                interleavedSamples: samples
            )
        } catch {
            reportRejectedFrame()
            return
        }

        lock.lock()
        guard acceptsFrames else {
            lock.unlock()
            return
        }
        let disposition = frameHandler(frame)
        switch disposition {
        case .accepted:
            rejectionEpisodeActive = false
            lock.unlock()
        case .backpressure:
            timeline.markDiscontinuity()
            let shouldEmit = !rejectionEpisodeActive
            rejectionEpisodeActive = true
            lock.unlock()
            if shouldEmit {
                eventHandler(.frameRejected(.microphone))
            }
        case .closed:
            acceptsFrames = false
            lock.unlock()
            eventHandler(.stopped(.microphone))
        case .rejected:
            timeline.markDiscontinuity()
            let shouldEmit = !rejectionEpisodeActive
            rejectionEpisodeActive = true
            lock.unlock()
            if shouldEmit {
                eventHandler(.frameRejected(.microphone))
            }
        }
    }

    private func reportRejectedFrame() {
        timeline.markDiscontinuity()
        lock.lock()
        let shouldEmit = !rejectionEpisodeActive
        rejectionEpisodeActive = true
        lock.unlock()
        if shouldEmit {
            eventHandler(.frameRejected(.microphone))
        }
    }
}
