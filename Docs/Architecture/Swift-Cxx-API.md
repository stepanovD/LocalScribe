# Swift/C++ API boundary

## Goals

The boundary must:

- remain source- and ABI-stable while ASR/diarization implementations change;
- preserve source identity, timestamps, sequence numbers, and discontinuities;
- make ownership and threading unambiguous;
- carry no Apple, Swift, STL, whisper.cpp, ONNX, or SQLite implementation type;
- expose enough diagnostics to prove no silent loss;
- be testable from a small C or Swift client.

The public header is C even though the implementation is C++20.

## ABI conventions

```c
#define LS_CORE_ABI_VERSION 1u

typedef struct ls_core ls_core_t;
typedef struct ls_session ls_session_t;
typedef struct ls_event ls_event_t;
typedef struct ls_owned_bytes ls_owned_bytes_t;
```

- All exported functions use `extern "C"` when compiled as C++.
- Every input/output struct begins with:

```c
uint32_t struct_size;
uint32_t abi_version;
```

- Enums have fixed `int32_t` storage and an `unknown = 0` member.
- Time uses signed 64-bit nanoseconds. Capture time is monotonic; wall time is
  ISO 8601 metadata supplied separately.
- Text is UTF-8 plus an explicit byte length. It is never assumed NUL-terminated.
- Functions return an `ls_status_code_t`. Optional error detail is copied into
  a caller-owned `ls_error_v1`.
- Destruction functions accept `NULL` and are idempotent with respect to `NULL`,
  not with respect to an already-destroyed handle.
- A handle is not used concurrently except where a function is explicitly
  marked thread-safe.

## Stable domain structs

### Audio frame

```c
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t source_id;
    uint64_t sequence_number;
    int64_t monotonic_time_ns;
    uint32_t sample_rate_hz;
    uint16_t channel_count;
    uint16_t sample_format;       /* float32 interleaved in ABI v1 */
    uint32_t frame_count;
    uint32_t flags;               /* discontinuity, end_of_stream */
    const float *samples;
} ls_audio_frame_v1;
```

`samples` is borrowed. `ls_session_push_audio_v1` either copies the complete
frame into the selected bounded queue and returns `LS_OK`, or rejects the
complete frame with a reason such as `LS_BACKPRESSURE`. It never accepts a
partial frame. Rejection increments an observable counter and forces a
discontinuity event; it is not silent.

### Session configuration

```c
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    ls_utf8_view_v1 session_id;
    ls_utf8_view_v1 journal_path;
    ls_utf8_view_v1 source_app;
    ls_utf8_view_v1 local_speaker_name;
    ls_utf8_view_v1 asr_backend_id;
    ls_utf8_view_v1 asr_model_path;
    ls_utf8_view_v1 diarization_backend_id;
    uint32_t language_mode;
    uint32_t audio_queue_capacity_frames;
} ls_session_config_v1;
```

Paths enter only the portable journal or model adapters. The core never receives
the security-scoped vault URL. Markdown publication remains a Swift file-sink
responsibility.

### Transcript segment

```c
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    ls_uuid_v1 stable_id;
    uint64_t source_id;
    int64_t start_time_ns;
    int64_t end_time_ns;
    uint64_t speaker_id;
    ls_utf8_view_v1 speaker_label;
    ls_utf8_view_v1 text;
    ls_utf8_view_v1 language;
    float confidence;
    uint32_t revision;
    uint32_t flags;               /* final, unintelligible */
} ls_transcript_segment_v1;
```

The same contract represents live and final processing. Partial segments may be
delivered to the UI but are never journaled as durable transcript content and
never rendered to Markdown. A higher revision supersedes a lower revision of
the same stable ID inside a transaction. A Final ASR hypothesis held by
inertial diarization is not yet an `ls_transcript_segment_v1`: it remains in
private durable staging until its whole pending group is promoted with either
the confirmed or saved fallback attribution. This internal state adds no public
ABI value or correction event.

## Lifecycle functions

The intended C surface is:

