import Foundation

private enum LifecycleCheckError: Error {
    case invariant(String)
}

func runCaptureBarrierCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let session = CaptureBarrierSession()
    let microphone = CaptureBarrierAdapter(
        sourceID: 1,
        kind: .microphone,
        suspendsStartup: false
    )
    let systemAudio = CaptureBarrierAdapter(
        sourceID: 2,
        kind: .systemAudio,
        suspendsStartup: true
    )
    let controller = SessionController(
        coreClient: CaptureBarrierCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/capture-barrier-check.sqlite3"
        )
    )
    let stateRecorder = CaptureBarrierStateRecorder()
    let stateObservation = Task {
        for await update in controller.updates {
            guard !Task.isCancelled else {
                return
            }
            await stateRecorder.append(update.state)
        }
    }
    defer { stateObservation.cancel() }

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let startToken = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    let startTask = Task {
        await controller.start(
            after: startToken,
            request: SessionStartRequest(
                sourceApplication: "Capture barrier check",
                title: "Capture barrier check",
                preferredFilenameStem: "Capture barrier check",
                localSpeakerName: "Me",
                languageMode: .russianEnglish
            )
        )
    }

    guard await eventually(timeout: .seconds(1), {
        await systemAudio.suspendedStartAttempt == 1
    }) else {
        await systemAudio.releaseStart()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "initial Start did not suspend in the second capture adapter"
        )
    }

    let initialMicrophoneDisposition = await microphone.startupDisposition(
        attempt: 1
    )
    let initialSystemDisposition = await systemAudio.startupDisposition(
        attempt: 1
    )
    guard initialMicrophoneDisposition == .accepted,
          initialSystemDisposition == .accepted,
          session.markSourcesReadyCount == 0,
          session.pushAudioCount == 0
    else {
        await systemAudio.releaseStart()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "PREPARING audio crossed the capture barrier"
        )
    }

    await systemAudio.releaseStart()
    await startTask.value
    guard await controller.currentSnapshot().state == .recording,
          session.markSourcesReadyCount == 1,
          session.pushAudioCount == 0
    else {
        throw LifecycleCheckError.invariant(
            "initial capture was not activated after both adapters became ready"
        )
    }
    guard await eventually(timeout: .seconds(1), {
        session.deliveredEventCount >= 2
    }) else {
        throw LifecycleCheckError.invariant(
            "capture-barrier core events were not delivered"
        )
    }
    let initialStates = await stateRecorder.values
    guard let recordingIndex = initialStates.firstIndex(of: .recording),
          !initialStates[initialStates.index(after: recordingIndex)...]
              .contains(.preparing)
    else {
        throw LifecycleCheckError.invariant(
            "a stale PREPARING event regressed the active recording indicator"
        )
    }

    let activeMicrophoneDisposition = await microphone.emitFrame()
    let activeSystemDisposition = await systemAudio.emitFrame()
    guard activeMicrophoneDisposition == .accepted,
          activeSystemDisposition == .accepted,
          session.pushAudioCount == 2,
          !session.pushedBeforeSourcesReady
    else {
        throw LifecycleCheckError.invariant(
            "RECORDING audio did not cross the activated capture barrier"
        )
    }

    let deliveredBeforeFailure = session.deliveredEventCount
    await systemAudio.emitFailure(code: "fixture_source_down")
    guard await eventually(timeout: .seconds(1), {
        await controller.currentSnapshot().systemAudio == .unavailable
    }) else {
        throw LifecycleCheckError.invariant(
            "capture failure did not expose the unavailable source"
        )
    }
    session.enqueueSourceEvent(
        CoreSourceEvent(
            sourceID: 2,
            sourceKind: .systemAudio,
            eventKind: .discontinuity,
            health: .active,
            startTimeNanoseconds: 1,
            endTimeNanoseconds: 1
        )
    )
    guard await eventually(timeout: .seconds(1), {
        session.deliveredEventCount > deliveredBeforeFailure
    }),
    await controller.currentSnapshot().systemAudio == .unavailable
    else {
        throw LifecycleCheckError.invariant(
            "late discontinuity hid a source that remained unavailable"
        )
    }

    await controller.pause()
    guard await controller.currentSnapshot().state == .paused else {
        throw LifecycleCheckError.invariant(
            "capture-barrier check did not enter PAUSED"
        )
    }

    let resumeToken = await MainActor.run {
        VisibleConsentIssuer().issue(for: .resume)
    }
    let resumeTask = Task {
        await controller.resume(after: resumeToken)
    }
    guard await eventually(timeout: .seconds(1), {
        await systemAudio.suspendedStartAttempt == 2
    }) else {
        await systemAudio.releaseStart()
        await resumeTask.value
        throw LifecycleCheckError.invariant(
            "Resume did not suspend in the second capture adapter"
        )
    }

    let resumeMicrophoneDisposition = await microphone.startupDisposition(
        attempt: 2
    )
    let resumeSystemDisposition = await systemAudio.startupDisposition(
        attempt: 2
    )
    guard resumeMicrophoneDisposition == .accepted,
          resumeSystemDisposition == .accepted,
          session.pushAudioCount == 2
    else {
        await systemAudio.releaseStart()
        await resumeTask.value
        throw LifecycleCheckError.invariant(
            "PAUSED audio crossed the capture barrier during Resume"
        )
    }

    await systemAudio.releaseStart()
    await resumeTask.value
    let resumeGapEvents = session.sourceEvents.filter {
        $0.reasonCode == "resume_capture_startup_gap"
    }
    guard await controller.currentSnapshot().state == .recording,
          Set(resumeGapEvents.map(\.kind)) == Set([
              CaptureSourceKind.microphone,
              .systemAudio,
          ]),
          resumeGapEvents.count == 2
    else {
        throw LifecycleCheckError.invariant(
            "Resume did not journal both source startup gaps"
        )
    }

    let resumedMicrophoneDisposition = await microphone.emitFrame()
    let resumedSystemDisposition = await systemAudio.emitFrame()
    guard resumedMicrophoneDisposition == .accepted,
          resumedSystemDisposition == .accepted,
          session.pushAudioCount == 4,
          !session.pushedBeforeSourcesReady
    else {
        throw LifecycleCheckError.invariant(
            "audio remained blocked after Resume activation"
        )
    }

    await controller.stop()
    let terminalSnapshot = await controller.currentSnapshot()
    guard terminalSnapshot.state == .complete,
          terminalSnapshot.lastPublishedFilename != nil,
          await writer.publishCount >= 2
    else {
        throw LifecycleCheckError.invariant(
            "Stop did not publish the final terminal Markdown snapshot"
        )
    }
}

func runBlockingNativeFinalizationCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let session = LifecycleCheckSession(blocksFinalize: true)
    let microphone = LifecycleCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = LifecycleCheckCapture(
        sourceID: 2,
        kind: .systemAudio
    )
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/blocking-finalize-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Blocking finalize check",
            title: "Blocking finalize check",
            preferredFilenameStem: "Blocking finalize check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )

    let stopTask = Task {
        await controller.stop()
    }
    guard await eventually(timeout: .seconds(1), {
        session.hasEnteredFinalize
    }) else {
        session.releaseFinalize()
        await stopTask.value
        throw LifecycleCheckError.invariant(
            "blocking native finalize was not entered"
        )
    }

    let completion = LifecycleCheckCompletion()
    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    let terminationReturned = await eventually(timeout: .seconds(1), {
        await completion.terminationReturned
    })
    guard terminationReturned else {
        session.releaseFinalize()
        await terminationTask.value
        await stopTask.value
        throw LifecycleCheckError.invariant(
            "native finalize kept Quit from entering the session actor"
        )
    }

    session.releaseFinalize()
    await stopTask.value
    await terminationTask.value
}

func runBlockingRenderTerminationCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let session = LifecycleCheckSession(blocksRenderAfterCount: 1)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: LifecycleCheckCapture(
            sourceID: 2,
            kind: .systemAudio
        ),
        journalURL: URL(
            fileURLWithPath: "/private/tmp/blocking-render-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Blocking render check",
            title: "Blocking render check",
            preferredFilenameStem: "Blocking render check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )
    guard await eventually(timeout: .seconds(1), {
        await writer.publishCount >= 1
    }) else {
        throw LifecycleCheckError.invariant(
            "blocking-render initial publication did not complete"
        )
    }

    let stopTask = Task {
        await controller.stop()
    }
    guard await eventually(timeout: .seconds(1), {
        session.hasEnteredBlockedRender
    }) else {
        session.releaseRender()
        await stopTask.value
        throw LifecycleCheckError.invariant(
            "terminal render did not enter the blocking fixture"
        )
    }

    let completion = LifecycleCheckCompletion()
    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    let terminationReturned = await eventually(timeout: .seconds(1), {
        await completion.terminationReturned
    })
    guard terminationReturned else {
        session.releaseRender()
        await terminationTask.value
        await stopTask.value
        throw LifecycleCheckError.invariant(
            "blocked native render monopolized the session actor"
        )
    }

    session.releaseRender()
    await stopTask.value
    await terminationTask.value
}

