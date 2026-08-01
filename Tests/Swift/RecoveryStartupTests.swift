import Foundation
import XCTest

@testable import LocalScribeApp

final class RecoveryStartupTests: XCTestCase {
    func testStartupRecoveryUsesJournalStateAndNeverStartsCapture() async throws {
        let fixture = try RecoveryFixture(stagingIsWritable: true)
        var iterator = fixture.controller.updates.makeAsyncIterator()

        await fixture.controller.recoverInterruptedSessions()

        var states: [SessionShellState] = []
        for _ in 0..<3 {
            let next = await iterator.next()
            states.append(try XCTUnwrap(next).state)
        }
        XCTAssertEqual(states, [.idle, .recoveryRequired, .interrupted])

        let snapshot = await fixture.controller.currentSnapshot()
        XCTAssertEqual(snapshot.state, .interrupted)
        XCTAssertEqual(snapshot.sessionID, fixture.sessionID)
        XCTAssertEqual(snapshot.lastPublicationDestination, .staging)
        XCTAssertNil(snapshot.failureCode)
        XCTAssertEqual(
            snapshot.metrics?.highestSegmentRevision,
            fixture.session.metricsValue.highestSegmentRevision
        )

        let permissionRequests = await fixture.permissions.requestCount()
        let microphoneStarts = await fixture.microphone.startCount()
        let systemAudioStarts = await fixture.systemAudio.startCount()
        XCTAssertEqual(permissionRequests, 0)
        XCTAssertEqual(microphoneStarts, 0)
        XCTAssertEqual(systemAudioStarts, 0)
        XCTAssertEqual(fixture.client.createCount, 0)

        let recoveryClosed = await waitForRecoveryClose(fixture.session)
        XCTAssertTrue(recoveryClosed)
        let observations = fixture.session.observations
        XCTAssertEqual(observations.finalizeReason, .recovery)
        XCTAssertTrue(observations.renderUsedOriginalCreatedAt)
        XCTAssertEqual(
            observations.acknowledgedCheckpoint,
            fixture.session.renderedValue.journalCheckpoint
        )
        XCTAssertEqual(
            observations.acknowledgedRevision,
            UInt64(fixture.session.renderedValue.highestSegmentRevision)
        )
        XCTAssertTrue(observations.closed)

        let filename = try XCTUnwrap(snapshot.lastPublishedFilename)
        XCTAssertEqual(
            filename,
            "Recovered Call — recovery-\(fixture.sessionID.uuidString.lowercased()).md"
        )
        let staged = fixture.stagingRoot
            .appendingPathComponent(
                fixture.sessionID.uuidString.lowercased(),
                isDirectory: true
            )
            .appendingPathComponent(filename)
        XCTAssertEqual(snapshot.lastPublishedURL, staged)
        XCTAssertEqual(
            try Data(contentsOf: staged),
            RecoverySession.renderedMarkdown
        )
    }

    func testStartupRecoverySurfacesPublicationFailureAndClosesHandle() async throws {
        let fixture = try RecoveryFixture(stagingIsWritable: false)

        await fixture.controller.recoverInterruptedSessions()

        let snapshot = await fixture.controller.currentSnapshot()
        XCTAssertEqual(snapshot.state, .recoveryRequired)
        XCTAssertEqual(snapshot.sessionID, fixture.sessionID)
        XCTAssertEqual(snapshot.failureCode, .publicationUnavailable)
        XCTAssertNil(snapshot.lastPublicationDestination)
        let recoveryClosed = await waitForRecoveryClose(fixture.session)
        XCTAssertTrue(recoveryClosed)
        XCTAssertTrue(fixture.session.observations.closed)
        XCTAssertNil(fixture.session.observations.acknowledgedCheckpoint)
        let permissionRequests = await fixture.permissions.requestCount()
        let microphoneStarts = await fixture.microphone.startCount()
        let systemAudioStarts = await fixture.systemAudio.startCount()
        XCTAssertEqual(permissionRequests, 0)
        XCTAssertEqual(microphoneStarts, 0)
        XCTAssertEqual(systemAudioStarts, 0)
    }