```c
ls_status_code_t ls_core_create_v1(
    const ls_core_config_v1 *config,
    ls_core_t **out_core,
    ls_error_v1 *out_error);

void ls_core_destroy(ls_core_t *core);

ls_status_code_t ls_session_create_after_consent_v1(
    ls_core_t *core,
    const ls_session_config_v1 *config,
    ls_session_t **out_session,
    ls_error_v1 *out_error);

ls_status_code_t ls_session_mark_sources_ready_v1(ls_session_t *session);
ls_status_code_t ls_session_push_audio_v1(
    ls_session_t *session,
    const ls_audio_frame_v1 *frame);
ls_status_code_t ls_session_pause_v1(ls_session_t *session);
ls_status_code_t ls_session_resume_after_consent_v1(ls_session_t *session);
ls_status_code_t ls_session_source_event_v1(
    ls_session_t *session,
    const ls_source_event_v1 *event);
ls_status_code_t ls_session_finalize_v1(
    ls_session_t *session,
    ls_finalize_reason_t reason);
void ls_session_destroy(ls_session_t *session);
```

The core cannot cryptographically prove a human gesture. The deliberately named
creation/resume calls make the trust boundary explicit, while the Swift
`SessionActor` only invokes them with a short-lived consent token generated by a
visible action.

## Event delivery

Swift does not receive arbitrary re-entrant callbacks from C++. It drains an
event queue from a dedicated task:

```c
ls_status_code_t ls_session_next_event_v1(
    ls_session_t *session,
    uint32_t timeout_ms,
    ls_event_t **out_event);

ls_event_kind_t ls_event_kind(const ls_event_t *event);
ls_status_code_t ls_event_copy_segment_v1(
    const ls_event_t *event,
    ls_transcript_segment_copy_v1 *out_segment);
ls_status_code_t ls_event_copy_metrics_v1(
    const ls_event_t *event,
    ls_pipeline_metrics_v1 *out_metrics);
ls_status_code_t ls_session_copy_state_v1(
    const ls_session_t *session,
    ls_state_event_copy_v1 *out_state);
void ls_event_destroy(ls_event_t *event);
```

Rules:

- only one consumer polls a session;
- a timeout is normal and returns `LS_TIMEOUT`;
- closing a session wakes the poller with `LS_CLOSED`;
- an event owns its payload until `ls_event_destroy`;
- the poller immediately converts the event into a Sendable Swift value and
  then destroys it;
- event payloads never contain raw audio;
- queue overflow is itself a durable error/metrics event. Partial events may be
  coalesced; state, final segment, source loss, and terminal events may not be
  coalesced away;
- diarization `hold` and `resolve` operations are not event kinds. A held Final
  Segment enters the public queue and committed-segment metrics only after the
  corresponding pending group has been atomically promoted.

`ls_session_copy_state_v1` reads the authoritative lifecycle state under the
core state lock without consuming the event queue. Swift uses it immediately
after synchronous finalization instead of guessing that a terminal event will
reach the UI poller within an arbitrary timeout.

The first ASR, diarization, segment-commit, or worker-journal failure latches a
fatal data-loss condition. The core stops accepting audio, accounts and clears
queued frames, emits one error event, and makes live rendering fail promptly.
The Swift lifecycle owner must stop both capture adapters and call
`ls_session_finalize_v1`; finalization skips backend flushes and durably commits
`interrupted/process_interrupted`, regardless of the caller's earlier stop
reason. It must never leave the menu-bar UI displaying `recording` after an
error event.

If capture adapter startup fails before sources become ready, Swift finalizes
the `preparing` session with `cancelled`. This is durably recorded as
`failed_to_start`; that phase is neither Markdown-publishable nor recoverable.

## Rendering and recovery

Rendering consumes the visible part of a committed immutable journal snapshot:

```c
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t journal_checkpoint;
    uint32_t highest_segment_revision;
    uint32_t reserved;
} ls_render_snapshot_v1;

ls_status_code_t ls_session_render_markdown_with_snapshot_v1(
    ls_session_t *session,
    const ls_markdown_options_v1 *options,
    ls_owned_bytes_t **out_markdown,
    ls_render_snapshot_v1 *out_snapshot,
    ls_error_v1 *out_error);

ls_status_code_t ls_session_render_markdown_v1(
    ls_session_t *session,
    const ls_markdown_options_v1 *options,
    ls_owned_bytes_t **out_markdown,
    ls_error_v1 *out_error);

const uint8_t *ls_owned_bytes_data(const ls_owned_bytes_t *bytes);
size_t ls_owned_bytes_size(const ls_owned_bytes_t *bytes);
void ls_owned_bytes_destroy(ls_owned_bytes_t *bytes);

ls_status_code_t ls_core_list_recoverable_sessions_v1(
    ls_core_t *core,
    ls_recovery_list_t **out_list);
ls_status_code_t ls_core_open_recoverable_session_v1(
    ls_core_t *core,
    ls_utf8_view_v1 session_id,
    ls_session_t **out_session);
```

