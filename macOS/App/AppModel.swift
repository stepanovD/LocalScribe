import AppKit
import Combine
import Foundation

@MainActor
final class AppModel: ObservableObject {
    @Published private(set) var session = SessionSnapshot.idle
    @Published private(set) var permissions = CapturePermissionSnapshot(
        microphone: .notDetermined,
        screenAndSystemAudio: .notDetermined
    )
    @Published private(set) var hasVaultSelection = false
    @Published private(set) var hasModelSelection = false
    @Published private(set) var setupFailure: SessionFailureCode?
    @Published private(set) var voiceProfiles: [CoreVoiceProfile] = []
    @Published private(set) var voiceProfileFailure: String?
    @Published private(set) var isVoiceProfileOperationInProgress = false
    @Published private(set) var detectedCallProposal: DetectedCallProposal?

    private let directoryStore: SecurityScopedDirectoryStore
    private let modelStore: SecurityScopedModelStore
    private let controller: SessionController?
    private let callDetectionMonitor: CallDetectionMonitor
    private let consentIssuer = VisibleConsentIssuer()
    private var updatesTask: Task<Void, Never>?
    private var callDetectionUpdatesTask: Task<Void, Never>?
    private var callDetectionOfferLedger = CallDetectionOfferLedger()
    private var voiceProfileRefreshGeneration = LatestRequestGeneration()

