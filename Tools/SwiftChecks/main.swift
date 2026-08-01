import Foundation

private enum CheckFailure: Error {
    case invariant(String)
}

private final class StaleBookmarkCheckLease:
    SecurityScopedResourceLeasing,
    @unchecked Sendable
{
    let url = URL(fileURLWithPath: "/private/tmp/stale-bookmark-check")
    private(set) var isActive = true

    func release() {
        isActive = false
    }
}

private final class AtomicConflictCheckLease:
    SecurityScopedResourceLeasing,
    @unchecked Sendable
{
    let url: URL

    init(url: URL) {
        self.url = url
    }

    func release() {}
}

private struct AtomicConflictDirectoryResolver:
    VaultDirectoryResolving,
    Sendable
{
    let url: URL

    func resolveLease() async throws -> any SecurityScopedResourceLeasing {
        AtomicConflictCheckLease(url: url)
    }
}

@main
struct SwiftChecks {
    static func main() async throws {
        try checkFilenameBoundary()
        try checkLocalModelProfile()
        try checkStaleBookmarkRefreshOrdering()
        try checkManagedMerge()
        try checkAtomicPublication()
        try await checkAtomicExternalEditRace()
        try await checkStaging()
        try checkCaptureTimeline()
        try await checkCaptureEventMailbox()
        try checkScreenCaptureBindingGate()
        try await runRecoveryCheck()
        try await runMultiSessionRecoveryQueueCheck()
        try await runLifecycleResponsivenessCheck()
        try await runVoiceProfileCheck()
        try runLatestRequestGenerationCheck()
        try await runPreparingTerminationCheck()
        try await runBlockingModelPreparationCheck()
        try await runReversePublicationOrderCheck()
        try await runHangingCaptureTerminationCheck()
        try await runDirectRecordingTerminationBoundedCheck()
        try await runAssignedPreparingTerminationBoundedCheck()
        try await runConcurrentStopTerminationCheck()
        try await runBlockingNativeFinalizationCheck()
        try await runBlockingRenderTerminationCheck()
        try await runLatePublicationResultIsolationCheck()
        try await runSourceRecoveryRetryCheck()
        try await runCancelledRecoveryDoesNotRearmCheck()
        try await runDelayedRecoveryEventsDoNotGhostCheck()
        try await runCaptureBarrierCheck()
        try checkTerminationRequestCoalescing()
        try await runCallDetectionCheck()
        print("swift checks: 31 passed")
    }

    private static func require(
        _ condition: @autoclosure () -> Bool,
        _ message: String
    ) throws {
        if !condition() {
            throw CheckFailure.invariant(message)
        }
    }

    private static func checkFilenameBoundary() throws {
        let sessionID = UUID(
            uuidString: "11111111-2222-3333-4444-555555555555"
        )!
        let filename = SafeFilename.markdownFilename(
            stem: "../../private\\secret:\u{0}\nCall",
            sessionID: sessionID,
            forceStableSuffix: true
        )
        try require(
            SafeFilename.isSingleMarkdownComponent(filename),
            "sanitized filename is not a single Markdown component"
        )
        try require(
            !filename.contains("/") && !filename.contains("\\")
                && !filename.contains(":"),
            "sanitized filename retained a separator"
        )
    }

    private static func checkLocalModelProfile() throws {
        try require(
            LocalASRModelProfile.accepts(
                filename: "ggml-base.bin",
                byteCount: 148_000_000
            )
                && LocalASRModelProfile.accepts(
                    filename: "ggml-small.bin",
                    byteCount: 488_000_000
                )
                && LocalASRModelProfile.accepts(
                    filename: "ggml-medium.bin",
                    byteCount: 1_500_000_000
                )
                && LocalASRModelProfile.accepts(
                    filename: "ggml-large-v3.bin",
                    byteCount: Int.max
                )
                && LocalASRModelProfile.accepts(
                    filename: "GGML-LARGE-V3-TURBO-Q5_0.BIN",
                    byteCount: 574_000_000
                ),
            "a valid local ggml model was rejected"
        )
        try require(
            !LocalASRModelProfile.accepts(
                filename: "large-v3.bin",
                byteCount: 3_000_000_000
                )
                && !LocalASRModelProfile.accepts(
                    filename: "ggml-large-v3.gguf",
                    byteCount: 3_000_000_000
                )
                && !LocalASRModelProfile.accepts(
                    filename: "ggml-.bin",
                    byteCount: 1
                )
                && !LocalASRModelProfile.accepts(
                    filename: "ggml-base.bin",
                    byteCount: 0
                )
                && !LocalASRModelProfile.accepts(
                    filename: "ggml-base.bin",
                    byteCount: -1
                ),
            "an invalid local model filename or size was accepted"
        )
    }

