# ADR-0003: One-line transcript segments

- **Status:** Accepted
- **Date:** 2026-07-30
- **Product source:** LocalScribe Product Reference / PRD v0.4, 2026-07-27
- **User decision:** Resolve the open heading-versus-bold-line format question
  in favor of a bold one-line label.

## Context

Schema version 1 rendered every final segment as a level-two heading followed
by a separate paragraph:

```markdown
## 00:00:31 — Speaker 1

Transcript text.
```

The requested Obsidian layout keeps one final segment on one physical line.
The literal spelling `**label : ** text` is not valid strong emphasis in
CommonMark because whitespace precedes the closing delimiter.

## Decision

New snapshots use schema version 2 and render each final segment as:

```markdown
**00:00:31 — Speaker 1 :** Transcript text.
```

- The time remains relative to the original session origin.
- The speaker label remains escaped plain text.
- Transcript line separators are folded to spaces.
- Transcript Markdown, Obsidian constructs, HTML, and managed ownership-marker
  syntax are escaped so ASR output cannot create document structure.
- Only final segments are rendered.
- Ownership markers and the SQLite journal schema do not change.

## Consequences

Existing completed schema-version-1 files are not migrated automatically.
New and recovered snapshots emitted by this build identify the new
representation with `schema_version: 2`. External processors should dispatch
on that value and continue to treat the exact ownership markers as the
transcript boundary.