func runLatePublicationResultIsolationCheck() async throws {
    try await runLatePublicationResultIsolationScenario(
        blocking: .acknowledgement
    )
    try await runLatePublicationResultIsolationScenario(blocking: .metrics)
}

private enum LatePublicationBlockingStage: String {
    case acknowledgement
    case metrics
}

private func runLatePublicationResultIsolationScenario(
    blocking stage: LatePublicationBlockingStage
) async throws {
    let writer = SequencedMarkdownPublisher()
    let session = LifecycleCheckSession(
        blocksMetricsAfterCount: stage == .metrics ? 1 : nil,
        blocksAcknowledgementAfterCount:
            stage == .acknowledgement ? 1 : nil
    )
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: LifecycleCheckCapture(
            sourceID: 2,
            kind: .systemAudio
        ),
        journalURL: URL(
            fileURLWithPath:
                "/private/tmp/late-publication-\(stage.rawValue).sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Late publication check",
            title: "Late publication check",
            preferredFilenameStem: "Late publication check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )

    guard await eventually(timeout: .seconds(1), {
        let snapshot = await controller.currentSnapshot()
        return snapshot.lastPublishedFilename == "Late publication 1.md"
            && session.completedPublicationOperationCount(for: stage) >= 1
    }) else {
        session.releasePublicationOperation(for: stage)
        await controller.prepareForTermination()
        throw LifecycleCheckError.invariant(
            "\(stage.rawValue) fixture did not complete initial publication"
        )
    }

    let completion = LifecycleCheckCompletion()
    let stopTask = Task {
        await controller.stop()
        await completion.markStopReturned()
    }
    guard await eventually(timeout: .seconds(1), {
        session.hasEnteredBlockedPublicationOperation(for: stage)
    }) else {
        session.releasePublicationOperation(for: stage)
        await stopTask.value
        throw LifecycleCheckError.invariant(
            "terminal \(stage.rawValue) did not enter the blocking fixture"
        )
    }

    let stopReturned = await eventually(timeout: .seconds(4), {
        await completion.stopReturned
    })
    guard stopReturned else {
        session.releasePublicationOperation(for: stage)
        await stopTask.value
        throw LifecycleCheckError.invariant(
            "terminal \(stage.rawValue) kept Stop blocked past its deadline"
        )
    }

    let timedOutSnapshot = await controller.currentSnapshot()
    guard timedOutSnapshot.state == .complete,
          timedOutSnapshot.failureCode == .publicationUnavailable,
          timedOutSnapshot.lastPublishedFilename == "Late publication 1.md"
    else {
        session.releasePublicationOperation(for: stage)
        await stopTask.value
        throw LifecycleCheckError.invariant(
            "terminal \(stage.rawValue) timeout did not preserve the "
                + "last completed publication"
        )
    }

    session.releasePublicationOperation(for: stage)
    await stopTask.value
    guard await eventually(timeout: .seconds(1), {
        session.completedPublicationOperationCount(for: stage) >= 2
    }) else {
        throw LifecycleCheckError.invariant(
            "late terminal \(stage.rawValue) did not leave the fixture"
        )
    }

    let clock = ContinuousClock()
    let observationDeadline = clock.now.advanced(by: .milliseconds(300))
    while clock.now < observationDeadline {
        let snapshot = await controller.currentSnapshot()
        guard snapshot.failureCode == .publicationUnavailable,
              snapshot.lastPublishedFilename == "Late publication 1.md"
        else {
            throw LifecycleCheckError.invariant(
                "late terminal \(stage.rawValue) mutated the finished snapshot"
            )
        }
        try? await Task.sleep(for: .milliseconds(10))
    }
}

func runBlockingModelPreparationCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let session = LifecycleCheckSession()
    let core = BlockingCreateCore(session: session)
    let microphone = LifecycleCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = LifecycleCheckCapture(
        sourceID: 2,
        kind: .systemAudio
    )
    let controller = SessionController(
        coreClient: core,
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/blocking-model-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    let completion = LifecycleCheckCompletion()
    let startTask = Task {
        await controller.start(
            after: token,
            request: SessionStartRequest(
                sourceApplication: "Blocking model check",
                title: "Blocking model check",
                preferredFilenameStem: "Blocking model check",
                localSpeakerName: "Me",
                languageMode: .russianEnglish
            )
        )
        await completion.markPreparingStartReturned()
    }

    guard await eventually(timeout: .seconds(1), {
        core.hasEnteredCreate
    }) else {
        core.releaseCreate()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "native model preparation was not entered"
        )
    }

    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    let terminationReturned = await eventually(timeout: .seconds(1), {
        await completion.terminationReturned
    })
    guard terminationReturned else {
        core.releaseCreate()
        await terminationTask.value
        await startTask.value
        throw LifecycleCheckError.invariant(
            "native model preparation blocked Quit on the session actor"
        )
    }

    core.releaseCreate()
    await startTask.value
    await terminationTask.value
    let snapshot = await controller.currentSnapshot()
    let coreState = session.currentState()
    guard await completion.preparingStartReturned,
          session.hasClosed,
          coreState.phase == .failedToStart,
          coreState.finalizeReason == .cancelled,
          snapshot.state != .recording
    else {
        throw LifecycleCheckError.invariant(
            "cancelled model preparation left a recoverable PREPARING row"
        )
    }
}

func runSourceRecoveryRetryCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let session = LifecycleCheckSession()
    let microphone = LifecycleCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = FlappingRecoveryCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/source-recovery-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Source recovery check",
            title: "Source recovery check",
            preferredFilenameStem: "Source recovery check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )
    guard await controller.currentSnapshot().state == .recording else {
        throw LifecycleCheckError.invariant(
            "source recovery check did not enter recording"
        )
    }

    await systemAudio.emitFailure()
    guard await eventually(timeout: .seconds(4), {
        let starts = await systemAudio.startCount
        let snapshot = await controller.currentSnapshot()
        return starts >= 3 && snapshot.systemAudio == .active
    }) else {
        throw LifecycleCheckError.invariant(
            "ready-then-failed recovery attempt silently stopped retries"
        )
    }

    await controller.stop()
}

func runCancelledRecoveryDoesNotRearmCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let microphone = LifecycleCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = StableCountingCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: LifecycleCheckSession()),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/cancelled-recovery-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let startToken = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: startToken,
        request: SessionStartRequest(
            sourceApplication: "Cancelled recovery check",
            title: "Cancelled recovery check",
            preferredFilenameStem: "Cancelled recovery check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )

    await systemAudio.emitFailure()
    guard await eventually(timeout: .seconds(1), {
        await controller.currentSnapshot().systemAudio == .unavailable
    }) else {
        throw LifecycleCheckError.invariant(
            "cancelled-recovery fixture did not expose source loss"
        )
    }
    await controller.pause()
    let resumeToken = await MainActor.run {
        VisibleConsentIssuer().issue(for: .resume)
    }
    await controller.resume(after: resumeToken)
    try? await Task.sleep(for: .milliseconds(800))

    let starts = await systemAudio.startCount
    guard starts == 2,
          await controller.currentSnapshot().state == .recording
    else {
        throw LifecycleCheckError.invariant(
            "cancelled recovery worker restarted the resumed healthy stream"
        )
    }
    await controller.stop()
}

func runDelayedRecoveryEventsDoNotGhostCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let systemAudio = DelayedStopRecoveryCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: LifecycleCheckSession()),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/delayed-recovery-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Delayed recovery check",
            title: "Delayed recovery check",
            preferredFilenameStem: "Delayed recovery check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )

    await systemAudio.emitFailure()
    guard await eventually(timeout: .seconds(2), {
        let starts = await systemAudio.startCount
        let snapshot = await controller.currentSnapshot()
        return starts >= 2 && snapshot.systemAudio == .active
    }) else {
        throw LifecycleCheckError.invariant(
            "delayed stop→ready recovery did not become active"
        )
    }

    try? await Task.sleep(for: .milliseconds(800))
    await systemAudio.emitFailure()
    guard await eventually(timeout: .seconds(2), {
        let starts = await systemAudio.startCount
        let snapshot = await controller.currentSnapshot()
        return starts >= 3 && snapshot.systemAudio == .active
    }) else {
        throw LifecycleCheckError.invariant(
            "delayed recovery event left a ghost worker suppressing a real outage"
        )
    }

    await controller.stop()
}

func runLifecycleResponsivenessCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    let session = LifecycleCheckSession()
    let microphone = LifecycleCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = LifecycleCheckCapture(
        sourceID: 2,
        kind: .systemAudio
    )
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(fileURLWithPath: "/private/tmp/lifecycle-check.sqlite3")
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    let completion = LifecycleCheckCompletion()
    let consentStartedAt = ContinuousClock.now
    let startTask = Task {
        await controller.start(
            after: token,
            request: SessionStartRequest(
                sourceApplication: "Lifecycle check",
                title: "Lifecycle check",
                preferredFilenameStem: "Lifecycle check",
                localSpeakerName: "Me",
                languageMode: .russianEnglish
            )
        )
        await completion.markStartReturned()
    }

    guard await eventually(timeout: .seconds(1), {
        await completion.startReturned
    }) else {
        await writer.release()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "start remained blocked by initial Markdown publication"
        )
    }
    let startedSnapshot = await controller.currentSnapshot()
    guard startedSnapshot.state == .recording else {
        await writer.release()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "lifecycle check did not enter recording"
        )
    }

    guard await eventually(timeout: .seconds(2), {
        await writer.hasStartedPublishing
    }) else {
        await writer.release()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "scheduled initial publication did not begin"
        )
    }
    guard let firstPublishStartedAt = await writer.firstPublishStartedAt,
          consentStartedAt.duration(to: firstPublishStartedAt)
              <= .seconds(3)
    else {
        await writer.release()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "initial Markdown publication missed the three-second startup gate"
        )
    }

    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    let captureStopped = await eventually(timeout: .seconds(1), {
        let microphoneStops = await microphone.stopCount
        let systemAudioStops = await systemAudio.stopCount
        return microphoneStops > 0 && systemAudioStops > 0
    })
    guard captureStopped else {
        await writer.release()
        await terminationTask.value
        await startTask.value
        throw LifecycleCheckError.invariant(
            "blocked Markdown writer delayed capture shutdown"
        )
    }

    let terminationReturned = await eventually(timeout: .seconds(4), {
        await completion.terminationReturned
    })
    await writer.release()
    await terminationTask.value
    await startTask.value
    guard terminationReturned else {
        throw LifecycleCheckError.invariant(
            "termination waited indefinitely for Markdown publication"
        )
    }

    guard await eventually(timeout: .seconds(1), {
        session.hasClosed
    }) else {
        throw LifecycleCheckError.invariant(
            "bounded Quit did not close the detached native session"
        )
    }
    let terminationSnapshot = await controller.currentSnapshot()
    let coreState = session.currentState()
    guard terminationSnapshot.state != .recording,
          coreState.phase == .complete,
          coreState.finalizeReason == .userStop,
          await writer.publishCount == 1
    else {
        throw LifecycleCheckError.invariant(
            "bounded Quit published terminal Markdown or lost durable finalization"
        )
    }
}

func runPreparingTerminationCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let microphone = LifecycleCheckCapture(
        sourceID: 1,
        kind: .microphone
    )
    let systemAudio = BlockingStartCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: LifecycleCheckSession()),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: microphone,
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/preparing-lifecycle-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    let completion = LifecycleCheckCompletion()
    let startTask = Task {
        await controller.start(
            after: token,
            request: SessionStartRequest(
                sourceApplication: "Preparing check",
                title: "Preparing check",
                preferredFilenameStem: "Preparing check",
                localSpeakerName: "Me",
                languageMode: .russianEnglish
            )
        )
        await completion.markPreparingStartReturned()
    }

    guard await eventually(timeout: .seconds(1), {
        await systemAudio.hasEnteredStart
    }) else {
        await systemAudio.releaseStart()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "preparing check did not enter system-audio startup"
        )
    }

    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    let terminationReturned = await eventually(timeout: .seconds(1), {
        await completion.terminationReturned
    })
    let startReturned = await eventually(timeout: .seconds(1), {
        await completion.preparingStartReturned
    })

    await systemAudio.releaseStart()
    await terminationTask.value
    await startTask.value
    let snapshot = await controller.currentSnapshot()
    let microphoneStops = await microphone.stopCount
    let systemStops = await systemAudio.stopCount
    guard terminationReturned,
          startReturned,
          microphoneStops > 0,
          systemStops > 0,
          snapshot.state != .recording
    else {
        throw LifecycleCheckError.invariant(
            "termination did not preempt partially-started capture"
        )
    }
}

func runReversePublicationOrderCheck() async throws {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent(
            "LocalScribe-PublicationOrder-\(UUID().uuidString)",
            isDirectory: true
        )
    defer { try? FileManager.default.removeItem(at: root) }

    let resolver = ReverseDirectoryResolver()
    let writer = VaultWriter(
        directoryStore: resolver,
        stagingDirectory: StagingDirectory(rootURL: root)
    )
    let sessionID = UUID(
        uuidString: "87654321-4321-4321-4321-ba0987654321"
    )!
    let oldBytes = Data("status: recording".utf8)
    let finalBytes = Data("status: complete".utf8)

    let oldTask = Task {
        do {
            _ = try await writer.publish(
                MarkdownPublicationRequest(
                    sessionID: sessionID,
                    preferredFilenameStem: "Reverse order",
                    markdown: oldBytes,
                    highestSegmentRevision: 1,
                    publicationSequence: 1,
                    expectedPrevious: nil
                )
            )
            return false
        } catch {
            return error as? VaultWriterError == .publicationSuperseded
        }
    }

    guard await eventually(timeout: .seconds(1), {
        await resolver.firstRequestIsSuspended
    }) else {
        await resolver.releaseFirstRequest()
        _ = await oldTask.value
        throw LifecycleCheckError.invariant(
            "publication-order check did not suspend the old request"
        )
    }

    let finalReceipt = try await writer.publish(
        MarkdownPublicationRequest(
            sessionID: sessionID,
            preferredFilenameStem: "Reverse order",
            markdown: finalBytes,
            highestSegmentRevision: 1,
            publicationSequence: 2,
            expectedPrevious: nil
        )
    )
    await resolver.releaseFirstRequest()
    let oldWasRejected = await oldTask.value
    let publishedBytes = try Data(contentsOf: finalReceipt.url)

    guard oldWasRejected, publishedBytes == finalBytes else {
        throw LifecycleCheckError.invariant(
            "superseded live snapshot overwrote terminal Markdown"
        )
    }
}

func runHangingCaptureTerminationCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let systemAudio = HangingStopCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: LifecycleCheckSession()),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/hanging-stop-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Hanging stop check",
            title: "Hanging stop check",
            preferredFilenameStem: "Hanging stop check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )
    guard await controller.currentSnapshot().state == .recording else {
        throw LifecycleCheckError.invariant(
            "hanging-stop check did not enter recording"
        )
    }

    let completion = LifecycleCheckCompletion()
    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    let returned = await eventually(timeout: .seconds(2), {
        await completion.terminationReturned
    })
    await systemAudio.releaseStop()
    await terminationTask.value

    guard returned, await systemAudio.stopWasEntered else {
        throw LifecycleCheckError.invariant(
            "framework stop hang kept AppKit termination pending"
        )
    }
}

func runDirectRecordingTerminationBoundedCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let session = LifecycleCheckSession(blocksFinalize: true)
    let systemAudio = HangingStopCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath:
                "/private/tmp/direct-recording-termination-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Direct termination check",
            title: "Direct termination check",
            preferredFilenameStem: "Direct termination check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )
    guard await controller.currentSnapshot().state == .recording else {
        throw LifecycleCheckError.invariant(
            "direct-termination check did not enter recording"
        )
    }

    let completion = LifecycleCheckCompletion()
    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    guard await eventually(timeout: .seconds(2), {
        await systemAudio.stopWasEntered
    }) else {
        session.releaseFinalize()
        await systemAudio.releaseStop()
        await terminationTask.value
        throw LifecycleCheckError.invariant(
            "direct Quit did not attempt bounded capture shutdown"
        )
    }
    guard await eventually(timeout: .seconds(2), {
        session.hasEnteredFinalize
    }) else {
        session.releaseFinalize()
        await systemAudio.releaseStop()
        await terminationTask.value
        throw LifecycleCheckError.invariant(
            "direct Quit did not start native terminalization off-actor"
        )
    }

    let returnedWhileDependenciesBlocked = await eventually(
        timeout: .seconds(3),
        {
            await completion.terminationReturned
        }
    )
    let snapshotBeforeRelease = await controller.currentSnapshot()
    session.releaseFinalize()
    await systemAudio.releaseStop()
    await terminationTask.value

    guard returnedWhileDependenciesBlocked,
          snapshotBeforeRelease.state != .recording
    else {
        throw LifecycleCheckError.invariant(
            "direct Quit waited for capture stop or native finalization"
        )
    }
    guard await eventually(timeout: .seconds(1), {
        session.hasClosed
    }) else {
        throw LifecycleCheckError.invariant(
            "direct Quit did not close the detached native session"
        )
    }
}

func runAssignedPreparingTerminationBoundedCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let session = LifecycleCheckSession(blocksFinalize: true)
    let systemAudio = HangingStartAndStopCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: session),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath:
                "/private/tmp/assigned-preparing-termination-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    let completion = LifecycleCheckCompletion()
    let startTask = Task {
        await controller.start(
            after: token,
            request: SessionStartRequest(
                sourceApplication: "Assigned preparing check",
                title: "Assigned preparing check",
                preferredFilenameStem: "Assigned preparing check",
                localSpeakerName: "Me",
                languageMode: .russianEnglish
            )
        )
        await completion.markPreparingStartReturned()
    }

    guard await eventually(timeout: .seconds(1), {
        await systemAudio.startWasEntered
    }) else {
        session.releaseFinalize()
        await systemAudio.releaseStop()
        await systemAudio.releaseStart()
        await startTask.value
        throw LifecycleCheckError.invariant(
            "assigned PREPARING fixture did not suspend capture startup"
        )
    }

    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    guard await eventually(timeout: .seconds(2), {
        await systemAudio.stopWasEntered
    }) else {
        session.releaseFinalize()
        await systemAudio.releaseStop()
        await systemAudio.releaseStart()
        await terminationTask.value
        await startTask.value
        throw LifecycleCheckError.invariant(
            "assigned PREPARING Quit did not attempt capture shutdown"
        )
    }
    guard await eventually(timeout: .seconds(2), {
        session.hasEnteredFinalize
    }) else {
        session.releaseFinalize()
        await systemAudio.releaseStop()
        await systemAudio.releaseStart()
        await terminationTask.value
        await startTask.value
        throw LifecycleCheckError.invariant(
            "assigned PREPARING Quit did not terminalize the native row"
        )
    }

    let returnedWhileDependenciesBlocked = await eventually(
        timeout: .seconds(3),
        {
            await completion.terminationReturned
        }
    )
    let snapshotBeforeRelease = await controller.currentSnapshot()
    session.releaseFinalize()
    await systemAudio.releaseStop()
    await systemAudio.releaseStart()
    await terminationTask.value
    await startTask.value

    let coreState = session.currentState()
    guard returnedWhileDependenciesBlocked,
          snapshotBeforeRelease.state != .recording,
          coreState.phase == .failedToStart,
          coreState.finalizeReason == .cancelled
    else {
        throw LifecycleCheckError.invariant(
            "assigned PREPARING Quit blocked or left a recoverable row"
        )
    }
    guard await eventually(timeout: .seconds(1), {
        session.hasClosed
    }) else {
        throw LifecycleCheckError.invariant(
            "assigned PREPARING Quit did not close its native session"
        )
    }
}

func runConcurrentStopTerminationCheck() async throws {
    let writer = BlockingMarkdownPublisher()
    await writer.release()
    let systemAudio = HangingStopCapture(sourceID: 2)
    let controller = SessionController(
        coreClient: LifecycleCheckCore(session: LifecycleCheckSession()),
        permissions: LifecycleCheckPermissions(),
        directoryStore: LifecycleCheckVaultSelection(),
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: systemAudio,
        journalURL: URL(
            fileURLWithPath: "/private/tmp/concurrent-stop-check.sqlite3"
        )
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Concurrent stop check",
            title: "Concurrent stop check",
            preferredFilenameStem: "Concurrent stop check",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )

    let userStopTask = Task {
        await controller.stop()
    }
    guard await eventually(timeout: .seconds(1), {
        await systemAudio.stopWasEntered
    }) else {
        await systemAudio.releaseStop()
        await userStopTask.value
        throw LifecycleCheckError.invariant(
            "concurrent-stop check did not suspend user Stop"
        )
    }

    let completion = LifecycleCheckCompletion()
    let terminationTask = Task {
        await controller.prepareForTermination()
        await completion.markTerminationReturned()
    }
    let returnedBeforeFramework = await eventually(
        timeout: .seconds(1),
        {
            await completion.terminationReturned
        }
    )

    await systemAudio.releaseStop()
    await terminationTask.value
    await userStopTask.value
    guard returnedBeforeFramework else {
        throw LifecycleCheckError.invariant(
            "Quit waited on lifecycle slot held by an earlier Stop"
        )
    }
}

func runVoiceProfileCheck() async throws {
    let anonymousOne = UInt64(1) << 63 | 1
    let anonymousTwo = UInt64(1) << 63 | 2
    let anonymousThree = UInt64(1) << 63 | 3
    let anonymousFinalized = UInt64(1) << 63 | 4
    let writer = SequencedMarkdownPublisher()
    let session = LifecycleCheckSession(
        finalSegmentOnFinalize: CoreTranscriptSegment(
            stableID: UUID(),
            sourceID: 2,
            startTimeNanoseconds: 7_000_000_000,
            endTimeNanoseconds: 8_000_000_000,
            speakerID: anonymousFinalized,
            speakerLabel: "Speaker finalized",
            text: "Final segment produced while draining",
            language: "en",
            confidence: 0.9,
            revision: 1,
            isFinal: true,
            isUnintelligible: false
        )
    )
    let core = LifecycleCheckCore(session: session)
    let vaultSelection = LifecycleCheckVaultSelection()
    let controller = SessionController(
        coreClient: core,
        permissions: LifecycleCheckPermissions(),
        directoryStore: vaultSelection,
        modelStore: LifecycleCheckModelSelection(),
        vaultWriter: writer,
        microphoneCapture: LifecycleCheckCapture(
            sourceID: 1,
            kind: .microphone
        ),
        systemAudioCapture: LifecycleCheckCapture(
            sourceID: 2,
            kind: .systemAudio
        ),
        journalURL: URL(fileURLWithPath: "/private/tmp/voice-profile-check.sqlite3")
    )

    await controller.recoverInterruptedSessions()
    try await controller.proposeManualStart()
    let token = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    await controller.start(
        after: token,
        request: SessionStartRequest(
            sourceApplication: "Voice profile check",
            title: "Original voice profile title",
            preferredFilenameStem: "Original voice profile filename",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
    )

    let speakerSequence: [(UInt64, String)] = [
        (anonymousOne, "Speaker 1"),
        (anonymousOne, "Speaker 1"),
        (anonymousTwo, "Speaker 2"),
        (anonymousTwo, "Speaker 2"),
        (anonymousThree, "Speaker 3"),
        (anonymousThree, "Speaker 3"),
    ]
    for (index, speaker) in speakerSequence.enumerated() {
        session.enqueueFinalSegment(
            CoreTranscriptSegment(
                stableID: UUID(),
                sourceID: 2,
                startTimeNanoseconds: Int64(index) * 1_000_000_000,
                endTimeNanoseconds: Int64(index + 1) * 1_000_000_000,
                speakerID: speaker.0,
                speakerLabel: speaker.1,
                text: "Voice profile segment \(index)",
                language: "en",
                confidence: 0.9,
                revision: 1,
                isFinal: true,
                isUnintelligible: false
            )
        )
    }

    guard await eventually(timeout: .seconds(2), {
        let snapshot = await controller.currentSnapshot()
        return snapshot.speakers.count == 3
            && snapshot.speakers.first(where: {
                $0.speakerID == anonymousOne
            })?.segmentCount == 2
    }) else {
        throw LifecycleCheckError.invariant(
            "speaker review was derived only from the recent-segment window"
        )
    }

    let activeSnapshot = await controller.currentSnapshot()
    guard let sessionID = activeSnapshot.sessionID else {
        throw LifecycleCheckError.invariant(
            "speaker review did not retain its session identity"
        )
    }
    var rejectedMismatchedSession = false
    do {
        _ = try await controller.enrollVoiceProfile(
            sessionID: UUID(),
            speakerID: anonymousOne,
            displayName: "Wrong call"
        )
    } catch {
        rejectedMismatchedSession = true
    }
    guard rejectedMismatchedSession else {
        throw LifecycleCheckError.invariant(
            "voice enrollment accepted a speaker from another session"
        )
    }

    let livePublishCount = await writer.publishCount
    let firstEnrollment = try await controller.enrollVoiceProfile(
        sessionID: sessionID,
        speakerID: anonymousOne,
        displayName: "Alice"
    )
    let firstSnapshot = await controller.currentSnapshot()
    let profilesAfterFirstEnrollment = try await controller.voiceProfiles()
    guard firstEnrollment.speakerID & (UInt64(1) << 62) != 0,
          firstEnrollment.speakerID & (UInt64(1) << 63) == 0,
          firstSnapshot.speakers.contains(where: {
              $0.sessionID == sessionID
                  && $0.speakerID == firstEnrollment.speakerID
                  && $0.displayName == "Alice"
                  && !$0.isAnonymous
          }),
          profilesAfterFirstEnrollment.map(\.displayName) == ["Alice"]
    else {
        throw LifecycleCheckError.invariant(
            "live enrollment did not replace the anonymous speaker locally"
        )
    }
    guard await eventually(timeout: .seconds(2), {
        await writer.publishCount > livePublishCount
    }) else {
        throw LifecycleCheckError.invariant(
            "live enrollment did not schedule a replacement publication"
        )
    }

    await controller.stop()
    let stoppedSnapshot = await controller.currentSnapshot()
    guard session.terminalDrainPollCount > 0,
          stoppedSnapshot.speakers.count == 4,
          stoppedSnapshot.speakers.contains(where: {
              $0.speakerID == anonymousFinalized
                  && $0.latestExcerpt
                    == "Final segment produced while draining"
          })
    else {
        throw LifecycleCheckError.invariant(
            "Stop discarded a final segment produced while finalizing"
        )
    }

    let terminalPublishCount = await writer.publishCount
    await writer.blockNextPublication()
    let enrollmentCompletion = LifecycleCheckCompletion()
    let secondEnrollmentTask = Task {
        let enrollment = try await controller.enrollVoiceProfile(
            sessionID: sessionID,
            speakerID: anonymousTwo,
            displayName: "Bob"
        )
        await enrollmentCompletion.markVoiceEnrollmentReturned()
        return enrollment
    }
    guard await eventually(timeout: .seconds(1), {
        let enrollmentReturned =
            await enrollmentCompletion.voiceEnrollmentReturned
        let publicationBlocked = await writer.blockedPublicationEntered
        return enrollmentReturned && publicationBlocked
    }) else {
        await writer.releaseBlockedPublication()
        secondEnrollmentTask.cancel()
        _ = try? await secondEnrollmentTask.value
        throw LifecycleCheckError.invariant(
            "terminal enrollment waited for a blocked transcript publication"
        )
    }
    try await Task.sleep(for: .milliseconds(3_200))
    let slowReviewSnapshot = await controller.currentSnapshot()
    guard slowReviewSnapshot.failureCode == nil else {
        await writer.releaseBlockedPublication()
        secondEnrollmentTask.cancel()
        _ = try? await secondEnrollmentTask.value
        throw LifecycleCheckError.invariant(
            "slow background review publication was reported as a failure"
        )
    }
    try await controller.retryLastPublication()
    try await controller.retryLastPublication()
    try await controller.retryLastPublication()
    let secondEnrollment = try await secondEnrollmentTask.value
    let thirdEnrollment = try await controller.enrollVoiceProfile(
        sessionID: sessionID,
        speakerID: anonymousThree,
        displayName: "Carol"
    )

    await vaultSelection.setSelected(false)
    try await controller.proposeManualStart()
    let failedStartToken = await MainActor.run {
        VisibleConsentIssuer().issue(for: .start)
    }
    let failedStartCompletion = LifecycleCheckCompletion()
    let failedStartTask = Task {
        await controller.start(
            after: failedStartToken,
            request: SessionStartRequest(
                sourceApplication: "Failed next call",
                title: "Failed next call",
                preferredFilenameStem: "Failed next call",
                localSpeakerName: "Me",
                languageMode: .russianEnglish
            )
        )
        await failedStartCompletion.markStartReturned()
    }
    guard await eventually(timeout: .seconds(1), {
        await failedStartCompletion.startReturned
    }) else {
        await writer.releaseBlockedPublication()
        await failedStartTask.value
        throw LifecycleCheckError.invariant(
            "a blocked review publication held the lifecycle slot"
        )
    }
    await failedStartTask.value
    let failedStartSnapshot = await controller.currentSnapshot()
    guard failedStartSnapshot.state == .failedToStart,
          failedStartSnapshot.failureCode == .vaultNotSelected,
          failedStartSnapshot.sessionID == sessionID,
          failedStartSnapshot.speakers.contains(where: {
              $0.speakerID == thirdEnrollment.speakerID
                  && $0.displayName == "Carol"
          })
    else {
        await writer.releaseBlockedPublication()
        throw LifecycleCheckError.invariant(
            "a failed next start discarded the previous speaker review"
        )
    }

    await writer.releaseBlockedPublication()
    guard await eventually(timeout: .seconds(2), {
        await writer.publishCount >= terminalPublishCount + 2
    }) else {
        throw LifecycleCheckError.invariant(
            "coalesced terminal enrollments were not republished"
        )
    }
    let terminalSnapshot = await controller.currentSnapshot()
    guard core.recoverableOpenCount > 0,
          await writer.lastPreferredFilenameStem
              == "Original voice profile filename",
          session.lastRenderedTitle == "Original voice profile title",
          terminalSnapshot.failureCode == .vaultNotSelected,
          terminalSnapshot.speakers.contains(where: {
              $0.speakerID == secondEnrollment.speakerID
                  && $0.displayName == "Bob"
                  && !$0.isAnonymous
          })
    else {
        throw LifecycleCheckError.invariant(
            "terminal enrollment did not safely republish the original transcript"
        )
    }

    try await controller.renameVoiceProfile(
        profileID: firstEnrollment.profileID,
        displayName: "Alicia"
    )
    let profilesAfterRename = try await controller.voiceProfiles()
    guard profilesAfterRename.contains(where: {
        $0.profileID == firstEnrollment.profileID
            && $0.displayName == "Alicia"
    }) else {
        throw LifecycleCheckError.invariant(
            "voice profile rename was not visible through the controller"
        )
    }
    try await controller.deleteVoiceProfile(
        profileID: secondEnrollment.profileID
    )
    let profilesAfterDelete = try await controller.voiceProfiles()
    guard !profilesAfterDelete.contains(where: {
        $0.profileID == secondEnrollment.profileID
    }) else {
        throw LifecycleCheckError.invariant(
            "voice profile deletion was not visible through the controller"
        )
    }
}

func runLatestRequestGenerationCheck() throws {
    var generation = LatestRequestGeneration()
    let stale = generation.advance()
    let latest = generation.advance()
    guard !generation.isCurrent(stale),
          generation.isCurrent(latest)
    else {
        throw LifecycleCheckError.invariant(
            "an older async profile refresh could overwrite a newer result"
        )
    }
}

private func eventually(
    timeout: Duration,
    _ predicate: @escaping @Sendable () async -> Bool
) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
        if await predicate() {
            return true
        }
        try? await Task.sleep(for: .milliseconds(10))
    }
    return await predicate()
}

