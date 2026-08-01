#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$repo_root/Scripts/verify-scope.sh"
"$repo_root/Scripts/run-core-tests.sh"
"$repo_root/Scripts/run-swift-checks.sh"
"$repo_root/Scripts/run-core-soak.sh" --smoke

if [[ -n "${LOCALSCRIBE_MODEL_PATH:-}" \
      && -n "${LOCALSCRIBE_WAV_PATH:-}" ]]; then
    "$repo_root/Scripts/run-whisper-smoke.sh" \
        "$LOCALSCRIBE_MODEL_PATH" \
        "$LOCALSCRIBE_WAV_PATH"
fi

"$repo_root/Scripts/build-app-bundle.sh" debug
