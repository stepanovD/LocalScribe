#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! xcrun --find xctest >/dev/null 2>&1; then
    echo "swift XCTest gate requires a full Xcode installation" >&2
    exit 2
fi

if [[ -n "${LOCALSCRIBE_SDKROOT:-}" ]]; then
    task_sdkroot="$LOCALSCRIBE_SDKROOT"
else
    task_sdkroot="$(xcrun --sdk macosx --show-sdk-path)"
fi

test_build_root="$repo_root/.build/swift-tests"
module_cache="$repo_root/.build/localscribe-module-cache"
mkdir -p \
    "$test_build_root" \
    "$module_cache/clang" \
    "$module_cache/swiftpm"

env \
    SDKROOT="$task_sdkroot" \
    CLANG_MODULE_CACHE_PATH="$module_cache/clang" \
    SWIFTPM_MODULECACHE_OVERRIDE="$module_cache/swiftpm" \
    swift test \
        --package-path "$repo_root" \
        --scratch-path "$test_build_root" \
        --disable-sandbox \
        --sdk "$task_sdkroot"
