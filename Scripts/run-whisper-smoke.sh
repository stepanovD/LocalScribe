#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 MODEL.bin INPUT.wav" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
framework_root="$repo_root/Vendor/whisper.xcframework/macos-arm64_x86_64"
smoke_binary="/private/tmp/localscribe-whisper-smoke"

clang++ \
    -std=c++20 \
    -O0 \
    -g \
    -DLOCALSCRIBE_ENABLE_WHISPER=1 \
    -I "$repo_root/Core/include" \
    -I "$repo_root/Core/src" \
    -F "$framework_root" \
    "$repo_root/Tools/WhisperSmoke.cpp" \
    "$repo_root/Core/src/abi/LocalScribeCore.cpp" \
    "$repo_root/Core/src/inference/FixtureAsrBackend.cpp" \
    "$repo_root/Core/src/inference/SourceDiarizationBackend.cpp" \
    "$repo_root/Core/src/inference/WhisperCppBackend.cpp" \
    "$repo_root/Core/src/output/MarkdownRenderer.cpp" \
    "$repo_root/Core/src/session/SessionStateMachine.cpp" \
    "$repo_root/Core/src/storage/Migrations.cpp" \
    "$repo_root/Core/src/storage/RecoveryJournal.cpp" \
    -framework whisper \
    -lsqlite3 \
    -pthread \
    "-Wl,-rpath,$framework_root" \
    -o "$smoke_binary"

"$smoke_binary" "$1" "$2"
