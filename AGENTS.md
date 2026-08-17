# Repository Guidelines

## Project Structure & Module Organization

LocalScribe is a macOS 14+ menu-bar app with a portable C++20 core. Keep platform-neutral logic in `Core/src/`; its public C ABI lives in `Core/include/LocalScribeCore/`. Swift UI, capture, storage, bridge, and session code are grouped by responsibility under `macOS/`. C++ tests and golden fixtures belong in `Core/tests/`; XCTest coverage belongs in `Tests/Swift/`. Build helpers are in `Scripts/`, diagnostic executables in `Tools/`, app metadata in `Config/`, architecture decisions in `Docs/`, and the pinned whisper.cpp binary in `Vendor/`. Do not commit generated `.build/`, `Build/`, or `dist/` artifacts.

## Build, Test, and Development Commands

The root `Makefile` wraps every script in `Scripts/`. Run `make help` for the
complete command list and see `Docs/Development.ru.md` for arguments and
examples. The main entry points are:

- `make build` creates the optimized release app bundle; use `make build-debug` for a debug bundle.
- `make test` runs the Core tests, standalone Swift checks, and Swift XCTest; full Xcode is required for XCTest.
- `make verify` runs the required complete local gate before review.
- `make soak-smoke`, `make soak-accelerated`, and `make soak` run the short, accelerated, and two-hour stability probes.
- `make whisper-smoke MODEL=/path/to/model.bin WAV=/path/to/input.wav` validates real Whisper ASR.
- `make github-release` verifies, builds, and packages the technical GitHub pre-release; overwrite and skip-verification variants are listed by `make help`.

The underlying scripts remain available directly:

- `Scripts/build-app-bundle.sh debug` builds and ad-hoc signs `Build/LocalScribe.app`; use `release` for an optimized local bundle.
- `Scripts/run-core-tests.sh` compiles and runs the portable C++ contract suite.
- `Scripts/run-swift-tests.sh` runs XCTest and requires a full Xcode installation.
- `Scripts/run-core-soak.sh --smoke` performs a short deterministic stability check.
- `Scripts/verify-mvp.sh` runs the complete local gate: scope checks, C++ and Swift checks, soak smoke, tests when available, and a debug bundle build.

Use `LOCALSCRIBE_MODEL_PATH` and `LOCALSCRIBE_WAV_PATH` with `Scripts/verify-mvp.sh` to include real Whisper ASR validation.

## Coding Style & Naming Conventions

Match existing formatting: four-space indentation, braces on the next line in C++, and standard Swift API layout with trailing commas in multiline literals. Use `UpperCamelCase` for types, `lowerCamelCase` for Swift members, and existing `LS_`/`ls_` conventions for the C ABI. Name files after their primary type or responsibility. Keep third-party headers out of public interfaces and Apple frameworks out of `Core/`. No repository formatter is configured, so preserve nearby style and keep changes focused.

## Testing Guidelines

Add behavior-focused tests with every change. C++ tests use `LS_TEST(snake_case_name)` in `Core/tests/*Tests.cpp`; Swift tests use XCTest methods named `testExpectedBehavior`. Update `Core/tests/fixtures/expected-session.md` only when a deliberate Markdown contract change is reviewed. There is no numeric coverage threshold; `Scripts/verify-mvp.sh` is the required pre-review gate.

## Commit & Pull Request Guidelines

Recent history favors concise imperative Conventional Commit subjects, for example `feat: add persistent voice profiles`. Use a suitable prefix such as `feat:`, `fix:`, `test:`, or `docs:` and keep each commit scoped. Pull requests should explain the user-visible effect, note privacy or recovery implications, link the relevant issue or ADR, list verification performed, and include screenshots for SwiftUI/AppKit changes.

## Security & Product Boundaries

Preserve explicit recording consent, local-only transcription, and the no-network capture-to-Markdown pipeline. Never commit models, recordings, databases, credentials, or user transcripts. Run `Scripts/verify-scope.sh` after changing dependencies or platform boundaries.
