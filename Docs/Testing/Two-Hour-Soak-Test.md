# Two-hour soak test

## Purpose

Prove the Stage 0 gate under sustained load:

- every accepted final segment survives in SQLite and appears exactly once at
  its highest revision in Markdown;
- microphone and system source identity, sequence, and time stay observable;
- capture producers are never blocked by inference or file publication;
- backpressure, source loss, staging, and conflicts are visible rather than
  silent;
- the menu bar UI remains responsive;
- final Markdown is valid, deterministic, and recoverable.

The release evidence consists of two runs:

1. a deterministic two-hour synthetic/core run suitable for automation;
2. a signed-app two-hour real-capture run with user-granted TCC permissions.

An accelerated run is useful during development but does not replace either
wall-clock run.

The portable Core gate can be exercised with:

```sh
Scripts/run-core-soak.sh --smoke
Scripts/run-core-soak.sh --duration-seconds 7200 --speed 1000
Scripts/run-core-soak.sh
```

The accelerated and wall-clock forms simulate the same 7200-second timeline.
The runner paces its synthetic producer below the configured queue capacity and
fails if backpressure rejections exceed 1% of logical frames. Exactly two
`invalid_state` probes are expected while paused. Both required sources receive
an injected outage longer than the configured completeness threshold, so the
expected terminal phase and rendered status are `incomplete_sources`, not
`complete`. The gate reconciles event counts, source gaps, all segment
revisions, highest-revision Markdown, the atomic render snapshot token, and the
publication receipt against SQLite.

## Preconditions

- Release-equivalent build and the intended local whisper.cpp `ggml-*.bin`
  model are already installed.
- The exact model filename, checksum, quantization, and Apple-Silicon machine
  baseline are recorded. Consent-click to first vault-file time, peak memory,
  and thermal behavior are measured from the signed app, not inferred from a
  backend microbenchmark. A larger model is not a release baseline until this
  evidence and a real-ASR smoke check pass.
- Network is denied for the application and runner.
- At least 5 GB free space at the internal staging and selected vault.
- A fresh temporary Obsidian folder is selected through the system picker.
- Screen & System Audio Recording and Microphone permissions are granted.
- After a first Screen Recording grant, the signed app has been quit and
  reopened and preflight now reports the permission as active.
- Energy/memory sampling tools and a UI heartbeat are enabled without content
  logging.
- Test audio is license-safe, contains RU, EN, mixed speech, silence, overlap,
  and deterministic cue IDs.
- The expected final segment manifest contains stable ID, source, revision,
  time range, and a hash of normalized text—not raw content in routine logs.

## Instrumentation

Record once per second:

- monotonic runtime and UI heartbeat latency;
- per-source frames offered/accepted/rejected;
- last sequence number and discontinuity count;
- ring depth and high-water mark;
- ASR input/output queue depth;
- partial events coalesced;
- final segments committed;
- journal commit latency;
- Markdown snapshot requested/published/coalesced;
- publication latency and destination (`vault`, `staging`, `recovery_copy`);
- resident memory, CPU, thermal state, and file descriptor count;
- current phase and source health.

Never record audio, transcript text, participant names, embeddings, or full
paths in diagnostics.

## Deterministic source contract

The runner creates two independent 48 kHz float32 streams and feeds them with
real-time pacing:

- source 1: microphone / local speaker;
- source 2: system / remote speaker.

Each frame has a monotonically increasing sequence and timestamp. The fixture
ASR backend produces a known final segment only after its complete cue window,
including deterministic revisions. The test counts:

```text
offered frame → accepted frame or explicit rejection/discontinuity
accepted cue → committed final segment → published highest revision
```

There is no permitted unaccounted state.

## Two-hour schedule

| Wall time | Action | Required observation |
|---|---|---|
| 00:00–00:10 | Explicit start, steady two-source RU/EN input | file ≤3 s, first final ≤15 s, both sources active |
| 00:10–00:20 | Alternating speech and long silence | no invented segments; queue returns to baseline |
| 00:20–00:30 | Mixed RU/EN, overlapping source windows | source attribution preserved; UI heartbeat continues |
| 00:30–00:40 | Slow vault sink: 2 s delay per publish | capture queue unaffected; snapshots coalesce; finals durable |
| 00:40–00:50 | Burst inference load and partial updates | partials coalesce first; no silent final loss |
| 00:50–01:00 | Pause for 90 s, then explicit resume | no frames accepted during pause; timeline gap recorded |
| 01:00–01:10 | Remove/reconnect microphone for 20 s | source warning and one bounded discontinuity; system continues |
| 01:10–01:20 | Remove/reconnect system source for 20 s; inject one ready→failed restart | source warning persists; retry continues to a healthy binding; mic continues |
| 01:20–01:30 | Make vault unavailable | snapshots go to staging with visible warning; journal continues |
| 01:30–01:40 | Restore vault access and reconcile staging | exact revision published; no silent destination change |
| 01:40–01:50 | Externally edit unmanaged prose; issue two successive atomic editor saves around one LocalScribe publish | every editor version remains at the target or a visible artifact; LocalScribe uses a recovery copy on conflict |
| 01:50–01:57 | Return to normal steady input | queues drain; metrics stabilize |
| 01:57–02:00 | Explicit stop and tail flush | terminal status, final receipt, byte-valid Markdown |

