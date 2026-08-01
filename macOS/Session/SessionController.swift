import Foundation

enum ConsentAction: Sendable {
    case start
    case resume
}

struct ConsentToken: Sendable {
    fileprivate let id: UUID
    fileprivate let action: ConsentAction
    fileprivate let issuedAt: ContinuousClock.Instant
}

/// The app model invokes this issuer only from a visible Start or Resume button.
/// Detection, restoration, and recovery adapters receive no issuer.
@MainActor
final class VisibleConsentIssuer {
    func issue(for action: ConsentAction) -> ConsentToken {
        ConsentToken(
            id: UUID(),
            action: action,
            issuedAt: .now
        )
    }
}

enum SessionShellState: String, Sendable {
    case idle
    case detected
    case awaitingConsent
    case preparing
    case recording
    case paused
    case finalizing
    case recoveryRequired
    case failedToStart
    case complete
    case incompleteSources
    case interrupted
}

enum SourceDisplayState: String, Sendable {
    case unknown
    case ready
    case active
    case unavailable
    case lost
}

enum SessionFailureCode: String, Sendable {
    case invalidTransition
    case consentExpired
    case consentAlreadyUsed
    case vaultNotSelected
    case modelNotSelected
    case permissionsDenied
    case screenPermissionRestartRequired
    case modelUnavailable
    case captureUnavailable
    case coreUnavailable
    case publicationUnavailable
    case internalFailure
}

struct SessionSpeakerSnapshot: Sendable, Equatable, Identifiable {
    let sessionID: UUID
    let speakerID: UInt64
    let displayName: String
    let segmentCount: Int
    let latestExcerpt: String
    let isAnonymous: Bool

    var id: UInt64 { speakerID }
}

struct SessionSnapshot: Sendable {
    let state: SessionShellState
    let sessionID: UUID?
    let startedAt: Date?
    let microphone: SourceDisplayState
    let systemAudio: SourceDisplayState
    let metrics: CorePipelineMetrics?
    let recentFinalSegments: [CoreTranscriptSegment]
    let speakers: [SessionSpeakerSnapshot]
    let lastPublicationDestination: PublicationDestination?
    let lastPublishedFilename: String?
    let lastPublishedURL: URL?
    let failureCode: SessionFailureCode?

    static let idle = SessionSnapshot(
        state: .idle,
        sessionID: nil,
        startedAt: nil,
        microphone: .unknown,
        systemAudio: .unknown,
        metrics: nil,
        recentFinalSegments: [],
        speakers: [],
        lastPublicationDestination: nil,
        lastPublishedFilename: nil,
        lastPublishedURL: nil,
        failureCode: nil
    )
}

struct SessionStartRequest: Sendable {
    let sourceApplication: String
    let title: String
    let preferredFilenameStem: String
    let localSpeakerName: String
    let languageMode: CoreLanguageMode
}

enum SessionControllerError: Error, Sendable {
    case invalidTransition
    case invalidConsent(SessionFailureCode)
    case preflight(SessionFailureCode)
}

