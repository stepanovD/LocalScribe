# ADR-0001: macOS technical vertical

- **Status:** Accepted
- **Date:** 2026-07-29
- **Product source:** LocalScribe Product Reference / PRD v0.4, 2026-07-27
- **Decision owners:** LocalScribe technical vertical

## Context

LocalScribe must prove one end-to-end macOS path:

`explicit consent → microphone + system audio → local inference → stable final segments → SQLite recovery journal → safe Markdown publication`

The core must remain portable, while capture, permissions, security-scoped
directory access, Keychain, power lifecycle, notifications, and UI are native
macOS concerns. The first gate is a two-hour session with no lost final
segments, no UI stall, and valid recoverable Markdown.

The Product Reference leaves several implementation choices open. This ADR
closes only choices required by the technical vertical.

## Decisions

### 1. Product and deployment scope

- Build one macOS application and portable command-line/core test products.
- Use Swift 6 language mode and a **macOS 14.0** deployment target.
- Do not create mobile or Mac Catalyst targets.
- Manual start is the Stage 0 entry point. Call detection remains an adapter
  seam and can only produce a proposal; it can never open capture streams.
- Every capture start passes through a visible preflight and an explicit
  `Start` action.

macOS 14 provides ScreenCaptureKit system-audio output and SwiftUI
`MenuBarExtra`. The microphone remains an independent AVFoundation adapter, so
the two source identities and clocks are never collapsed.

### 2. Layering

The application has four dependency layers:

1. **App shell (SwiftUI/AppKit):** menu bar lifecycle, visible consent,
   live status, settings, notifications, and accessibility.
2. **macOS adapters (Swift):** ScreenCaptureKit, AVFoundation, TCC preflight,
   security-scoped bookmarks, atomic file publication, Keychain, and
   sleep/wake events.
3. **C facade:** a small versioned C ABI using opaque handles, fixed-width
   integers, byte spans, explicit ownership, and numeric error codes.
4. **Portable C++20 core:** domain state, bounded audio queues, backend ports,
   segment ordering, SQLite journal, recovery, and deterministic renderers.

Dependencies point inward. The C++ target must compile and its golden tests
must run without importing or linking SwiftUI, AppKit, ScreenCaptureKit,
AVFoundation, Keychain, or Obsidian-specific code.

### 3. Swift/C++ boundary

Swift does not consume public STL types or inference-library types. The
supported boundary is a C ABI defined in
`Core/include/LocalScribeCore/LocalScribeCore.h`.

- Handles are opaque and destroyed explicitly.
- Every public struct starts with `struct_size` and `abi_version`.
- Audio sample memory is borrowed only for the duration of `push_audio`; the
  core copies accepted frames into its bounded queue before returning.
- Text and rendered documents returned by the core are owned byte buffers and
  are released with the matching core function.
- No C++ exception, reference, `std::string`, allocator, or RTTI type crosses
  the ABI.
- The core never calls Swift while holding an internal lock. Swift receives
  events by polling from a dedicated non-main task and forwards value snapshots
  to a `SessionActor`.

The detailed contract is in [Swift/C++ API](Swift-Cxx-API.md).

### 4. Capture and threading

- `ScreenCaptureKitAdapter` captures system audio only.
- `MicrophoneCaptureAdapter` uses AVAudioEngine/AVFoundation and enables Voice
  I/O processing when the active input/output route supports it. Unsupported
  aggregate or virtual routes fall back to unprocessed capture instead of
  losing the microphone.
- Each adapter emits the same portable `AudioFrameV1` contract with a distinct
  `source_id`, monotonic timestamp, sequence number, native format, and
  discontinuity bit.
- During Start and Resume, the frame router remains parked: callbacks are
  acknowledged but discarded until both required adapters have synchronously
  reported ready and the core transition is committed. Resume records the
  two-source startup gap before routing reopens.
- Capture callbacks perform format inspection, a bounded copy, meter
  calculation, and enqueue only. They perform no inference, SQLite, Markdown,
  filesystem, UI, or logging work.
- The core has a bounded queue per source. Queue depth, high-water mark,
  rejected-frame count, and discontinuities are observable.
- Backpressure first suppresses unstable partial events. Final segments and
  journal commits are never silently discarded.
- Markdown projection compares aligned groups of up to four adjacent fragments
  in each source. It removes the microphone group when its text matches the
  direct system-audio group, even if Whisper chose different segment
  boundaries or confidence scores. This deterministic cross-source guard
  handles loudspeaker echo that an external application's playback cannot
  reliably expose to Voice I/O.

### 5. Replaceable inference backends

The C++ core owns backend-neutral interfaces:

- `IAsrBackend`
- `IDiarizationBackend`
- `ISpeakerEmbeddingBackend`

Domain input/output types belong to LocalScribe. Concrete whisper.cpp or ONNX
objects stay inside their adapters.