`out_markdown` and `out_snapshot` are derived from the same immutable SQLite
read snapshot. Swift must copy the token into the publication receipt instead
of consulting later pipeline metrics. The legacy render call remains ABI
compatible and discards this token.

A live recording render reads committed state immediately; it does not wait
for accepted ASR work to drain. Pause and finalization own their explicit drain
barriers. Hidden pending groups and their staged Final Segment payloads are
excluded from transcript rows, participants, and Markdown. Staging may advance
`journal_checkpoint` without advancing `highest_segment_revision` or changing
the rendered transcript bytes; promotion advances the visible values together.
The Markdown bytes have no output path and perform no filesystem I/O.
Final transcript segments and capture diagnostics use separate owned ranges:
`<!-- transcript:start -->` / `<!-- transcript:end -->` and
`<!-- capture-events:start -->` / `<!-- capture-events:end -->`. The capture
range is emitted even when empty so a merge can remove stale diagnostics
without replacing user-authored text. Marker-shaped transcript input is
HTML-neutralized.
Swift publishes them through the security-scoped sink and then acknowledges
the exact token and digest to the journal:

```c
ls_status_code_t ls_session_ack_publication_v1(
    ls_session_t *session,
    const ls_publication_receipt_v1 *receipt);
```

The journal accepts an older genuine rendered checkpoint while a live session
has advanced, provided its highest revision matches the historical checkpoint
and no newer receipt is already recorded. Future or regressing receipts are
rejected. Recovery and terminal publications must acknowledge the current
checkpoint so a crash after terminal commit but before publication remains
recoverable.

The receipt also contains the SHA-256 digest, file identity, publication
timestamp, and destination class (`vault`, `staging`, or `recovery_copy`). It
contains no user-visible path in diagnostic events.

Within the Swift sink, every render attempt also receives a strictly increasing
per-session publication sequence. `VaultWriter` and private staging both reject
an older attempt after a newer attempt has begun. This prevents a cancelled
live `recording` snapshot whose URL resolution finishes late from overwriting a
terminal snapshot.

## Voice-profile lifecycle

The C facade exposes core-scoped list, enroll, rename, and delete operations.
Enrollment accepts a durable session ID, an anonymous remote `speaker_id`, and a
UTF-8 display name. It does not accept raw audio or descriptor bytes from Swift:
the core derives a bounded profile only from embeddings attached to visible,
committed Final Segments. Descriptors in a hidden pending group do not affect
profile observations, participant queries, or relabeling. The result returns the
persistent profile/speaker IDs, observation count, relabeled-segment count, and
exact render checkpoint needed to republish the selected call.

Profile-list values expose only profile ID, display name, and observation count.
Centroids, prototypes, embedding dimensions, and embedding model/version remain
private C++/SQLite values and never enter the UI bridge. List ownership follows
the same opaque-handle/copy/destroy convention as recovery lists.

Speaker IDs use disjoint namespaces: `1` remains the microphone owner,
anonymous per-session remote speakers carry the high bit, and persisted
profiles carry the next-highest bit. Swift treats these IDs as opaque. The
diarization backend compares a persisted profile only with the same embedding
model/version and dimension, requires a stricter match plus separation from the
runner-up, and otherwise retains an anonymous label.

Enrollment and relabeling commit atomically. Swift then reopens/renders the
current or last reviewable session and publishes the returned checkpoint using
the normal conflict-safe writer. Rename and delete affect future matching and
do not trigger a global historical-file rewrite.

## C++ backend contracts

The backend contracts are internal C++ interfaces, not ABI:

```cpp
class IAsrBackend {
public:
    virtual ~IAsrBackend() = default;
    virtual BackendInfo info() const = 0;
    virtual Expected<void> prepare(const AsrConfiguration&) = 0;
    virtual Expected<std::vector<AsrTimelineBatch>>
        accept(const AudioWindow&) = 0;
    virtual Expected<std::vector<AsrTimelineBatch>> flush() = 0;
    virtual void requestAbort() noexcept {}
};

class IDiarizationBackend {
public:
    virtual ~IDiarizationBackend() = default;
    virtual BackendInfo info() const = 0;
    virtual Expected<void> prepare(const DiarizationConfiguration&) = 0;
    virtual Expected<DiarizationUpdate>
        assign(const AsrTimelineBatch&) = 0;
    virtual Expected<DiarizationUpdate>
        flush(DiarizationFlushReason reason) = 0;
};
```

