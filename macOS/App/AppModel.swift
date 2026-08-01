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

    private let directoryStore: SecurityScopedDirectoryStore
    private let modelStore: SecurityScopedModelStore
    private let controller: SessionController?
    private let consentIssuer = VisibleConsentIssuer()
    private var updatesTask: Task<Void, Never>?
    private var voiceProfileRefreshGeneration = LatestRequestGeneration()

    init() {
        let directoryStore = SecurityScopedDirectoryStore()
        let modelStore = SecurityScopedModelStore()
        self.directoryStore = directoryStore
        self.modelStore = modelStore

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
        Task {
            await controller?.recoverInterruptedSessions()
            await refreshSetupState()
        }
    }

    deinit {
        updatesTask?.cancel()
    }

    var menuBarSymbol: String {
        switch session.state {
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
        guard let controller else {
            setupFailure = .coreUnavailable
            return
        }

        let now = Date()
        let filenameFormatter = DateFormatter()
        filenameFormatter.locale = Locale(identifier: "en_US_POSIX")
        filenameFormatter.dateFormat = "yyyy-MM-dd HH-mm"
        let titleFormatter = DateFormatter()
        titleFormatter.locale = .current
        titleFormatter.dateStyle = .medium
        titleFormatter.timeStyle = .short

        let token = consentIssuer.issue(for: .start)
        let request = SessionStartRequest(
            sourceApplication: "Manual",
            title: "Call \(titleFormatter.string(from: now))",
            preferredFilenameStem:
                "\(filenameFormatter.string(from: now)) — Call",
            localSpeakerName: "Me",
            languageMode: .russianEnglish
        )
        Task {
            await controller.start(after: token, request: request)
            await refreshSetupState()
        }
    }

    func dismissStart() {
        guard let controller else {
            return
        }
        Task { await controller.dismissProposal() }
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
                guard !Task.isCancelled else {
                    return
                }
                self?.session = value
            }
        }
    }
}