private actor LifecycleCheckCompletion {
    private(set) var startReturned = false
    private(set) var stopReturned = false
    private(set) var terminationReturned = false
    private(set) var preparingStartReturned = false
    private(set) var voiceEnrollmentReturned = false

    func markStartReturned() {
        startReturned = true
    }

    func markStopReturned() {
        stopReturned = true
    }

    func markTerminationReturned() {
        terminationReturned = true
    }

    func markPreparingStartReturned() {
        preparingStartReturned = true
    }

    func markVoiceEnrollmentReturned() {
        voiceEnrollmentReturned = true
    }
}

private actor SequencedMarkdownPublisher: MarkdownPublishing {
    private var latestBySession: [
        UUID: MarkdownPublicationReceipt
    ] = [:]
    private(set) var publishCount = 0
    private(set) var lastPreferredFilenameStem: String?
    private var shouldBlockNextPublication = false
    private var blockedContinuation: CheckedContinuation<Void, Never>?
    private(set) var blockedPublicationEntered = false

    func blockNextPublication() {
        shouldBlockNextPublication = true
        blockedPublicationEntered = false
    }

    func releaseBlockedPublication() {
        shouldBlockNextPublication = false
        let continuation = blockedContinuation
        blockedContinuation = nil
        continuation?.resume()
    }

    func publish(
        _ request: MarkdownPublicationRequest
    ) async throws -> MarkdownPublicationReceipt {
        if shouldBlockNextPublication {
            shouldBlockNextPublication = false
            blockedPublicationEntered = true
            await withCheckedContinuation { continuation in
                blockedContinuation = continuation
            }
        }
        publishCount += 1
        lastPreferredFilenameStem = request.preferredFilenameStem
        let sequence = publishCount
        let filename = "Late publication \(sequence).md"
        let receipt = MarkdownPublicationReceipt(
            destination: .staging,
            fingerprint: PublicationFingerprint(
                sha256Hex: String(
                    repeating: sequence.isMultiple(of: 2) ? "2" : "1",
                    count: 64
                ),
                fileIdentity: nil,
                byteCount: request.markdown.count
            ),
            highestSegmentRevision: request.highestSegmentRevision,
            publishedAt: Date(),
            filename: filename,
            url: URL(fileURLWithPath: "/private/tmp/\(filename)")
        )
        latestBySession[request.sessionID] = receipt
        return receipt
    }

    func latestReceipt(
        for sessionID: UUID
    ) async -> MarkdownPublicationReceipt? {
        latestBySession[sessionID]
    }
}

private actor BlockingMarkdownPublisher: MarkdownPublishing {
    private var released = false
    private var waiters: [CheckedContinuation<Void, Never>] = []
    private(set) var publishCount = 0
    private(set) var firstPublishStartedAt: ContinuousClock.Instant?

    var hasStartedPublishing: Bool {
        publishCount > 0
    }

    func publish(
        _ request: MarkdownPublicationRequest
    ) async throws -> MarkdownPublicationReceipt {
        publishCount += 1
        if firstPublishStartedAt == nil {
            firstPublishStartedAt = .now
        }
        if !released {
            await withCheckedContinuation { continuation in
                waiters.append(continuation)
            }
        }
        return MarkdownPublicationReceipt(
            destination: .staging,
            fingerprint: PublicationFingerprint(
                sha256Hex: String(repeating: "0", count: 64),
                fileIdentity: nil,
                byteCount: request.markdown.count
            ),
            highestSegmentRevision: request.highestSegmentRevision,
            publishedAt: Date(),
            filename: "Lifecycle check.md",
            url: URL(fileURLWithPath: "/private/tmp/Lifecycle check.md")
        )
    }

    func latestReceipt(
        for sessionID: UUID
    ) async -> MarkdownPublicationReceipt? {
        nil
    }

    func release() {
        released = true
        let pending = waiters
        waiters.removeAll()
        for waiter in pending {
            waiter.resume()
        }
    }
}

