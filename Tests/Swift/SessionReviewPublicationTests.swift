import Foundation
import XCTest

@testable import LocalScribeApp

final class SessionReviewPublicationTests: XCTestCase {
    func testSlowReviewPublicationCompletesAndCoalescesRetries() async throws {
        let fixture = ReviewPublicationFixture()
        let sessionID = try await fixture.completeSession()
        let acknowledgementsBeforeReview = fixture.client.acknowledgementCount

        await fixture.writer.holdNextReviewPublication()
        _ = try await fixture.controller.enrollVoiceProfile(
            sessionID: sessionID,
            speakerID: 2,
            displayName: "Vladimir"
        )
        let publicationWasHeld = await waitForReviewCondition {
            await fixture.writer.isReviewPublicationHeld()
        }
        XCTAssertTrue(publicationWasHeld)

        // This exceeds the former three-second review deadline. The review
        // worker is asynchronous and must not convert normal latency into a
        // publication failure or abandon the eventual acknowledgement.
        try await Task.sleep(for: .milliseconds(3_200))
        var snapshot = await fixture.controller.currentSnapshot()
        XCTAssertNil(snapshot.failureCode)

        try await fixture.controller.retryLastPublication()
        try await fixture.controller.retryLastPublication()
        try await fixture.controller.retryLastPublication()
        await fixture.writer.releaseHeldReviewPublication()

        let retriesCompleted = await waitForReviewCondition {
            await fixture.writer.reviewPublicationCount() == 2
                && fixture.client.acknowledgementCount
                    == acknowledgementsBeforeReview + 2
        }
        XCTAssertTrue(retriesCompleted)
        snapshot = await fixture.controller.currentSnapshot()
        XCTAssertNil(snapshot.failureCode)
        let maximumConcurrent =
            await fixture.writer.maximumConcurrentReviewPublishes()
        let reviewPublicationCount =
            await fixture.writer.reviewPublicationCount()
        XCTAssertEqual(maximumConcurrent, 1)
        XCTAssertEqual(reviewPublicationCount, 2)
        XCTAssertEqual(fixture.client.acknowledgedRevision, 2)
    }

    func testReviewRetryDistinguishesCoreAndStorageFailures() async throws {
        let fixture = ReviewPublicationFixture()
        let sessionID = try await fixture.completeSession()

        fixture.client.failNextRecoverableOpen()
        _ = try await fixture.controller.enrollVoiceProfile(
            sessionID: sessionID,
            speakerID: 2,
            displayName: "Vladimir"
        )
        let coreFailureWasReported = await waitForReviewCondition {
            let snapshot = await fixture.controller.currentSnapshot()
            return snapshot.failureCode == .coreUnavailable
        }
        XCTAssertTrue(coreFailureWasReported)

        try await fixture.controller.retryLastPublication()
        let coreRetryCompleted = await waitForReviewCondition {
            let snapshot = await fixture.controller.currentSnapshot()
            return snapshot.failureCode == nil
                && fixture.client.acknowledgedRevision == 2
        }
        XCTAssertTrue(coreRetryCompleted)

        await fixture.writer.failNextReviewPublication()
        _ = try await fixture.controller.enrollVoiceProfile(
            sessionID: sessionID,
            speakerID: 3,
            displayName: "Vladimir"
        )
        let storageFailureWasReported = await waitForReviewCondition {
            let snapshot = await fixture.controller.currentSnapshot()
            return snapshot.failureCode == .publicationUnavailable
        }
        XCTAssertTrue(storageFailureWasReported)

        try await fixture.controller.retryLastPublication()
        let storageRetryCompleted = await waitForReviewCondition {
            let snapshot = await fixture.controller.currentSnapshot()
            return snapshot.failureCode == nil
                && fixture.client.acknowledgedRevision == 3
        }
        XCTAssertTrue(storageRetryCompleted)
    }

    func testRetryWithoutPublicationContextIsRejected() async {
        let fixture = ReviewPublicationFixture()

        do {
            try await fixture.controller.retryLastPublication()
            XCTFail("retry unexpectedly succeeded without a session")
        } catch SessionControllerError.invalidTransition {
            // Expected.
        } catch {
            XCTFail("unexpected retry error: \(error)")
        }
    }
}

private func waitForReviewCondition(
    timeout: Duration = .seconds(2),
    _ condition: @escaping @Sendable () async -> Bool
) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
        if await condition() {
            return true
        }
        try? await Task.sleep(for: .milliseconds(10))
    }
    return await condition()
}

private final class ReviewPublicationFixture: @unchecked Sendable {
    let client = ReviewCoreClient()
    let writer = ReviewPublishingSpy()
    let controller: SessionController

    init() {
        let selection = ReviewSelectionStore(
            url: FileManager.default.temporaryDirectory
                .appendingPathComponent("ggml-review-test.bin")
        )
        controller = SessionController(
            coreClient: client,
            permissions: ReviewPermissionProvider(),
            directoryStore: selection,
            modelStore: selection,
            vaultWriter: writer,
            microphoneCapture: ReviewCaptureSource(
                sourceID: 1,
                kind: .microphone
            ),
            systemAudioCapture: ReviewCaptureSource(
                sourceID: 2,
                kind: .systemAudio
            ),
            journalURL: FileManager.default.temporaryDirectory
                .appendingPathComponent("review-test-\(UUID().uuidString).sqlite3")
        )
    }

