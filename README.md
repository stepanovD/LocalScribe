# LocalScribe

> Русскоязычная инструкция: [установка и первый запуск](Docs/Installation.ru.md).
> Если вы публикуете собственный форк, используйте
> [чек-лист публикации на GitHub](Docs/Publishing.ru.md).

> **License:** LocalScribe is source-available under the
> [PolyForm Noncommercial License 1.0.0](LICENSE.md). Personal and other
> noncommercial use is permitted; commercial use and sale require separate
> permission from the copyright holders. This is not an OSI-approved open-source
> license. Third-party components keep their own terms; see
> [Third-party notices](THIRD_PARTY_NOTICES.md).

LocalScribe is a local-first macOS menu bar application that records microphone
and system audio only after explicit consent, transcribes both streams locally,
and incrementally publishes a recoverable Markdown transcript into a
user-selected Obsidian folder.

The implementation follows **LocalScribe Product Reference / PRD v0.4
(2026-07-27)**. The product scope is deliberately narrow:

- macOS only;
- no hidden or automatic recording;
- no mobile targets;
- no summaries, daily-note mutation, calendar integration, external LLM calls,
  cloud storage, or content-bearing telemetry;
- no network dependency in the capture-to-Markdown pipeline.

## MVP vertical

The repository contains one working macOS technical vertical:

- SwiftUI/AppKit menu bar shell with a separate visible consent step;
- independent ScreenCaptureKit system-audio and AVAudioEngine microphone
  adapters, with best-effort system voice processing on the microphone path
  plus grouped cross-source echo suppression when Whisper splits the same
  loudspeaker phrase differently between the two channels;
- a versioned C ABI over the portable C++20 session core;
- local whisper.cpp v1.9.1 ASR with a user-selected `ggml-*.bin` model and a
  stateful 8–96 kHz streaming resampler;
- replaceable ASR and diarization interfaces, with source-enforced `Me`
  attribution for the microphone, a `Me` exclusion for system audio,
  robust prototype-based local clustering, and explicitly saved voice profiles
  that can carry a remote speaker's name into later calls;
- a WAL-mode SQLite recovery journal and startup recovery that never restarts
  capture;
- deterministic final-only Markdown rendering with compact dialogue rows and
  transcript language metadata restricted to Russian or English;
- security-scoped Obsidian-folder access, collision-safe filenames,
  ownership-aware external-edit merging, atomic replacement, recovery copies,
  and private staging;
- a macOS-only app bundle builder and reproducible core/Swift/ASR/soak checks.

The deterministic fixture ASR exists only behind an explicit test flag. The
production app fails visibly if the selected local whisper model is missing;
there is no network or fixture fallback.

## Build and run

Requirements:

- macOS 14 or later;
- Swift 6 / Xcode or compatible Command Line Tools;
- an existing whisper.cpp `ggml-*.bin` model. There is no artificial file-size
  cap; whisper.cpp validates compatibility when the model is loaded. Larger
  models can substantially increase startup time and memory use, so
  model-specific signed-app timing remains a release evidence gate. Model
  weights are not committed or downloaded by the app.

Build a locally signed application bundle:

```sh
Scripts/build-app-bundle.sh release
open Build/LocalScribe.app
```

In Settings, choose:

1. the folder inside the Obsidian vault that should receive transcripts;
2. the local multilingual ggml model.

Choose **New transcript…**, read the separate consent screen, and press
**Start Recording**. TCC prompts are requested only after that action. The app
can then pause, explicitly resume, stop, show source health, and open the last
published Markdown file.

On the first Screen Recording grant, macOS may require LocalScribe to be quit
and reopened before capture is enabled; the app reports that state explicitly
instead of attempting a misleading failed recording.

The bundle script uses ad-hoc signing for local development. Distribution
signing, notarization, and release TCC evidence require a Developer ID build
with full Xcode.

For a step-by-step guide covering prerequisites, model selection, macOS
permissions, updates, removal, and troubleshooting, see
[Installation (Russian)](Docs/Installation.ru.md).

## Verification

Run the complete local gate:

```sh
Scripts/verify-mvp.sh
```

The gate verifies the excluded scope, portable core contracts, Swift storage
and recovery invariants, the linked macOS application bundle, and signatures.
When full Xcode is selected, it also runs the Swift XCTest target. A
Command-Line-Tools-only machine reports that XCTest was skipped; release or CI
jobs can set `LOCALSCRIBE_REQUIRE_SWIFT_TESTS=1` to make its absence fail the
gate. Run that target directly with:

```sh
Scripts/run-swift-tests.sh
```

If local test speech and a model are available, include real production ASR:

```sh
LOCALSCRIBE_MODEL_PATH=/path/to/ggml-base.bin \
LOCALSCRIBE_WAV_PATH=/path/to/speech.wav \
Scripts/verify-mvp.sh
```

The standalone ASR check accepts little-endian PCM16 or Float32 WAV input and
exercises both the whisper adapter and the full
`audio → journal → Markdown` C ABI:

```sh
Scripts/run-whisper-smoke.sh /path/to/ggml-base.bin /path/to/speech.wav
```

Run the accelerated core soak probe:

```sh
Scripts/run-core-soak.sh --smoke
```

The same runner performs the specified two-hour, real-time synthetic run when
invoked without arguments:

```sh
Scripts/run-core-soak.sh
```

