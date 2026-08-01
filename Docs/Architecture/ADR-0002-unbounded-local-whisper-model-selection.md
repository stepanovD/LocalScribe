# ADR-0002: Unbounded local whisper.cpp model selection

- **Status:** Accepted
- **Date:** 2026-07-30
- **Product source:** LocalScribe Product Reference / PRD v0.4, 2026-07-27
- **Supersedes:** The tiny/base and 300 MiB selection boundary in ADR-0001

## Context

The Stage 0 picker originally accepted only `ggml-tiny*.bin` and
`ggml-base*.bin` files up to 300 MiB. That implementation boundary was not a
Product Reference requirement and prevented the user from selecting more
capable local whisper.cpp models.

Model size alone does not prove compatibility, available memory, acceptable
latency, or transcription quality. The backend already performs the
authoritative model load, and the Product Reference requires model-specific
benchmark evidence.

## Decision

- Accept any non-empty regular local file named `ggml-*.bin`.
- Do not impose an application-level maximum file size or model-family
  whitelist.
- Keep the security-scoped bookmark and regular-file checks.
- Let whisper.cpp reject corrupt or incompatible model contents during
  preflight, before capture starts.
- Explain in Settings that larger models may start more slowly and use more
  memory.
- Record exact model identity, startup latency, peak memory, thermal behavior,
  and real-ASR results for each release baseline.

## Consequences

Users can select `small`, `medium`, `large`, turbo, and quantized ggml model
families without rebuilding LocalScribe. This preserves offline operation and
the replaceable backend boundary.

The three-second consent-to-file target is no longer implied merely by passing
the picker. Large models may exceed it or fail on machines without enough
memory. Such a model is usable at the user's discretion but is not a supported
release baseline until the signed-app timing and two-hour soak evidence pass.
