import Foundation

private enum RecoveryCheckError: Error {
    case invariant(String)
}

func runRecoveryCheck() async throws {
    let sessionID = UUID(
        uuidString: "11111111-2222-3333-4444-555555555555"
    )!
    let temporary = FileManager.default.temporaryDirectory
        .appendingPathComponent(
            "LocalScribe-RecoveryCheck-\(UUID().uuidString)",
            isDirectory: true
        )
    try FileManager.default.createDirectory(
        at: temporary,
        withIntermediateDirectories: false
    )
    defer { try? FileManager.default.removeItem(at: temporary) }

    let defaultsName = "LocalScribe.RecoveryCheck.\(UUID().uuidString)"
    guard let defaults = UserDefaults(suiteName: defaultsName) else {
        throw RecoveryCheckError.invariant(
            "could not create isolated defaults"
        )
    }
    defaults.removePersistentDomain(forName: defaultsName)
    let directoryStore = SecurityScopedDirectoryStore(
        defaults: defaults,
        defaultsKey: "vault"
    )
    let modelStore = SecurityScopedModelStore(
        defaults: defaults,
        defaultsKey: "model"
    )
    let stagingRoot = temporary.appendingPathComponent(
        "Staging",
        isDirectory: true
    )
    let writer = VaultWriter(
        directoryStore: directoryStore,
        stagingDirectory: StagingDirectory(rootURL: stagingRoot)
    )
    let recoveredSession = RecoveryCheckSession(sessionID: sessionID)
    let core = RecoveryCheckCore(
        recoverableID: sessionID.uuidString.lowercased(),
        session: recoveredSession
    )
    let permissions = RecoveryCheckPermissions()
    let microphone = RecoveryCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = RecoveryCheckCapture(
        sourceID: 2,
        kind: .systemAudio
    )
    let controller = SessionController(
        coreClient: core,
        permissions: permissions,
        directoryStore: directoryStore,
        modelStore: modelStore,
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: temporary.appendingPathComponent("journal.sqlite3")
    )

    var updates = controller.updates.makeAsyncIterator()
    await controller.recoverInterruptedSessions()
    let snapshot = await controller.currentSnapshot()
    var observedStates: [SessionShellState] = []
    for _ in 0..<3 {
        guard let update = await updates.next() else {
            throw RecoveryCheckError.invariant(
                "recovery update stream ended unexpectedly"
            )
        }
        observedStates.append(update.state)
    }
    guard snapshot.state == .interrupted,
          snapshot.sessionID == sessionID,
          snapshot.lastPublicationDestination == .staging,
          snapshot.failureCode == nil,
          snapshot.metrics?.highestSegmentRevision == 7,
          let filename = snapshot.lastPublishedFilename,
          let url = snapshot.lastPublishedURL,
          observedStates == [.idle, .recoveryRequired, .interrupted]
    else {
        throw RecoveryCheckError.invariant(
            "startup recovery did not reach a published interrupted state"
        )
    }

    let expectedURL = stagingRoot
        .appendingPathComponent(
            sessionID.uuidString.lowercased(),
            isDirectory: true
        )
        .appendingPathComponent(filename)
    guard url == expectedURL,
          try Data(contentsOf: url) == RecoveryCheckSession.markdown
    else {
        throw RecoveryCheckError.invariant(
            "startup recovery did not preserve the exact staged bytes"
        )
    }

    guard await waitForRecoverySessionsToClose([recoveredSession]) else {
        throw RecoveryCheckError.invariant(
            "startup recovery did not release its native session"
        )
    }
    let observation = recoveredSession.observation
    guard observation.finalizeReason == .recovery,
          observation.usedJournalCreatedAt,
          observation.checkpoint == 17,
          observation.revision == 6,
          observation.closed,
          await permissions.requestCount() == 0,
          await microphone.startCount() == 0,
          await systemAudio.startCount() == 0,
          core.createCount == 0
    else {
        throw RecoveryCheckError.invariant(
            "startup recovery touched capture/consent or acknowledged stale state"
        )
    }

    let completeSessionID = UUID(
        uuidString: "99999999-8888-7777-6666-555555555555"
    )!
    let completeSession = RecoveryCheckSession(
        sessionID: completeSessionID,
        initialPhase: .complete
    )
    let completePermissions = RecoveryCheckPermissions()
    let completeMicrophone = RecoveryCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let completeSystemAudio = RecoveryCheckCapture(
        sourceID: 2,
        kind: .systemAudio
    )
    let completeController = SessionController(
        coreClient: RecoveryCheckCore(
            recoverableID: completeSessionID.uuidString.lowercased(),
            session: completeSession
        ),
        permissions: completePermissions,
        directoryStore: directoryStore,
        modelStore: modelStore,
        vaultWriter: writer,
        microphoneCapture: completeMicrophone,
        systemAudioCapture: completeSystemAudio,
        journalURL: temporary.appendingPathComponent("complete.sqlite3")
    )
    await completeController.recoverInterruptedSessions()
    let completeSnapshot = await completeController.currentSnapshot()
    guard await waitForRecoverySessionsToClose([completeSession]) else {
        throw RecoveryCheckError.invariant(
            "terminal publication recovery did not release its native session"
        )
    }
    let completeObservation = completeSession.observation
    guard completeSnapshot.state == .complete,
          completeSnapshot.lastPublicationDestination == .staging,
          completeSnapshot.failureCode == nil,
          completeObservation.finalizeReason == nil,
          completeObservation.checkpoint == 17,
          completeObservation.revision == 6,
          completeObservation.closed,
          await completePermissions.requestCount() == 0,
          await completeMicrophone.startCount() == 0,
          await completeSystemAudio.startCount() == 0
    else {
        throw RecoveryCheckError.invariant(
            "terminal publication recovery changed durable state or started capture"
        )
    }

    let blockedStaging = temporary.appendingPathComponent("BlockedStaging")
    try Data("not a directory".utf8).write(to: blockedStaging)
    let failedSessionID = UUID(
        uuidString: "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    )!
    let failedSession = RecoveryCheckSession(sessionID: failedSessionID)
    let failedCore = RecoveryCheckCore(
        recoverableID: failedSessionID.uuidString.lowercased(),
        session: failedSession
    )
    let failedPermissions = RecoveryCheckPermissions()
    let failedMicrophone = RecoveryCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let failedSystemAudio = RecoveryCheckCapture(
        sourceID: 2,
        kind: .systemAudio
    )
    let failedController = SessionController(
        coreClient: failedCore,
        permissions: failedPermissions,
        directoryStore: directoryStore,
        modelStore: modelStore,
        vaultWriter: VaultWriter(
            directoryStore: directoryStore,
            stagingDirectory: StagingDirectory(rootURL: blockedStaging)
        ),
        microphoneCapture: failedMicrophone,
        systemAudioCapture: failedSystemAudio,
        journalURL: temporary.appendingPathComponent("failed.sqlite3")
    )
    await failedController.recoverInterruptedSessions()
    let failedSnapshot = await failedController.currentSnapshot()
    guard await waitForRecoverySessionsToClose([failedSession]) else {
        throw RecoveryCheckError.invariant(
            "failed recovery did not release its native session"
        )
    }
    let failedObservation = failedSession.observation
    guard failedSnapshot.state == .recoveryRequired,
          failedSnapshot.failureCode == .publicationUnavailable,
          failedSnapshot.lastPublishedURL == nil,
          failedObservation.closed,
          failedObservation.checkpoint == nil,
          await failedPermissions.requestCount() == 0,
          await failedMicrophone.startCount() == 0,
          await failedSystemAudio.startCount() == 0
    else {
        throw RecoveryCheckError.invariant(
            "failed publication was not surfaced without capture"
        )
    }
}

func runMultiSessionRecoveryQueueCheck() async throws {
    let firstID = UUID(
        uuidString: "10101010-1111-2222-3333-444444444444"
    )!
    let secondID = UUID(
        uuidString: "20202020-1111-2222-3333-444444444444"
    )!
    let first = RecoveryCheckSession(sessionID: firstID)
    let second = RecoveryCheckSession(sessionID: secondID)
    let core = MultiRecoveryCheckCore(orderedSessions: [first, second])
    let writer = FailOnceRecoveryPublisher(sessionID: firstID)
    let permissions = RecoveryCheckPermissions()
    let microphone = RecoveryCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = RecoveryCheckCapture(
        sourceID: 2,
        kind: .systemAudio
    )
    let controller = SessionController(
        coreClient: core,
        permissions: permissions,
        directoryStore: RecoveryCheckUnavailableVault(),
        modelStore: RecoveryCheckUnavailableModel(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/multi-recovery-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    let failedSnapshot = await controller.currentSnapshot()
    guard await waitForRecoverySessionsToClose([first, second]) else {
        throw RecoveryCheckError.invariant(
            "queued recovery did not release every native session"
        )
    }
    let firstObservation = first.observation
    let secondObservation = second.observation
    guard failedSnapshot.state == .recoveryRequired,
          failedSnapshot.sessionID == firstID,
          failedSnapshot.failureCode == .publicationUnavailable,
          firstObservation.checkpoint == nil,
          firstObservation.closed,
          secondObservation.checkpoint == 17,
          secondObservation.closed,
          await writer.attemptCount(for: firstID) == 1,
          await writer.attemptCount(for: secondID) == 1
    else {
        throw RecoveryCheckError.invariant(
            "a later recovery success hid or skipped an earlier failure"
        )
    }

    do {
        try await controller.proposeManualStart()
        throw RecoveryCheckError.invariant(
            "capture became available while durable recovery work remained"
        )
    } catch SessionControllerError.invalidTransition {
        // Expected: an unacknowledged journal row blocks a new capture.
    }

    await controller.recoverInterruptedSessions()
    let retriedSnapshot = await controller.currentSnapshot()
    guard retriedSnapshot.state == .interrupted,
          retriedSnapshot.sessionID == firstID,
          retriedSnapshot.failureCode == nil,
          first.observation.checkpoint == 17,
          second.observation.checkpoint == 17,
          await writer.attemptCount(for: firstID) == 2,
          await writer.attemptCount(for: secondID) == 1,
          await permissions.requestCount() == 0,
          await microphone.startCount() == 0,
          await systemAudio.startCount() == 0
    else {
        throw RecoveryCheckError.invariant(
            "explicit recovery retry did not drain only pending journal work"
        )
    }

    let detectedProposalID = UUID(
        uuidString: "30303030-1111-2222-3333-444444444444"
    )!
    guard await controller.proposeDetectedCall(id: detectedProposalID),
          await controller.currentSnapshot().state == .awaitingConsent,
          await permissions.requestCount() == 0,
          await microphone.startCount() == 0,
          await systemAudio.startCount() == 0
    else {
        throw RecoveryCheckError.invariant(
            "a detected call did not expose consent without capture"
        )
    }

    await controller.dismissProposal(
        expectedDetectedProposalID: UUID()
    )
    guard await controller.currentSnapshot().state == .awaitingConsent else {
        throw RecoveryCheckError.invariant(
            "a stale call episode dismissed a newer consent proposal"
        )
    }
    await controller.dismissProposal(
        expectedDetectedProposalID: detectedProposalID
    )
    try await controller.proposeManualStart()
    guard await controller.currentSnapshot().state == .awaitingConsent else {
        throw RecoveryCheckError.invariant(
            "capture proposals did not unlock after recovery was acknowledged"
        )
    }
}

private func waitForRecoverySessionsToClose(
    _ sessions: [RecoveryCheckSession]
) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: .seconds(1))
    while clock.now < deadline {
        if sessions.allSatisfy({ $0.observation.closed }) {
            return true
        }
        try? await Task.sleep(for: .milliseconds(10))
    }
    return sessions.allSatisfy({ $0.observation.closed })
}

private final class RecoveryCheckCore: @unchecked Sendable, CoreClientProtocol {
    private let lock = NSLock()
    private let recoverableID: String
    private let session: RecoveryCheckSession
    private var creates = 0

    init(recoverableID: String, session: RecoveryCheckSession) {
        self.recoverableID = recoverableID
        self.session = session
    }

    var createCount: Int {
        lock.withLock { creates }
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) throws -> any CoreSessionProtocol {
        lock.withLock { creates += 1 }
        throw CoreBridgeError.unavailable
    }

    func recoverableSessionIDs() throws -> [String] {
        [recoverableID]
    }

    func openRecoverableSession(id: String) throws
        -> any CoreSessionProtocol
    {
        guard id == recoverableID else {
            throw CoreBridgeError.malformedCoreValue
        }
        return session
    }
}

private final class MultiRecoveryCheckCore:
    @unchecked Sendable,
    CoreClientProtocol
{
    private let orderedSessions: [RecoveryCheckSession]
    private let sessionsByID: [String: RecoveryCheckSession]

    init(orderedSessions: [RecoveryCheckSession]) {
        self.orderedSessions = orderedSessions
        sessionsByID = Dictionary(
            uniqueKeysWithValues: orderedSessions.map {
                ($0.sessionID.uuidString.lowercased(), $0)
            }
        )
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) throws -> any CoreSessionProtocol {
        throw CoreBridgeError.unavailable
    }

    func recoverableSessionIDs() -> [String] {
        orderedSessions.compactMap { session in
            session.observation.checkpoint == nil
                ? session.sessionID.uuidString.lowercased()
                : nil
        }
    }

    func openRecoverableSession(id: String) throws
        -> any CoreSessionProtocol
    {
        guard let session = sessionsByID[id] else {
            throw CoreBridgeError.malformedCoreValue
        }
        return session
    }
}

private final class RecoveryCheckSession:
    @unchecked Sendable,
    CoreSessionProtocol
{
    struct Observation {
        let finalizeReason: CoreFinalizeReason?
        let usedJournalCreatedAt: Bool
        let checkpoint: UInt64?
        let revision: UInt64?
        let closed: Bool
    }

    static let markdown = Data(
        """
        ---
        status: "interrupted"
        created: "2026-07-29T10:00:00+04:00"
        ---

        <!-- transcript:start -->
        recovered
        <!-- transcript:end -->
        """.utf8
    )

    let sessionID: UUID
    private let initialPhase: CorePhase
    private let lock = NSLock()
    private var reason: CoreFinalizeReason?
    private var usedJournalCreatedAt = false
    private var checkpoint: UInt64?
    private var revision: UInt64?
    private var closed = false
    private var deliveredInitialState = false

    init(
        sessionID: UUID,
        initialPhase: CorePhase = .recoveryRequired
    ) {
        self.sessionID = sessionID
        self.initialPhase = initialPhase
    }

    var observation: Observation {
        lock.withLock {
            Observation(
                finalizeReason: reason,
                usedJournalCreatedAt: usedJournalCreatedAt,
                checkpoint: checkpoint,
                revision: revision,
                closed: closed
            )
        }
    }

    func markSourcesReady() throws { throw CoreBridgeError.unavailable }
    func pushAudio(_ frame: CapturedAudioFrame) -> AudioFrameDisposition {
        .rejected
    }
    func pause() throws { throw CoreBridgeError.unavailable }
    func resumeAfterConsent() throws { throw CoreBridgeError.unavailable }
    func sourceEvent(
        sourceID: UInt64,
        kind: CaptureSourceKind,
        event: CoreSourceEventKind,
        health: CoreSourceHealth,
        startTimeNanoseconds: Int64,
        endTimeNanoseconds: Int64,
        reasonCode: String
    ) throws {
        throw CoreBridgeError.unavailable
    }

    func finalize(reason: CoreFinalizeReason) throws {
        lock.withLock { self.reason = reason }
    }

    func nextEvent(timeoutMilliseconds: UInt32) throws -> CoreEvent? {
        lock.withLock {
            guard !deliveredInitialState else {
                return nil
            }
            deliveredInitialState = true
            return .stateChanged(
                CoreStateEvent(
                    phase: initialPhase,
                    publishedStatus: initialPhase == .complete
                        ? .complete
                        : .recording,
                    finalizeReason: .processInterrupted
                )
            )
        }
    }

    func currentState() -> CoreStateEvent {
        CoreStateEvent(
            phase: initialPhase,
            publishedStatus: initialPhase == .complete
                ? .complete
                : (
                    initialPhase == .incompleteSources
                        ? .incompleteSources
                        : (
                            initialPhase == .interrupted
                                ? .interrupted
                                : .recording
                        )
                ),
            finalizeReason: initialPhase == .recoveryRequired
                ? .processInterrupted
                : .unknown
        )
    }

    func metrics() throws -> CorePipelineMetrics {
        CorePipelineMetrics(
            framesOffered: 1,
            framesAccepted: 1,
            framesRejected: 0,
            discontinuities: 0,
            finalSegmentsCommitted: 1,
            partialEventsCoalesced: 0,
            audioQueueDepth: 0,
            audioQueueHighWater: 1,
            journalCheckpoint: 19,
            highestSegmentRevision: 7
        )
    }

    func renderMarkdown(
        options: CoreMarkdownOptions
    ) throws -> CoreRenderedMarkdown {
        lock.withLock {
            usedJournalCreatedAt =
                options.createdAt == nil && options.endedAt == nil
        }
        let data = initialPhase == .complete
            ? Data(
                String(
                    decoding: Self.markdown,
                    as: UTF8.self
                )
                .replacingOccurrences(
                    of: "status: \"interrupted\"",
                    with: "status: \"complete\""
                )
                .utf8
            )
            : Self.markdown
        return CoreRenderedMarkdown(
            data: data,
            journalCheckpoint: 17,
            highestSegmentRevision: 6
        )
    }

    func acknowledgePublication(
        receipt: MarkdownPublicationReceipt,
        journalCheckpoint: UInt64
    ) throws {
        guard journalCheckpoint == 17,
              receipt.highestSegmentRevision == 6
        else {
            throw CoreBridgeError.malformedCoreValue
        }
        lock.withLock {
            checkpoint = journalCheckpoint
            revision = receipt.highestSegmentRevision
        }
    }

    func close() {
        lock.withLock { closed = true }
    }
}

private actor RecoveryCheckPermissions: PermissionProviding {
    private var requests = 0

    func currentSnapshot() -> CapturePermissionSnapshot {
        CapturePermissionSnapshot(
            microphone: .notDetermined,
            screenAndSystemAudio: .notDetermined
        )
    }

    func requestRequiredPermissionsAfterConsent() throws
        -> CapturePermissionSnapshot
    {
        requests += 1
        return CapturePermissionSnapshot(
            microphone: .authorized,
            screenAndSystemAudio: .authorized
        )
    }

    func requestCount() -> Int { requests }
}

private actor RecoveryCheckCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind
    private var starts = 0

    init(sourceID: UInt64, kind: CaptureSourceKind) {
        self.sourceID = sourceID
        self.kind = kind
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        starts += 1
    }

    func stop() {}
    func startCount() -> Int { starts }
}

