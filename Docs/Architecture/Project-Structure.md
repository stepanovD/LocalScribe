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
│       ├── AsrTimelineBatchTests.cpp
│       ├── BackendTests.cpp
│       ├── ContractTests.cpp
│       ├── DiarizationInertiaTests.cpp
│       ├── JournalRecoveryTests.cpp
│       ├── MarkdownGoldenTests.cpp
│       ├── PendingJournalTests.cpp
│       ├── SpeakerSwitchContractTests.cpp
│       ├── StateMachineTests.cpp
│       ├── VoiceProfileTests.cpp
│       └── fixtures/
├── macOS/
│   ├── App/
│   │   ├── LocalScribeApp.swift
│   │   ├── ApplicationDelegate.swift
│   │   ├── DetectedCallPromptWindowController.swift
│   │   ├── DetectedCallProposal.swift
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
│       ├── CallDetectionModel.swift
│       ├── SystemCallDetectionMonitor.swift
│       └── PermissionClient.swift
├── Tests/Swift/
│   ├── CallDetectionTests.swift
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
│   ├── run-swift-tests.sh
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
- emits ordered `AsrTimelineBatch` values only for fully decoded per-source
  intervals, including empty decoded-silence batches and discontinuity markers;
- expresses diarization output as atomic `DiarizationUpdate` operations:
  per-hypothesis `commit`/`hold` decisions and whole-group `resolve` results;
- owns the in-memory acoustic evidence and five-second decoded System Audio
  deadline, but does not own persistence or publication;
- seeds each diarization session with compatible saved voice profiles and keeps
  incompatible or low-confidence observations anonymous.

### `Core/session`

- owns the canonical transition table;
- consumes each returned ASR batch sequence in order and passes its per-source
  decoded watermark to diarization after the batch's hypotheses;
- orders source events, transcript revisions, and diarization group operations;
- decides the terminal completeness result;
- routes `commit` directly to visible persistence and `hold` to hidden durable
  staging, then atomically promotes every revision named by `resolve`;
- flushes pending diarization with an explicit pause or end-of-stream reason and
  forbids paused/terminal boundaries while a group remains unresolved;
- makes every visible Final Segment durable before publishing its event.

### `Core/storage`

- owns SQLite migrations and transactions;
- persists session phase, sources, discontinuities, visible Final Segment
  revisions, render checkpoint, publication receipts, and bounded versioned
  voice-profile descriptors;
- owns SQLite schema version 3 hidden pending groups with complete Final Segment
  payloads, descriptors, fallback attribution, confidence, and deadline, but
  never persists the speculative target identity;
- advances the journal checkpoint when a group is staged without advancing the
  highest visible revision or timeline origin, and atomically promotes a whole
  group under one later checkpoint;
- fallback-promotes every unresolved durable group before marking a crashed
  session `recovery_required`, so final text survives without reconstructing
  speculative acoustic state;
- atomically enrolls a named profile and relabels the selected session without
  retaining raw audio or reading hidden pending descriptors;
- discovers nonterminal sessions after a crash;
- contains no vault or security-scoped URL logic.

### `Core/output`

- renders deterministic UTF-8 Markdown/JSON values from the visible rows of
  immutable journal snapshots;
- excludes hidden pending text, descriptors, and fallback labels until atomic
  promotion;
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
- retains an explicit current/last-call review context for saving an anonymous
  speaker, then republishes the relabeled durable snapshot;
- exposes profile list, rename, and delete operations to Settings without
  putting descriptor bytes into Swift UI values;
- retries a system stream that reports ready and then fails during restart,
  using sticky per-source mailbox health and identity-tagged workers;
- never starts a source from a detection-only event.

### `macOS/System` call detection

- samples process audio-input state and Window Server metadata without opening
  an audio tap or requesting a permission;
- reduces raw window titles to Zoom/Telemost/Google Meet signatures before
  emitting an event and never persists or logs them;
- debounces call beginnings and endings into identity-scoped episodes;
- treats an unavailable system snapshot as unknown, never as evidence that a
  call ended;
- applies a conservative provider-level absence/countdown/snooze reducer only
  to the session associated with the exact proposal UUID accepted by the user;
- can request finalization only after a visible countdown; it never starts
  capture and cannot stop a manual or differently identified session.

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
- only visible, promoted Final Segments reach renderer golden files;
- hidden pending groups cannot affect public events, committed-segment metrics,
  participants, Voice Profile enrollment, or Markdown;
- app network APIs are absent from the main product target;
- content-like fields are absent from diagnostics.

The most important proof is not directory naming: the standalone portable core
test executable must compile, link, and pass without any macOS framework.

## Test ownership

| Behavior | Primary test surface |
|---|---|
| State transitions/invariants | portable C++ unit tests |
| SQLite migrations/recovery | portable C++ tests with temporary DB |
| ASR decoded watermarks, empty batches, and discontinuity ordering | portable C++ backend/session tests |
| Diarization inertia and `commit`/`hold`/`resolve` semantics | portable C++ backend tests |
| Hidden staging, atomic promotion, flush boundaries, and recovery fallback | portable C++ storage/session/ABI tests |
| Voice-profile CRUD, format compatibility, and cross-session matching | portable C++ storage/backend/ABI tests |
| Markdown escaping/determinism | portable golden tests |
| ABI sizes/ownership/errors | C contract tests + Swift bridge tests |
| Explicit enrollment and Settings management | Swift tests/checks + signed-app manual test |
| Security-scoped bookmarks | Swift tests and signed-app manual test |
| External-edit conflict | Swift storage tests |
| Capture timestamps/formats | adapter contract tests |
| Zoom/Telemost/Google Meet/Skype matching, debounce, and proposal-bound provider auto-stop | Swift tests/checks + signed-app manual matrix |
| TCC/device changes | signed-app manual matrix |
| Two-hour durability | `LocalScribeSoak` + signed-app run |