    init() {
        let directoryStore = SecurityScopedDirectoryStore()
        let modelStore = SecurityScopedModelStore()
        let callDetectionMonitor = CallDetectionMonitor()
        self.directoryStore = directoryStore
        self.modelStore = modelStore
        self.callDetectionMonitor = callDetectionMonitor

        let applicationSupport = URL.applicationSupportDirectory
            .appendingPathComponent("LocalScribe", isDirectory: true)
        let journalURL = applicationSupport.appendingPathComponent(
            "recovery.sqlite3",
            isDirectory: false
        )

        do {
            try FileManager.default.createDirectory(
                at: applicationSupport,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            let staging = StagingDirectory(
                rootURL: applicationSupport.appendingPathComponent(
                    "Staging",
                    isDirectory: true
                )
            )
            let writer = VaultWriter(
                directoryStore: directoryStore,
                stagingDirectory: staging
            )
            let core = try CoreClient(journalURL: journalURL)
            controller = SessionController(
                coreClient: core,
                permissions: PermissionClient(),
                directoryStore: directoryStore,
                modelStore: modelStore,
                vaultWriter: writer,
                microphoneCapture: MicrophoneCaptureAdapter(),
                systemAudioCapture: ScreenCaptureKitAdapter(),
                journalURL: journalURL
            )
        } catch {
            controller = nil
            setupFailure = .coreUnavailable
        }

        beginObserving()
        beginObservingCallDetection()
        Task {
            await controller?.recoverInterruptedSessions()
            await refreshSetupState()
            await startCallDetectionIfAvailable()
        }
    }

    deinit {
        updatesTask?.cancel()
        callDetectionUpdatesTask?.cancel()
    }

    var menuBarSymbol: String {
        switch session.state {
        case .detected, .awaitingConsent:
            "phone.badge.waveform"
        case .recording:
            "record.circle.fill"
        case .paused:
            "pause.circle.fill"
        case .preparing, .finalizing:
            "waveform.badge.magnifyingglass"
        case .failedToStart, .interrupted, .incompleteSources:
            "exclamationmark.triangle.fill"
        default:
            "waveform"
        }
    }

    var menuBarTitle: String {
        switch session.state {
        case .detected, .awaitingConsent:
            "Call detected"
        case .recording:
            "Recording"
        case .paused:
            "Paused"
        case .preparing:
            "Preparing"
        case .finalizing:
            "Finalizing"
        case .failedToStart:
            "Needs attention"
        default:
            "LocalScribe"
        }
    }

    var canStartRecording: Bool {
        controller != nil && hasVaultSelection && hasModelSelection
    }

    var isRecordingEngineAvailable: Bool {
        controller != nil
    }

    func requestManualStart() {
        guard let controller else {
            setupFailure = .coreUnavailable
            return
        }
        Task {
            do {
                try await controller.proposeManualStart()
            } catch {
                setupFailure = .invalidTransition
            }
        }
    }

    /// Called only by the visible, specifically labelled confirmation button.
    func confirmVisibleStart() {
        confirmVisibleStart(expectedProposalID: detectedCallProposal?.id)
    }

    /// Called only by the Start button in the detected-call panel.
    func confirmDetectedCallStart(proposalID: UUID) {
        guard detectedCallProposal?.id == proposalID else {
            return
        }
        confirmVisibleStart(expectedProposalID: proposalID)
    }

    private func confirmVisibleStart(expectedProposalID: UUID?) {
        guard let controller else {
            setupFailure = .coreUnavailable
            return
        }
        guard canStartRecording else {
            return
        }
        guard detectedCallProposal?.id == expectedProposalID else {
            return
        }

        let acceptedProposal = detectedCallProposal
        suppressActiveDetectedCalls()
        detectedCallProposal = nil

        let now = Date()
        let filenameFormatter = DateFormatter()
        filenameFormatter.locale = Locale(identifier: "en_US_POSIX")
        filenameFormatter.dateFormat = "yyyy-MM-dd HH-mm"
        let titleFormatter = DateFormatter()
        titleFormatter.locale = .current
        titleFormatter.dateStyle = .medium
        titleFormatter.timeStyle = .short

        let token = consentIssuer.issue(for: .start)
        let applicationName = acceptedProposal?.applicationName ?? "Manual"
        let callName: String
        if let acceptedProposal {
            callName = "\(acceptedProposal.applicationName) Call"
        } else {
            callName = "Call"
        }
        let request = SessionStartRequest(
            sourceApplication: applicationName,
            title: "\(callName) \(titleFormatter.string(from: now))",
            preferredFilenameStem:
                "\(filenameFormatter.string(from: now)) — \(callName)",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
        Task {
            await controller.start(
                after: token,
                request: request,
                expectedDetectedProposalID: expectedProposalID
            )
            await refreshSetupState()
        }
    }

    func dismissStart() {
        guard let controller else {
            return
        }
        let proposalID = detectedCallProposal?.id
        detectedCallProposal = nil
        Task {
            await controller.dismissProposal(
                expectedDetectedProposalID: proposalID
            )
        }
    }

    func dismissDetectedCall(proposalID: UUID) {
        guard detectedCallProposal?.id == proposalID else {
            return
        }
        dismissStart()
    }

    func pause() {
        guard let controller else {
            return
        }
        Task { await controller.pause() }
    }

    /// Resume is also a fresh visible action; its token cannot be reused.
    func resumeFromVisibleButton() {
        guard let controller else {
            return
        }
        let token = consentIssuer.issue(for: .resume)
        Task { await controller.resume(after: token) }
    }

    func stop() {
        guard let controller else {
            return
        }
        Task { await controller.stop() }
    }

    func retryRecovery() {
        guard let controller else {
            setupFailure = .coreUnavailable
            return
        }
        Task {
            await controller.recoverInterruptedSessions()
            await refreshSetupState()
            await startCallDetectionIfAvailable()
        }
    }

    func openLastTranscript() {
        guard let url = session.lastPublishedURL else {
            return
        }
        let destination = session.lastPublicationDestination
        Task {
            var lease: (any SecurityScopedResourceLeasing)?
            if destination != .staging {
                do {
                    lease = try await directoryStore.resolveLease()
                } catch {
                    setupFailure = .vaultNotSelected
                    return
                }
            }
            defer { lease?.release() }
            if !NSWorkspace.shared.open(url) {
                setupFailure = .internalFailure
            }
        }
    }

    func chooseVaultDirectory() {
        let panel = NSOpenPanel()
        panel.title = "Choose Obsidian transcript folder"
        panel.prompt = "Choose Folder"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true

        guard panel.runModal() == .OK, let url = panel.url else {
            return
        }
        Task {
            do {
                try await directoryStore.saveDirectory(url)
                await refreshSetupState()
            } catch {
                setupFailure = .vaultNotSelected
            }
        }
    }

    func chooseLocalModel() {
        let panel = NSOpenPanel()
        panel.title = "Choose a local ASR model"
        panel.prompt = "Choose Model"
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false

        guard panel.runModal() == .OK, let url = panel.url else {
            return
        }
        Task {
            do {
                try await modelStore.saveModel(url)
                await refreshSetupState()
            } catch {
                setupFailure = .modelNotSelected
            }
        }
    }

    func saveVoiceProfile(
        sessionID: UUID,
        speakerID: UInt64,
        displayName: String
    ) {
        guard let controller else {
            voiceProfileFailure = "Voice profiles are unavailable."
            return
        }
        guard !isVoiceProfileOperationInProgress else {
            return
        }
        let name = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            voiceProfileFailure = "Enter a name for this speaker."
            return
        }
        _ = voiceProfileRefreshGeneration.advance()
        isVoiceProfileOperationInProgress = true
        voiceProfileFailure = nil
        Task {
            defer { isVoiceProfileOperationInProgress = false }
            do {
                try await controller.enrollVoiceProfile(
                    sessionID: sessionID,
                    speakerID: speakerID,
                    displayName: name
                )
                await refreshVoiceProfiles()
            } catch {
                _ = voiceProfileRefreshGeneration.advance()
                voiceProfileFailure =
                    "The voice profile could not be saved. Try again."
            }
        }
    }

    func renameVoiceProfile(_ profile: CoreVoiceProfile, to displayName: String) {
        guard let controller else {
            voiceProfileFailure = "Voice profiles are unavailable."
            return
        }
        guard !isVoiceProfileOperationInProgress else {
            return
        }
        let name = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            voiceProfileFailure = "A voice profile name cannot be empty."
            return
        }
        _ = voiceProfileRefreshGeneration.advance()
        isVoiceProfileOperationInProgress = true
        voiceProfileFailure = nil
        Task {
            defer { isVoiceProfileOperationInProgress = false }
            do {
                try await controller.renameVoiceProfile(
                    profileID: profile.profileID,
                    displayName: name
                )
                await refreshVoiceProfiles()
            } catch {
                _ = voiceProfileRefreshGeneration.advance()
                voiceProfileFailure =
                    "The voice profile could not be renamed. Try again."
            }
        }
    }