private actor RecoveryCheckUnavailableVault: VaultSelectionProviding {
    func hasSelection() -> Bool { false }
}

private actor RecoveryCheckUnavailableModel: ModelSelectionProviding {
    func hasSelection() -> Bool { false }

    func resolveLease() throws -> any SecurityScopedResourceLeasing {
        throw SecurityScopedResourceError.noSelection
    }
}

private actor FailOnceRecoveryPublisher: MarkdownPublishing {
    private let failedSessionID: UUID
    private var didFail = false
    private var attempts: [UUID: Int] = [:]
    private var receipts: [UUID: MarkdownPublicationReceipt] = [:]

    init(sessionID: UUID) {
        failedSessionID = sessionID
    }

    func publish(
        _ request: MarkdownPublicationRequest
    ) throws -> MarkdownPublicationReceipt {
        attempts[request.sessionID, default: 0] += 1
        if request.sessionID == failedSessionID, !didFail {
            didFail = true
            throw VaultWriterError.publicationFailed
        }
        let filename = "recovered-\(request.sessionID.uuidString).md"
        let receipt = MarkdownPublicationReceipt(
            destination: .staging,
            fingerprint: PublicationFingerprint(
                sha256Hex: String(repeating: "a", count: 64),
                fileIdentity: nil,
                byteCount: request.markdown.count
            ),
            highestSegmentRevision: request.highestSegmentRevision,
            publishedAt: Date(),
            filename: filename,
            url: URL(fileURLWithPath: "/private/tmp/\(filename)")
        )
        receipts[request.sessionID] = receipt
        return receipt
    }

    func latestReceipt(
        for sessionID: UUID
    ) -> MarkdownPublicationReceipt? {
        receipts[sessionID]
    }

    func attemptCount(for sessionID: UUID) -> Int {
        attempts[sessionID, default: 0]
    }
}
