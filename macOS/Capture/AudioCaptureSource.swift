import Foundation

enum CaptureSourceKind: String, Sendable, Hashable {
    case microphone
    case systemAudio
}

struct AudioFrameFlags: OptionSet, Sendable {
    let rawValue: UInt32

    static let discontinuity = AudioFrameFlags(rawValue: 1 << 0)
    static let endOfStream = AudioFrameFlags(rawValue: 1 << 1)
}

struct CapturedAudioFrame: Sendable {
    let sourceID: UInt64
    let sequenceNumber: UInt64
    let monotonicTimeNanoseconds: Int64
    let sampleRateHz: UInt32
    let channelCount: UInt16
    let frameCount: UInt32
    let flags: AudioFrameFlags
    let interleavedSamples: [Float]

    init(
        sourceID: UInt64,
        sequenceNumber: UInt64,
        monotonicTimeNanoseconds: Int64,
        sampleRateHz: UInt32,
        channelCount: UInt16,
        frameCount: UInt32,
        flags: AudioFrameFlags,
        interleavedSamples: [Float]
    ) throws {
        guard sampleRateHz > 0, channelCount > 0, frameCount > 0 else {
            throw AudioCaptureError.invalidFormat
        }

        let expectedSampleCount = Int(frameCount) * Int(channelCount)
        guard interleavedSamples.count == expectedSampleCount else {
            throw AudioCaptureError.invalidBufferSize(
                expected: expectedSampleCount,
                actual: interleavedSamples.count
            )
        }

        self.sourceID = sourceID
        self.sequenceNumber = sequenceNumber
        self.monotonicTimeNanoseconds = monotonicTimeNanoseconds
        self.sampleRateHz = sampleRateHz
        self.channelCount = channelCount
        self.frameCount = frameCount
        self.flags = flags
        self.interleavedSamples = interleavedSamples
    }
}

enum AudioFrameDisposition: Sendable, Equatable {
    case accepted
    case backpressure
    case closed
    case rejected
}

enum CaptureSourceEvent: Sendable, Equatable {
    case ready(CaptureSourceKind)
    case stopped(CaptureSourceKind)
    case frameRejected(CaptureSourceKind)
    case failed(CaptureSourceKind, code: String)
}

typealias AudioFrameHandler = @Sendable (CapturedAudioFrame) -> AudioFrameDisposition
typealias CaptureEventHandler = @Sendable (CaptureSourceEvent) -> Void

protocol AudioCaptureSource: Sendable {
    var sourceID: UInt64 { get }
    var kind: CaptureSourceKind { get }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) async throws

    func stop() async
}

enum AudioCaptureError: Error, Sendable, Equatable {
    case alreadyRunning
    case notRunning
    case permissionDenied(CaptureSourceKind)
    case noCaptureDevice(CaptureSourceKind)
    case invalidFormat
    case invalidBufferSize(expected: Int, actual: Int)
    case unsupportedPCMFormat
    case frameworkFailure(code: String)
}

/// Owns sequence epochs and discontinuity detection without making capture
/// callbacks hop through an actor or enqueue unbounded tasks.
final class CaptureTimeline: @unchecked Sendable {
    struct Stamp: Sendable {
        let sequenceNumber: UInt64
        let flags: AudioFrameFlags
    }

    private let lock = NSLock()
    private let sequenceGenerator: CaptureSequenceGenerator
    private var previousTimestampNanoseconds: Int64?
    private var previousFrameDurationNanoseconds: Int64 = 0
    private var formatSignature: UInt64?
    private var forceDiscontinuity = true

    init(sequenceGenerator: CaptureSequenceGenerator = CaptureSequenceGenerator()) {
        self.sequenceGenerator = sequenceGenerator
    }

    func resetEpoch() {
        lock.lock()
        defer { lock.unlock() }

        previousTimestampNanoseconds = nil
        previousFrameDurationNanoseconds = 0
        formatSignature = nil
        forceDiscontinuity = true
    }

    func markDiscontinuity() {
        lock.lock()
        forceDiscontinuity = true
        lock.unlock()
    }

    func next(
        timestampNanoseconds: Int64,
        frameCount: UInt32,
        sampleRateHz: UInt32,
        channelCount: UInt16
    ) -> Stamp {
        lock.lock()
        defer { lock.unlock() }

        var flags: AudioFrameFlags = []
        let signature = UInt64(sampleRateHz) << 16 | UInt64(channelCount)
        let frameDuration = Int64(
            (Double(frameCount) / Double(sampleRateHz)) * 1_000_000_000
        )

        if forceDiscontinuity || formatSignature.map({ $0 != signature }) == true {
            flags.insert(.discontinuity)
        } else if let previousTimestampNanoseconds {
            let observedGap = timestampNanoseconds - previousTimestampNanoseconds
            let expectedGap = max(previousFrameDurationNanoseconds, 1)
            let tolerance = max(100_000_000, expectedGap * 2)
            if observedGap <= 0 || abs(observedGap - expectedGap) > tolerance {
                flags.insert(.discontinuity)
            }
        }

        let result = Stamp(
            sequenceNumber: sequenceGenerator.next(),
            flags: flags
        )
        previousTimestampNanoseconds = timestampNanoseconds
        previousFrameDurationNanoseconds = frameDuration
        formatSignature = signature
        forceDiscontinuity = false
        return result
    }
}

/// Sequence numbers remain strictly increasing across pause/resume capture
/// epochs because the C ABI has no separate epoch field.
final class CaptureSequenceGenerator: @unchecked Sendable {
    private let lock = NSLock()
    private var nextValue: UInt64 = 0

    func next() -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        let value = nextValue
        nextValue &+= 1
        return value
    }
}