`AsrTimelineBatch` carries `sourceId`, the actually decoded interval
`processedStartTimeNs...finalizedThroughTimeNs`, `discontinuityBefore`, and an
ordered hypothesis vector. A backend emits a batch only after that interval has
been fully processed. An empty batch for decoded silence is meaningful; raw
audio still buffered by ASR and wall-clock progress must not advance the decoded
watermark. Watermarks are per source, and only fully processed System Audio
advances the five-second deadline for a pending remote-speaker switch. Batches
from different sources may interleave; microphone or wall-clock progress must
never be treated as a substitute System Audio watermark.

`DiarizationUpdate` contains one ordered `SpeakerTurnDecision` for every input
hypothesis plus zero or more whole-group resolutions. A decision is either
`commit`, which is ready for normal visible persistence, or `hold`, which names
a bounded pending group and its deadline. A resolution is the internal
`resolve`: it supplies every staged revision in that group with either the
confirmed attribution or the persisted fallback attribution. The session must
apply staging or whole-group promotion atomically. It classifies all hypotheses
in a batch before applying that batch's decoded watermark, so evidence ending
exactly at the deadline can still confirm a switch.

`flush(DiarizationFlushReason::pause)` and
`flush(DiarizationFlushReason::endOfStream)` resolve every remaining group to
its saved fallback. Pause and terminal finalization first drain accepted audio,
process every ASR tail batch, then flush diarization; neither boundary may leave
a pending group alive. On recovery the speculative target is intentionally not
reconstructed: durable pending groups are fallback-promoted before the session
is marked `recovery_required`.

Contracts:

- input timestamps remain on the LocalScribe monotonic timeline;
- ASR batches preserve per-source decode order, including empty silence and
  discontinuity boundaries; capture timestamps alone never expire diarization
  evidence;
- whisper input conversion keeps a rational sample-rate remainder and FIR
  history per source across capture callbacks; a discontinuity, rate change, or
  EOS finishes and resets only that source, preventing callback-floor drift
  during a two-hour session;
- the ratio-scaled Blackman-sinc converter supports 8–96 kHz and validates the
  projected 16 kHz output bound before mutating per-source state;
- backends return LocalScribe-owned values;
- backend implementations return typed errors; defensive worker/finalize
  guards convert an unexpected backend exception into a latched internal error,
  so exceptions never cross the C boundary;
- backend IDs and model versions are written to SQLite for reproducibility, not
  to transcript prose;
- a backend cannot access the vault sink or network;
- factories validate local model presence during `preparing`;
- test backends are selected only by explicit test configuration.

## Swift orchestration

```text
MenuBar UI
   │ visible action produces ConsentToken
   ▼
@MainActor AppModel
   ▼
SessionActor ── starts/stops ── Capture adapters
   │                                │
   │ polls value events             │ pushes borrowed AudioFrame
   ▼                                ▼
C facade ─────────────────────── C++20 core
   │ committed immutable snapshot
   ▼
VaultWriter actor ── security scope / conflict check / atomic publish
```

- `AppModel` owns display state only.
- `SessionActor` is the sole lifecycle authority.
- native model creation, pause/drain, and ordinary finalization execute in
  Sendable detached work while the actor retains ownership of the handle;
  Direct Quit first transfers that ownership to one detached bounded teardown
  task, whose eventual close waits for all active C calls;
- capture start and publication run outside one another's lifecycle critical
  paths; Quit can preempt a suspended capture start;
- the capture frame router is parked until both adapters are sticky-ready and
  the core is recording; stale queued phases are filtered against
  `ls_session_copy_state_v1`;
- Capture adapters never call `VaultWriter`.
- `VaultWriter` never reads audio or invokes inference.
- profile UI values contain IDs, names, and counts only; acoustic descriptor
  bytes stay behind the C facade;
- at most one live publication worker is tracked per session; updates coalesce,
  terminal publication has a bounded wait, and an exact unacknowledged terminal
  snapshot remains recoverable from SQLite;
- All state changes shown in UI originate from committed core events or
  explicit platform-adapter status.

## Error categories

The numeric C error space is stable and grouped:

- invalid ABI/argument/state;
- consent/lifecycle invariant;
- model/backend unavailable;
- audio format/sequence/backpressure;
- SQLite/schema/recovery;
- render/encoding;
- closed/cancelled/timeout;
- internal invariant failure.

Platform errors such as TCC denial, security-scope failure, atomic-replace
failure, and Obsidian launch failure remain typed Swift errors. UI messages
combine categories without placing transcript content or full paths into logs.
