#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${repo_dir}/.build/core-soak"
mkdir -p "${build_dir}"

cxx="${CXX:-clang++}"
"${cxx}" \
    -std=c++20 \
    -O2 \
    -pthread \
    -I "${repo_dir}/Core/include" \
    -I "${repo_dir}/Core/src" \
    "${repo_dir}/Core/src/abi/LocalScribeCore.cpp" \
    "${repo_dir}/Core/src/inference/FixtureAsrBackend.cpp" \
    "${repo_dir}/Core/src/inference/SourceDiarizationBackend.cpp" \
    "${repo_dir}/Core/src/inference/WhisperCppBackend.cpp" \
    "${repo_dir}/Core/src/output/MarkdownRenderer.cpp" \
    "${repo_dir}/Core/src/session/SessionStateMachine.cpp" \
    "${repo_dir}/Core/src/storage/Migrations.cpp" \
    "${repo_dir}/Core/src/storage/RecoveryJournal.cpp" \
    "${repo_dir}/Tools/CoreSoak/main.cpp" \
    -lsqlite3 \
    -o "${build_dir}/LocalScribeCoreSoak"

exec "${build_dir}/LocalScribeCoreSoak" "$@"