Fault injection times are recorded as monotonic test events. Unexpected gaps are
failures even if final counts happen to match.

## UI responsiveness probe

Every second a main-actor probe schedules a lightweight state update and records
completion latency outside the content path. During the signed-app run, the
operator also performs at least once per 15 minutes:

- open and close the menu;
- navigate controls by keyboard;
- verify VoiceOver labels for state and Start/Pause/Resume/Stop;
- open the current file;
- verify recording state is expressed by text/icon, not color alone.

Pass criteria:

- no main-thread hang ≥1 s;
- p99 heartbeat ≤250 ms under normal load and ≤500 ms during injected slow
  publication;
- every control action is acknowledged from committed state.

## Continuous invariants

Checked after every successful snapshot:

- UTF-8 decodes strictly;
- frontmatter is complete and parseable YAML;
- exactly one transcript start marker and one end marker exist in order;
- only committed final segments appear;
- stable IDs/revisions in the rendered ownership block match the renderer
  manifest;
- no summary section, raw audio, embedding, credential, or test control payload
  appears;
- a pre-existing collision file is unchanged;
- the target is inside the selected directory;
- the snapshot digest matches the publication receipt.

## End-of-run reconciliation

Using one read transaction:

1. select the highest committed final revision per stable segment ID;
2. select durable state/source/discontinuity events;
3. select the last publication receipt;
4. independently parse the final Markdown;
5. compare the ordered segment manifest to renderer output;
6. verify the receipt digest and terminal phase/status mapping;
7. verify every offered frame is accepted or explicitly rejected;
8. verify only injected/known discontinuities occurred.

Required equalities:

```text
journal highest-final IDs/revisions
    == renderer manifest IDs/revisions
    == published transcript IDs/revisions

last journal committed revision
    == last publication receipt revision

accepted frames + explicit rejected frames
    == offered frames
```

## Pass/fail thresholds

Hard failures:

- any committed final segment missing, duplicated at the same revision, or
  silently downgraded;
- corrupted/truncated Markdown or frontmatter;
- unreported source loss or rejected frame;
- capture callback performing inference/filesystem work;
- user external edit silently overwritten;
- recording before explicit consent;
- network connection attempt from the product process;
- UI hang ≥1 s;
- incorrect terminal `complete` after an unrecovered required-source failure.

Performance gates:

- median live partial latency ≤1.5 s;
- final segment ≤8 s after cue end;
- first final in vault ≤15 s after first speech;
- capture enqueue p99 ≤5 ms;
- journal commit p99 ≤100 ms on the test machine;
- no monotonic resident-memory growth after warm-up (linear slope below
  1 MiB/hour and final RSS below the agreed model-specific budget);
- no file descriptor growth after source reconnect cycles.

Model quality WER/DER gates use a separate benchmark corpus. A soak test proves
durability and latency, not recognition accuracy.

## Voice-profile cross-call companion

Persistent speaker naming needs a multi-session companion; it is not proven by
one long recording. The automated form uses deterministic embeddings, and the
signed-app form uses consented, retention-controlled speech from at least two
people:

1. record call A, leave two remote clusters anonymous, and explicitly save one
   cluster with a name;
2. verify the durable call-A segments and republished Markdown use that name,
   while the other cluster remains `Speaker N`;
3. close the session and core, relaunch the application, then record call B with
   different words from the enrolled and an unknown speaker;
4. verify only compatible, unambiguous evidence receives the saved name; short,
   close-score, corrupt, wrong-dimension, and wrong-model fixtures remain
   anonymous;
5. rename the profile and verify a later call uses the new name without silently
   rewriting unrelated historical files;
6. delete the profile, relaunch, and verify the same fixture is anonymous;
7. run SQLite integrity/foreign-key checks and verify raw audio and descriptor
   bytes never appear in Markdown, diagnostics, or crash metadata.

The real-audio evidence separates enrollment and evaluation calls and varies
phrase, gain, playback device, background noise, RU/EN speech, and overlap. It
reports false accepts, false rejects, anonymous coverage, and named-speaker DER;
a deterministic pass is not a biometric-accuracy claim.

## Crash-recovery companion

A separate automated companion run is mandatory because killing the product
would otherwise invalidate the continuous two-hour UI run:

1. start a deterministic session and wait for at least 100 committed finals;
2. kill the process after a journal commit but before a delayed Markdown
   publication;
3. relaunch;
4. verify one idempotent `recovery_required` event;
5. verify all 100+ committed finals are re-rendered;
6. finalize as `interrupted`;
7. repeat kills at `recording`, `paused`, and `finalizing`;
8. repeat with vault unavailable and with an externally modified target.
9. recover two rows where the first publication fails and the second succeeds;
   verify the first remains presented and Retry drains only pending work.

Every recovered output must contain the stable session ID and must never receive
`complete`.

## Evidence bundle

Keep content-free artifacts:

- exact build/version and model/backend IDs;
- environment and permission state;
- runner configuration and deterministic random seed;
- aggregated metrics and fault timeline;
- journal integrity result (`quick_check` and foreign-key check);
- final manifest/digest comparison;
- memory/CPU/thermal charts;
- Markdown schema/golden validation result;
- network-deny result;
- signed operator checklist for the real-capture run.

The transcript and audio remain only in the designated test storage and are
removed according to the test retention policy.