private actor ReverseDirectoryResolver: VaultDirectoryResolving {
    private var requestCount = 0
    private var firstContinuation: CheckedContinuation<Void, Never>?
    private(set) var firstRequestIsSuspended = false

    func resolveLease() async throws -> any SecurityScopedResourceLeasing {
        requestCount += 1
        if requestCount == 1 {
            firstRequestIsSuspended = true
            await withCheckedContinuation { continuation in
                firstContinuation = continuation
            }
        }
        throw SecurityScopedResourceError.noSelection
    }

    func releaseFirstRequest() {
        let continuation = firstContinuation
        firstContinuation = nil
        continuation?.resume()
    }
}

private actor LifecycleCheckPermissions: PermissionProviding {
    func currentSnapshot() -> CapturePermissionSnapshot {
        CapturePermissionSnapshot(
            microphone: .authorized,
            screenAndSystemAudio: .authorized
        )
    }

    func requestRequiredPermissionsAfterConsent()
        -> CapturePermissionSnapshot
    {
        currentSnapshot()
    }
}

private actor LifecycleCheckVaultSelection: VaultSelectionProviding {
    private var selected: Bool

    init(selected: Bool = true) {
        self.selected = selected
    }

    func hasSelection() -> Bool { selected }

    func setSelected(_ selected: Bool) {
        self.selected = selected
    }
}

private actor LifecycleCheckModelSelection: ModelSelectionProviding {
    func hasSelection() -> Bool { true }

    func resolveLease() -> any SecurityScopedResourceLeasing {
        LifecycleCheckLease(
            url: URL(fileURLWithPath: "/private/tmp/ggml-lifecycle-check.bin")
        )
    }
}

private final class LifecycleCheckLease:
    SecurityScopedResourceLeasing,
    @unchecked Sendable
{
    let url: URL

    init(url: URL) {
        self.url = url
    }

    func release() {}
}

private actor LifecycleCheckCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind
    private(set) var stopCount = 0

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

    func stop() {
        stopCount += 1
    }
}

private actor FlappingRecoveryCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .systemAudio
    private(set) var startCount = 0
    private var eventHandler: CaptureEventHandler?

    init(sourceID: UInt64) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        startCount += 1
        self.eventHandler = eventHandler
        eventHandler(.ready(.systemAudio))
        if startCount == 2 {
            eventHandler(
                .failed(.systemAudio, code: "ready_then_failed")
            )
        }
    }

    func stop() {
        eventHandler = nil
    }

    func emitFailure() {
        eventHandler?(.failed(.systemAudio, code: "fixture_failure"))
    }
}

private actor StableCountingCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .systemAudio
    private(set) var startCount = 0
    private(set) var stopCount = 0
    private var eventHandler: CaptureEventHandler?

    init(sourceID: UInt64) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        startCount += 1
        self.eventHandler = eventHandler
        eventHandler(.ready(.systemAudio))
    }

    func stop() {
        stopCount += 1
        eventHandler = nil
    }

    func emitFailure() {
        eventHandler?(.failed(.systemAudio, code: "fixture_failure"))
    }
}

private actor DelayedStopRecoveryCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .systemAudio
    private(set) var startCount = 0
    private var eventHandler: CaptureEventHandler?

    init(sourceID: UInt64) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        startCount += 1
        self.eventHandler = eventHandler
        if startCount == 2 {
            eventHandler(.stopped(.systemAudio))
        }
        eventHandler(.ready(.systemAudio))
    }

    func stop() {
        eventHandler = nil
    }

    func emitFailure() {
        eventHandler?(.failed(.systemAudio, code: "fixture_failure"))
    }
}

private actor BlockingStartCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .systemAudio
    private(set) var hasEnteredStart = false
    private(set) var stopCount = 0
    private var startContinuation: CheckedContinuation<Void, Never>?
    private var wasStopped = false

    init(sourceID: UInt64) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) async throws {
        hasEnteredStart = true
        await withCheckedContinuation { continuation in
            startContinuation = continuation
        }
        guard !wasStopped else {
            throw CancellationError()
        }
        eventHandler(.ready(.systemAudio))
    }

    func stop() {
        stopCount += 1
        wasStopped = true
        let continuation = startContinuation
        startContinuation = nil
        continuation?.resume()
    }

    func releaseStart() {
        let continuation = startContinuation
        startContinuation = nil
        continuation?.resume()
    }
}

private actor HangingStopCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .systemAudio
    private(set) var stopWasEntered = false
    private var stopContinuation: CheckedContinuation<Void, Never>?

    init(sourceID: UInt64) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) {
        eventHandler(.ready(.systemAudio))
    }

    func stop() async {
        guard !stopWasEntered else {
            return
        }
        stopWasEntered = true
        await withCheckedContinuation { continuation in
            stopContinuation = continuation
        }
    }

    func releaseStop() {
        let continuation = stopContinuation
        stopContinuation = nil
        continuation?.resume()
    }
}

private actor HangingStartAndStopCapture: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind = .systemAudio
    private(set) var startWasEntered = false
    private(set) var stopWasEntered = false
    private var startContinuation: CheckedContinuation<Void, Never>?
    private var stopContinuation: CheckedContinuation<Void, Never>?

    init(sourceID: UInt64) {
        self.sourceID = sourceID
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) async {
        startWasEntered = true
        await withCheckedContinuation { continuation in
            startContinuation = continuation
        }
        eventHandler(.ready(.systemAudio))
    }

    func stop() async {
        guard !stopWasEntered else {
            return
        }
        stopWasEntered = true
        await withCheckedContinuation { continuation in
            stopContinuation = continuation
        }
    }

    func releaseStart() {
        let continuation = startContinuation
        startContinuation = nil
        continuation?.resume()
    }

    func releaseStop() {
        let continuation = stopContinuation
        stopContinuation = nil
        continuation?.resume()
    }
}

private actor CaptureBarrierAdapter: AudioCaptureSource {
    nonisolated let sourceID: UInt64
    nonisolated let kind: CaptureSourceKind

    private let suspendsStartup: Bool
    private var frameHandler: AudioFrameHandler?
    private var eventHandler: CaptureEventHandler?
    private var startContinuation: CheckedContinuation<Void, Never>?
    private var startAttempt = 0
    private var nextSequenceNumber: UInt64 = 0
    private var startupDispositions:
        [Int: AudioFrameDisposition] = [:]
    private(set) var suspendedStartAttempt: Int?

    init(
        sourceID: UInt64,
        kind: CaptureSourceKind,
        suspendsStartup: Bool
    ) {
        self.sourceID = sourceID
        self.kind = kind
        self.suspendsStartup = suspendsStartup
    }

    func start(
        frameHandler: @escaping AudioFrameHandler,
        eventHandler: @escaping CaptureEventHandler
    ) async throws {
        startAttempt += 1
        let attempt = startAttempt
        self.frameHandler = frameHandler
        self.eventHandler = eventHandler

        if !suspendsStartup {
            eventHandler(.ready(kind))
        }
        startupDispositions[attempt] = frameHandler(makeFrame())

        if suspendsStartup {
            suspendedStartAttempt = attempt
            await withCheckedContinuation { continuation in
                startContinuation = continuation
            }
            suspendedStartAttempt = nil
            eventHandler(.ready(kind))
        }
    }

    func stop() {
        frameHandler = nil
        eventHandler = nil
        let continuation = startContinuation
        startContinuation = nil
        continuation?.resume()
    }

    func releaseStart() {
        let continuation = startContinuation
        startContinuation = nil
        continuation?.resume()
    }

    func startupDisposition(
        attempt: Int
    ) -> AudioFrameDisposition? {
        startupDispositions[attempt]
    }

    func emitFrame() -> AudioFrameDisposition? {
        frameHandler?(makeFrame())
    }

    func emitFailure(code: String) {
        eventHandler?(.failed(kind, code: code))
    }

    private func makeFrame() -> CapturedAudioFrame {
        let sequenceNumber = nextSequenceNumber
        nextSequenceNumber &+= 1
        return try! CapturedAudioFrame(
            sourceID: sourceID,
            sequenceNumber: sequenceNumber,
            monotonicTimeNanoseconds: Int64(sequenceNumber) * 10_000_000,
            sampleRateHz: 48_000,
            channelCount: 1,
            frameCount: 480,
            flags: sequenceNumber == 0 ? [.discontinuity] : [],
            interleavedSamples: Array(repeating: 0.25, count: 480)
        )
    }
}

private struct CaptureBarrierSourceRecord: Sendable {
    let kind: CaptureSourceKind
    let reasonCode: String
}

private actor CaptureBarrierStateRecorder {
    private(set) var values: [SessionShellState] = []

    func append(_ state: SessionShellState) {
        values.append(state)
    }
}