The first production ASR candidate is a direct **whisper.cpp adapter** loaded
with an explicitly selected local `ggml-*.bin` model. The picker imposes no
artificial file-size or model-family cap; whisper.cpp remains the authority on
model compatibility. Signed-app consent-to-file timing, peak memory, and real
ASR checks are recorded per model/machine baseline. Native model construction
runs off the session actor, and the first recording publication has no
debounce. A deterministic fixture backend is also built for unit, recovery,
golden, and soak tests; it is visibly labelled as a test backend and is not a
production fallback.

Whisper receives 16 kHz mono audio through a stateful per-source rational
resampler. Its ratio-scaled Blackman-sinc anti-alias filter supports input from
8 through 96 kHz, preserves phase/history across callback partitions, and
resets or drains only the affected source at discontinuity/EOS. A callback is
rejected before state mutation if its rate or projected output exceeds the
bounded adapter contract.

The default diarization backend is source- and acoustics-aware:

- the configured source ID is authoritative even if auxiliary source-kind
  metadata is stale;
- microphone → configured local speaker (`Me` by default), enforced again
  before a final segment is committed;
- system audio → never the local speaker;
- system audio → up to eight anonymous, session-local remote speakers,
  clustered from trimmed per-segment spectral voice descriptors using bounded
  robust centroids and multiple recent prototypes;
- gray-zone acoustic changes remain provisional until a second consistent
  segment either expands the current voice profile or confirms a new speaker;
- a single acoustic outlier cannot create a new anonymous speaker;
- acoustically unstable short segments use temporal or turn-hint continuity
  instead of creating noisy new identities;
- compatible `*-tdrz` whisper.cpp models → additional explicit speaker-turn
  hints.

The legacy source-aware backend remains available for deterministic tests and
compatibility. The interface supports revisions and confidence so a learned
sherpa-onnx/ONNX speaker-embedding adapter can replace acoustic clustering
without changing Swift or the journal schema. Calibrated production accuracy,
overlap handling, and persistent voice profiles remain post-vertical work.

If a production ASR model is unavailable, preflight fails visibly. The app must
not fabricate text or silently switch to a network service.

Mixed Russian/English sessions persist and render only `ru` or `en` language
labels. If Whisper auto-detects another language on a short or noisy fragment,
LocalScribe deterministically maps it from the transcript script and recent
source language. Dialogue rows in the managed Markdown block are contiguous;
blank lines are reserved for section boundaries rather than individual turns.

### 6. Canonical state and recovery

SQLite is the canonical recovery store. Markdown is a deterministic projection,
not the database.

The journal uses WAL mode, foreign keys, explicit schema versioning, and
transactions. A final segment is eligible for Markdown publication only after
its current revision is committed. The journal stores no raw audio by default;
therefore recovery guarantees all committed final segments but may lose the
unfinalized inference tail at a crash. This limitation is surfaced as
`interrupted`.

There is one canonical internal `phase` in core and SQLite. Markdown exposes
the smaller Product Reference `status` vocabulary:

| Internal phase | Published Markdown status |
|---|---|
| `recording`, `paused`, `finalizing` | `recording` |
| `complete` | `complete` |
| `interrupted`, `recovery_required` | `interrupted` |
| `incomplete_sources` | `incomplete_sources` |

`detected`, `awaiting_consent`, `preparing`, and `failed_to_start` do not create
a transcript file. This resolves the PRD ambiguity without inventing
frontmatter values or writing pre-consent artifacts into the vault.

On launch, any journal session left nonterminal is moved to
`recovery_required` and finalized as `interrupted` without reopening capture.
Terminal sessions that lack a receipt for their exact checkpoint are
publication-recovered without changing their committed terminal phase. Stage 0
does not resume a recovered capture session. Recovery scans every durable row;
an earlier failure is retained even if a later row succeeds, blocks new
capture, and can be retried explicitly without TCC, model loading, or capture.

### 7. Safe Markdown publication

The core produces one deterministic UTF-8 snapshot from committed journal
state. The macOS file sink publishes it.

- The filename is normalized to a single safe path component. `/`, `:`, NUL,
  control characters, `.`/`..`, and path traversal are rejected or replaced.
- Name collisions use a short stable session-ID suffix; existing files are
  never overwritten merely because a template collides.
- YAML values use a deterministic quoted-scalar encoder. User-controlled text
  cannot introduce frontmatter keys.
- Transcript content is normalized to valid UTF-8, strips NUL/control
  characters, folds line separators, and escapes Markdown, Obsidian, HTML, and
  literal ownership-marker syntax so each final segment remains one physical
  plain-text line.
- Speaker labels are plain escaped text, never interpreted as paths or YAML.
- Only final segments appear between the exact ownership markers.
- Capture diagnostics use a second exact managed marker pair so source events
  update safely without entering the dialogue block or overwriting user prose.
- User YAML comments and blank layout trivia remain user-owned and survive
  repeated managed-frontmatter replacement.
- Publication writes and syncs a sibling temporary file, then atomically
  replaces the owned target.