The synthetic run complements, but does not replace, the signed-app two-hour
capture checklist in `Docs/Testing/Two-Hour-Soak-Test.md`.

## Recovery behavior

SQLite is canonical. At launch, nonterminal sessions become
`recovery_required`; their committed finals are rendered as an `interrupted`
transcript. A session that had already durably reached `complete`,
`incomplete_sources`, or `interrupted` but crashed before its exact publication
receipt is re-published without changing that terminal status. Output goes to
the selected folder or private staging. Recovery does not request TCC, load the
ASR model, or start a capture adapter. A publishable session without a receipt
for its exact journal checkpoint remains recoverable on the next launch.
If one of several sessions cannot be published, later sessions are still
recovered but cannot hide that failure: new capture remains blocked and the
menu exposes **Retry Recovery**, which re-scans only durable pending work.
Direct Quit has a bounded privacy shutdown and does not wait on vault I/O; if
native finalization or its exact receipt loses the race with process exit,
startup recovery publishes the durable journal state without restarting
capture.

Because Stage 0 does not retain raw audio, audio that had not yet become a
committed final segment at the instant of a crash cannot be reconstructed.
Only a session whose terminal `complete` transition was already committed
before the crash can be recovered as `complete`.

## Persistent voice profiles

Remote speakers begin as anonymous, per-call clusters such as `Speaker 1`.
From the current or most recently completed call, the user can explicitly save
one of those clusters as a named voice profile. LocalScribe stores only compact
acoustic descriptors and their format/version metadata, never the source audio.
Saving a profile relabels that speaker in the durable current/last call and
publishes a new Markdown snapshot with the chosen name.

To make post-call naming possible, the local recovery journal keeps these
bounded descriptors with remote final segments before the user creates a
profile. This evidence is not automatic enrollment and never receives a saved
identity without the explicit naming action.

At the start of a later call, compatible saved profiles seed local diarization.
A remote turn receives a saved name only after a stricter acoustic match; a
weak, ambiguous, short, corrupt, or format-incompatible observation remains
`Speaker N`. Profile recognition failure does not stop transcription. The
microphone source remains the configured local speaker and is never matched to
a remote profile.

Settings lists saved profiles and allows renaming or deleting them. These
actions affect future recognition; LocalScribe does not silently rewrite every
historical transcript. Profile creation is always a visible user action—calls
do not automatically enroll every participant.

Voice descriptors are sensitive biometric-derived data even though they cannot
reconstruct the original recording. Descriptor bytes remain inside LocalScribe
application storage and are excluded from Markdown, diagnostics, crash
metadata, and network traffic; the chosen display name intentionally appears as
the transcript's speaker label and participant. This version does not claim
separate application-level encryption or forensic secure erasure from SSD
snapshots, SQLite remnants, or backups; macOS account/device protection is
still part of the privacy boundary.
Deleting a profile makes it unavailable for future matching, but it does not
erase descriptor evidence already retained with historical recovery-journal
segments. Users with stronger at-rest or deletion requirements should treat
that limitation explicitly.

The current lightweight spectral descriptor is a convenience labeler, not
biometric authentication or proof of identity. Different voices can be merged,
and one voice can be split when devices, playback processing, noise, overlap,
illness, or speaking style change. LocalScribe therefore prefers an anonymous
label over a low-confidence name.

## Architecture package

The architecture is fixed before implementation:

- [ADR-0001: macOS technical vertical](Docs/Architecture/ADR-0001-macos-technical-vertical.md)
- [ADR-0002: unbounded local whisper.cpp model selection](Docs/Architecture/ADR-0002-unbounded-local-whisper-model-selection.md)
- [ADR-0003: one-line transcript segments](Docs/Architecture/ADR-0003-one-line-transcript-segments.md)
- [Swift/C++ API boundary](Docs/Architecture/Swift-Cxx-API.md)
- [Project structure](Docs/Architecture/Project-Structure.md)
- [Session state machine](Docs/Architecture/Session-State-Machine.md)
- [Two-hour soak test](Docs/Testing/Two-Hour-Soak-Test.md)

## Deliberate Stage 0 limits

- Start is manual; detection remains a proposal-only seam.
- Remote-speaker labels come from lightweight, model-free acoustic clustering.
  Each call can still create up to eight new anonymous clusters. Explicitly
  saved compatible profiles may supply names across calls, while unmatched or
  ambiguous speech remains anonymous. Similar voices can merge and very short
  or noisy turns can split; compatible `*-tdrz` Whisper models also contribute
  explicit speaker-turn hints.
- The model is selected locally rather than bundled or downloaded.
- Stage 0 accepts a user-selected local whisper.cpp `ggml-*.bin` model without
  an artificial size cap. Each model/machine combination needs its own startup,
  memory, and real-ASR evidence before it can be treated as a release baseline.
- There is no mobile target, summarization, external LLM call, calendar,
  cloud, daily-note mutation, or content telemetry.

## License

Original LocalScribe code and documentation are available under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md). It permits personal,
educational, research, and other noncommercial use, including noncommercial
modification and distribution. Commercial use, including selling LocalScribe or
using it with an anticipated commercial application, requires separate
permission from the copyright holders.

This is a source-available license, not an OSI-approved open-source license.
The vendored whisper.cpp framework remains under the MIT License, SQLite is
public domain, and system frameworks remain under their respective terms. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
