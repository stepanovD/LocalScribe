import Foundation

enum CorePhase: Int32, Sendable {
    case unknown = 0
    case preparing = 1
    case recording = 2
    case paused = 3
    case finalizing = 4
    case recoveryRequired = 5
    case complete = 6
    case incompleteSources = 7
    case interrupted = 8
    case failedToStart = 9

    var isTerminal: Bool {
        self == .complete || self == .incompleteSources || self == .interrupted
    }
}

enum CorePublishedStatus: Int32, Sendable {
    case unknown = 0
    case recording = 1
    case complete = 2
    case interrupted = 3
    case incompleteSources = 4
}

enum CoreFinalizeReason: Int32, Sendable {
    case unknown = 0
    case userStop = 1
    case callEnded = 2
    case recovery = 3
    case cancelled = 4
    case processInterrupted = 5
}

enum CoreLanguageMode: UInt32, Sendable {
    case unknown = 0
    case russian = 1
    case english = 2
    case russianEnglish = 3
}

enum CoreSourceHealth: Int32, Sendable {
    case unknown = 0
    case ready = 1
    case active = 2
    case temporarilyUnavailable = 3
    case permanentlyLost = 4
}

enum CoreSourceEventKind: Int32, Sendable {
    case unknown = 0
    case ready = 1
    case active = 2
    case unavailable = 3
    case recovered = 4
    case permanentlyLost = 5
    case discontinuity = 6
}

struct CoreSessionConfiguration: Sendable {
    let sessionID: UUID
    let journalURL: URL
    let sourceApplication: String
    let localSpeakerName: String
    let asrBackendID: String
    let asrModelURL: URL
    let diarizationBackendID: String
    let createdAt: Date
    let languageMode: CoreLanguageMode
    let audioQueueCapacityFrames: UInt32
    let microphoneSourceID: UInt64
    let systemAudioSourceID: UInt64
    let sourceCompletenessThresholdNanoseconds: Int64
}

struct CoreMarkdownOptions: Sendable {
    let title: String
    /// Nil preserves the original durable journal timestamp.
    let createdAt: Date?
    let endedAt: Date?
    let durationSeconds: Int64
    let microphoneCaptured: Bool
    let systemAudioCaptured: Bool
}

/// Immutable Markdown bytes and the exact durable journal boundary used to
/// render them. Publication acknowledgements must use this token, never a
/// separately sampled metrics value.
struct CoreRenderedMarkdown: Sendable, Equatable {
    let data: Data
    let journalCheckpoint: UInt64
    let highestSegmentRevision: UInt32
}

struct CoreVoiceProfile: Sendable, Equatable, Identifiable {
    let profileID: UInt64
    let displayName: String
    let sampleCount: UInt64

    var id: UInt64 { profileID }
}

struct CoreVoiceProfileEnrollment: Sendable, Equatable {
    let profileID: UInt64
    let speakerID: UInt64
    let sampleCount: UInt64
    let relabeledSegments: UInt32
    let journalCheckpoint: UInt64
    let highestSegmentRevision: UInt32
}

struct LatestRequestGeneration: Sendable {
    private var current: UInt64 = 0

    mutating func advance() -> UInt64 {
        current &+= 1
        if current == 0 {
            current = 1
        }
        return current
    }

    func isCurrent(_ generation: UInt64) -> Bool {
        generation != 0 && generation == current
    }
}

struct CoreTranscriptSegment: Sendable, Equatable {
    let stableID: UUID
    let sourceID: UInt64
    let startTimeNanoseconds: Int64
    let endTimeNanoseconds: Int64
    let speakerID: UInt64
    let speakerLabel: String
    let text: String
    let language: String
    let confidence: Float
    let revision: UInt32
    let isFinal: Bool
    let isUnintelligible: Bool
}

struct CorePipelineMetrics: Sendable, Equatable {
    let framesOffered: UInt64
    let framesAccepted: UInt64
    let framesRejected: UInt64
    let discontinuities: UInt64
    let finalSegmentsCommitted: UInt64
    let partialEventsCoalesced: UInt64
    let audioQueueDepth: UInt32
    let audioQueueHighWater: UInt32
    let journalCheckpoint: UInt64
    let highestSegmentRevision: UInt32
}

struct CoreStateEvent: Sendable, Equatable {
    let phase: CorePhase
    let publishedStatus: CorePublishedStatus
    let finalizeReason: CoreFinalizeReason
}

struct CoreSourceEvent: Sendable, Equatable {
    let sourceID: UInt64
    let sourceKind: CaptureSourceKind
    let eventKind: CoreSourceEventKind
    let health: CoreSourceHealth
    let startTimeNanoseconds: Int64
    let endTimeNanoseconds: Int64
}

enum CoreEvent: Sendable, Equatable {
    case stateChanged(CoreStateEvent)
    case finalSegment(CoreTranscriptSegment)
    case sourceChanged(CoreSourceEvent)
    case metrics(CorePipelineMetrics)
    case terminal(CoreStateEvent)
    case error(code: String)
}

enum CoreBridgeError: Error, Sendable, Equatable {
    case status(Int32)
    case closed
    case malformedCoreValue
    case invalidUTF8
    case unavailable
}

protocol CoreClientProtocol: Sendable {
    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) throws -> any CoreSessionProtocol

    func recoverableSessionIDs() throws -> [String]
    func openRecoverableSession(id: String) throws -> any CoreSessionProtocol
    func listVoiceProfiles() throws -> [CoreVoiceProfile]
    func enrollVoiceProfile(
        sessionID: UUID,
        speakerID: UInt64,
        displayName: String
    ) throws -> CoreVoiceProfileEnrollment
    func renameVoiceProfile(profileID: UInt64, displayName: String) throws
    func deleteVoiceProfile(profileID: UInt64) throws
}

extension CoreClientProtocol {
    func listVoiceProfiles() throws -> [CoreVoiceProfile] { [] }

    func enrollVoiceProfile(
        sessionID: UUID,
        speakerID: UInt64,
        displayName: String
    ) throws -> CoreVoiceProfileEnrollment {
        throw CoreBridgeError.unavailable
    }

    func renameVoiceProfile(
        profileID: UInt64,
        displayName: String
    ) throws {
        throw CoreBridgeError.unavailable
    }

    func deleteVoiceProfile(profileID: UInt64) throws {
        throw CoreBridgeError.unavailable
    }
}

protocol CoreSessionProtocol: AnyObject, Sendable {
    var sessionID: UUID { get }

    func markSourcesReady() throws
    func pushAudio(_ frame: CapturedAudioFrame) -> AudioFrameDisposition
    func pause() throws
    func resumeAfterConsent() throws
    func sourceEvent(
        sourceID: UInt64,
        kind: CaptureSourceKind,
        event: CoreSourceEventKind,
        health: CoreSourceHealth,
        startTimeNanoseconds: Int64,
        endTimeNanoseconds: Int64,
        reasonCode: String
    ) throws
    func finalize(reason: CoreFinalizeReason) throws
    func nextEvent(timeoutMilliseconds: UInt32) throws -> CoreEvent?
    func currentState() throws -> CoreStateEvent
    func metrics() throws -> CorePipelineMetrics
    func renderMarkdown(
        options: CoreMarkdownOptions
    ) throws -> CoreRenderedMarkdown
    func acknowledgePublication(
        receipt: MarkdownPublicationReceipt,
        journalCheckpoint: UInt64
    ) throws
    func close()
}