private final class CaptureBarrierCore:
    CoreClientProtocol,
    @unchecked Sendable
{
    private let session: CaptureBarrierSession

    init(session: CaptureBarrierSession) {
        self.session = session
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) -> any CoreSessionProtocol {
        session
    }

    func recoverableSessionIDs() -> [String] { [] }

    func openRecoverableSession(id: String) throws
        -> any CoreSessionProtocol
    {
        throw CoreBridgeError.unavailable
    }
}

private final class CaptureBarrierSession:
    CoreSessionProtocol,
    @unchecked Sendable
{
    let sessionID = UUID(
        uuidString: "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    )!

    private let condition = NSCondition()
    private var events: [CoreEvent] = []
    private var isClosed = false
    private var phase: CorePhase = .preparing
    private var finalizeReason: CoreFinalizeReason = .unknown
    private var sourcesReady = false
    private var markSourcesReadyCalls = 0
    private var pushAudioCalls = 0
    private var observedPushBeforeSourcesReady = false
    private var recordedSourceEvents: [CaptureBarrierSourceRecord] = []
    private var deliveredEvents = 0

    init() {
        events.append(
            .stateChanged(
                CoreStateEvent(
                    phase: .preparing,
                    publishedStatus: .unknown,
                    finalizeReason: .unknown
                )
            )
        )
    }

    var markSourcesReadyCount: Int {
        condition.lock()
        defer { condition.unlock() }
        return markSourcesReadyCalls
    }

    var pushAudioCount: Int {
        condition.lock()
        defer { condition.unlock() }
        return pushAudioCalls
    }

    var pushedBeforeSourcesReady: Bool {
        condition.lock()
        defer { condition.unlock() }
        return observedPushBeforeSourcesReady
    }

    var sourceEvents: [CaptureBarrierSourceRecord] {
        condition.lock()
        defer { condition.unlock() }
        return recordedSourceEvents
    }

    var deliveredEventCount: Int {
        condition.lock()
        defer { condition.unlock() }
        return deliveredEvents
    }

    func enqueueSourceEvent(_ event: CoreSourceEvent) {
        condition.lock()
        events.append(.sourceChanged(event))
        condition.signal()
        condition.unlock()
    }

    func markSourcesReady() {
        condition.lock()
        sourcesReady = true
        markSourcesReadyCalls += 1
        phase = .recording
        events.append(
            .stateChanged(
                CoreStateEvent(
                    phase: .recording,
                    publishedStatus: .recording,
                    finalizeReason: .unknown
                )
            )
        )
        condition.signal()
        condition.unlock()
    }

    func pushAudio(_ frame: CapturedAudioFrame) -> AudioFrameDisposition {
        condition.lock()
        if !sourcesReady || phase != .recording {
            observedPushBeforeSourcesReady = true
        }
        pushAudioCalls += 1
        condition.unlock()
        return .accepted
    }

    func pause() {
        condition.lock()
        phase = .paused
        condition.unlock()
    }

    func resumeAfterConsent() {
        condition.lock()
        phase = .recording
        condition.unlock()
    }

    func sourceEvent(
        sourceID: UInt64,
        kind: CaptureSourceKind,
        event: CoreSourceEventKind,
        health: CoreSourceHealth,
        startTimeNanoseconds: Int64,
        endTimeNanoseconds: Int64,
        reasonCode: String
    ) {
        condition.lock()
        recordedSourceEvents.append(
            CaptureBarrierSourceRecord(
                kind: kind,
                reasonCode: reasonCode
            )
        )
        condition.unlock()
    }

    func finalize(reason: CoreFinalizeReason) {
        condition.lock()
        finalizeReason = reason
        phase = reason == .cancelled ? .failedToStart : .complete
        let status: CorePublishedStatus =
            phase == .complete ? .complete : .interrupted
        let state = CoreStateEvent(
            phase: phase,
            publishedStatus: status,
            finalizeReason: reason
        )
        events.append(
            phase == .failedToStart
                ? .stateChanged(state)
                : .terminal(state)
        )
        condition.signal()
        condition.unlock()
    }

    func nextEvent(timeoutMilliseconds: UInt32) throws -> CoreEvent? {
        condition.lock()
        defer { condition.unlock() }
        if events.isEmpty && !isClosed {
            let deadline = Date().addingTimeInterval(
                Double(timeoutMilliseconds) / 1_000
            )
            _ = condition.wait(until: deadline)
        }
        if !events.isEmpty {
            deliveredEvents += 1
            return events.removeFirst()
        }
        if isClosed {
            throw CoreBridgeError.closed
        }
        return nil
    }

    func currentState() -> CoreStateEvent {
        condition.lock()
        defer { condition.unlock() }
        let status: CorePublishedStatus
        switch phase {
        case .complete:
            status = .complete
        case .incompleteSources:
            status = .incompleteSources
        case .interrupted, .failedToStart:
            status = .interrupted
        default:
            status = .recording
        }
        return CoreStateEvent(
            phase: phase,
            publishedStatus: status,
            finalizeReason: finalizeReason
        )
    }

    func metrics() -> CorePipelineMetrics {
        condition.lock()
        defer { condition.unlock() }
        return CorePipelineMetrics(
            framesOffered: UInt64(pushAudioCalls),
            framesAccepted: UInt64(pushAudioCalls),
            framesRejected: 0,
            discontinuities: 0,
            finalSegmentsCommitted: 0,
            partialEventsCoalesced: 0,
            audioQueueDepth: 0,
            audioQueueHighWater: 0,
            journalCheckpoint: 1,
            highestSegmentRevision: 0
        )
    }

    func renderMarkdown(
        options: CoreMarkdownOptions
    ) -> CoreRenderedMarkdown {
        return CoreRenderedMarkdown(
            data: Data(
                """
                ---
                status: "recording"
                ---

                <!-- transcript:start -->
                <!-- transcript:end -->
                <!-- capture-events:start -->
                <!-- capture-events:end -->
                """.utf8
            ),
            journalCheckpoint: 1,
            highestSegmentRevision: 0
        )
    }

    func acknowledgePublication(
        receipt: MarkdownPublicationReceipt,
        journalCheckpoint: UInt64
    ) {}

    func close() {
        condition.lock()
        isClosed = true
        condition.broadcast()
        condition.unlock()
    }
}

private final class LifecycleCheckCore:
    CoreClientProtocol,
    @unchecked Sendable
{
    private let session: LifecycleCheckSession
    private let lock = NSLock()
    private var createdSessionID: UUID?
    private var profiles: [UInt64: CoreVoiceProfile] = [:]
    private var nextProfileID: UInt64 = 1
    private var recoverableOpens = 0

    init(session: LifecycleCheckSession) {
        self.session = session
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) -> any CoreSessionProtocol {
        lock.withLock {
            createdSessionID = configuration.sessionID
        }
        return session
    }

    func recoverableSessionIDs() -> [String] { [] }

    func openRecoverableSession(id: String) throws
        -> any CoreSessionProtocol
    {
        let matches = lock.withLock { () -> Bool in
            guard createdSessionID?.uuidString.lowercased() == id else {
                return false
            }
            recoverableOpens += 1
            return true
        }
        guard matches else {
            throw CoreBridgeError.unavailable
        }
        return session
    }

    var recoverableOpenCount: Int {
        lock.withLock { recoverableOpens }
    }

    func listVoiceProfiles() -> [CoreVoiceProfile] {
        lock.withLock {
            profiles.values.sorted { $0.profileID < $1.profileID }
        }
    }

    func enrollVoiceProfile(
        sessionID: UUID,
        speakerID: UInt64,
        displayName: String
    ) throws -> CoreVoiceProfileEnrollment {
        try lock.withLock {
            guard sessionID == createdSessionID else {
                throw CoreBridgeError.unavailable
            }
            let profileID = nextProfileID
            nextProfileID += 1
            let samples: UInt64 = 2
            profiles[profileID] = CoreVoiceProfile(
                profileID: profileID,
                displayName: displayName,
                sampleCount: samples
            )
            return CoreVoiceProfileEnrollment(
                profileID: profileID,
                speakerID: UInt64(1) << 62 | profileID,
                sampleCount: samples,
                relabeledSegments: 2,
                journalCheckpoint: 3,
                highestSegmentRevision: 1
            )
        }
    }

    func renameVoiceProfile(
        profileID: UInt64,
        displayName: String
    ) throws {
        try lock.withLock {
            guard let profile = profiles[profileID] else {
                throw CoreBridgeError.unavailable
            }
            profiles[profileID] = CoreVoiceProfile(
                profileID: profile.profileID,
                displayName: displayName,
                sampleCount: profile.sampleCount
            )
        }
    }

    func deleteVoiceProfile(profileID: UInt64) throws {
        try lock.withLock {
            guard profiles.removeValue(forKey: profileID) != nil else {
                throw CoreBridgeError.unavailable
            }
        }
    }
}

private final class BlockingCreateCore:
    CoreClientProtocol,
    @unchecked Sendable
{
    private let condition = NSCondition()
    private let session: LifecycleCheckSession
    private var createWasEntered = false
    private var createReleased = false

    init(session: LifecycleCheckSession) {
        self.session = session
    }

    var hasEnteredCreate: Bool {
        condition.lock()
        defer { condition.unlock() }
        return createWasEntered
    }

    func releaseCreate() {
        condition.lock()
        createReleased = true
        condition.broadcast()
        condition.unlock()
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) -> any CoreSessionProtocol {
        condition.lock()
        createWasEntered = true
        condition.broadcast()
        while !createReleased {
            condition.wait()
        }
        condition.unlock()
        return session
    }

    func recoverableSessionIDs() -> [String] { [] }

    func openRecoverableSession(id: String) throws
        -> any CoreSessionProtocol
    {
        throw CoreBridgeError.unavailable
    }
}

private final class LifecycleCheckSession:
    CoreSessionProtocol,
    @unchecked Sendable
{
    let sessionID = UUID(
        uuidString: "12345678-1234-1234-1234-1234567890ab"
    )!

    private let condition = NSCondition()
    private var events: [CoreEvent] = []
    private var isClosed = false
    private var phase: CorePhase = .preparing
    private var finalizeReason: CoreFinalizeReason = .unknown
    private let blocksFinalize: Bool
    private let blocksRenderAfterCount: Int?
    private let blocksMetricsAfterCount: Int?
    private let blocksAcknowledgementAfterCount: Int?
    private let finalSegmentOnFinalize: CoreTranscriptSegment?
    private var finalizeReleased = false
    private var finalizeWasEntered = false
    private var renderCalls = 0
    private var renderReleased = false
    private var blockedRenderWasEntered = false
    private var metricsCalls = 0
    private var completedMetricsCalls = 0
    private var metricsReleased = false
    private var blockedMetricsWasEntered = false
    private var acknowledgementCalls = 0
    private var completedAcknowledgementCalls = 0
    private var acknowledgementReleased = false
    private var blockedAcknowledgementWasEntered = false
    private var renderedTitle: String?
    private var terminalDrainPolls = 0

    init(
        blocksFinalize: Bool = false,
        blocksRenderAfterCount: Int? = nil,
        blocksMetricsAfterCount: Int? = nil,
        blocksAcknowledgementAfterCount: Int? = nil,
        finalSegmentOnFinalize: CoreTranscriptSegment? = nil
    ) {
        self.blocksFinalize = blocksFinalize
        self.blocksRenderAfterCount = blocksRenderAfterCount
        self.blocksMetricsAfterCount = blocksMetricsAfterCount
        self.blocksAcknowledgementAfterCount =
            blocksAcknowledgementAfterCount
        self.finalSegmentOnFinalize = finalSegmentOnFinalize
    }

    var hasEnteredFinalize: Bool {
        condition.lock()
        defer { condition.unlock() }
        return finalizeWasEntered
    }

    var hasClosed: Bool {
        condition.lock()
        defer { condition.unlock() }
        return isClosed
    }

    var lastRenderedTitle: String? {
        condition.lock()
        defer { condition.unlock() }
        return renderedTitle
    }

    var terminalDrainPollCount: Int {
        condition.lock()
        defer { condition.unlock() }
        return terminalDrainPolls
    }

    func enqueueFinalSegment(_ segment: CoreTranscriptSegment) {
        condition.lock()
        events.append(.finalSegment(segment))
        condition.signal()
        condition.unlock()
    }

    func releaseFinalize() {
        condition.lock()
        finalizeReleased = true
        condition.broadcast()
        condition.unlock()
    }

    var hasEnteredBlockedRender: Bool {
        condition.lock()
        defer { condition.unlock() }
        return blockedRenderWasEntered
    }

    func releaseRender() {
        condition.lock()
        renderReleased = true
        condition.broadcast()
        condition.unlock()
    }

    func hasEnteredBlockedPublicationOperation(
        for stage: LatePublicationBlockingStage
    ) -> Bool {
        condition.lock()
        defer { condition.unlock() }
        switch stage {
        case .acknowledgement:
            return blockedAcknowledgementWasEntered
        case .metrics:
            return blockedMetricsWasEntered
        }
    }

    func completedPublicationOperationCount(
        for stage: LatePublicationBlockingStage
    ) -> Int {
        condition.lock()
        defer { condition.unlock() }
        switch stage {
        case .acknowledgement:
            return completedAcknowledgementCalls
        case .metrics:
            return completedMetricsCalls
        }
    }

    func releasePublicationOperation(
        for stage: LatePublicationBlockingStage
    ) {
        condition.lock()
        switch stage {
        case .acknowledgement:
            acknowledgementReleased = true
        case .metrics:
            metricsReleased = true
        }
        condition.broadcast()
        condition.unlock()
    }

    func markSourcesReady() {
        condition.lock()
        phase = .recording
        events.append(
            .stateChanged(
                CoreStateEvent(
                    phase: phase,
                    publishedStatus: .recording,
                    finalizeReason: finalizeReason
                )
            )
        )
        condition.signal()
        condition.unlock()
    }

    func pushAudio(_ frame: CapturedAudioFrame) -> AudioFrameDisposition {
        .accepted
    }

    func pause() {}
    func resumeAfterConsent() {}

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
        condition.lock()
        if blocksFinalize {
            finalizeWasEntered = true
            condition.broadcast()
            while !finalizeReleased {
                condition.wait()
            }
        }
        finalizeReason = reason
        phase = reason == .cancelled ? .failedToStart : .complete
        let status: CorePublishedStatus =
            phase == .complete ? .complete : .interrupted
        if let finalSegmentOnFinalize {
            events.append(.finalSegment(finalSegmentOnFinalize))
        }
        events.append(
            phase == .failedToStart
                ? .stateChanged(
                    CoreStateEvent(
                        phase: phase,
                        publishedStatus: status,
                        finalizeReason: reason
                    )
                )
                : .terminal(
                    CoreStateEvent(
                        phase: phase,
                        publishedStatus: status,
                        finalizeReason: reason
                    )
                )
        )
        condition.signal()
        condition.unlock()
    }

    func nextEvent(timeoutMilliseconds: UInt32) throws -> CoreEvent? {
        condition.lock()
        defer { condition.unlock() }
        if timeoutMilliseconds == 0,
           phase == .complete || phase == .incompleteSources
                || phase == .interrupted
        {
            terminalDrainPolls += 1
        }
        if events.isEmpty && !isClosed {
            let deadline = Date().addingTimeInterval(
                Double(timeoutMilliseconds) / 1_000
            )
            _ = condition.wait(until: deadline)
        }
        if !events.isEmpty {
            return events.removeFirst()
        }
        if isClosed {
            throw CoreBridgeError.closed
        }
        return nil
    }

    func currentState() -> CoreStateEvent {
        condition.lock()
        defer { condition.unlock() }
        let status: CorePublishedStatus
        switch phase {
        case .complete:
            status = .complete
        case .incompleteSources:
            status = .incompleteSources
        case .interrupted, .failedToStart:
            status = .interrupted
        default:
            status = .recording
        }
        return CoreStateEvent(
            phase: phase,
            publishedStatus: status,
            finalizeReason: finalizeReason
        )
    }

    func metrics() -> CorePipelineMetrics {
        condition.lock()
        metricsCalls += 1
        if let blocksMetricsAfterCount,
           metricsCalls > blocksMetricsAfterCount
        {
            blockedMetricsWasEntered = true
            condition.broadcast()
            while !metricsReleased {
                condition.wait()
            }
        }
        completedMetricsCalls += 1
        condition.unlock()
        return CorePipelineMetrics(
            framesOffered: 0,
            framesAccepted: 0,
            framesRejected: 0,
            discontinuities: 0,
            finalSegmentsCommitted: 0,
            partialEventsCoalesced: 0,
            audioQueueDepth: 0,
            audioQueueHighWater: 0,
            journalCheckpoint: 2,
            highestSegmentRevision: 0
        )
    }

    func renderMarkdown(
        options: CoreMarkdownOptions
    ) -> CoreRenderedMarkdown {
        condition.lock()
        renderCalls += 1
        renderedTitle = options.title
        if let blocksRenderAfterCount,
           renderCalls > blocksRenderAfterCount
        {
            blockedRenderWasEntered = true
            condition.broadcast()
            while !renderReleased {
                condition.wait()
            }
        }
        condition.unlock()
        return CoreRenderedMarkdown(
            data: Data(
                """
                ---
                status: "recording"
                ---

                <!-- transcript:start -->
                <!-- transcript:end -->
                <!-- capture-events:start -->
                <!-- capture-events:end -->
                """.utf8
            ),
            journalCheckpoint: 2,
            highestSegmentRevision: 0
        )
    }

    func acknowledgePublication(
        receipt: MarkdownPublicationReceipt,
        journalCheckpoint: UInt64
    ) {
        condition.lock()
        acknowledgementCalls += 1
        if let blocksAcknowledgementAfterCount,
           acknowledgementCalls > blocksAcknowledgementAfterCount
        {
            blockedAcknowledgementWasEntered = true
            condition.broadcast()
            while !acknowledgementReleased {
                condition.wait()
            }
        }
        completedAcknowledgementCalls += 1
        condition.unlock()
    }

    func close() {
        condition.lock()
        isClosed = true
        condition.broadcast()
        condition.unlock()
    }

}