    func deleteVoiceProfile(_ profile: CoreVoiceProfile) {
        guard let controller else {
            voiceProfileFailure = "Voice profiles are unavailable."
            return
        }
        guard !isVoiceProfileOperationInProgress else {
            return
        }
        _ = voiceProfileRefreshGeneration.advance()
        isVoiceProfileOperationInProgress = true
        voiceProfileFailure = nil
        Task {
            defer { isVoiceProfileOperationInProgress = false }
            do {
                try await controller.deleteVoiceProfile(
                    profileID: profile.profileID
                )
                await refreshVoiceProfiles()
            } catch {
                _ = voiceProfileRefreshGeneration.advance()
                voiceProfileFailure =
                    "The voice profile could not be deleted. Try again."
            }
        }
    }

    func dismissVoiceProfileFailure() {
        voiceProfileFailure = nil
    }

    func quit() {
        NSApplication.shared.terminate(nil)
    }

    func prepareForTermination() async {
        callDetectionUpdatesTask?.cancel()
        detectedCallProposal = nil
        callDetectionOfferLedger.removeAll()
        await callDetectionMonitor.stop()
        await controller?.prepareForTermination()
    }

    func refreshSetupState() async {
        hasVaultSelection = await directoryStore.hasSelection()
        hasModelSelection = await modelStore.hasSelection()
        if let controller {
            permissions = await controller.currentPermissionSnapshot()
        }
        await refreshVoiceProfiles()
    }

    func refreshVoiceProfiles() async {
        let generation = voiceProfileRefreshGeneration.advance()
        guard let controller else {
            if voiceProfileRefreshGeneration.isCurrent(generation) {
                voiceProfiles = []
            }
            return
        }
        do {
            let profiles = try await controller.voiceProfiles()
            guard voiceProfileRefreshGeneration.isCurrent(generation) else {
                return
            }
            voiceProfiles = profiles
            voiceProfileFailure = nil
        } catch {
            guard voiceProfileRefreshGeneration.isCurrent(generation) else {
                return
            }
            voiceProfileFailure = "Saved voice profiles could not be loaded."
        }
    }

    private func beginObserving() {
        guard let controller else {
            return
        }
        updatesTask = Task { [weak self] in
            for await value in controller.updates {
                guard !Task.isCancelled, let self else {
                    return
                }
                self.session = value
                if self.captureLifecycleOwnsCurrentCalls(value.state) {
                    self.suppressActiveDetectedCalls()
                } else {
                    await self.offerPendingDetectedCallIfPossible()
                }
            }
        }
    }

    private func beginObservingCallDetection() {
        let events = callDetectionMonitor.events
        callDetectionUpdatesTask = Task { [weak self] in
            for await event in events {
                guard !Task.isCancelled, let self else {
                    return
                }
                await self.handleCallDetectionEvent(event)
            }
        }
    }

    private func startCallDetectionIfAvailable() async {
        guard let controller else {
            return
        }
        let snapshot = await controller.currentSnapshot()
        guard snapshot.state != .recoveryRequired else {
            return
        }
        await callDetectionMonitor.start()
    }

    private func handleCallDetectionEvent(
        _ event: CallDetectionEvent
    ) async {
        guard let controller else {
            return
        }

        switch event {
        case let .began(proposal):
            let shouldSuppress = captureLifecycleOwnsCurrentCalls(
                session.state
            )
            callDetectionOfferLedger.observeBegan(
                proposal,
                suppress: shouldSuppress
            )
            if shouldSuppress {
                return
            }
            await offerPendingDetectedCallIfPossible()

        case let .ended(proposal):
            callDetectionOfferLedger.observeEnded(proposal)
            guard detectedCallProposal?.id == proposal.id else {
                return
            }
            detectedCallProposal = nil
            await controller.dismissProposal(
                expectedDetectedProposalID: proposal.id
            )
        }
    }

    private func offerPendingDetectedCallIfPossible() async {
        guard detectedCallProposal == nil, let controller else {
            return
        }

        guard let proposal = callDetectionOfferLedger.nextOffer(),
              callDetectionOfferLedger.reserve(proposal)
        else {
            return
        }

        // The reservation spans the actor hop. Rejected proposals are made
        // eligible for a retry on the next shell-state change.
        let accepted = await controller.proposeDetectedCall(id: proposal.id)
        guard accepted else {
            callDetectionOfferLedger.releaseRejectedReservation(proposal)
            return
        }

        guard callDetectionOfferLedger.contains(proposal) else {
            await controller.dismissProposal(
                expectedDetectedProposalID: proposal.id
            )
            return
        }
        detectedCallProposal = proposal
    }

    private func suppressActiveDetectedCalls() {
        callDetectionOfferLedger.suppressAllActive()
    }

    private func captureLifecycleOwnsCurrentCalls(
        _ state: SessionShellState
    ) -> Bool {
        state == .preparing
            || state == .recording
            || state == .paused
            || state == .finalizing
    }
}