    func testTerminalSessionWithoutReceiptIsPublishedWithoutRefinalizing() async throws {
        let fixture = try RecoveryFixture(
            stagingIsWritable: true,
            initialPhase: .complete
        )

        await fixture.controller.recoverInterruptedSessions()

        let snapshot = await fixture.controller.currentSnapshot()
        XCTAssertEqual(snapshot.state, .complete)
        XCTAssertNil(snapshot.failureCode)
        XCTAssertEqual(snapshot.lastPublicationDestination, .staging)
        let recoveryClosed = await waitForRecoveryClose(fixture.session)
        XCTAssertTrue(recoveryClosed)
        XCTAssertNil(fixture.session.observations.finalizeReason)
        XCTAssertEqual(
            fixture.session.observations.acknowledgedCheckpoint,
            fixture.session.renderedValue.journalCheckpoint
        )
        XCTAssertTrue(fixture.session.observations.closed)
        let permissionRequests = await fixture.permissions.requestCount()
        let microphoneStarts = await fixture.microphone.startCount()
        let systemAudioStarts = await fixture.systemAudio.startCount()
        XCTAssertEqual(permissionRequests, 0)
        XCTAssertEqual(microphoneStarts, 0)
        XCTAssertEqual(systemAudioStarts, 0)
    }

    private func waitForRecoveryClose(
        _ session: RecoverySession
    ) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(1))
        while clock.now < deadline {
            if session.observations.closed {
                return true
            }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return session.observations.closed
    }
}

private final class RecoveryFixture {
    let sessionID = UUID(
        uuidString: "11111111-2222-3333-4444-555555555555"
    )!
    let stagingRoot: URL
    let session: RecoverySession
    let client: RecoveryCoreClient
    let permissions = RecoveryPermissionSpy()
    let microphone = RecoveryCaptureSpy(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = RecoveryCaptureSpy(
        sourceID: 2,
        kind: .systemAudio
    )
    let controller: SessionController

    init(
        stagingIsWritable: Bool,
        initialPhase: CorePhase = .recoveryRequired
    ) throws {
        let temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "LocalScribe-Recovery-\(UUID().uuidString)",
                isDirectory: true
            )
        try FileManager.default.createDirectory(
            at: temporary,
            withIntermediateDirectories: false
        )

        stagingRoot = temporary.appendingPathComponent(
            "Staging",
            isDirectory: true
        )
        if !stagingIsWritable {
            try Data("not a directory".utf8).write(to: stagingRoot)
        }

        let defaultsName = "LocalScribe.RecoveryTests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: defaultsName))
        defaults.removePersistentDomain(forName: defaultsName)
        let directoryStore = SecurityScopedDirectoryStore(
            defaults: defaults,
            defaultsKey: "vault"
        )
        let modelStore = SecurityScopedModelStore(
            defaults: defaults,
            defaultsKey: "model"
        )
        let writer = VaultWriter(
            directoryStore: directoryStore,
            stagingDirectory: StagingDirectory(rootURL: stagingRoot)
        )

        session = RecoverySession(
            sessionID: sessionID,
            initialPhase: initialPhase
        )
        client = RecoveryCoreClient(
            recoverableID: sessionID.uuidString.lowercased(),
            session: session
        )
        controller = SessionController(
            coreClient: client,
            permissions: permissions,
            directoryStore: directoryStore,
            modelStore: modelStore,
            vaultWriter: writer,
            microphoneCapture: microphone,
            systemAudioCapture: systemAudio,
            journalURL: temporary.appendingPathComponent("journal.sqlite3")
        )
    }

    deinit {
        try? FileManager.default.removeItem(
            at: stagingRoot.deletingLastPathComponent()
        )
    }
}

private final class RecoveryCoreClient: @unchecked Sendable, CoreClientProtocol {
    private let lock = NSLock()
    private let recoverableID: String
    private let session: RecoverySession
    private var _createCount = 0