actor SessionController {
    private struct TerminationSourceGap: Sendable {
        let sourceID: UInt64
        let kind: CaptureSourceKind
        let startTimeNanoseconds: Int64
        let endTimeNanoseconds: Int64
    }

    private struct TerminationResources: Sendable {
        let session: any CoreSessionProtocol
        let modelLease: (any SecurityScopedResourceLeasing)?
        let sourceGaps: [TerminationSourceGap]
    }

    private struct SessionReviewContext: Sendable {
        let sessionID: UUID
        let request: SessionStartRequest
        let createdAt: Date
        let endedAt: Date
        let microphoneCaptured: Bool
        let systemAudioCaptured: Bool
    }

    private struct SpeakerAccumulator {
        var displayName: String
        var segmentCount: Int
        var firstStartTimeNanoseconds: Int64
        var latestEndTimeNanoseconds: Int64
        var latestExcerpt: String
    }

    private struct VoiceProfileEnrollmentTarget {
        let sessionID: UUID
        let activeGeneration: UInt64?
        let reviewContext: SessionReviewContext?
    }

    nonisolated let updates: AsyncStream<SessionSnapshot>

    private let updateContinuation: AsyncStream<SessionSnapshot>.Continuation
    private let coreClient: any CoreClientProtocol
    private let permissions: any PermissionProviding
    private let directoryStore: any VaultSelectionProviding
    private let modelStore: any ModelSelectionProviding
    private let vaultWriter: any MarkdownPublishing
    private let microphoneCapture: any AudioCaptureSource
    private let systemAudioCapture: any AudioCaptureSource
    private let journalURL: URL

    private var snapshot: SessionSnapshot = .idle
    private var session: (any CoreSessionProtocol)?
    private var frameRouter = AudioFrameRouter()
    private var modelLease: (any SecurityScopedResourceLeasing)?
    private var eventPollTask: Task<Void, Never>?
    private var captureEventTask: Task<Void, Never>?
    private var publicationTask: Task<Void, Never>?
    private var publicationWorkerID: UUID?
    private var publicationDirty = false
    private var nextPublicationSequence: UInt64 = 0
    private var captureEventMailbox: CaptureEventMailbox?
    private var consumedConsentIDs = Set<UUID>()
    private var activeRequest: SessionStartRequest?
    private var activeCreatedAt: Date?
    private var lastReviewContext: SessionReviewContext?
    private var observedFinalSegments: [UUID: CoreTranscriptSegment] = [:]
    private var speakerAliases:
        [UInt64: (speakerID: UInt64, displayName: String)] = [:]
    private var reviewPublicationTask: Task<Void, Never>?
    private var reviewPublicationWorkerID: UUID?
    private var reviewPublicationContext: SessionReviewContext?
    private var reviewPublicationDirty = false
    private var intentionalCaptureStop = false
    private var sourceUnavailableSince: [CaptureSourceKind: Int64] = [:]
    private var sourceRecoveryTasks:
        [CaptureSourceKind: Task<Void, Never>] = [:]
    private var sourceRecoveryWorkerIDs:
        [CaptureSourceKind: UUID] = [:]
    private var sourcesBeingRestarted = Set<CaptureSourceKind>()
    private var terminalPublicationClaimed = false
    private var lifecycleLocked = false
    private var lifecycleWaiters: [CheckedContinuation<Void, Never>] = []
    private var activeGeneration: UInt64 = 0
    private var startupRecoveryScanned = false
    private var startupRecoveryInProgress = false
    private var terminationRequested = false

    init(
        coreClient: any CoreClientProtocol,
        permissions: any PermissionProviding,
        directoryStore: any VaultSelectionProviding,
        modelStore: any ModelSelectionProviding,
        vaultWriter: any MarkdownPublishing,
        microphoneCapture: any AudioCaptureSource,
        systemAudioCapture: any AudioCaptureSource,
        journalURL: URL
    ) {
        let pair = AsyncStream<SessionSnapshot>.makeStream(
            bufferingPolicy: .bufferingNewest(16)
        )
        updates = pair.stream
        updateContinuation = pair.continuation
        self.coreClient = coreClient
        self.permissions = permissions
        self.directoryStore = directoryStore
        self.modelStore = modelStore
        self.vaultWriter = vaultWriter
        self.microphoneCapture = microphoneCapture
        self.systemAudioCapture = systemAudioCapture
        self.journalURL = journalURL
        pair.continuation.yield(.idle)
    }

    func proposeManualStart() throws {
        guard session == nil,
              startupRecoveryScanned,
              snapshot.state == .idle
                || snapshot.state == .detected
                || snapshot.state == .failedToStart
                || snapshot.state == .complete
                || snapshot.state == .incompleteSources
                || snapshot.state == .interrupted
        else {
            throw SessionControllerError.invalidTransition
        }
        update(state: .awaitingConsent, failure: nil)
    }

    func proposeDetectedCall() {
        guard startupRecoveryScanned, snapshot.state == .idle else {
            return
        }
        update(state: .detected, failure: nil)
    }

    func dismissProposal() {
        guard snapshot.state == .awaitingConsent || snapshot.state == .detected else {
            return
        }
        update(state: .idle, failure: nil)
    }

    func start(
        after token: ConsentToken,
        request: SessionStartRequest
    ) async {
        await acquireLifecycleSlot()
        defer { releaseLifecycleSlot() }

        guard !terminationRequested,
              session == nil,
              snapshot.state == .awaitingConsent
        else {
            return
        }

        do {
            try consume(token, expectedAction: .start)
        } catch let error as SessionControllerError {
            if case let .invalidConsent(code) = error {
                failStart(code)
            } else {
                failStart(.invalidTransition)
            }
            return
        } catch {
            failStart(.invalidTransition)
            return
        }

        update(state: .preparing, failure: nil)
        let sessionID = UUID()
        let createdAt = Date()

        do {
            guard await directoryStore.hasSelection() else {
                throw SessionControllerError.preflight(.vaultNotSelected)
            }
            try checkTerminationRequested()
            guard await modelStore.hasSelection() else {
                throw SessionControllerError.preflight(.modelNotSelected)
            }
            try checkTerminationRequested()

            do {
                _ = try await permissions.requestRequiredPermissionsAfterConsent()
            } catch PermissionClientError.screenRecordingRestartRequired {
                throw SessionControllerError.preflight(
                    .screenPermissionRestartRequired
                )
            } catch {
                throw SessionControllerError.preflight(.permissionsDenied)
            }
            try checkTerminationRequested()

            let lease: any SecurityScopedResourceLeasing
            do {
                lease = try await modelStore.resolveLease()
            } catch {
                throw SessionControllerError.preflight(.modelUnavailable)
            }
            modelLease = lease
            try checkTerminationRequested()

            let configuration = CoreSessionConfiguration(
                sessionID: sessionID,
                journalURL: journalURL,
                sourceApplication: request.sourceApplication,
                localSpeakerName: request.localSpeakerName,
                asrBackendID: "whisper.cpp",
                asrModelURL: lease.url,
                diarizationBackendID: "acoustic-clustering",
                createdAt: createdAt,
                languageMode: request.languageMode,
                // Core queues bounded 250 ms aggregate blocks. Twenty-four
                // blocks retain roughly six seconds per source without
                // recreating the old 256-callback backlog.
                audioQueueCapacityFrames: 24,
                microphoneSourceID: microphoneCapture.sourceID,
                systemAudioSourceID: systemAudioCapture.sourceID,
                sourceCompletenessThresholdNanoseconds: 10_000_000_000
            )
            let coreSession: any CoreSessionProtocol
            do {
                let client = coreClient
                coreSession = try await Task.detached(
                    priority: .userInitiated
                ) {
                    try client.createSessionAfterConsent(
                        configuration: configuration
                    )
                }.value
            } catch {
                try checkTerminationRequested()
                throw SessionControllerError.preflight(.coreUnavailable)
            }
            do {
                try checkTerminationRequested()
            } catch {
                _ = try? await Task.detached(priority: .userInitiated) {
                    try coreSession.finalize(reason: .cancelled)
                    return try coreSession.currentState()
                }.value
                coreSession.close()
                throw error
            }

            session = coreSession
            activeRequest = request
            activeCreatedAt = createdAt
            lastReviewContext = nil
            observedFinalSegments.removeAll(keepingCapacity: true)
            speakerAliases.removeAll(keepingCapacity: true)
            sourceUnavailableSince.removeAll()
            cancelSourceRecoveryTasks()
            sourcesBeingRestarted.removeAll()
            terminalPublicationClaimed = false
            activeGeneration &+= 1
            let generation = activeGeneration
            frameRouter.park(coreSession)
            beginCaptureEventRelay(generation: generation)

            do {
                try await startCaptureAdapters(generation: generation)
                try checkTerminationRequested(generation: generation)
                try coreSession.markSourcesReady()
                frameRouter.activate(coreSession)
                beginEventPolling(coreSession, generation: generation)
            } catch {
                await stopCaptureAdaptersForTerminationBounded()
                frameRouter.detach()
                if let resources = takeTerminationResources(
                    includeSourceGaps: false
                ) {
                    await finalizeForTerminationBounded(
                        resources,
                        reason: .cancelled
                    )
                }
                throw SessionControllerError.preflight(.captureUnavailable)
            }

            snapshot = SessionSnapshot(
                state: .recording,
                sessionID: sessionID,
                startedAt: createdAt,
                microphone: .active,
                systemAudio: .active,
                metrics: nil,
                recentFinalSegments: [],
                speakers: [],
                lastPublicationDestination: nil,
                lastPublishedFilename: nil,
                lastPublishedURL: nil,
                failureCode: nil
            )
            emit()
            // Publishing is deliberately outside the lifecycle critical
            // section. A slow/removable vault must never delay Stop or Quit
            // while capture is active.
            schedulePublication(
                generation: generation,
                delayNanoseconds: 0
            )
        } catch let error as SessionControllerError {
            cleanupFailedStart()
            if case let .preflight(code) = error {
                failStart(code)
            } else {
                failStart(.internalFailure)
            }
        } catch {
            cleanupFailedStart()
            failStart(.internalFailure)
        }
    }

    func pause() async {
        await acquireLifecycleSlot()
        defer { releaseLifecycleSlot() }

        guard snapshot.state == .recording, let session else {
            return
        }

        cancelSourceRecoveryTasks()
        intentionalCaptureStop = true
        defer { intentionalCaptureStop = false }
        frameRouter.detach()
        await stopCaptureAdapters()
        do {
            try closeOutstandingSourceGaps(on: session)
            try await Task.detached(priority: .userInitiated) {
                try session.pause()
            }.value
            guard !terminationRequested else {
                return
            }
            update(state: .paused, failure: nil)
            schedulePublication(generation: activeGeneration)
        } catch {
            await interruptActiveSession(code: .coreUnavailable)
        }
    }

    func resume(after token: ConsentToken) async {
        await acquireLifecycleSlot()
        defer { releaseLifecycleSlot() }

        guard !terminationRequested,
              snapshot.state == .paused,
              let session
        else {
            return
        }

        do {
            try consume(token, expectedAction: .resume)
            let permissionSnapshot = await permissions.currentSnapshot()
            guard permissionSnapshot.allRequiredAuthorized else {
                throw SessionControllerError.preflight(.permissionsDenied)
            }

            let captureStartupBegan = monotonicNow()
            frameRouter.park(session)
            try await startCaptureAdapters(generation: activeGeneration)
            try checkTerminationRequested(generation: activeGeneration)
            try session.resumeAfterConsent()
            let captureStartupEnded = monotonicNow()
            try recordResumeCaptureStartupGap(
                on: session,
                startTimeNanoseconds: captureStartupBegan,
                endTimeNanoseconds: captureStartupEnded
            )
            frameRouter.activate(session)
            update(state: .recording, failure: nil)
        } catch let error as SessionControllerError {
            if case let .preflight(code) = error {
                await interruptActiveSession(code: code)
            } else {
                await interruptActiveSession(code: .invalidTransition)
            }
        } catch {
            await interruptActiveSession(code: .captureUnavailable)
        }
    }

    func stop() async {
        await acquireLifecycleSlot()
        defer { releaseLifecycleSlot() }

        guard snapshot.state == .recording || snapshot.state == .paused,
              let session
        else {
            return
        }

        let generation = activeGeneration
        cancelSourceRecoveryTasks()
        update(state: .finalizing, failure: nil)
        cancelPublicationWorker()
        terminalPublicationClaimed = true
        intentionalCaptureStop = true
        frameRouter.detach()
        await stopCaptureAdaptersForTerminationBounded()
        do {
            try closeOutstandingSourceGaps(on: session)
            let terminalState = try await Task.detached(
                priority: .userInitiated
            ) {
                try session.finalize(reason: .userStop)
                return try session.currentState()
            }.value
            guard terminalState.phase.isTerminal else {
                throw SessionControllerError.invalidTransition
            }
            await drainFinalEventsThroughTerminalBoundary(
                from: session,
                generation: generation
            )
            guard generation == activeGeneration else {
                intentionalCaptureStop = false
                return
            }
            applyCorePhase(terminalState.phase)
            let endedAt = Date()
            rememberReviewContext(endedAt: endedAt)
            await publishTerminalSnapshotBounded(
                endedAt: endedAt,
                generation: generation
            )
            finishTerminalSession()
        } catch {
            await interruptActiveSession(code: .coreUnavailable)
        }
        intentionalCaptureStop = false
    }

    /// Quiesces a partially-started capture without waiting for the lifecycle
    /// slot held by `start`. This is the privacy escape hatch used by AppKit
    /// termination while ScreenCaptureKit is suspended in framework startup.
    func prepareForTermination() async {
        terminationRequested = true
        cancelPublicationWorker()
        frameRouter.detach()
        intentionalCaptureStop = true
        await stopCaptureAdaptersForTerminationBounded()
        intentionalCaptureStop = false

        // A PREPARING start observes terminationRequested after every await
        // and tears down its local handle. Terminalize the shared handle here
        // as FAILED_TO_START as well, so a clean Quit cannot leave a
        // user-cancelled PREPARING row to be misclassified as crash recovery.
        if snapshot.state == .preparing {
            if let resources = takeTerminationResources(
                includeSourceGaps: false
            ) {
                await finalizeForTerminationBounded(
                    resources,
                    reason: .cancelled
                )
            }
            failStart(.captureUnavailable)
            return
        }

        // A user Stop/Pause or terminal handler may already own the lifecycle
        // slot while suspended in a framework call. Capture is quiesced above;
        // waiting for that slot would only keep AppKit in terminateLater.
        guard !lifecycleLocked else {
            return
        }
        if snapshot.state == .recording || snapshot.state == .paused {
            update(state: .finalizing, failure: nil)
            if let resources = takeTerminationResources(
                includeSourceGaps: true
            ) {
                await finalizeForTerminationBounded(
                    resources,
                    reason: .userStop
                )
            }
        }
    }

    func currentPermissionSnapshot() async -> CapturePermissionSnapshot {
        await permissions.currentSnapshot()
    }

    func voiceProfiles() async throws -> [CoreVoiceProfile] {
        let client = coreClient
        return try await Task.detached(priority: .utility) {
            try client.listVoiceProfiles()
        }.value
    }

    @discardableResult
    func enrollVoiceProfile(
        sessionID expectedSessionID: UUID,
        speakerID: UInt64,
        displayName: String
    ) async throws -> CoreVoiceProfileEnrollment {
        guard voiceProfileEnrollmentTarget(
            expectedSessionID: expectedSessionID
        ) != nil else {
            throw SessionControllerError.invalidTransition
        }

        await acquireLifecycleSlot()
        let name = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, speakerID != 1,
              let target = voiceProfileEnrollmentTarget(
                  expectedSessionID: expectedSessionID
              )
        else {
            releaseLifecycleSlot()
            throw SessionControllerError.invalidTransition
        }

        let client = coreClient
        let enrollment: CoreVoiceProfileEnrollment
        do {
            enrollment = try await Task.detached(priority: .userInitiated) {
                try client.enrollVoiceProfile(
                    sessionID: target.sessionID,
                    speakerID: speakerID,
                    displayName: name
                )
            }.value
        } catch {
            releaseLifecycleSlot()
            throw error
        }

        applyEnrollment(
            enrollment,
            originalSpeakerID: speakerID,
            displayName: name,
            sessionID: target.sessionID
        )
        releaseLifecycleSlot()

        if let generation = target.activeGeneration,
           generation == activeGeneration,
           session != nil,
           snapshot.sessionID == target.sessionID
        {
            schedulePublication(
                generation: generation,
                delayNanoseconds: 0
            )
        } else if let reviewContext = target.reviewContext {
            scheduleReviewPublication(reviewContext)
        }
        return enrollment
    }

    func renameVoiceProfile(
        profileID: UInt64,
        displayName: String
    ) async throws {
        let name = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            throw SessionControllerError.invalidTransition
        }
        let client = coreClient
        try await Task.detached(priority: .utility) {
            try client.renameVoiceProfile(
                profileID: profileID,
                displayName: name
            )
        }.value
    }

    func deleteVoiceProfile(profileID: UInt64) async throws {
        let client = coreClient
        try await Task.detached(priority: .utility) {
            try client.deleteVoiceProfile(profileID: profileID)
        }.value
    }

    /// Recovers durable journal sessions without touching consent, TCC,
    /// capture adapters, the ASR model, or the active-session slot.
    func recoverInterruptedSessions() async {
        guard session == nil,
              !startupRecoveryInProgress
        else {
            return
        }
        startupRecoveryInProgress = true
        startupRecoveryScanned = false
        defer {
            startupRecoveryInProgress = false
        }

        let identifiers: [String]
        do {
            let client = coreClient
            identifiers = try await Task.detached(priority: .utility) {
                try client.recoverableSessionIDs()
            }.value
        } catch {
            snapshot = recoverySnapshot(
                state: .recoveryRequired,
                sessionID: nil,
                failure: .coreUnavailable
            )
            emit()
            return
        }

        var firstFailure: SessionSnapshot?
        for identifier in identifiers {
            guard let sessionID = UUID(uuidString: identifier) else {
                snapshot = recoverySnapshot(
                    state: .recoveryRequired,
                    sessionID: nil,
                    failure: .coreUnavailable
                )
                emit()
                if firstFailure == nil {
                    firstFailure = snapshot
                }
                continue
            }
            let succeeded = await recoverInterruptedSession(
                id: sessionID,
                durableIdentifier: identifier
            )
            if !succeeded, firstFailure == nil {
                firstFailure = snapshot
            }
        }

        if let firstFailure {
            // Successful later sessions must not hide an earlier durable
            // recovery failure. A retry re-scans the journal, whose
            // publication acknowledgements remove already-completed work.
            snapshot = firstFailure
            emit()
            return
        }

        startupRecoveryScanned = true
        if identifiers.isEmpty, snapshot.state == .recoveryRequired {
            snapshot = .idle
            emit()
        }
    }

    func currentSnapshot() -> SessionSnapshot {
        snapshot
    }

    private func recoverInterruptedSession(
        id sessionID: UUID,
        durableIdentifier: String
    ) async -> Bool {
        snapshot = recoverySnapshot(
            state: .recoveryRequired,
            sessionID: sessionID
        )
        emit()

        let recovered: any CoreSessionProtocol
        do {
            let client = coreClient
            recovered = try await Task.detached(priority: .utility) {
                try client.openRecoverableSession(id: durableIdentifier)
            }.value
        } catch {
            snapshot = recoverySnapshot(
                state: .recoveryRequired,
                sessionID: sessionID,
                failure: .coreUnavailable
            )
            emit()
            return false
        }
        defer {
            // A backend close may wait for an inference or SQLite worker.
            // Recovery must not monopolize the controller actor (and AppKit
            // Quit) while that native teardown completes.
            _ = Task.detached(priority: .utility) {
                recovered.close()
            }
        }

        let terminalState: SessionShellState
        let rendered: CoreRenderedMarkdown
        let metrics: CorePipelineMetrics?
        do {
            let nativeResult = try await Task.detached(
                priority: .utility
            ) {
                let state = try recovered.currentState()
                let terminalState: SessionShellState
                switch state.phase {
                case .recoveryRequired:
                    try recovered.finalize(reason: .recovery)
                    terminalState = .interrupted
                case .complete:
                    terminalState = .complete
                case .incompleteSources:
                    terminalState = .incompleteSources
                case .interrupted:
                    terminalState = .interrupted
                default:
                    throw SessionControllerError.invalidTransition
                }
                let rendered = try recovered.renderMarkdown(
                    options: CoreMarkdownOptions(
                        title: "Recovered Call",
                        createdAt: nil,
                        endedAt: nil,
                        durationSeconds: -1,
                        microphoneCaptured: false,
                        systemAudioCaptured: false
                    )
                )
                return (
                    terminalState,
                    rendered,
                    try? recovered.metrics()
                )
            }.value
            terminalState = nativeResult.0
            rendered = nativeResult.1
            metrics = nativeResult.2
        } catch {
            snapshot = recoverySnapshot(
                state: .recoveryRequired,
                sessionID: sessionID,
                failure: .coreUnavailable
            )
            emit()
            return false
        }

        let receipt: MarkdownPublicationReceipt
        do {
            let publicationSequence = reservePublicationSequence()
            receipt = try await vaultWriter.publish(
                MarkdownPublicationRequest(
                    sessionID: sessionID,
                    preferredFilenameStem: Self.recoveryFilenameStem(
                        for: sessionID
                    ),
                    markdown: rendered.data,
                    highestSegmentRevision:
                        UInt64(rendered.highestSegmentRevision),
                    publicationSequence: publicationSequence,
                    expectedPrevious: nil
                )
            )
        } catch {
            snapshot = recoverySnapshot(
                state: .recoveryRequired,
                sessionID: sessionID,
                metrics: metrics,
                failure: .publicationUnavailable
            )
            emit()
            return false
        }

        do {
            try await Task.detached(priority: .utility) {
                try recovered.acknowledgePublication(
                    receipt: receipt,
                    journalCheckpoint: rendered.journalCheckpoint
                )
            }.value
        } catch {
            snapshot = recoverySnapshot(
                state: .recoveryRequired,
                sessionID: sessionID,
                metrics: metrics,
                destination: receipt.destination,
                filename: receipt.filename,
                url: receipt.url,
                failure: .coreUnavailable
            )
            emit()
            return false
        }

        snapshot = recoverySnapshot(
            state: terminalState,
            sessionID: sessionID,
            metrics: metrics,
            destination: receipt.destination,
            filename: receipt.filename,
            url: receipt.url
        )
        emit()
        return true
    }

    private func startCaptureAdapters(generation: UInt64) async throws {
        guard let captureEventMailbox else {
            throw SessionControllerError.preflight(.captureUnavailable)
        }
        try checkTerminationRequested(generation: generation)
        captureEventMailbox.beginHealthAttempt()

        let frameHandler: AudioFrameHandler = { [frameRouter] frame in
            frameRouter.push(frame)
        }
        let eventHandler: CaptureEventHandler = { event in
            captureEventMailbox.yield(event)
        }

        do {
            try await microphoneCapture.start(
                frameHandler: frameHandler,
                eventHandler: eventHandler
            )
            try checkTerminationRequested(generation: generation)
            try await systemAudioCapture.start(
                frameHandler: frameHandler,
                eventHandler: eventHandler
            )
            try checkTerminationRequested(generation: generation)
            // Capture callbacks can arrive while this actor is suspended in
            // PREPARING/PAUSED. The relay intentionally does not mutate the
            // session in those states, so consult the mailbox's synchronous
            // sticky health before declaring both required sources active.
            guard captureEventMailbox.requiredSourcesAreReady else {
                throw SessionControllerError.preflight(.captureUnavailable)
            }
        } catch {
            await microphoneCapture.stop()
            await systemAudioCapture.stop()
            throw error
        }
    }

    private func stopCaptureAdapters() async {
        await microphoneCapture.stop()
        await systemAudioCapture.stop()
    }

    private func stopCaptureAdaptersForTerminationBounded() async {
        let completion = PublicationAttemptCompletion()
        let microphoneCapture = self.microphoneCapture
        let systemAudioCapture = self.systemAudioCapture
        let task = Task {
            async let microphoneStop: Void = microphoneCapture.stop()
            async let systemAudioStop: Void = systemAudioCapture.stop()
            _ = await (microphoneStop, systemAudioStop)
            completion.complete(true)
        }

        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(1))
        while clock.now < deadline {
            if completion.result != nil {
                return
            }
            try? await Task.sleep(for: .milliseconds(20))
        }
        // Frame routing was detached before this attempt. Cancellation is
        // advisory for framework calls, but Quit must not remain hostage to a
        // non-returning SCStream.stopCapture; process teardown is the final
        // capture boundary.
        task.cancel()
    }

    private func beginEventPolling(
        _ session: any CoreSessionProtocol,
        generation: UInt64
    ) {
        eventPollTask?.cancel()
        eventPollTask = Task.detached(priority: .userInitiated) { [weak self] in
            while !Task.isCancelled {
                do {
                    if let event = try session.nextEvent(timeoutMilliseconds: 250) {
                        await self?.receiveCoreEvent(
                            event,
                            generation: generation
                        )
                    }
                } catch CoreBridgeError.closed {
                    return
                } catch {
                    guard !Task.isCancelled else {
                        return
                    }
                    await self?.receiveCorePollFailure(
                        generation: generation
                    )
                    return
                }
            }
        }
    }

    private func drainFinalEventsThroughTerminalBoundary(
        from coreSession: any CoreSessionProtocol,
        generation: UInt64
    ) async {
        let pollingTask = eventPollTask
        eventPollTask = nil
        pollingTask?.cancel()
        if let pollingTask {
            await pollingTask.value
        }

        guard generation == activeGeneration, session != nil else {
            return
        }
        while true {
            let event: CoreEvent?
            do {
                event = try coreSession.nextEvent(timeoutMilliseconds: 0)
            } catch {
                return
            }
            guard let event else {
                return
            }
            switch event {
            case let .finalSegment(segment):
                if segment.isFinal {
                    recordFinalSegment(segment)
                }
            case let .metrics(metrics):
                snapshot = replacingSnapshot(metrics: metrics)
                emit()
            case let .sourceChanged(source):
                applySourceEvent(source)
            case .terminal:
                return
            case .stateChanged, .error:
                continue
            }
        }
    }

    private func beginCaptureEventRelay(generation: UInt64) {
        captureEventTask?.cancel()
        captureEventMailbox?.finish()
        let mailbox = CaptureEventMailbox()
        captureEventMailbox = mailbox
        captureEventTask = Task { [weak self] in
            for await event in mailbox.stream {
                guard !Task.isCancelled else {
                    return
                }
                await self?.receiveCaptureEvent(
                    event,
                    generation: generation
                )
                mailbox.didConsume(event)
            }
        }
    }

    private func receiveCoreEvent(
        _ event: CoreEvent,
        generation: UInt64
    ) async {
        guard let session, generation == activeGeneration else {
            return
        }

        switch event {
        case let .stateChanged(state):
            // The core event queue can still contain PREPARING/RECORDING
            // transitions after Swift has completed a later transition.
            // Never let a stale queued event regress the privacy indicator or
            // temporarily remove Stop while capture routing is active.
            guard let authoritative = try? session.currentState(),
                  authoritative.phase == state.phase
            else {
                return
            }
            applyCorePhase(state.phase)
        case let .terminal(state):
            guard let authoritative = try? session.currentState(),
                  authoritative.phase == state.phase
            else {
                return
            }
            applyCorePhase(state.phase)
            guard !terminalPublicationClaimed else {
                return
            }
            await handleTerminalEvent(state, generation: generation)
        case let .finalSegment(segment):
            guard segment.isFinal else {
                return
            }
            recordFinalSegment(segment)
            schedulePublication(generation: generation)
        case let .metrics(metrics):
            snapshot = replacingSnapshot(metrics: metrics)
            emit()
        case let .sourceChanged(event):
            applySourceEvent(event)
        case .error:
            guard !terminalPublicationClaimed else {
                return
            }
            await receiveCorePollFailure(generation: generation)
        }
    }

    private func handleTerminalEvent(
        _ state: CoreStateEvent,
        generation: UInt64
    ) async {
        await acquireLifecycleSlot()
        defer { releaseLifecycleSlot() }

        guard session != nil,
              generation == activeGeneration,
              !terminalPublicationClaimed
        else {
            return
        }
        applyCorePhase(state.phase)
        terminalPublicationClaimed = true
        cancelPublicationWorker()
        frameRouter.detach()
        intentionalCaptureStop = true
        await stopCaptureAdapters()
        intentionalCaptureStop = false
        guard generation == activeGeneration else {
            return
        }
        let endedAt = Date()
        rememberReviewContext(endedAt: endedAt)
        await publishTerminalSnapshotBounded(
            endedAt: endedAt,
            generation: generation
        )
        guard generation == activeGeneration else {
            return
        }
        finishTerminalSession()
    }

    private func receiveCorePollFailure(generation: UInt64) async {
        guard !terminalPublicationClaimed else {
            return
        }
        await acquireLifecycleSlot()
        defer { releaseLifecycleSlot() }

        guard !terminalPublicationClaimed,
              session != nil,
              generation == activeGeneration
        else {
            return
        }
        // This callback is running on the poll task itself. Relinquish the
        // stored handle before interruption drains the now-exclusive queue.
        eventPollTask = nil
        await interruptActiveSession(code: .coreUnavailable)
    }

    private func receiveCaptureEvent(
        _ event: CaptureSourceEvent,
        generation: UInt64
    ) async {
        guard let session,
              generation == activeGeneration,
              snapshot.state == .recording
        else {
            return
        }

        switch event {
        case let .ready(kind):
            setSource(kind, state: .active)
            let now = monotonicNow()
            if let unavailableSince = sourceUnavailableSince.removeValue(
                forKey: kind
            ) {
                try? session.sourceEvent(
                    sourceID: sourceID(for: kind),
                    kind: kind,
                    event: .recovered,
                    health: .active,
                    startTimeNanoseconds: unavailableSince,
                    endTimeNanoseconds: now,
                    reasonCode: "capture_recovered"
                )
            } else {
                try? session.sourceEvent(
                    sourceID: sourceID(for: kind),
                    kind: kind,
                    event: .active,
                    health: .active,
                    startTimeNanoseconds: now,
                    endTimeNanoseconds: now,
                    reasonCode: "capture_ready"
                )
            }
        case let .stopped(kind):
            if !intentionalCaptureStop
                && !sourcesBeingRestarted.contains(kind)
            {
                let openedGap = markSourceUnavailable(
                    kind,
                    reasonCode: "capture_stopped",
                    session: session
                )
                if openedGap {
                    scheduleSourceRecoveryIfSupported(
                        kind,
                        generation: generation
                    )
                }
            }
        case let .frameRejected(kind):
            // The core owns per-source overload episodes and journals one
            // durable gap when the source recovers. Mirroring every capture
            // rejection here previously made source_events and the WAL grow
            // at callback frequency.
            _ = kind
        case let .failed(kind, code):
            let openedGap = markSourceUnavailable(
                kind,
                reasonCode: code,
                session: session
            )
            if openedGap {
                scheduleSourceRecoveryIfSupported(
                    kind,
                    generation: generation
                )
            }
        }
    }

    private func markSourceUnavailable(
        _ kind: CaptureSourceKind,
        reasonCode: String,
        session: any CoreSessionProtocol
    ) -> Bool {
        setSource(kind, state: .unavailable)
        guard sourceUnavailableSince[kind] == nil else {
            return false
        }
        let now = monotonicNow()
        sourceUnavailableSince[kind] = now
        try? session.sourceEvent(
            sourceID: sourceID(for: kind),
            kind: kind,
            event: .unavailable,
            health: .temporarilyUnavailable,
            startTimeNanoseconds: now,
            endTimeNanoseconds: now,
            reasonCode: reasonCode
        )
        return true
    }

    private func scheduleSourceRecoveryIfSupported(
        _ kind: CaptureSourceKind,
        generation: UInt64
    ) {
        // The microphone adapter owns route-change/watchdog recovery because
        // it must rebuild its AVAudioEngine tap. A failed ScreenCaptureKit
        // stream is restarted here through the generic capture contract.
        guard kind == .systemAudio,
              snapshot.state == .recording,
              generation == activeGeneration,
              sourceUnavailableSince[kind] != nil,
              sourceRecoveryTasks[kind] == nil
        else {
            return
        }

        let workerID = UUID()
        sourceRecoveryWorkerIDs[kind] = workerID
        sourceRecoveryTasks[kind] = Task { [weak self] in
            var recovered = false
            let backoffs: [UInt64] = [
                500_000_000,
                1_000_000_000,
                2_000_000_000,
                4_000_000_000,
                8_000_000_000,
                8_000_000_000,
                8_000_000_000,
            ]
            for delay in backoffs {
                do {
                    try await Task<Never, Never>.sleep(
                        nanoseconds: delay
                    )
                } catch {
                    break
                }
                guard !Task.isCancelled else {
                    break
                }
                if await self?.attemptSystemAudioRecovery(
                    generation: generation
                ) == true {
                    recovered = true
                    break
                }
            }
            let wasCancelled = Task.isCancelled
            await self?.sourceRecoveryFinished(
                kind,
                generation: generation,
                workerID: workerID,
                recovered: recovered,
                wasCancelled: wasCancelled
            )
        }
    }

    private func attemptSystemAudioRecovery(
        generation: UInt64
    ) async -> Bool {
        await acquireLifecycleSlot()
        defer { releaseLifecycleSlot() }

        guard generation == activeGeneration,
              snapshot.state == .recording,
              sourceUnavailableSince[.systemAudio] != nil,
              session != nil,
              let captureEventMailbox
        else {
            return false
        }

        sourcesBeingRestarted.insert(.systemAudio)
        defer { sourcesBeingRestarted.remove(.systemAudio) }
        await systemAudioCapture.stop()
        guard generation == activeGeneration,
              snapshot.state == .recording,
              !Task.isCancelled
        else {
            return false
        }

        let frameHandler: AudioFrameHandler = { [frameRouter] frame in
            frameRouter.push(frame)
        }
        let eventHandler: CaptureEventHandler = { event in
            captureEventMailbox.yield(event)
        }
        captureEventMailbox.beginHealthAttempt(for: .systemAudio)
        do {
            try await systemAudioCapture.start(
                frameHandler: frameHandler,
                eventHandler: eventHandler
            )
        } catch {
            return false
        }

        guard generation == activeGeneration,
              snapshot.state == .recording,
              !Task.isCancelled
        else {
            await systemAudioCapture.stop()
            return false
        }
        guard captureEventMailbox.isReady(.systemAudio) else {
            await systemAudioCapture.stop()
            return false
        }
        return true
    }

    private func sourceRecoveryFinished(
        _ kind: CaptureSourceKind,
        generation: UInt64,
        workerID: UUID,
        recovered: Bool,
        wasCancelled: Bool
    ) {
        guard generation == activeGeneration,
              sourceRecoveryWorkerIDs[kind] == workerID
        else {
            return
        }
        sourceRecoveryTasks.removeValue(forKey: kind)
        sourceRecoveryWorkerIDs.removeValue(forKey: kind)
        guard recovered, !wasCancelled else {
            return
        }
        guard snapshot.state == .recording,
              sourceUnavailableSince[kind] != nil,
              captureEventMailbox?.isReady(kind) != true
        else {
            return
        }
        // A stream can emit ready→failed while start() is still returning.
        // That failure is sticky in the mailbox but its relay cannot install
        // a second task until this task deregisters. Re-arm here so no source
        // loss silently exhausts recovery.
        scheduleSourceRecoveryIfSupported(
            kind,
            generation: generation
        )
    }

    private func cancelSourceRecoveryTasks() {
        for task in sourceRecoveryTasks.values {
            task.cancel()
        }
        sourceRecoveryTasks.removeAll()
        sourceRecoveryWorkerIDs.removeAll()
    }

    private func closeOutstandingSourceGaps(
        on session: any CoreSessionProtocol
    ) throws {
        let end = monotonicNow()
        let outstanding = sourceUnavailableSince
        for (kind, start) in outstanding {
            try session.sourceEvent(
                sourceID: sourceID(for: kind),
                kind: kind,
                event: .unavailable,
                health: .temporarilyUnavailable,
                startTimeNanoseconds: start,
                endTimeNanoseconds: max(start, end),
                reasonCode: "capture_gap_closed"
            )
            sourceUnavailableSince.removeValue(forKey: kind)
        }
    }

    private func recordResumeCaptureStartupGap(
        on session: any CoreSessionProtocol,
        startTimeNanoseconds: Int64,
        endTimeNanoseconds: Int64
    ) throws {
        let end = max(startTimeNanoseconds, endTimeNanoseconds)
        for kind in [CaptureSourceKind.microphone, .systemAudio] {
            try session.sourceEvent(
                sourceID: sourceID(for: kind),
                kind: kind,
                event: .recovered,
                health: .active,
                startTimeNanoseconds: startTimeNanoseconds,
                endTimeNanoseconds: end,
                reasonCode: "resume_capture_startup_gap"
            )
        }
    }

    private func recordFinalSegment(_ incoming: CoreTranscriptSegment) {
        let segment = applyingSpeakerAlias(to: incoming)
        if let existing = observedFinalSegments[segment.stableID],
           existing.revision > segment.revision
        {
            return
        }
        observedFinalSegments[segment.stableID] = segment
        rebuildSpeakerSnapshot()
        emit()
    }

    private func voiceProfileEnrollmentTarget(
        expectedSessionID: UUID
    ) -> VoiceProfileEnrollmentTarget? {
        if snapshot.sessionID == expectedSessionID,
           session != nil,
           activeRequest != nil,
           activeCreatedAt != nil
        {
            return VoiceProfileEnrollmentTarget(
                sessionID: expectedSessionID,
                activeGeneration: activeGeneration,
                reviewContext: nil
            )
        }
        guard let context = lastReviewContext,
              context.sessionID == expectedSessionID,
              session == nil
        else {
            return nil
        }
        return VoiceProfileEnrollmentTarget(
            sessionID: expectedSessionID,
            activeGeneration: nil,
            reviewContext: context
        )
    }

    private func applyingSpeakerAlias(
        to segment: CoreTranscriptSegment
    ) -> CoreTranscriptSegment {
        guard let alias = speakerAliases[segment.speakerID] else {
            return segment
        }
        return CoreTranscriptSegment(
            stableID: segment.stableID,
            sourceID: segment.sourceID,
            startTimeNanoseconds: segment.startTimeNanoseconds,
            endTimeNanoseconds: segment.endTimeNanoseconds,
            speakerID: alias.speakerID,
            speakerLabel: alias.displayName,
            text: segment.text,
            language: segment.language,
            confidence: segment.confidence,
            revision: segment.revision,
            isFinal: segment.isFinal,
            isUnintelligible: segment.isUnintelligible
        )
    }

    private func rebuildSpeakerSnapshot() {
        let ordered = observedFinalSegments.values.sorted { left, right in
            if left.startTimeNanoseconds != right.startTimeNanoseconds {
                return left.startTimeNanoseconds < right.startTimeNanoseconds
            }
            if left.endTimeNanoseconds != right.endTimeNanoseconds {
                return left.endTimeNanoseconds < right.endTimeNanoseconds
            }
            return left.stableID.uuidString < right.stableID.uuidString
        }
        guard let sessionID = snapshot.sessionID else {
            snapshot = replacingSnapshot(
                recentFinalSegments: Array(ordered.suffix(3)),
                speakers: []
            )
            return
        }
        var speakers: [UInt64: SpeakerAccumulator] = [:]
        for segment in ordered where segment.speakerID != 1 {
            if var current = speakers[segment.speakerID] {
                current.segmentCount += 1
                if segment.endTimeNanoseconds >= current.latestEndTimeNanoseconds {
                    current.displayName = segment.speakerLabel
                    current.latestEndTimeNanoseconds = segment.endTimeNanoseconds
                    current.latestExcerpt = segment.text
                }
                speakers[segment.speakerID] = current
            } else {
                speakers[segment.speakerID] = SpeakerAccumulator(
                    displayName: segment.speakerLabel,
                    segmentCount: 1,
                    firstStartTimeNanoseconds: segment.startTimeNanoseconds,
                    latestEndTimeNanoseconds: segment.endTimeNanoseconds,
                    latestExcerpt: segment.text
                )
            }
        }
        let speakerSnapshots = speakers.map { speakerID, value in
            SessionSpeakerSnapshot(
                sessionID: sessionID,
                speakerID: speakerID,
                displayName: value.displayName,
                segmentCount: value.segmentCount,
                latestExcerpt: value.latestExcerpt,
                isAnonymous:
                    speakerID & (UInt64(1) << 63) != 0
            )
        }.sorted { left, right in
            let leftStart = speakers[left.speakerID]?.firstStartTimeNanoseconds
                ?? Int64.max
            let rightStart = speakers[right.speakerID]?.firstStartTimeNanoseconds
                ?? Int64.max
            if leftStart != rightStart {
                return leftStart < rightStart
            }
            return left.speakerID < right.speakerID
        }
        snapshot = replacingSnapshot(
            recentFinalSegments: Array(ordered.suffix(3)),
            speakers: speakerSnapshots
        )
    }

    private func applyEnrollment(
        _ enrollment: CoreVoiceProfileEnrollment,
        originalSpeakerID: UInt64,
        displayName: String,
        sessionID: UUID
    ) {
        guard snapshot.sessionID == sessionID else {
            return
        }
        let alias = (
            speakerID: enrollment.speakerID,
            displayName: displayName
        )
        speakerAliases[originalSpeakerID] = alias
        speakerAliases[enrollment.speakerID] = alias
        observedFinalSegments = observedFinalSegments.mapValues { segment in
            guard segment.speakerID == originalSpeakerID
                    || segment.speakerID == enrollment.speakerID
            else {
                return segment
            }
            return applyingSpeakerAlias(to: segment)
        }
        rebuildSpeakerSnapshot()
        emit()
    }

    private func rememberReviewContext(endedAt: Date) {
        guard let sessionID = snapshot.sessionID,
              let request = activeRequest,
              let createdAt = activeCreatedAt
        else {
            return
        }
        lastReviewContext = SessionReviewContext(
            sessionID: sessionID,
            request: request,
            createdAt: createdAt,
            endedAt: endedAt,
            microphoneCaptured: snapshot.microphone == .active
                || snapshot.microphone == .ready,
            systemAudioCaptured: snapshot.systemAudio == .active
                || snapshot.systemAudio == .ready
        )
    }

    private func scheduleReviewPublication(
        _ context: SessionReviewContext
    ) {
        reviewPublicationContext = context
        reviewPublicationDirty = true
        guard reviewPublicationTask == nil else {
            return
        }

        let workerID = UUID()
        reviewPublicationWorkerID = workerID
        reviewPublicationTask = Task { [weak self] in
            await self?.runReviewPublicationWorker(workerID: workerID)
        }
    }

    private func runReviewPublicationWorker(workerID: UUID) async {
        while !Task.isCancelled,
              reviewPublicationWorkerID == workerID,
              reviewPublicationDirty,
              let context = reviewPublicationContext
        {
            reviewPublicationDirty = false
            let published = await publishReviewSnapshotBounded(context)
            if !published, snapshot.sessionID == context.sessionID {
                updateFailureOnly(.publicationUnavailable)
            }
        }
        guard reviewPublicationWorkerID == workerID else {
            return
        }
        reviewPublicationTask = nil
        reviewPublicationWorkerID = nil
        reviewPublicationContext = nil
        reviewPublicationDirty = false
    }

    private func publishReviewSnapshotBounded(
        _ context: SessionReviewContext
    ) async -> Bool {
        let completion = PublicationAttemptCompletion()
        let task = Task { [weak self] in
            let success = await self?.publishReviewSnapshot(context) ?? false
            completion.complete(success)
        }

        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(3))
        while clock.now < deadline {
            if let result = completion.result {
                return result
            }
            try? await Task.sleep(for: .milliseconds(20))
        }
        task.cancel()
        return false
    }

    private func publishReviewSnapshot(
        _ context: SessionReviewContext
    ) async -> Bool {
        let client = coreClient
        let reopened: any CoreSessionProtocol
        do {
            reopened = try await Task.detached(priority: .userInitiated) {
                try client.openRecoverableSession(
                    id: context.sessionID.uuidString.lowercased()
                )
            }.value
        } catch {
            return false
        }
        defer {
            Task.detached(priority: .utility) {
                reopened.close()
            }
        }
        guard !Task.isCancelled else {
            return false
        }

        do {
            let duration = Int64(
                max(0, context.endedAt.timeIntervalSince(context.createdAt))
            )
            let rendered = try await Task.detached(priority: .userInitiated) {
                try reopened.renderMarkdown(
                    options: CoreMarkdownOptions(
                        title: context.request.title,
                        createdAt: context.createdAt,
                        endedAt: context.endedAt,
                        durationSeconds: duration,
                        microphoneCaptured: context.microphoneCaptured,
                        systemAudioCaptured: context.systemAudioCaptured
                    )
                )
            }.value
            guard !Task.isCancelled else {
                return false
            }
            let previous = await vaultWriter.latestReceipt(
                for: context.sessionID
            )
            guard !Task.isCancelled else {
                return false
            }
            let publicationSequence = reservePublicationSequence()
            let receipt = try await vaultWriter.publish(
                MarkdownPublicationRequest(
                    sessionID: context.sessionID,
                    preferredFilenameStem:
                        context.request.preferredFilenameStem,
                    markdown: rendered.data,
                    highestSegmentRevision:
                        UInt64(rendered.highestSegmentRevision),
                    publicationSequence: publicationSequence,
                    expectedPrevious: previous?.fingerprint
                )
            )
            guard !Task.isCancelled else {
                return false
            }
            try await Task.detached(priority: .userInitiated) {
                try reopened.acknowledgePublication(
                    receipt: receipt,
                    journalCheckpoint: rendered.journalCheckpoint
                )
            }.value
            guard !Task.isCancelled else {
                return false
            }
            if snapshot.sessionID == context.sessionID {
                snapshot = replacingSnapshot(
                    destination: receipt.destination,
                    filename: receipt.filename,
                    url: receipt.url,
                    clearFailure:
                        snapshot.failureCode == .publicationUnavailable
                )
                emit()
            }
            return true
        } catch {
            return false
        }
    }

    private func schedulePublication(
        generation: UInt64,
        delayNanoseconds: UInt64 = 500_000_000
    ) {
        guard generation == activeGeneration,
              !terminalPublicationClaimed
        else {
            return
        }
        publicationDirty = true
        guard publicationTask == nil else {
            return
        }

        let workerID = UUID()
        publicationWorkerID = workerID
        publicationTask = Task { [weak self] in
            if delayNanoseconds > 0 {
                try? await Task<Never, Never>.sleep(
                    nanoseconds: delayNanoseconds
                )
            }
            await self?.runPublicationWorker(
                generation: generation,
                workerID: workerID
            )
        }
    }

    private func runPublicationWorker(
        generation: UInt64,
        workerID: UUID
    ) async {
        while !Task.isCancelled,
              publicationWorkerID == workerID,
              generation == activeGeneration,
              !terminalPublicationClaimed
        {
            publicationDirty = false
            _ = await publishCurrentSnapshot(
                endedAt: nil,
                generation: generation,
                allowTerminal: false
            )
            guard publicationDirty else {
                break
            }
            try? await Task.sleep(for: .milliseconds(500))
        }
        guard publicationWorkerID == workerID else {
            return
        }
        publicationTask = nil
        publicationWorkerID = nil
        publicationDirty = false
    }

    @discardableResult
    private func publishCurrentSnapshot(
        endedAt: Date?,
        generation: UInt64,
        allowTerminal: Bool
    ) async -> Bool {
        guard generation == activeGeneration,
              let session,
              let request = activeRequest,
              let createdAt = activeCreatedAt,
              let sessionID = snapshot.sessionID
        else {
            return false
        }
        let publicationSequence = reservePublicationSequence()

        let effectiveEnd = endedAt
        let duration = Int64(
            max(0, (effectiveEnd ?? Date()).timeIntervalSince(createdAt))
        )

        do {
            let renderOptions = CoreMarkdownOptions(
                title: request.title,
                createdAt: createdAt,
                endedAt: effectiveEnd,
                durationSeconds: duration,
                microphoneCaptured: snapshot.microphone == .active
                    || snapshot.microphone == .ready,
                systemAudioCaptured: snapshot.systemAudio == .active
                    || snapshot.systemAudio == .ready
            )
            let rendered = try await Task.detached(
                priority: .userInitiated
            ) {
                try session.renderMarkdown(options: renderOptions)
            }.value
            guard generation == activeGeneration,
                  !Task.isCancelled,
                  allowTerminal || !terminalPublicationClaimed
            else {
                return false
            }
            let previous = await vaultWriter.latestReceipt(for: sessionID)
            guard generation == activeGeneration,
                  !Task.isCancelled,
                  allowTerminal || !terminalPublicationClaimed
            else {
                return false
            }
            let receipt = try await vaultWriter.publish(
                MarkdownPublicationRequest(
                    sessionID: sessionID,
                    preferredFilenameStem: request.preferredFilenameStem,
                    markdown: rendered.data,
                    highestSegmentRevision:
                        UInt64(rendered.highestSegmentRevision),
                    publicationSequence: publicationSequence,
                    expectedPrevious: previous?.fingerprint
                )
            )
            guard generation == activeGeneration,
                  !Task.isCancelled,
                  allowTerminal || !terminalPublicationClaimed
            else {
                return false
            }
            try await Task.detached(priority: .userInitiated) {
                try session.acknowledgePublication(
                    receipt: receipt,
                    journalCheckpoint: rendered.journalCheckpoint
                )
            }.value
            guard generation == activeGeneration,
                  !Task.isCancelled,
                  allowTerminal || !terminalPublicationClaimed
            else {
                return false
            }
            let metrics = try? await Task.detached {
                try session.metrics()
            }.value
            guard generation == activeGeneration,
                  !Task.isCancelled,
                  allowTerminal || !terminalPublicationClaimed
            else {
                return false
            }
            snapshot = replacingSnapshot(
                metrics: metrics,
                destination: receipt.destination,
                filename: receipt.filename,
                url: receipt.url,
                clearFailure: true
            )
            emit()
            return true
        } catch {
            if generation == activeGeneration, !Task.isCancelled {
                updateFailureOnly(.publicationUnavailable)
            }
            return false
        }
    }

    private func publishTerminalSnapshotBounded(
        endedAt: Date,
        generation: UInt64
    ) async {
        let completion = PublicationAttemptCompletion()
        let task = Task { [weak self] in
            let success = await self?.publishCurrentSnapshot(
                endedAt: endedAt,
                generation: generation,
                allowTerminal: true
            ) ?? false
            completion.complete(success)
        }

        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(3))
        while clock.now < deadline {
            if completion.result != nil {
                return
            }
            try? await Task.sleep(for: .milliseconds(20))
        }
        task.cancel()
        if generation == activeGeneration {
            updateFailureOnly(.publicationUnavailable)
        }
    }

    private func cancelPublicationWorker() {
        publicationTask?.cancel()
        publicationTask = nil
        publicationWorkerID = nil
        publicationDirty = false
    }

    private func reservePublicationSequence() -> UInt64 {
        nextPublicationSequence &+= 1
        if nextPublicationSequence == 0 {
            nextPublicationSequence = 1
        }
        return nextPublicationSequence
    }

    private func acquireLifecycleSlot() async {
        if !lifecycleLocked {
            lifecycleLocked = true
            return
        }
        await withCheckedContinuation { continuation in
            lifecycleWaiters.append(continuation)
        }
    }

    private func releaseLifecycleSlot() {
        if lifecycleWaiters.isEmpty {
            lifecycleLocked = false
            return
        }
        let next = lifecycleWaiters.removeFirst()
        next.resume()
    }

    private func interruptActiveSession(code: SessionFailureCode) async {
        let generation = activeGeneration
        cancelSourceRecoveryTasks()
        cancelPublicationWorker()
        terminalPublicationClaimed = true
        frameRouter.detach()
        intentionalCaptureStop = true
        await stopCaptureAdapters()
        intentionalCaptureStop = false
        if let activeSession = session {
            try? closeOutstandingSourceGaps(on: activeSession)
            _ = try? await Task.detached(priority: .userInitiated) {
                try activeSession.finalize(reason: .processInterrupted)
                return try activeSession.currentState()
            }.value
            await drainFinalEventsThroughTerminalBoundary(
                from: activeSession,
                generation: generation
            )
        }
        update(state: .interrupted, failure: code)
        let endedAt = Date()
        rememberReviewContext(endedAt: endedAt)
        await publishTerminalSnapshotBounded(
            endedAt: endedAt,
            generation: generation
        )
        finishTerminalSession()
    }

    /// Removes actor ownership before beginning any potentially unbounded
    /// native teardown. AppKit termination may then complete on a fixed
    /// deadline even if an inference or SQLite backend fails to return.
    private func takeTerminationResources(
        includeSourceGaps: Bool
    ) -> TerminationResources? {
        let ownedSession = session
        let ownedModelLease = modelLease
        let gapEnd = monotonicNow()
        let gaps: [TerminationSourceGap]
        if includeSourceGaps {
            gaps = sourceUnavailableSince.map { kind, start in
                TerminationSourceGap(
                    sourceID: sourceID(for: kind),
                    kind: kind,
                    startTimeNanoseconds: start,
                    endTimeNanoseconds: max(start, gapEnd)
                )
            }
        } else {
            gaps = []
        }

        activeGeneration &+= 1
        cancelSourceRecoveryTasks()
        cancelPublicationWorker()
        eventPollTask?.cancel()
        eventPollTask = nil
        captureEventTask?.cancel()
        captureEventTask = nil
        captureEventMailbox?.finish()
        captureEventMailbox = nil
        frameRouter.detach()
        session = nil
        modelLease = nil
        activeRequest = nil
        activeCreatedAt = nil
        sourceUnavailableSince.removeAll()
        sourcesBeingRestarted.removeAll()

        guard let ownedSession else {
            ownedModelLease?.release()
            return nil
        }
        return TerminationResources(
            session: ownedSession,
            modelLease: ownedModelLease,
            sourceGaps: gaps
        )
    }

    private func finalizeForTerminationBounded(
        _ resources: TerminationResources,
        reason: CoreFinalizeReason
    ) async {
        let completion = PublicationAttemptCompletion()
        _ = Task.detached(priority: .userInitiated) {
            defer {
                resources.session.close()
                resources.modelLease?.release()
            }
            for gap in resources.sourceGaps {
                try? resources.session.sourceEvent(
                    sourceID: gap.sourceID,
                    kind: gap.kind,
                    event: .unavailable,
                    health: .temporarilyUnavailable,
                    startTimeNanoseconds: gap.startTimeNanoseconds,
                    endTimeNanoseconds: gap.endTimeNanoseconds,
                    reasonCode: "capture_gap_closed_for_termination"
                )
            }
            do {
                try resources.session.finalize(reason: reason)
                completion.complete(true)
            } catch {
                completion.complete(false)
            }
        }

        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(1))
        while clock.now < deadline {
            if completion.result != nil {
                return
            }
            try? await Task.sleep(for: .milliseconds(20))
        }
    }

    private func finishTerminalSession() {
        let closingSession = session
        activeGeneration &+= 1
        cancelSourceRecoveryTasks()
        cancelPublicationWorker()
        eventPollTask?.cancel()
        eventPollTask = nil
        captureEventTask?.cancel()
        captureEventTask = nil
        captureEventMailbox?.finish()
        captureEventMailbox = nil
        frameRouter.detach()
        session = nil
        if let closingSession {
            Task.detached(priority: .utility) {
                closingSession.close()
            }
        }
        modelLease?.release()
        modelLease = nil
        activeRequest = nil
        activeCreatedAt = nil
        sourceUnavailableSince.removeAll()
        sourcesBeingRestarted.removeAll()
    }

    private func cleanupFailedStart() {
        activeGeneration &+= 1
        cancelSourceRecoveryTasks()
        cancelPublicationWorker()
        eventPollTask?.cancel()
        eventPollTask = nil
        captureEventTask?.cancel()
        captureEventTask = nil
        captureEventMailbox?.finish()
        captureEventMailbox = nil
        frameRouter.detach()
        session?.close()
        session = nil
        modelLease?.release()
        modelLease = nil
        activeRequest = nil
        activeCreatedAt = nil
        sourceUnavailableSince.removeAll()
        sourcesBeingRestarted.removeAll()
    }

    private func consume(
        _ token: ConsentToken,
        expectedAction: ConsentAction
    ) throws {
        guard token.action == expectedAction else {
            throw SessionControllerError.invalidConsent(.invalidTransition)
        }
        guard !consumedConsentIDs.contains(token.id) else {
            throw SessionControllerError.invalidConsent(.consentAlreadyUsed)
        }
        guard token.issuedAt.duration(to: .now) <= .seconds(30) else {
            throw SessionControllerError.invalidConsent(.consentExpired)
        }
        consumedConsentIDs.insert(token.id)
    }

    private func checkTerminationRequested(
        generation: UInt64? = nil
    ) throws {
        guard !terminationRequested,
              generation.map({ $0 == activeGeneration }) ?? true
        else {
            throw CancellationError()
        }
    }

    private func failStart(_ code: SessionFailureCode) {
        if let context = lastReviewContext,
           snapshot.sessionID == context.sessionID
        {
            update(state: .failedToStart, failure: code)
            return
        }
        snapshot = SessionSnapshot(
            state: .failedToStart,
            sessionID: nil,
            startedAt: nil,
            microphone: .unknown,
            systemAudio: .unknown,
            metrics: nil,
            recentFinalSegments: [],
            speakers: [],
            lastPublicationDestination: nil,
            lastPublishedFilename: nil,
            lastPublishedURL: nil,
            failureCode: code
        )
        emit()
    }

    private func applyCorePhase(_ phase: CorePhase) {
        let state: SessionShellState
        switch phase {
        case .unknown:
            return
        case .preparing:
            state = .preparing
        case .recording:
            state = .recording
        case .paused:
            state = .paused
        case .finalizing:
            state = .finalizing
        case .recoveryRequired:
            state = .recoveryRequired
        case .complete:
            state = .complete
        case .incompleteSources:
            state = .incompleteSources
        case .interrupted:
            state = .interrupted
        case .failedToStart:
            state = .failedToStart
        }
        update(state: state, failure: snapshot.failureCode)
    }

    private func applySourceEvent(_ event: CoreSourceEvent) {
        // A discontinuity describes an audio boundary, not recovery. Late
        // worker/journal events must never hide a source that the capture
        // adapter still reports as unavailable.
        guard event.eventKind != .discontinuity else {
            return
        }
        if sourceUnavailableSince[event.sourceKind] != nil {
            switch event.eventKind {
            case .unavailable, .permanentlyLost:
                break
            case .unknown, .ready, .active, .recovered, .discontinuity:
                return
            }
        }

        let state: SourceDisplayState
        switch event.health {
        case .unknown:
            state = .unknown
        case .ready:
            state = .ready
        case .active:
            state = .active
        case .temporarilyUnavailable:
            state = .unavailable
        case .permanentlyLost:
            state = .lost
        }
        setSource(event.sourceKind, state: state)
    }

    private func setSource(
        _ kind: CaptureSourceKind,
        state: SourceDisplayState
    ) {
        snapshot = SessionSnapshot(
            state: snapshot.state,
            sessionID: snapshot.sessionID,
            startedAt: snapshot.startedAt,
            microphone: kind == .microphone ? state : snapshot.microphone,
            systemAudio: kind == .systemAudio ? state : snapshot.systemAudio,
            metrics: snapshot.metrics,
            recentFinalSegments: snapshot.recentFinalSegments,
            speakers: snapshot.speakers,
            lastPublicationDestination: snapshot.lastPublicationDestination,
            lastPublishedFilename: snapshot.lastPublishedFilename,
            lastPublishedURL: snapshot.lastPublishedURL,
            failureCode: snapshot.failureCode
        )
        emit()
    }

    private func update(
        state: SessionShellState,
        failure: SessionFailureCode?
    ) {
        snapshot = SessionSnapshot(
            state: state,
            sessionID: snapshot.sessionID,
            startedAt: snapshot.startedAt,
            microphone: snapshot.microphone,
            systemAudio: snapshot.systemAudio,
            metrics: snapshot.metrics,
            recentFinalSegments: snapshot.recentFinalSegments,
            speakers: snapshot.speakers,
            lastPublicationDestination: snapshot.lastPublicationDestination,
            lastPublishedFilename: snapshot.lastPublishedFilename,
            lastPublishedURL: snapshot.lastPublishedURL,
            failureCode: failure
        )
        emit()
    }

    private func updateFailureOnly(_ failure: SessionFailureCode) {
        snapshot = replacingSnapshot(failure: failure)
        emit()
    }

    private func replacingSnapshot(
        metrics: CorePipelineMetrics? = nil,
        recentFinalSegments: [CoreTranscriptSegment]? = nil,
        speakers: [SessionSpeakerSnapshot]? = nil,
        destination: PublicationDestination? = nil,
        filename: String? = nil,
        url: URL? = nil,
        failure: SessionFailureCode? = nil,
        clearFailure: Bool = false
    ) -> SessionSnapshot {
        SessionSnapshot(
            state: snapshot.state,
            sessionID: snapshot.sessionID,
            startedAt: snapshot.startedAt,
            microphone: snapshot.microphone,
            systemAudio: snapshot.systemAudio,
            metrics: metrics ?? snapshot.metrics,
            recentFinalSegments:
                recentFinalSegments ?? snapshot.recentFinalSegments,
            speakers: speakers ?? snapshot.speakers,
            lastPublicationDestination:
                destination ?? snapshot.lastPublicationDestination,
            lastPublishedFilename: filename ?? snapshot.lastPublishedFilename,
            lastPublishedURL: url ?? snapshot.lastPublishedURL,
            failureCode: clearFailure
                ? failure
                : (failure ?? snapshot.failureCode)
        )
    }

    private func recoverySnapshot(
        state: SessionShellState,
        sessionID: UUID?,
        metrics: CorePipelineMetrics? = nil,
        destination: PublicationDestination? = nil,
        filename: String? = nil,
        url: URL? = nil,
        failure: SessionFailureCode? = nil
    ) -> SessionSnapshot {
        SessionSnapshot(
            state: state,
            sessionID: sessionID,
            startedAt: nil,
            microphone: .unknown,
            systemAudio: .unknown,
            metrics: metrics,
            recentFinalSegments: [],
            speakers: [],
            lastPublicationDestination: destination,
            lastPublishedFilename: filename,
            lastPublishedURL: url,
            failureCode: failure
        )
    }

    private static func recoveryFilenameStem(for sessionID: UUID) -> String {
        "Recovered Call — recovery-\(sessionID.uuidString.lowercased())"
    }

    private func sourceID(for kind: CaptureSourceKind) -> UInt64 {
        kind == .microphone
            ? microphoneCapture.sourceID
            : systemAudioCapture.sourceID
    }

    private func monotonicNow() -> Int64 {
        let value = DispatchTime.now().uptimeNanoseconds
        return value > UInt64(Int64.max) ? Int64.max : Int64(value)
    }

    private func emit() {
        updateContinuation.yield(snapshot)
    }
}

