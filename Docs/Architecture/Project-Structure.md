# Project structure

## Repository layout

```text
LocalScribe/
├── Package.swift
├── CMakeLists.txt
├── README.md
├── Config/
│   ├── LocalScribe.entitlements
│   └── Info.plist
├── Core/
│   ├── include/
│   │   └── LocalScribeCore/
│   │       └── LocalScribeCore.h
│   ├── src/
│   │   ├── abi/
│   │   ├── common/
│   │   ├── inference/
│   │   │   ├── AsrBackend.hpp
│   │   │   ├── DiarizationBackend.hpp
│   │   │   ├── FixtureAsrBackend.cpp
│   │   │   ├── SourceDiarizationBackend.cpp
│   │   │   ├── WhisperStreamingResampler.hpp
│   │   │   └── WhisperCppBackend.cpp
│   │   ├── output/
│   │   │   └── MarkdownRenderer.cpp
│   │   ├── session/
│   │   │   └── SessionStateMachine.cpp
│   │   └── storage/
│   │       ├── Migrations.cpp
│   │       └── RecoveryJournal.cpp
│   └── tests/
│       ├── BackendTests.cpp
│       ├── ContractTests.cpp
│       ├── JournalRecoveryTests.cpp
│       ├── MarkdownGoldenTests.cpp
│       ├── StateMachineTests.cpp
│       └── fixtures/
├── macOS/
│   ├── App/
│   │   ├── LocalScribeApp.swift
│   │   ├── ApplicationDelegate.swift
│   │   ├── TerminationRequestCoordinator.swift
│   │   ├── AppModel.swift
│   │   └── Views/
│   ├── Bridge/
│   │   ├── CoreClient.swift
│   │   └── CoreValues.swift
│   ├── Capture/
│   │   ├── AudioCaptureSource.swift
│   │   ├── MicrophoneCaptureAdapter.swift
│   │   └── ScreenCaptureKitAdapter.swift
│   ├── Session/
│   │   └── SessionController.swift
│   ├── Storage/
│   │   ├── SecurityScopedDirectoryStore.swift
│   │   ├── StagingDirectory.swift
│   │   └── VaultWriter.swift
│   └── System/
│       └── PermissionClient.swift
├── Tests/Swift/
│   ├── CaptureContractTests.swift
│   ├── RecoveryStartupTests.swift
│   └── StorageSafetyTests.swift
├── Vendor/
│   └── whisper.xcframework/          # macOS arm64 + x86_64 only
├── Tools/
│   ├── CoreSoak/
│   ├── SwiftChecks/
│   │   ├── LifecycleCheckSupport.swift
│   │   ├── RecoveryCheckSupport.swift
│   │   └── main.swift
│   └── WhisperSmoke.cpp
├── Scripts/
│   ├── build-app-bundle.sh
│   ├── run-core-tests.sh
│   ├── run-core-soak.sh
│   ├── run-swift-checks.sh
│   ├── run-whisper-smoke.sh
│   ├── verify-mvp.sh
│   └── verify-scope.sh
└── Docs/
    ├── Architecture/
    └── Testing/
```

Only directories needed by implemented slices are added. Empty architecture
placeholders are avoided.

## Target graph

```text
LocalScribeApp (macOS executable)
├── SwiftUI / AppKit
├── ScreenCaptureKit
├── AVFoundation
├── CLocalScribeCore
└── whisper.xcframework (macOS slice only)

CLocalScribeCore (C ABI + C++20 implementation)
├── SQLite3
└── backend-neutral inference adapters

LocalScribeCoreTests (portable C++ executable)
└── CLocalScribeCore

LocalScribeCoreSoak (portable command-line executable)
└── CLocalScribeCore
```

The graph has no edge from the core to `LocalScribeMacOS`.

## Module responsibilities

### Core audio path

- validates formats and sequence continuity;
- copies into bounded source queues;
- maps clocks onto a shared monotonic timeline;
- records discontinuities and backpressure;
- never performs filesystem or UI work.

For the vertical this code lives in the ABI/session runtime; it remains
backend-neutral and has no Apple dependency.

### `Core/inference`

