#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd -P)"
check_build_root="$(mktemp -d /private/tmp/localscribe-swift-checks.XXXXXX)"
if [[ -n "${LOCALSCRIBE_SDKROOT:-}" ]]; then
    sdk_path="$LOCALSCRIBE_SDKROOT"
elif [[ -d /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk ]]; then
    sdk_path="/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk"
else
    sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
fi

swiftc \
    -sdk "$sdk_path" \
    -target arm64-apple-macosx14.0 \
    -module-cache-path "$check_build_root/module-cache" \
    -parse-as-library \
    "$project_root/macOS/Capture/AudioCaptureSource.swift" \
    "$project_root/macOS/Capture/MicrophoneCaptureAdapter.swift" \
    "$project_root/macOS/Capture/ScreenCaptureKitAdapter.swift" \
    "$project_root/macOS/Bridge/CoreValues.swift" \
    "$project_root/macOS/System/PermissionClient.swift" \
    "$project_root/macOS/Storage/SecurityScopedDirectoryStore.swift" \
    "$project_root/macOS/Storage/StagingDirectory.swift" \
    "$project_root/macOS/Storage/VaultWriter.swift" \
    "$project_root/macOS/App/TerminationRequestCoordinator.swift" \
    "$project_root/macOS/Session/SessionController.swift" \
    "$project_root/Tools/SwiftChecks/RecoveryCheckSupport.swift" \
    "$project_root/Tools/SwiftChecks/LifecycleCheckSupport.swift" \
    "$project_root/Tools/SwiftChecks/main.swift" \
    -framework AVFoundation \
    -framework AudioToolbox \
    -framework CoreGraphics \
    -framework CoreMedia \
    -framework ScreenCaptureKit \
    -o "$check_build_root/localscribe-swift-checks"

"$check_build_root/localscribe-swift-checks"