    init(recoverableID: String, session: RecoverySession) {
        self.recoverableID = recoverableID
        self.session = session
    }

    var createCount: Int {
        lock.withLock { _createCount }
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) throws -> any CoreSessionProtocol {
        lock.withLock { _createCount += 1 }
        throw CoreBridgeError.unavailable
    }

    func recoverableSessionIDs() throws -> [String] {
        [recoverableID]
    }

    func openRecoverableSession(id: String) throws -> any CoreSessionProtocol {
        guard id == recoverableID else {
            throw CoreBridgeError.malformedCoreValue
        }
        return session
    }
}

private final class RecoverySession: @unchecked Sendable, CoreSessionProtocol {
    struct Observations {
        let finalizeReason: CoreFinalizeReason?
        let renderUsedOriginalCreatedAt: Bool
        let acknowledgedCheckpoint: UInt64?
        let acknowledgedRevision: UInt64?
        let closed: Bool
    }

    static let renderedMarkdown = Data(
        """
        ---
        status: "interrupted"
        session_id: "11111111-2222-3333-4444-555555555555"
        created: "2026-07-29T10:00:00+04:00"
        ---

        <!-- transcript:start -->
        recovered
        <!-- transcript:end -->
        """.utf8
    )

    let sessionID: UUID
    private let initialPhase: CorePhase
    let metricsValue = CorePipelineMetrics(
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

    private let lock = NSLock()
    private var finalizeReason: CoreFinalizeReason?
    private var renderUsedOriginalCreatedAt = false
    private var acknowledgedCheckpoint: UInt64?
    private var acknowledgedRevision: UInt64?
    private var isClosed = false
    private var deliveredInitialState = false

    lazy var renderedValue = CoreRenderedMarkdown(
        data: Self.renderedMarkdown,
        journalCheckpoint: 17,
        highestSegmentRevision: 6
    )

    init(
        sessionID: UUID,
        initialPhase: CorePhase = .recoveryRequired
    ) {
        self.sessionID = sessionID
        self.initialPhase = initialPhase
    }

    var observations: Observations {
        lock.withLock {
            Observations(
                finalizeReason: finalizeReason,
                renderUsedOriginalCreatedAt: renderUsedOriginalCreatedAt,
                acknowledgedCheckpoint: acknowledgedCheckpoint,
                acknowledgedRevision: acknowledgedRevision,
                closed: isClosed
            )
        }
    }

    func markSourcesReady() throws {
        throw CoreBridgeError.unavailable
    }

    func pushAudio(_ frame: CapturedAudioFrame) -> AudioFrameDisposition {
        .rejected
    }

    func pause() throws {
        throw CoreBridgeError.unavailable
    }

    func resumeAfterConsent() throws {
        throw CoreBridgeError.unavailable
    }

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
        lock.withLock { finalizeReason = reason }
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
        metricsValue
    }

    func renderMarkdown(
        options: CoreMarkdownOptions
    ) throws -> CoreRenderedMarkdown {
        lock.withLock {
            renderUsedOriginalCreatedAt =
                options.createdAt == nil && options.endedAt == nil
        }
        return renderedValue
    }

    func acknowledgePublication(
        receipt: MarkdownPublicationReceipt,
        journalCheckpoint: UInt64
    ) throws {
        guard journalCheckpoint == renderedValue.journalCheckpoint,
              receipt.highestSegmentRevision
                == UInt64(renderedValue.highestSegmentRevision)
        else {
            throw CoreBridgeError.malformedCoreValue
        }
        lock.withLock {
            acknowledgedCheckpoint = journalCheckpoint
            acknowledgedRevision = receipt.highestSegmentRevision
        }
    }

    func close() {
        lock.withLock { isClosed = true }
    }
}

private actor RecoveryPermissionSpy: PermissionProviding {
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

    func requestCount() -> Int {
        requests
    }
}

private actor RecoveryCaptureSpy: AudioCaptureSource {
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

    func startCount() -> Int {
        starts
    }
}
