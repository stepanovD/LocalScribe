import Foundation
import SwiftUI

struct SettingsView: View {
    @ObservedObject var model: AppModel
    @State private var profileDraftNames: [UInt64: String] = [:]
    @State private var profilePendingDeletion: CoreVoiceProfile?

    var body: some View {
        Form {
            Section("Obsidian output") {
                HStack {
                    Label(
                        model.hasVaultSelection ? "Folder selected" : "No folder selected",
                        systemImage: "folder"
                    )
                    Spacer()
                    Button("Choose…") {
                        model.chooseVaultDirectory()
                    }
                }
                Text(
                    "LocalScribe writes only safe Markdown snapshots to the folder you select."
                )
                .font(.caption)
                .foregroundStyle(.secondary)
            }

            Section("Offline transcription") {
                HStack {
                    Label(
                        model.hasModelSelection ? "Local model selected" : "No model selected",
                        systemImage: "internaldrive"
                    )
                    Spacer()
                    Button("Choose…") {
                        model.chooseLocalModel()
                    }
                }
                Text(
                    "Choose an existing whisper.cpp ggml-*.bin model. Larger models may start more slowly and use substantially more memory. It is never downloaded during a call."
                )
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Picker(
                    "Default meeting language",
                    selection: $model.defaultLanguageMode
                ) {
                    ForEach(
                        CoreLanguageMode.selectableCases,
                        id: \.rawValue
                    ) { mode in
                        Text(mode.displayName).tag(mode)
                    }
                }
                Text(
                    "This language is preselected for new meetings. You can change it before starting an individual recording."
                )
                .font(.caption)
                .foregroundStyle(.secondary)
            }

            Section("Voice profiles") {
                if model.voiceProfiles.isEmpty {
                    Text(
                        "No saved voices yet. During or after a transcript, name an anonymous speaker to recognize them in future calls."
                    )
                    .foregroundStyle(.secondary)
                } else {
                    ForEach(model.voiceProfiles) { profile in
                        HStack {
                            TextField(
                                "Speaker name",
                                text: profileNameBinding(for: profile)
                            )
                            .accessibilityLabel(
                                "Name for voice profile \(profile.displayName)"
                            )
                            Text(
                                "\(profile.sampleCount) "
                                    + (profile.sampleCount == 1 ? "sample" : "samples")
                            )
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            Button("Save") {
                                model.renameVoiceProfile(
                                    profile,
                                    to: profileDraftNames[profile.profileID]
                                        ?? profile.displayName
                                )
                            }
                            .disabled(
                                model.isVoiceProfileOperationInProgress
                                    || !profileNameHasChanges(profile)
                            )
                            Button(role: .destructive) {
                                profilePendingDeletion = profile
                            } label: {
                                Image(systemName: "trash")
                            }
                            .disabled(model.isVoiceProfileOperationInProgress)
                            .accessibilityLabel(
                                "Delete voice profile \(profile.displayName)"
                            )
                        }
                    }
                }
                Text(
                    "Acoustic voice signatures stay in LocalScribe's recovery storage. Deleting a profile stops future matching, but does not purge acoustic evidence from historical calls or rewrite existing transcripts."
                )
                .font(.caption)
                .foregroundStyle(.secondary)
                if let failure = model.voiceProfileFailure {
                    Label(
                        failure,
                        systemImage: "person.crop.circle.badge.exclamationmark"
                    )
                    .font(.caption)
                    .foregroundStyle(.orange)
                }
            }

            Section("Capture permissions") {
                permissionRow("Microphone", state: model.permissions.microphone)
                permissionRow(
                    "Screen & system audio",
                    state: model.permissions.screenAndSystemAudio
                )
                Text(
                    "Permission prompts appear only after you explicitly press Start Recording."
                )
                .font(.caption)
                .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(width: 580, height: 520)
        .task {
            await model.refreshSetupState()
        }
        .alert(item: $profilePendingDeletion) { profile in
            Alert(
                title: Text("Delete \(profile.displayName)?"),
                message: Text(
                    "LocalScribe will stop matching this saved voice in future calls. Existing transcripts keep their current speaker names, and acoustic evidence retained with historical calls is not purged."
                ),
                primaryButton: .destructive(Text("Delete")) {
                    model.deleteVoiceProfile(profile)
                },
                secondaryButton: .cancel()
            )
        }
    }

    private func permissionRow(_ title: String, state: PermissionState) -> some View {
        HStack {
            Text(title)
            Spacer()
            Text(permissionText(state))
                .foregroundStyle(.secondary)
        }
    }

    private func permissionText(_ state: PermissionState) -> String {
        switch state {
        case .authorized:
            "Allowed"
        case .denied:
            "Denied"
        case .notDetermined:
            "Not requested"
        case .restartRequired:
            "Restart LocalScribe to finish"
        }
    }

    private func profileNameBinding(
        for profile: CoreVoiceProfile
    ) -> Binding<String> {
        Binding(
            get: {
                profileDraftNames[profile.profileID] ?? profile.displayName
            },
            set: { profileDraftNames[profile.profileID] = $0 }
        )
    }

    private func profileNameHasChanges(_ profile: CoreVoiceProfile) -> Bool {
        let value = (profileDraftNames[profile.profileID] ?? profile.displayName)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return !value.isEmpty && value != profile.displayName
    }
}
