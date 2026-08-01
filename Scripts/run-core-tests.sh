#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="/private/tmp/localscribe-core-tests"

clang++ \
    -std=c++20 \
    -O0 \
    -g \
    -I "$repo_root/Core/include" \
    -I "$repo_root/Core/src" \
    -DLOCALSCRIBE_TEST_SOURCE_DIR="\"$repo_root\"" \
    "$repo_root/Core/src/abi/LocalScribeCore.cpp" \
    "$repo_root/Core/src/inference/FixtureAsrBackend.cpp" \
    "$repo_root/Core/src/inference/SourceDiarizationBackend.cpp" \
    "$repo_root/Core/src/output/MarkdownRenderer.cpp" \
    "$repo_root/Core/src/session/SessionStateMachine.cpp" \
    "$repo_root/Core/src/storage/Migrations.cpp" \
    "$repo_root/Core/src/storage/RecoveryJournal.cpp" \
    "$repo_root/Core/tests/TestMain.cpp" \
    "$repo_root/Core/tests/BackendTests.cpp" \
    "$repo_root/Core/tests/ContractTests.cpp" \
    "$repo_root/Core/tests/JournalRecoveryTests.cpp" \
    "$repo_root/Core/tests/MarkdownGoldenTests.cpp" \
    "$repo_root/Core/tests/StateMachineTests.cpp" \
    "$repo_root/Core/tests/VoiceProfileTests.cpp" \
    -lsqlite3 \
    -pthread \
    -o "$test_binary"

"$test_binary"
