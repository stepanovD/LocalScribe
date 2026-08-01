# ADR-0004: best-effort local call detection

- Status: accepted
- Date: 2026-08-01

## Context

LocalScribe should offer to start a transcript when a Zoom, Yandex Telemost,
Google Meet, or Skype call begins. Detection must not weaken the existing
consent boundary: observing a possible call is not permission to open the
microphone, capture system audio, create a core session, or write a transcript.

The supported applications do not expose a shared, stable call-presence API.
Native clients and browser-hosted calls also expose different operating-system
signals. Browser URL and foreign-window metadata can be privacy-gated by macOS,
so detection must remain best effort and manual start must remain available.

## Decision

The macOS shell owns a metadata-only detector with three layers:

1. A system adapter snapshots Core Audio process activity and visible Window
   Server metadata. It never creates an audio tap and never asks for a TCC
   permission.
2. A pure matcher recognizes the native Zoom meeting helper, active-input
   native Zoom/Telemost/Skype clients, and active-input allowlisted browsers
   whose same browser family has a recognized meeting window.
3. A pure episode reducer requires two consecutive positive samples, tolerates
   two missing samples, and ends an episode on the third. It emits at most one
   proposal per provider per episode, including after **Not Now**.

The adapter immediately reduces process/window metadata to `Sendable` values.
Raw window titles do not cross the matcher boundary, enter a proposal, reach
the journal, or appear in logs. The implementation does not read browser
history, send Apple Events, request Accessibility access, or use a local or
remote network service.

An accepted detection follows the existing shell states:

```text
idle/safe terminal -> detected -> awaiting_consent
awaiting_consent -> preparing       only from the visible Start button
awaiting_consent -> idle            Not Now or detected call ended
```

`SessionController` owns a UUID for the pending detected proposal. Start,
dismiss, and end actions must present that UUID, so a late event from one call
cannot affect a later proposal or a manual consent screen. The detector never
receives `VisibleConsentIssuer`; the issuer remains reachable only from the
visible **Start Recording** action.

The proposal is a single nonmodal floating `NSPanel`, because a `MenuBarExtra`
cannot be opened reliably by program logic. The panel has **Start Recording**
and **Not Now**, joins the current Space, does not bind Return as a default
action, and mirrors the same consent state in the menu. Closing the panel is
equivalent to **Not Now**. A detected end closes only its still-pending panel;
it never stops a recording that the user already started.

## Recognition rules

Production matching uses narrow identities and requires stable observations:

- Zoom desktop: the meeting-specific `us.zoom.zCCIMeetingHost` helper, or a
  known Zoom process actively using audio input;
- Yandex Telemost desktop: `ru.yandex.desktop.telemost` actively using audio
  input;
- Google Meet: a visible `Meet` meeting title containing the standard
  three-four-three meeting code, owned by an allowlisted browser family
  (including a Chrome-hosted Meet PWA) that is actively using audio input;
- Skype desktop: the `com.skype.skype` or
  `com.microsoft.skypeforbusiness` process family present and actively using
  audio input;
- browser calls: an allowlisted browser family actively using audio input and
  a same-family visible meeting title containing a supported signature.

Opening the Zoom home window, opening a Telemost or Google Meet landing page,
playing browser audio, opening the Skype Dial Pad, viewing a Skype support page,
or seeing a matching title in a different browser family is insufficient. If
several providers begin at once, proposal priority is explicitly Zoom,
Telemost, Google Meet, then Skype.

## Consequences

- Native Zoom, Yandex Telemost, and compatible Skype calls can be proposed
  without adding an entitlement or moving the TCC prompts before consent.
- Google Meet on macOS is browser-hosted, including its Chrome PWA. A Meet
  green room may activate input and produce a proposal shortly before the user
  joins; a muted or embedded Meet surface without matching window metadata may
  remain undetected.
- [Consumer Skype was retired in 2025](https://support.microsoft.com/en-us/skype/01af0c65-529f-4a4d-8e3a-a393033a359a).
  Its native matcher remains for installed legacy clients; Skype for Business
  shares the `Skype` proposal label. The browser Skype Dial Pad and Microsoft
  Teams are not classified as Skype because neither exposes a trustworthy
  Skype-call-specific local signal.
- Browser detection can be unavailable until macOS exposes foreign-window
  titles (for example after Screen Recording access was granted during an
  earlier explicitly started transcript). This is a false-negative preference,
  not a reason to broaden browser microphone activity into a call signal.
- Chromium-family helpers do not expose a public tab identity. A visible
  supported meeting title plus unrelated microphone use in another tab of the
  same browser profile can therefore still produce a proposal. Hidden
  matching windows may sustain an existing episode but cannot begin one. This
  residual ambiguity can also keep a dismissed browser episode latched while
  that hidden window and unrelated same-family input both remain. It is accepted
  because the result is consent UI only; it never starts capture, and the
  native detectors plus manual start remain available.
- Client bundle IDs and meeting-title signatures are integration contracts and
  require signed-app acceptance checks when a provider or macOS changes.
- Detection failures have no capture side effects; manual start remains the
  deterministic fallback.

## Verification

Deterministic tests cover signature matching, cross-browser evidence isolation,
onset debounce, end grace, deduplication, and new episodes after a confirmed
end. Controller checks preserve the stronger invariant: a proposal alone makes
zero permission, core-session, capture, or publication calls.

Signed-app acceptance should additionally exercise native and browser calls,
the Meet PWA, installed legacy Skype and Skype for Business, muted joins,
full-screen/minimized windows, **Not Now**, a second call after a completed
transcript, and first-run behavior before Screen Recording access.