    func completeSession() async throws -> UUID {
        await controller.recoverInterruptedSessions()
        try await controller.proposeManualStart()
        let token = await MainActor.run {
            VisibleConsentIssuer().issue(for: .start)
        }
        await controller.start(
            after: token,
            request: SessionStartRequest(
                sourceApplication: "Review Test",
                title: "Review Test",
                preferredFilenameStem: "review-test",
                localSpeakerName: "Me",
                languageMode: .russianEnglish
            )
        )
        let recording = await controller.currentSnapshot()
        guard recording.state == .recording,
              let sessionID = recording.sessionID
        else {
            throw ReviewTestError.sessionDidNotStart
        }

        await controller.stop()
        let completed = await controller.currentSnapshot()
        guard completed.state == .complete,
              completed.sessionID == sessionID,
              completed.failureCode == nil
        else {
            throw ReviewTestError.sessionDidNotComplete
        }
        return sessionID
    }
}

private enum ReviewTestError: Error {
    case sessionDidNotStart
    case sessionDidNotComplete
}

private struct ReviewSelectionStore:
    VaultSelectionProviding,
    ModelSelectionProviding
{
    let url: URL

    func hasSelection() async -> Bool {
        true
    }

    func resolveLease() async throws -> any SecurityScopedResourceLeasing {
        ReviewResourceLease(url: url)
    }
}

private final class ReviewResourceLease:
    SecurityScopedResourceLeasing,
    @unchecked Sendable
{
    let url: URL

    init(url: URL) {
        self.url = url
    }

    func release() {}
}

private actor ReviewPermissionProvider: PermissionProviding {
    func currentSnapshot() -> CapturePermissionSnapshot {
        authorizedSnapshot
    }

    func requestRequiredPermissionsAfterConsent()
        -> CapturePermissionSnapshot
    {
        authorizedSnapshot
    }

    private var authorizedSnapshot: CapturePermissionSnapshot {
        CapturePermissionSnapshot(
            microphone: .authorized,
            screenAndSystemAudio: .authorized
        )
    }
}

private actor ReviewCaptureSource: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind

    init(sourceID: UInt64, kind: CaptureSourceKind) {
        self.sourceID = sourceID
        self.kind = kind
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        eventHandler(.ready(kind))
    }

    func stop() {}
}

private final class ReviewCoreClient: @unchecked Sendable, CoreClientProtocol {
    private let lock = NSLock()
    private var session: ReviewCoreSession?
    private var shouldFailNextOpen = false

    var acknowledgedRevision: UInt64? {
        lock.withLock { session?.acknowledgedRevision }
    }

    var acknowledgementCount: Int {
        lock.withLock { session?.acknowledgementCount ?? 0 }
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) throws -> any CoreSessionProtocol {
        lock.withLock {
            let created = ReviewCoreSession(sessionID: configuration.sessionID)
            session = created
            return created
        }
    }

    func recoverableSessionIDs() throws -> [String] {
        []
    }

    func openRecoverableSession(id: String) throws -> any CoreSessionProtocol {
        try lock.withLock {
            if shouldFailNextOpen {
                shouldFailNextOpen = false
                throw CoreBridgeError.unavailable
            }
            guard let session,
                  id == session.sessionID.uuidString.lowercased()
            else {
                throw CoreBridgeError.malformedCoreValue
            }
            return session
        }
    }

    func enrollVoiceProfile(
        sessionID: UUID,
        speakerID: UInt64,
        displayName: String
    ) throws -> CoreVoiceProfileEnrollment {
        try lock.withLock {
            guard let session, session.sessionID == sessionID else {
                throw CoreBridgeError.malformedCoreValue
            }
            let revision = session.recordEnrollment()
            return CoreVoiceProfileEnrollment(
                profileID: 7,
                speakerID: speakerID,
                sampleCount: UInt64(revision),
                relabeledSegments: 1,
                journalCheckpoint: UInt64(revision),
                highestSegmentRevision: revision
            )
        }
    }

    func failNextRecoverableOpen() {
        lock.withLock { shouldFailNextOpen = true }
    }
}

private final class ReviewCoreSession: @unchecked Sendable, CoreSessionProtocol {
    let sessionID: UUID

    private let lock = NSLock()
    private var phase: CorePhase = .preparing
    private var enrollmentRevision: UInt32 = 1
    private var storedAcknowledgedRevision: UInt64?
    private var storedAcknowledgementCount = 0

    init(sessionID: UUID) {
        self.sessionID = sessionID
    }

    var acknowledgedRevision: UInt64? {
        lock.withLock { storedAcknowledgedRevision }
    }

    var acknowledgementCount: Int {
        lock.withLock { storedAcknowledgementCount }
    }

    func recordEnrollment() -> UInt32 {
        lock.withLock {
            enrollmentRevision &+= 1
            return enrollmentRevision
        }
    }

