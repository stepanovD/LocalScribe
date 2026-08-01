import AVFoundation
import CoreGraphics
import Foundation

enum PermissionState: String, Sendable {
    case authorized
    case denied
    case notDetermined
    case restartRequired
}

struct CapturePermissionSnapshot: Sendable, Equatable {
    let microphone: PermissionState
    let screenAndSystemAudio: PermissionState

    var allRequiredAuthorized: Bool {
        microphone == .authorized && screenAndSystemAudio == .authorized
    }
}

enum PermissionClientError: Error, Sendable, Equatable {
    case microphoneDenied
    case screenRecordingDenied
    case screenRecordingRestartRequired
}

protocol PermissionProviding: Sendable {
    func currentSnapshot() async -> CapturePermissionSnapshot
    func requestRequiredPermissionsAfterConsent() async throws -> CapturePermissionSnapshot
}

/// This actor is intentionally only called from the post-consent preflight.
/// Merely opening the menu or restoring application state never invokes a TCC
/// request.
actor PermissionClient: PermissionProviding {
    private var screenRequestWasDenied = false
    private var screenRestartRequired = false

    func currentSnapshot() -> CapturePermissionSnapshot {
        CapturePermissionSnapshot(
            microphone: Self.microphoneState(),
            screenAndSystemAudio: CGPreflightScreenCaptureAccess()
                ? .authorized
                : (
                    screenRestartRequired
                        ? .restartRequired
                        : (screenRequestWasDenied ? .denied : .notDetermined)
                )
        )
    }

    func requestRequiredPermissionsAfterConsent() async throws -> CapturePermissionSnapshot {
        let microphoneGranted: Bool
        switch Self.microphoneState() {
        case .authorized:
            microphoneGranted = true
        case .denied, .restartRequired:
            microphoneGranted = false
        case .notDetermined:
            microphoneGranted = await AVCaptureDevice.requestAccess(for: .audio)
        }

        guard microphoneGranted else {
            throw PermissionClientError.microphoneDenied
        }

        if CGPreflightScreenCaptureAccess() {
            screenRequestWasDenied = false
            screenRestartRequired = false
            return CapturePermissionSnapshot(
                microphone: .authorized,
                screenAndSystemAudio: .authorized
            )
        }

        guard CGRequestScreenCaptureAccess() else {
            screenRequestWasDenied = true
            screenRestartRequired = false
            throw PermissionClientError.screenRecordingDenied
        }
        screenRequestWasDenied = false
        if CGPreflightScreenCaptureAccess() {
            screenRestartRequired = false
            return CapturePermissionSnapshot(
                microphone: .authorized,
                screenAndSystemAudio: .authorized
            )
        }
        screenRestartRequired = true
        throw PermissionClientError.screenRecordingRestartRequired
    }

    private static func microphoneState() -> PermissionState {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            .authorized
        case .denied, .restricted:
            .denied
        case .notDetermined:
            .notDetermined
        @unknown default:
            .denied
        }
    }
}