- defines LocalScribe-owned ASR/diarization value types and interfaces;
- contains backend factories;
- keeps third-party headers out of public include directories;
- produces partial hypotheses for UI and stable final revisions for the session.

### `Core/session`

- owns the canonical transition table;
- orders source events and transcript revisions;
- decides the terminal completeness result;
- makes a final segment durable before publishing its event.

### `Core/storage`

- owns SQLite migrations and transactions;
- persists session phase, sources, discontinuities, final segment revisions,
  render checkpoint, and publication receipts;
- discovers nonterminal sessions after a crash;
- contains no vault or security-scoped URL logic.

### `Core/output`

- renders deterministic UTF-8 Markdown/JSON values from immutable journal
  snapshots;
- owns schema versions, YAML escaping, marker neutralization, and golden files;
- does not write to a filesystem.

### `macOS/Capture`

- translates ScreenCaptureKit and AVFoundation buffers to the ABI audio format;
- retains independent source IDs, timestamps, levels, and device status;
- does bounded, nonblocking audio handoff only;
- coalesces pressure notifications while retaining source-health transitions,
  including failures that arrive during capture startup;
- rebinds the microphone after route/configuration loss and exposes system-audio
  loss for bounded controller-managed restart.

### `macOS/Storage`

- stores and resolves a security-scoped bookmark;
- refreshes stale bookmarks only while the resolved security scope is active;
- validates the selected folder and filename component;
- publishes with directory-bound, no-follow atomic swap/no-clobber operations;
- detects external edits by exact merge-input bytes and cryptographic digest,
  preserving racing editor saves as visible artifacts plus a LocalScribe
  recovery copy;
- stages safely when the vault is unavailable;
- never invokes ASR or mutates core state except with a publication receipt.

### `macOS/Session`

- serializes consent, preflight, capture, core, writer, and power events in one
  actor;
- parks audio routing until both required adapters are ready, while slow native
  model/drain/finalization calls run outside the actor;
- gives Direct Quit a fixed capture/finalization deadline by detaching native
  handle ownership before teardown; an unacknowledged result remains durable
  recovery work;
- treats required-source failure during `preparing` as failed preflight instead
  of declaring a dead adapter active;
- keeps UI responsive;
- makes source loss and recovery visible;
- retries a system stream that reports ready and then fails during restart,
  using sticky per-source mailbox health and identity-tagged workers;
- never starts a source from a detection-only event.

### `Tools`

- provides deterministic fixtures and fault injection;
- does not become a second product UI;
- is not installed into the user’s vault.

## Build rules

1. `Package.swift` declares `.macOS(.v14)` only.
2. The core target uses C++20 and links SQLite; it declares no Apple framework.
3. CMake can build the core and portable tests with
   `CMAKE_CXX_STANDARD=20`.
4. Third-party inference sources, if vendored, live under `Vendor/` and are
   consumed only by private adapter targets.
5. Model files are not committed to source control and never fetched during a
   recording.
6. Entitlements include microphone input, user-selected read/write access, and
   app-scoped security bookmarks; `Info.plist` carries the microphone, system
   audio, and screen-capture purpose strings required by the signed app.
7. CI/release checks fail if a mobile platform, external LLM SDK, analytics SDK,
   network client, summary feature, or cloud dependency appears.

## Dependency enforcement

The repository checks:

- no `Core/` source imports an Apple framework;
- no public core header exposes C++ or inference types;
- no mobile platform appears in manifest/project product declarations;
- only final segments reach renderer golden files;
- app network APIs are absent from the main product target;
- content-like fields are absent from diagnostics.

The most important proof is not directory naming: the standalone portable core
test executable must compile, link, and pass without any macOS framework.

## Test ownership

| Behavior | Primary test surface |
|---|---|
| State transitions/invariants | portable C++ unit tests |
| SQLite migrations/recovery | portable C++ tests with temporary DB |
| Markdown escaping/determinism | portable golden tests |
| ABI sizes/ownership/errors | C contract tests + Swift bridge tests |
| Security-scoped bookmarks | Swift tests and signed-app manual test |
| External-edit conflict | Swift storage tests |
| Capture timestamps/formats | adapter contract tests |
| TCC/device changes | signed-app manual matrix |
| Two-hour durability | `LocalScribeSoak` + signed-app run |
