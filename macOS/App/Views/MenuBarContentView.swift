import Foundation
import SwiftUI

struct MenuBarContentView: View {
    @ObservedObject var model: AppModel
    @State private var speakerDraftNames: [UInt64: String] = [:]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            if let failure = model.voiceProfileFailure {
                Label(failure, systemImage: "person.crop.circle.badge.exclamationmark")
                    .font(.caption)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                    .layoutPriority(2)
            }
            Divider()
            content
            Divider()
            setupSummary
            HStack {
                SettingsLink {
                    Label("Settings", systemImage: "gear")
                }
                Spacer()
                Button("Quit") {
                    model.quit()
                }
                .keyboardShortcut("q")
            }
        }
        .padding(16)
        .frame(width: 500)
        .frame(
            minHeight: model.session.speakers.isEmpty ? 420 : 680,
            idealHeight: model.session.speakers.isEmpty ? 480 : 740,
            maxHeight: model.session.speakers.isEmpty ? 560 : 780,
            alignment: .topLeading
        )
        .task {
            await model.refreshSetupState()
        }
        .onChange(of: model.session.sessionID) { _, _ in
            speakerDraftNames.removeAll()
        }
    }

    private var header: some View {
        HStack(spacing: 9) {
            Image(systemName: model.menuBarSymbol)
                .font(.title2)
                .accessibilityHidden(true)
            VStack(alignment: .leading) {
                Text("LocalScribe")
                    .font(.headline)
                Text(statusText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if model.session.state == .recording,
               let startedAt = model.session.startedAt
            {
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    Text(elapsed(from: startedAt, to: context.date))
                        .font(.system(.body, design: .monospaced))
                        .accessibilityLabel(
                            "Recording duration \(elapsed(from: startedAt, to: context.date))"
                        )
                }
            }
        }
    }

    @ViewBuilder
    private var content: some View {
        switch model.session.state {
        case .idle, .failedToStart, .complete,
             .incompleteSources, .interrupted:
            if let failure = model.session.failureCode ?? model.setupFailure {
                Label(failureText(failure), systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                    .layoutPriority(2)
            }
            speakerProfiles
            Button {
                model.requestManualStart()
            } label: {
                Label("New transcript…", systemImage: "record.circle")
            }
            .buttonStyle(.borderedProminent)
            .disabled(!model.hasVaultSelection || !model.hasModelSelection)
            .accessibilityHint("Opens a separate consent step before capture")

        case .detected, .awaitingConsent:
            VStack(alignment: .leading, spacing: 10) {
                Text(consentTitle)
                    .font(.headline)
                Text(
                    "After you press Start Recording, LocalScribe will request access and capture both your microphone and system audio."
                )
                .font(.callout)
                .fixedSize(horizontal: false, vertical: true)
                if model.detectedCallProposal != nil {
                    Text(
                        "If this call later appears to have ended, LocalScribe will show a countdown before stopping automatically."
                    )
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                }

                HStack {
                    Text("Meeting language")
                    Spacer()
                    Picker(
                        "Meeting language",
                        selection: $model.meetingLanguageMode
                    ) {
                        ForEach(
                            CoreLanguageMode.selectableCases,
                            id: \.rawValue
                        ) { mode in
                            Text(mode.displayName).tag(mode)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                }

                HStack {
                    Button("Not Now") {
                        model.dismissStart()
                    }
                    Spacer()
                    Button("Start Recording") {
                        model.confirmVisibleStart()
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!model.canStartRecording)
                    .keyboardShortcut(.defaultAction)
                    .accessibilityHint(
                        model.detectedCallProposal == nil
                            ? "Explicitly consents to microphone and system audio capture"
                            : "Explicitly consents to capture; a confirmed call end will show a warning before stopping"
                    )
                }
                if !model.canStartRecording {
                    Text(
                        "Choose a transcript folder and local ASR model in Settings before recording."
                    )
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                }
            }

        case .preparing:
            HStack {
                ProgressView()
                    .controlSize(.small)
                Text("Checking permissions, model, and audio sources…")
            }

        case .recording:
            sourceRows
            speakerProfiles
            autoStopWarning
            if let metrics = model.session.metrics {
                Text(
                    "Processing queue: \(metrics.audioQueueDepth) / high \(metrics.audioQueueHighWater)"
                )
                .font(.caption)
                .foregroundStyle(.secondary)
            }
            HStack {
                Button {
                    model.pause()
                } label: {
                    Label("Pause", systemImage: "pause.fill")
                }
                Spacer()
                Button(role: .destructive) {
                    model.stop()
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
            }

        case .paused:
            sourceRows
            speakerProfiles
            autoStopWarning
            Text("Capture is paused. Already accepted audio is still journaled.")
                .font(.callout)
            HStack {
                Button("Resume Recording") {
                    model.resumeFromVisibleButton()
                }
                .buttonStyle(.borderedProminent)
                Spacer()
                Button("Finish") {
                    model.stop()
                }
            }

        case .finalizing:
            HStack {
                ProgressView()
                    .controlSize(.small)
                Text("Finalizing durable segments and Markdown…")
            }

        case .recoveryRequired:
            VStack(alignment: .leading, spacing: 10) {
                Label(
                    "An interrupted session needs recovery. Capture will not resume automatically.",
                    systemImage: "arrow.counterclockwise.circle"
                )
                Button("Retry Recovery") {
                    model.retryRecovery()
                }
                .buttonStyle(.borderedProminent)
            }
        }
    }

    @ViewBuilder
    private var autoStopWarning: some View {
        if let prompt = model.detectedCallAutoStopPrompt {
            VStack(alignment: .leading, spacing: 8) {
                Label(
                    "Call appears to have ended",
                    systemImage: "phone.down.fill"
                )
                .font(.headline)
                .foregroundStyle(.orange)

                TimelineView(.periodic(from: .now, by: 1)) { _ in
                    Text(
                        "Stopping recording in "
                            + "\(prompt.remainingSeconds()) seconds."
                    )
                    .font(.callout)
                    .accessibilityLabel(
                        "Stopping recording in "
                            + "\(prompt.remainingSeconds()) seconds"
                    )
                }

                HStack {
                    Button("Keep Recording") {
                        model.keepRecordingAfterCallEndWarning(
                            promptID: prompt.id
                        )
                    }
                    .buttonStyle(.borderedProminent)
                    .accessibilityHint(
                        "Cancels this countdown and keeps capture running"
                    )
                    Spacer()
                    Button("Stop Now", role: .destructive) {
                        model.stopNowAfterCallEndWarning(
                            promptID: prompt.id
                        )
                    }
                    .accessibilityHint(
                        "Stops capture and finalizes the transcript now"
                    )
                }
            }
            .padding(10)
            .background(.orange.opacity(0.10), in: RoundedRectangle(
                cornerRadius: 10,
                style: .continuous
            ))
        }
    }

    private var sourceRows: some View {
        VStack(alignment: .leading, spacing: 6) {
            sourceRow(
                title: "Microphone",
                symbol: "mic",
                state: model.session.microphone
            )
            sourceRow(
                title: "System audio",
                symbol: "speaker.wave.2",
                state: model.session.systemAudio
            )
        }
    }

    private var setupSummary: some View {
        VStack(alignment: .leading, spacing: 5) {
            Label(
                model.hasVaultSelection
                    ? "Transcript folder selected"
                    : "Choose a transcript folder in Settings",
                systemImage: model.hasVaultSelection ? "folder.fill.badge.checkmark" : "folder"
            )
            Label(
                model.hasModelSelection
                    ? "Local ASR model selected"
                    : "Choose a local ASR model in Settings",
                systemImage: model.hasModelSelection
                    ? "internaldrive.fill"
                    : "internaldrive"
            )
            if model.session.lastPublicationDestination == .staging {
                Label(
                    "Vault unavailable; exact transcript is safe in staging",
                    systemImage: "exclamationmark.arrow.triangle.2.circlepath"
                )
                .foregroundStyle(.orange)
            } else if model.session.lastPublicationDestination == .recoveryCopy {
                Label(
                    "The note changed externally; your edit was preserved and the transcript was saved as a recovery copy",
                    systemImage: "doc.on.doc.fill"
                )
                .foregroundStyle(.orange)
            }
            if canRetryPublication {
                Button {
                    model.retryLastPublication()
                } label: {
                    Label(
                        "Retry Transcript Update",
                        systemImage: "arrow.clockwise"
                    )
                }
                .accessibilityHint(
                    "Retries updating the transcript without saving the voice profile again"
                )
            }
            if model.session.lastPublishedURL != nil {
                Button {
                    model.openLastTranscript()
                } label: {
                    Label(
                        "Open Transcript",
                        systemImage: "doc.text.magnifyingglass"
                    )
                }
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
    }

    @ViewBuilder
    private var speakerProfiles: some View {
        if !model.session.speakers.isEmpty {
            VStack(alignment: .leading, spacing: 8) {
                Text("Speakers")
                    .font(.headline)
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(model.session.speakers) { speaker in
                            VStack(alignment: .leading, spacing: 7) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Label(
                                        speaker.displayName,
                                        systemImage: speaker.isAnonymous
                                            ? "person.wave.2"
                                            : "person.crop.circle.badge.checkmark"
                                    )
                                    .font(.subheadline.weight(.medium))
                                    .lineLimit(2)
                                    .fixedSize(horizontal: false, vertical: true)

                                    Text(segmentCountText(speaker.segmentCount))
                                        .font(.caption2.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                                .accessibilityElement(children: .combine)

                                if !speaker.latestExcerpt.isEmpty {
                                    Text(speaker.latestExcerpt)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(2)
                                }

                                if speaker.isAnonymous {
                                    TextField(
                                        "Name this speaker",
                                        text: speakerDraftBinding(
                                            for: speaker.speakerID
                                        )
                                    )
                                    .textFieldStyle(.roundedBorder)
                                    .frame(maxWidth: .infinity)
                                    .accessibilityLabel(
                                        "Name for \(speaker.displayName)"
                                    )

                                    HStack(spacing: 8) {
                                        if !model.voiceProfiles.isEmpty {
                                            Menu {
                                                ForEach(model.voiceProfiles) { profile in
                                                    Button(profile.displayName) {
                                                        attach(
                                                            speaker,
                                                            to: profile
                                                        )
                                                    }
                                                    .accessibilityLabel(
                                                        "Attach \(speaker.displayName) to \(profile.displayName)"
                                                    )
                                                }
                                            } label: {
                                                Label(
                                                    "Attach to Saved Profile…",
                                                    systemImage: "person.2"
                                                )
                                            }
                                            .disabled(
                                                model.isVoiceProfileOperationInProgress
                                            )
                                            .accessibilityLabel(
                                                "Attach \(speaker.displayName) to a saved profile"
                                            )
                                            .accessibilityHint(
                                                "Adds this speaker's voice samples to an existing saved profile"
                                            )
                                        }

                                        Spacer(minLength: 8)

                                        Button("Save Name") {
                                            saveVoiceProfile(for: speaker)
                                        }
                                        .disabled(!canSaveVoiceProfile(for: speaker))
                                        .accessibilityLabel(
                                            "Save name for \(speaker.displayName)"
                                        )
                                    }
                                } else {
                                    Text("Recognized from a saved voice profile")
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                }
                            }
                            .padding(7)
                            .background(
                                .quaternary,
                                in: RoundedRectangle(cornerRadius: 7)
                            )
                        }
                    }
                }
                .frame(minHeight: 260, idealHeight: 340, maxHeight: 420)
                Text(
                    "Saved voice profiles stay on this Mac and are used for future transcripts."
                )
                .font(.caption2)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func speakerDraftBinding(for speakerID: UInt64) -> Binding<String> {
        Binding(
            get: { speakerDraftNames[speakerID] ?? "" },
            set: { speakerDraftNames[speakerID] = $0 }
        )
    }

    private func segmentCountText(_ segmentCount: Int) -> String {
        "\(segmentCount) \(segmentCount == 1 ? "segment" : "segments")"
    }

    private func canSaveVoiceProfile(
        for speaker: SessionSpeakerSnapshot
    ) -> Bool {
        !model.isVoiceProfileOperationInProgress
            && speakerDraftNames[speaker.speakerID]?.trimmingCharacters(
                in: .whitespacesAndNewlines
            ).isEmpty == false
    }

    private func saveVoiceProfile(for speaker: SessionSpeakerSnapshot) {
        model.saveVoiceProfile(
            sessionID: speaker.sessionID,
            speakerID: speaker.speakerID,
            displayName: speakerDraftNames[speaker.speakerID] ?? ""
        )
    }

    private func attach(
        _ speaker: SessionSpeakerSnapshot,
        to profile: CoreVoiceProfile
    ) {
        speakerDraftNames[speaker.speakerID] = profile.displayName
        model.saveVoiceProfile(
            sessionID: speaker.sessionID,
            speakerID: speaker.speakerID,
            displayName: profile.displayName
        )
    }

    private func sourceRow(
        title: String,
        symbol: String,
        state: SourceDisplayState
    ) -> some View {
        HStack {
            Label(title, systemImage: symbol)
            Spacer()
            Text(sourceStateText(state))
                .foregroundStyle(state == .unavailable || state == .lost ? .orange : .secondary)
        }
        .accessibilityElement(children: .combine)
    }

    private var statusText: String {
        if model.detectedCallAutoStopPrompt != nil {
            return "Call appears to have ended"
        }
        return switch model.session.state {
        case .idle:
            "Ready — no capture"
        case .detected:
            "Possible call — no capture"
        case .awaitingConsent:
            "Waiting for your consent — no capture"
        case .preparing:
            "Preparing after consent"
        case .recording:
            "Recording microphone and system audio"
        case .paused:
            "Paused — no new audio"
        case .finalizing:
            "Finalizing — capture stopped"
        case .recoveryRequired:
            "Recovery required — no capture"
        case .failedToStart:
            "Could not start — no capture"
        case .complete:
            "Transcript complete"
        case .incompleteSources:
            "Transcript saved with source gaps"
        case .interrupted:
            "Interrupted transcript preserved"
        }
    }

    private var consentTitle: String {
        if let proposal = model.detectedCallProposal {
            return "\(proposal.applicationName) call detected. Start recording?"
        }
        return "Create a local transcript?"
    }

    private var canRetryPublication: Bool {
        guard model.session.failureCode == .publicationUnavailable
                || model.session.failureCode == .coreUnavailable
        else {
            return false
        }
        switch model.session.state {
        case .complete, .incompleteSources, .interrupted:
            return true
        default:
            return false
        }
    }

    private func failureText(_ failure: SessionFailureCode) -> String {
        switch failure {
        case .vaultNotSelected:
            "Choose an Obsidian transcript folder first."
        case .modelNotSelected, .modelUnavailable:
            "Choose a local whisper.cpp ggml-*.bin model."
        case .permissionsDenied:
            "Microphone and Screen Recording access are required."
        case .screenPermissionRestartRequired:
            "Screen Recording access was granted. Quit and reopen LocalScribe before starting."
        case .captureUnavailable:
            "One of the required audio sources is unavailable."
        case .publicationUnavailable:
            "The transcript update could not be written to the selected folder or local safety storage."
        case .coreUnavailable:
            if canRetryPublication {
                "The transcript update could not be rendered or confirmed. Its journal and saved voice profiles remain on this Mac."
            } else {
                "The local transcription engine is unavailable."
            }
        case .consentExpired:
            "Consent expired. Please start again."
        case .consentAlreadyUsed, .invalidTransition, .internalFailure:
            "The operation could not be completed safely."
        }
    }

    private func sourceStateText(_ state: SourceDisplayState) -> String {
        switch state {
        case .unknown:
            "Unknown"
        case .ready:
            "Ready"
        case .active:
            "Active"
        case .unavailable:
            "Unavailable"
        case .lost:
            "Lost"
        }
    }

    private func elapsed(from start: Date, to end: Date) -> String {
        let total = max(0, Int(end.timeIntervalSince(start)))
        return String(format: "%02d:%02d:%02d", total / 3_600, total / 60 % 60, total % 60)
    }
}
