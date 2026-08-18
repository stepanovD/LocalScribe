# ADR-0005: Inertial speaker switches

- **Status:** Accepted
- **Date:** 2026-08-17
- **Decision owner:** LocalScribe diarization vertical

## Context

Acoustic diarization previously had several independent fast paths. A strong
Voice Profile match, a match to an existing anonymous cluster, or a
TinyDiarize turn hint could change the published speaker after one Final
Segment. The novel-cluster path had limited confirmation logic, but the other
paths could still turn a single noisy descriptor into a visible speaker
change.

Publishing the segment immediately and correcting it later would expose the
instability to live events, Markdown, participant lists, and Voice Profile
enrollment. Keeping only an in-memory candidate would avoid that exposure but
would lose already-final ASR text on a process failure.

## Decision

Acoustic diarization uses one `PendingSpeakerSwitch` policy for transitions to
new or known anonymous clusters, persisted Voice Profiles, and TinyDiarize
turn hints.

- The first local and remote speakers are assigned immediately.
- A known acoustic identity may switch immediately only at similarity `0.94`
  or higher and with a margin of at least `0.04` over both the active speaker
  and every other candidate.
- Every other switch needs either two compatible Final Segments with distinct
  stable IDs or 1.5 seconds of unique compatible suspected speech.
- Compatible evidence uses the same descriptor model and has pairwise
  similarity of at least `0.92`. A newer revision of one stable ID replaces
  its evidence and does not count as a second observation.
- TinyDiarize is a turn hint only. A switch cannot be confirmed without a
  usable acoustic descriptor.
- Speculative evidence does not update either the active or target cluster.
- A return to the active voice, a different candidate, discontinuity, pause,
  finalization, capacity overflow, or expiry resolves the held group to its
  saved fallback attribution.

The hard deadline is five seconds of fully processed System Audio after the
end of the first held segment. ASR therefore emits ordered timeline batches,
including empty batches for decoded silence. Raw capture timestamps and wall
clock time do not advance this deadline. Hypotheses in a batch are classified
before that batch's watermark is applied, so evidence ending exactly on the
deadline can still confirm the switch.

A discontinuity is also an ordered timeline boundary. Whisper emits a
zero-duration boundary batch after all pre-gap chunks even when the first
post-gap chunk is still buffered, and an explicit source-discontinuity event
is queued through the same inference path. A boundary may reset that source's
watermark; without a boundary, watermarks remain monotonic. Every hypothesis
must stay inside the processed interval of its batch.

The public C ABI, Swift API, and Markdown format do not expose this protocol.
Internally, diarization returns atomic `commit`, `hold`, and `resolve`
operations. A pending group holds at most 64 revisions.

## Durability and publication

SQLite schema version 3 stores hidden pending groups and their complete Final
Segment payloads, Voice Descriptors, fallback Speaker Labels, confidence, and
deadline. It deliberately does not persist the speculative target identity.

Staging a group advances the journal checkpoint once but does not advance the
highest visible revision or timeline origin, increment committed-segment
metrics, emit a Final Segment event, affect participant or enrollment queries,
or enter Markdown. Confirming or falling back atomically promotes the entire
group into visible `segments` under one new checkpoint. Only then are existing
Final Segment events and metrics produced.

This refines the durable-before-event invariant: a final ASR hypothesis may be
durably staged while its speaker decision remains private; every publicly
visible Final Segment is still durable before its event is created.

Recovery cannot reconstruct speculative acoustic state. Before a live session
is marked `recovery_required`, every unresolved group is atomically promoted
using its persisted fallback attribution. Thus a crash may conservatively keep
the previous speaker, but it cannot lose final text. Terminal transitions are
allowed only after no pending group remains.

## Consequences

- Normal ambiguous switches gain up to five seconds of processed-audio
  latency, while strong unambiguous matches remain immediate.
- A confirmed group has the correct speaker from its first visible segment;
  clients never need a public speaker-correction event.
- Empty decoded ASR batches are semantically observable inside the core even
  though they contain no transcript hypothesis.
- The recovery journal owns a small amount of non-published final text and
  bounded voice-descriptor data for the duration of a pending decision. It
  still stores no raw audio and sends no data over the network.