    func markSourcesReady() {
        lock.withLock { phase = .recording }
    }

    func pushAudio(_ frame: CapturedAudioFrame) -> AudioFrameDisposition {
        .accepted
    }

    func pause() {
        lock.withLock { phase = .paused }
    }

    func resumeAfterConsent() {
        lock.withLock { phase = .recording }
    }

    func sourceEvent(
        sourceID: UInt64,
        kind: CaptureSourceKind,
        event: CoreSourceEventKind,
        health: CoreSourceHealth,
        startTimeNanoseconds: Int64,
        endTimeNanoseconds: Int64,
        reasonCode: String
    ) {}

    func finalize(reason: CoreFinalizeReason) {
        lock.withLock { phase = .complete }
    }

    func nextEvent(timeoutMilliseconds: UInt32) -> CoreEvent? {
        if timeoutMilliseconds > 0 {
            Thread.sleep(forTimeInterval: 0.005)
        }
        return nil
    }

    func currentState() -> CoreStateEvent {
        lock.withLock {
            CoreStateEvent(
                phase: phase,
                publishedStatus: phase == .complete ? .complete : .recording,
                finalizeReason: phase == .complete ? .userStop : .unknown
            )
        }
    }

    func metrics() -> CorePipelineMetrics {
        let revision = lock.withLock { enrollmentRevision }
        return CorePipelineMetrics(
            framesOffered: 0,
            framesAccepted: 0,
            framesRejected: 0,
            discontinuities: 0,
            finalSegmentsCommitted: 1,
            partialEventsCoalesced: 0,
            audioQueueDepth: 0,
            audioQueueHighWater: 0,
            journalCheckpoint: UInt64(revision),
            highestSegmentRevision: revision
        )
    }

    func renderMarkdown(
        options: CoreMarkdownOptions
    ) -> CoreRenderedMarkdown {
        let revision = lock.withLock { enrollmentRevision }
        let body = revision > 1 ? "review-\(revision)" : "original"
        return CoreRenderedMarkdown(
            data: Data(body.utf8),
            journalCheckpoint: UInt64(revision),
            highestSegmentRevision: revision
        )
    }

    func acknowledgePublication(
        receipt: MarkdownPublicationReceipt,
        journalCheckpoint: UInt64
    ) throws {
        guard receipt.highestSegmentRevision == journalCheckpoint else {
            throw CoreBridgeError.malformedCoreValue
        }
        lock.withLock {
            storedAcknowledgedRevision = receipt.highestSegmentRevision
            storedAcknowledgementCount += 1
        }
    }

    func close() {}
}

private actor ReviewPublishingSpy: MarkdownPublishing {
    private var receipts: [UUID: MarkdownPublicationReceipt] = [:]
    private var shouldHoldNextReview = false
    private var shouldFailNextReview = false
    private var heldContinuation: CheckedContinuation<Void, Never>?
    private var held = false
    private var reviewPublishes = 0
    private var activeReviewPublishes = 0
    private var maximumActiveReviewPublishes = 0

    func holdNextReviewPublication() {
        shouldHoldNextReview = true
    }

    func failNextReviewPublication() {
        shouldFailNextReview = true
    }

    func isReviewPublicationHeld() -> Bool {
        held
    }

    func releaseHeldReviewPublication() {
        heldContinuation?.resume()
        heldContinuation = nil
    }

    func reviewPublicationCount() -> Int {
        reviewPublishes
    }

    func maximumConcurrentReviewPublishes() -> Int {
        maximumActiveReviewPublishes
    }

    func publish(
        _ request: MarkdownPublicationRequest
    ) async throws -> MarkdownPublicationReceipt {
        let isReview = String(decoding: request.markdown, as: UTF8.self)
            .hasPrefix("review-")
        if isReview {
            reviewPublishes += 1
            activeReviewPublishes += 1
            maximumActiveReviewPublishes = max(
                maximumActiveReviewPublishes,
                activeReviewPublishes
            )
            defer { activeReviewPublishes -= 1 }

            if shouldFailNextReview {
                shouldFailNextReview = false
                throw VaultWriterError.publicationFailed
            }
            if shouldHoldNextReview {
                shouldHoldNextReview = false
                held = true
                await withCheckedContinuation { continuation in
                    heldContinuation = continuation
                }
                held = false
            }
        }

        let filename = "review-test.md"
        let receipt = MarkdownPublicationReceipt(
            destination: .vault,
            fingerprint: PublicationFingerprint(
                sha256Hex: "revision-\(request.highestSegmentRevision)",
                fileIdentity: nil,
                byteCount: request.markdown.count
            ),
            highestSegmentRevision: request.highestSegmentRevision,
            publishedAt: Date(),
            filename: filename,
            url: FileManager.default.temporaryDirectory
                .appendingPathComponent(filename)
        )
        receipts[request.sessionID] = receipt
        return receipt
    }

    func latestReceipt(
        for sessionID: UUID
    ) -> MarkdownPublicationReceipt? {
        receipts[sessionID]
    }
}
