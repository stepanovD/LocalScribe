#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$repo_root/Scripts/verify-scope.sh"
"$repo_root/Scripts/run-core-tests.sh"
"$repo_root/Scripts/run-swift-checks.sh"
if xcrun --find xctest >/dev/null 2>&1; then
    "$repo_root/Scripts/run-swift-tests.sh"
elif [[ "${LOCALSCRIBE_REQUIRE_SWIFT_TESTS:-0}" == "1" ]]; then
    echo "verify-mvp: full Xcode/XCTest is required but unavailable" >&2
    exit 1
else
    echo "verify-mvp: Swift XCTest skipped (install/select full Xcode to enable)"
fi
"$repo_root/Scripts/run-core-soak.sh" --smoke

if [[ -n "${LOCALSCRIBE_MODEL_PATH:-}" \
      && -n "${LOCALSCRIBE_WAV_PATH:-}" ]]; then
    "$repo_root/Scripts/run-whisper-smoke.sh" \
        "$LOCALSCRIBE_MODEL_PATH" \
        "$LOCALSCRIBE_WAV_PATH"
fi

"$repo_root/Scripts/build-app-bundle.sh" debug