private final class PublicationAttemptCompletion: @unchecked Sendable {
    private let lock = NSLock()
    private var storedResult: Bool?

    var result: Bool? {
        lock.lock()
        defer { lock.unlock() }
        return storedResult
    }

    func complete(_ result: Bool) {
        lock.lock()
        if storedResult == nil {
            storedResult = result
        }
        lock.unlock()
    }
}

/// Capture callbacks never block. Low-volume source-health transitions use an
/// unbounded stream so they cannot be displaced by backpressure notifications;
/// at most one rejection notification per source is pending at a time.
final class CaptureEventMailbox: @unchecked Sendable {
    let stream: AsyncStream<CaptureSourceEvent>

    private let continuation: AsyncStream<CaptureSourceEvent>.Continuation
    private let lock = NSLock()
    private var pendingRejections = Set<CaptureSourceKind>()
    private var latestHealth: [CaptureSourceKind: Bool] = [:]
    private var isFinished = false

    init() {
        let pair = AsyncStream<CaptureSourceEvent>.makeStream(
            bufferingPolicy: .unbounded
        )
        stream = pair.stream
        continuation = pair.continuation
    }

    func yield(_ event: CaptureSourceEvent) {
        lock.lock()
        guard !isFinished else {
            lock.unlock()
            return
        }
        switch event {
        case let .ready(kind):
            latestHealth[kind] = true
        case let .stopped(kind), let .failed(kind, _):
            latestHealth[kind] = false
        case let .frameRejected(kind):
            guard !pendingRejections.contains(kind) else {
                lock.unlock()
                return
            }
            pendingRejections.insert(kind)
        }
        let target = continuation
        lock.unlock()
        target.yield(event)
    }

