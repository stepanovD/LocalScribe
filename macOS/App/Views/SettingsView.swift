import SwiftUI

struct SettingsView: View {
    @ObservedObject var model: AppModel

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
        .frame(width: 520, height: 390)
        .task {
            await model.refreshSetupState()
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
}