- Replacement is directory-FD-relative and uses macOS no-follow
  `RENAME_EXCL`/`RENAME_SWAP` operations. For an owned-target update, the
  synced candidate first receives a visible conflict-slot name; any displaced
  external bytes therefore remain visible even if the process dies immediately
  after the swap.
- The sink records a cryptographic digest and byte count derived from the exact
  bytes handed to atomic replacement; optional file identity is never sampled
  from a later pathname that an external atomic save could race.
- Before replacement, the sink verifies that the target bytes still equal the
  bytes used as merge input. If ownership-aware merge of managed frontmatter
  keys and marker blocks cannot be proven safe, it creates a session-ID
  recovery copy or stages the snapshot instead of overwriting unrelated prose.
- Before attempting a conflict restore, it also makes an independent synced
  visible copy of the first displaced edit. Later atomic saves remain either
  at the requested target or at a visible conflict artifact; no user-owned
  inode is left under an anonymous temporary name.
- Per-session publication sequences are monotonic in both the vault writer and
  staging writer, so an older async live attempt cannot overwrite a newer
  terminal attempt.
- If the security-scoped directory is unavailable, the exact snapshot is
  published to application staging. The destination is never silently changed.
- A stale bookmark is refreshed only while its resolved security scope is
  active; validation or refresh failure releases the lease and never selects a
  different folder.

The renderer never emits summaries, embeddings, raw audio, credentials, model
prompts, or hidden metadata.

### 8. Privacy and diagnostics

- The capture-to-Markdown path has no network client or analytics dependency.
- Logs contain IDs, state names, counts, durations, and error codes only.
- Audio samples, transcript text, participant names, file contents, embeddings,
  and full user paths are prohibited in logs and crash metadata.
- Recording state is always visibly distinguishable by icon and text, not color
  alone.
- Stale queued core phases are compared with the authoritative current state,
  so an old `preparing` event cannot regress the active recording indicator.
- Potentially slow native model preparation, pause/drain, and finalization run
  outside the Swift session actor. Quit can quiesce routing/capture while
  SQLite remains recoverable even if native work is delayed.
- Direct Quit detaches frame routing first, attempts both adapter stops in
  parallel, removes actor ownership of the native handle, and bounds its wait
  for finalization/close. It performs no terminal vault write in the Quit
  critical path; a missing exact receipt is recovered from SQLite on launch.
- Repeated AppKit termination requests coalesce behind the same deferred
  cleanup; a second Quit cannot bypass it.
- Accessibility means keyboard navigation, VoiceOver labels, and Dynamic Type;
  the app does not request Accessibility TCC permission.

### 9. Build strategy

- Swift Package Manager is the primary build surface available to the shell and
  bridge.
- CMake is the portable core build surface for non-Apple-framework unit,
  golden, and benchmark runners.
- A complete Xcode installation is required to produce, sign, and exercise the
  sandboxed `.app`, entitlements, Screen Recording permission, and microphone
  permission. Command Line Tools alone can still compile the package and core
  tests.

## Consequences

### Positive

- Capture code and the UI can evolve without changing the journal or inference
  contracts.
- A backend can be replaced without exposing its types to Swift.
- Recovery and Markdown equality are testable without audio hardware or
  Obsidian.
- No second platform is needed to prove portability.
- Explicit consent is enforced by both UI workflow and state transitions.

### Costs

- Audio crosses one copy at the Swift/C boundary to keep lifetime rules safe.
- A versioned C facade adds mapping code.
- SQLite-to-Markdown publication is eventually consistent rather than direct.
- Crash recovery cannot promise an unfinalized inference tail until an optional
  encrypted audio spool policy is separately accepted.
- Runtime capture and signing cannot be fully verified without full Xcode and
  user-granted TCC permissions.

## Rejected alternatives

- **Direct Swift/C++ interop as the public boundary:** compiler-version and STL
  ownership surface is too broad for a stable core contract.
- **Markdown as the journal:** cannot safely represent revisions, atomic
  transitions, backpressure, or crash recovery.
- **One mixed audio stream:** destroys reliable `Me/system` attribution and
  complicates clock-gap diagnosis.
- **ScreenCaptureKit microphone output as the only microphone adapter:** would
  raise the minimum platform to macOS 15 and couple both sources to one stream.
- **Automatic start after detection:** violates explicit consent and the
  Product Reference.
- **Apple Speech or a cloud ASR fallback:** does not provide the required
  portable, offline, backend-controlled behavior.
- **Electron/Tauri/mobile shell:** adds bridges around every critical macOS API
  without reducing the core complexity.

## Deferred decisions

These are not silently chosen by the technical vertical:

- bundled model and model-download UX;
- learned production diarization backend and calibrated thresholds;
- encrypted voice-profile lifecycle;
- encrypted raw-audio spool and retention;
- call-detection signal scoring;
- import and JSON/SRT/VTT export;
- participant-based automatic file renaming.

None of these deferrals permits a network fallback, hidden capture, or a mobile
target.