    func beginHealthAttempt() {
        lock.lock()
        latestHealth.removeAll()
        lock.unlock()
    }

    func beginHealthAttempt(for kind: CaptureSourceKind) {
        lock.lock()
        latestHealth.removeValue(forKey: kind)
        lock.unlock()
    }

    func isReady(_ kind: CaptureSourceKind) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return latestHealth[kind] == true
    }

    var requiredSourcesAreReady: Bool {
        lock.lock()
        defer { lock.unlock() }
        return latestHealth[.microphone] == true
            && latestHealth[.systemAudio] == true
    }

    func didConsume(_ event: CaptureSourceEvent) {
        guard case let .frameRejected(kind) = event else {
            return
        }
        lock.lock()
        pendingRejections.remove(kind)
        lock.unlock()
    }

    func finish() {
        lock.lock()
        guard !isFinished else {
            lock.unlock()
            return
        }
        isFinished = true
        pendingRejections.removeAll()
        latestHealth.removeAll()
        let target = continuation
        lock.unlock()
        target.finish()
    }
}

private final class AudioFrameRouter: @unchecked Sendable {
    private let lock = NSLock()
    private var session: (any CoreSessionProtocol)?
    private var isActive = false

    /// Accepts and intentionally discards capture callbacks while both
    /// required adapters are starting. This keeps adapters healthy without
    /// offering audio to a PREPARING or PAUSED core session.
    func park(_ session: any CoreSessionProtocol) {
        lock.lock()
        self.session = session
        isActive = false
        lock.unlock()
    }

    func activate(_ session: any CoreSessionProtocol) {
        lock.lock()
        self.session = session
        isActive = true
        lock.unlock()
    }

    func detach() {
        lock.lock()
        session = nil
        isActive = false
        lock.unlock()
    }

    func push(_ frame: CapturedAudioFrame) -> AudioFrameDisposition {
        lock.lock()
        let current = session
        let active = isActive
        lock.unlock()
        guard let current else {
            return .closed
        }
        guard active else {
            return .accepted
        }
        return current.pushAudio(frame)
    }
}