    private static func checkStaleBookmarkRefreshOrdering() throws {
        let lease = StaleBookmarkCheckLease()
        var operations: [String] = []
        let returned = try SecurityScopedBookmarkRefresh.finish(
            lease: lease,
            isStale: true,
            validate: {
                try require(
                    lease.isActive,
                    "stale bookmark validation ran outside its security scope"
                )
                operations.append("validate")
            },
            refresh: {
                try require(
                    lease.isActive,
                    "stale bookmark refresh ran outside its security scope"
                )
                operations.append("refresh")
            }
        )
        try require(
            operations == ["validate", "refresh"] && returned.url == lease.url,
            "stale bookmark refresh order was not deterministic"
        )
        returned.release()
        try require(
            !lease.isActive,
            "stale bookmark lease did not release its security scope"
        )
    }

    private static func checkManagedMerge() throws {
        let existing = Data(
            """
            ---
            # prefix user comment
            type: "call-transcript"
            schema_version: 1
            status: "recording"
            # inter-key user comment
            session_id: "session-1"
            custom_user_key: "keep me"
            # suffix user comment
            ---

            User prose before.

            <!-- transcript:start -->

            Old transcript.

            <!-- transcript:end -->

            <!-- capture-events:start -->

            ## Capture events

            - Old capture event.

            <!-- capture-events:end -->

            User prose after.
            """.utf8
        )
        let snapshot = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 2
            status: "complete"
            session_id: "session-1"
            ended: "2026-07-29T12:00:00Z"
            ---

            # Call

            <!-- transcript:start -->

            **00:00:01 — Me :** Final transcript.

            <!-- transcript:end -->

            <!-- capture-events:start -->

            ## Capture events

            - New capture event.

            <!-- capture-events:end -->
            """.utf8
        )

        let data = try ManagedMarkdownMerger.merge(
            existing: existing,
            renderedSnapshot: snapshot
        )
        guard let merged = String(data: data, encoding: .utf8) else {
            throw CheckFailure.invariant("managed merge emitted invalid UTF-8")
        }
        try require(
            merged.contains("custom_user_key: \"keep me\""),
            "managed merge lost a user frontmatter key"
        )
        try require(
            merged.contains("# prefix user comment")
                && merged.contains("# inter-key user comment")
                && merged.contains("# suffix user comment"),
            "managed merge lost user YAML comments"
        )
        try require(
            merged.contains("User prose before.")
                && merged.contains("User prose after."),
            "managed merge lost user prose"
        )
        try require(
            merged.contains("schema_version: 2"),
            "managed merge did not publish the current Markdown schema"
        )
        try require(
            merged.contains("Final transcript.")
                && !merged.contains("Old transcript.")
                && merged.contains("New capture event.")
                && !merged.contains("Old capture event."),
            "managed merge did not replace all managed body blocks"
        )
        try require(
            merged.components(
                separatedBy: "<!-- transcript:start -->"
            ).count == 2
                && merged.components(
                separatedBy: "<!-- transcript:end -->"
            ).count == 2,
            "managed merge emitted ambiguous markers"
        )
        try require(
            merged.components(
                separatedBy: "<!-- capture-events:start -->"
            ).count == 2
                && merged.components(
                    separatedBy: "<!-- capture-events:end -->"
                ).count == 2,
            "managed merge emitted ambiguous capture-event markers"
        )

        let nextSnapshot = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 1
            status: "complete"
            session_id: "session-1"
            ended: "2026-07-29T12:05:00Z"
            ---

            # Call

            <!-- transcript:start -->

            Newer final transcript.

            <!-- transcript:end -->

            <!-- capture-events:start -->

            ## Capture events

            - Newest capture event.

            <!-- capture-events:end -->
            """.utf8
        )
        let twiceMerged = try ManagedMarkdownMerger.merge(
            existing: data,
            renderedSnapshot: nextSnapshot
        )
        let twiceMergedText = String(decoding: twiceMerged, as: UTF8.self)
        try require(
            twiceMergedText.contains("custom_user_key: \"keep me\"")
                && twiceMergedText.contains("# prefix user comment")
                && twiceMergedText.contains("# inter-key user comment")
                && twiceMergedText.contains("# suffix user comment")
                && twiceMergedText.contains("User prose before.")
                && twiceMergedText.contains("User prose after.")
                && twiceMergedText.contains("Newer final transcript.")
                && twiceMergedText.contains("Newest capture event.")
                && !twiceMergedText.contains("New capture event.")
                && !twiceMergedText.contains("Final transcript."),
            "a later managed merge erased user-owned content"
        )
    }

    private static func checkAtomicPublication() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "LocalScribe-Atomic-\(UUID().uuidString)",
                isDirectory: true
            )
        try FileManager.default.createDirectory(
            at: root,
            withIntermediateDirectories: false
        )
        defer { try? FileManager.default.removeItem(at: root) }

        let target = root.appendingPathComponent("Call.md")
        try AtomicFilePublisher.publish(Data("first".utf8), to: target)
        try AtomicFilePublisher.publish(Data("second".utf8), to: target)
        try AtomicFilePublisher.publish(
            Data("third".utf8),
            to: target,
            expectation: .bytes(Data("second".utf8))
        )
        let publishedData = try Data(contentsOf: target)
        try require(
            publishedData == Data("third".utf8),
            "atomic replacement did not publish the complete new document"
        )
        let entries = try FileManager.default.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: nil
        )
        try require(
            entries.map(\.lastPathComponent) == ["Call.md"],
            "atomic replacement left a temporary file"
        )

        let external = Data("external edit".utf8)
        try external.write(to: target)
        do {
            try AtomicFilePublisher.publish(
                Data("fourth".utf8),
                to: target,
                expectation: .bytes(Data("third".utf8))
            )
            throw CheckFailure.invariant(
                "atomic publisher overwrote a changed target"
            )
        } catch VaultWriterError.externalEditConflict {
            // Expected: preserve the external edit.
        }
        let afterConflict = try Data(contentsOf: target)
        try require(
            afterConflict == external,
            "atomic conflict handling changed external bytes"
        )
    }

    private static func checkAtomicExternalEditRace() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "LocalScribe-Atomic-Race-\(UUID().uuidString)",
                isDirectory: true
            )
        let vault = root.appendingPathComponent("Vault", isDirectory: true)
        let staging = root.appendingPathComponent("Staging", isDirectory: true)
        try FileManager.default.createDirectory(
            at: vault,
            withIntermediateDirectories: true
        )
        defer { try? FileManager.default.removeItem(at: root) }

        let sessionID = UUID(
            uuidString: "8a6bd83e-e658-4877-8f4f-47a52ec90ced"
        )!
        let initial = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 1
            status: "recording"
            session_id: "\(sessionID.uuidString.lowercased())"
            ---
            <!-- transcript:start -->
            Initial transcript.
            <!-- transcript:end -->
            """.utf8
        )
        let updated = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 1
            status: "complete"
            session_id: "\(sessionID.uuidString.lowercased())"
            ---
            <!-- transcript:start -->
            Final transcript.
            <!-- transcript:end -->
            """.utf8
        )
        let resolver = AtomicConflictDirectoryResolver(url: vault)
        let stagingDirectory = StagingDirectory(rootURL: staging)
        let initialWriter = VaultWriter(
            directoryStore: resolver,
            stagingDirectory: stagingDirectory
        )
        let firstReceipt = try await initialWriter.publish(
            MarkdownPublicationRequest(
                sessionID: sessionID,
                preferredFilenameStem: "Atomic Race",
                markdown: initial,
                highestSegmentRevision: 1,
                publicationSequence: 1,
                expectedPrevious: nil
            )
        )
        try require(
            firstReceipt.destination == .vault,
            "atomic race setup did not publish its initial vault note"
        )

        let firstExternal = Data(
            "# User's first external edit after the precheck.\n".utf8
        )
        let latestExternal = Data(
            "# User's newer external edit after the atomic swap.\n".utf8
        )
        let racingWriter = VaultWriter(
            directoryStore: resolver,
            stagingDirectory: stagingDirectory,
            beforeAtomicVaultRename: { target in
                try firstExternal.write(to: target, options: .atomic)
            },
            afterVaultSwapConflict: { target in
                try latestExternal.write(to: target, options: .atomic)
            }
        )
        let receipt = try await racingWriter.publish(
            MarkdownPublicationRequest(
                sessionID: sessionID,
                preferredFilenameStem: "Atomic Race",
                markdown: updated,
                highestSegmentRevision: 2,
                publicationSequence: 2,
                expectedPrevious: firstReceipt.fingerprint
            )
        )

        let preservedTarget = try Data(contentsOf: firstReceipt.url)
        try require(
            preservedTarget == latestExternal,
            "the newest post-swap external edit did not remain at the target"
        )
        try require(
            receipt.destination == .recoveryCopy
                || receipt.destination == .staging,
            "conflicting LocalScribe snapshot was reported as a vault overwrite"
        )
        let preservedSnapshot = try Data(contentsOf: receipt.url)
        try require(
            preservedSnapshot == updated,
            "conflicting LocalScribe snapshot was not preserved separately"
        )
        let conflictArtifacts = try FileManager.default
            .contentsOfDirectory(
                at: vault,
                includingPropertiesForKeys: nil
            )
            .filter {
                $0.lastPathComponent.hasPrefix(
                    "LocalScribe — external-edit-"
                )
            }
        let conflictArtifactData = try conflictArtifacts.map {
            try Data(contentsOf: $0)
        }
        try require(
            conflictArtifactData.contains(firstExternal),
            "the earlier displaced external edit was not surfaced visibly"
        )
        let vaultNames = try FileManager.default.contentsOfDirectory(
            atPath: vault.path
        )
        try require(
            !vaultNames.contains(where: { $0.hasPrefix(".localscribe-") }),
            "external conflict bytes remained hidden in a temporary file"
        )

        let absentSessionID = UUID(
            uuidString: "203a4384-f134-4e8e-ac3f-cbbbd2d3214a"
        )!
        let absentSnapshot = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 1
            status: "recording"
            session_id: "\(absentSessionID.uuidString.lowercased())"
            ---
            <!-- transcript:start -->
            New LocalScribe transcript.
            <!-- transcript:end -->
            """.utf8
        )
        let absentExternal = Data("# User created this note concurrently.\n".utf8)
        let absentWriter = VaultWriter(
            directoryStore: resolver,
            stagingDirectory: stagingDirectory,
            beforeAtomicVaultRename: { target in
                try absentExternal.write(to: target, options: .atomic)
            }
        )
        let absentReceipt = try await absentWriter.publish(
            MarkdownPublicationRequest(
                sessionID: absentSessionID,
                preferredFilenameStem: "Absent Race",
                markdown: absentSnapshot,
                highestSegmentRevision: 1,
                publicationSequence: 1,
                expectedPrevious: nil
            )
        )
        let absentTarget = vault.appendingPathComponent(
            SafeFilename.markdownFilename(
                stem: "Absent Race",
                sessionID: absentSessionID,
                forceStableSuffix: false
            )
        )
        let preservedAbsentTarget = try Data(contentsOf: absentTarget)
        let preservedAbsentSnapshot = try Data(contentsOf: absentReceipt.url)
        try require(
            preservedAbsentTarget == absentExternal,
            "atomic no-clobber rename overwrote a concurrently created note"
        )
        try require(
            absentReceipt.destination == .recoveryCopy
                || absentReceipt.destination == .staging,
            "absent-target race was reported as a vault overwrite"
        )
        try require(
            preservedAbsentSnapshot == absentSnapshot,
            "no-clobber conflict did not preserve the LocalScribe snapshot"
        )
    }

    private static func checkStaging() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "LocalScribe-Staging-\(UUID().uuidString)",
                isDirectory: true
            )
        defer { try? FileManager.default.removeItem(at: root) }

        let staging = StagingDirectory(rootURL: root)
        let sessionID = UUID(
            uuidString: "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
        )!
        let url = try await staging.publish(
            data: Data("safe".utf8),
            safeFilename: "Call.md",
            sessionID: sessionID
        )
        try require(
            url.deletingLastPathComponent().lastPathComponent
                == sessionID.uuidString.lowercased(),
            "staging output was not isolated by stable session ID"
        )
        let stagedData = try Data(contentsOf: url)
        try require(
            stagedData == Data("safe".utf8),
            "staging output bytes differ"
        )
    }

    private static func checkCaptureTimeline() throws {
        let timeline = CaptureTimeline()
        timeline.resetEpoch()
        let first = timeline.next(
            timestampNanoseconds: 1_000_000_000,
            frameCount: 480,
            sampleRateHz: 48_000,
            channelCount: 1
        )
        let second = timeline.next(
            timestampNanoseconds: 1_010_000_000,
            frameCount: 480,
            sampleRateHz: 48_000,
            channelCount: 1
        )
        let changed = timeline.next(
            timestampNanoseconds: 1_020_000_000,
            frameCount: 480,
            sampleRateHz: 48_000,
            channelCount: 2
        )
        try require(
            first.sequenceNumber == 0
                && first.flags.contains(.discontinuity),
            "capture epoch did not begin with a discontinuity"
        )
        try require(
            second.sequenceNumber == 1
                && !second.flags.contains(.discontinuity),
            "contiguous capture was marked discontinuous"
        )
        try require(
            changed.sequenceNumber == 2
                && changed.flags.contains(.discontinuity),
            "format change did not create a discontinuity"
        )
    }

    private static func checkCaptureEventMailbox() async throws {
        let mailbox = CaptureEventMailbox()
        var iterator = mailbox.stream.makeAsyncIterator()
        mailbox.beginHealthAttempt()
        for _ in 0..<1_000 {
            mailbox.yield(.frameRejected(.microphone))
        }
        mailbox.yield(.ready(.systemAudio))
        mailbox.yield(.failed(.microphone, code: "route_lost"))
        mailbox.yield(.stopped(.systemAudio))

        var observed: [CaptureSourceEvent] = []
        for _ in 0..<4 {
            guard let event = await iterator.next() else {
                throw CheckFailure.invariant(
                    "capture event mailbox ended unexpectedly"
                )
            }
            observed.append(event)
            mailbox.didConsume(event)
        }
        try require(
            observed == [
                .frameRejected(.microphone),
                .ready(.systemAudio),
                .failed(.microphone, code: "route_lost"),
                .stopped(.systemAudio),
            ],
            "capture health transition was lost behind rejection pressure"
        )
        try require(
            !mailbox.requiredSourcesAreReady,
            "startup health did not retain a consumed source failure"
        )
        mailbox.yield(.ready(.microphone))
        mailbox.yield(.ready(.systemAudio))
        try require(
            mailbox.requiredSourcesAreReady,
            "source readiness did not clear sticky startup failure"
        )
        for _ in 0..<2 {
            guard let event = await iterator.next() else {
                throw CheckFailure.invariant(
                    "capture readiness event was not delivered"
                )
            }
            mailbox.didConsume(event)
        }

        mailbox.yield(.frameRejected(.microphone))
        guard let next = await iterator.next() else {
            throw CheckFailure.invariant(
                "coalesced rejection did not reopen after consumption"
            )
        }
        mailbox.didConsume(next)
        try require(
            next == .frameRejected(.microphone),
            "capture rejection coalescing changed source identity"
        )
        mailbox.finish()
    }

    private static func checkScreenCaptureBindingGate() throws {
        let gate = ScreenCaptureBindingGate()
        let collector = CaptureEventCollector()
        let handler: CaptureEventHandler = { event in
            collector.append(event)
        }

        gate.activate(1)
        try require(
            gate.emit(
                .ready(.systemAudio),
                bindingGeneration: 1,
                eventHandler: handler
            ),
            "active screen-capture binding rejected its own event"
        )
        gate.activate(2)
        try require(
            !gate.emit(
                .failed(.systemAudio, code: "stale"),
                bindingGeneration: 1,
                eventHandler: handler
            ),
            "superseded screen-capture binding delivered a delayed error"
        )
        _ = gate.close(
            bindingGeneration: 2,
            events: [.stopped(.systemAudio)],
            eventHandler: handler
        )
        try require(
            collector.events == [
                .ready(.systemAudio),
                .stopped(.systemAudio),
            ],
            "screen-capture binding gate changed event ownership"
        )
    }

    private static func checkTerminationRequestCoalescing() throws {
        var coordinator = TerminationRequestCoordinator()

        let first = coordinator.request(requiresCleanup: true)
        try require(
            first.reply == .terminateLater && first.shouldStartCleanup,
            "first termination request did not start deferred cleanup"
        )

        for _ in 0..<100 {
            let repeated = coordinator.request(requiresCleanup: true)
            try require(
                repeated.reply == .terminateLater
                    && !repeated.shouldStartCleanup,
                "repeated termination bypassed or duplicated cleanup"
            )
        }

        try require(
            coordinator.cleanupDidFinish(),
            "completed termination cleanup was not accepted"
        )
        let whileReplying = coordinator.request(requiresCleanup: true)
        try require(
            whileReplying.reply == .terminateLater
                && !whileReplying.shouldStartCleanup,
            "termination was approved before the deferred reply finished"
        )

        coordinator.replyDidFinish()
        let approved = coordinator.request(requiresCleanup: true)
        try require(
            approved.reply == .terminateNow
                && !approved.shouldStartCleanup,
            "termination remained deferred after its reply finished"
        )
    }
}

private final class CaptureEventCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [CaptureSourceEvent] = []

    var events: [CaptureSourceEvent] {
        lock.lock()
        defer { lock.unlock() }
        return stored
    }

    func append(_ event: CaptureSourceEvent) {
        lock.lock()
        stored.append(event)
        lock.unlock()
    }
}
