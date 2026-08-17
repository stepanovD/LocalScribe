# LocalScribe

LocalScribe turns audio capture explicitly started by the user into a recoverable, locally produced Markdown transcript. It keeps microphone and system audio distinct, may offer to start a Transcript Session when it detects a possible Call, and never treats detection or recovery as Recording Consent.

## Language

### Calls and capture

**Call**:
A real conversation through a supported Call Provider that LocalScribe may detect. A Call can exist without a Transcript Session, and a Transcript Session can be started without a detected Call.
_Avoid_: Session, recording, detection episode

**Call Provider**:
The service whose call presence LocalScribe observes, currently Zoom, Yandex Telemost, Google Meet, or Skype. For Call Detection and Call-End Warnings, any matching surface from the same provider counts as continuing presence.
_Avoid_: Platform, application, client

**Transcript Session**:
A single user-authorized attempt to produce one evolving Transcript, from preparation to a terminal outcome. It may be a Manual Session or a Call-Linked Session.
_Avoid_: Call, recording, transcript, job

**Manual Session**:
A Transcript Session started independently of a Call Proposal. Call Detection cannot stop it.
_Avoid_: Manual call, undetected call

**Call-Linked Session**:
A Transcript Session explicitly started from a Call Proposal. It is the only kind eligible for a Call-End Warning associated with that proposal.
_Avoid_: Detected recording, automatic recording

**Recording**:
The part of a Transcript Session during which new audio is accepted from its Audio Sources. Pausing suspends new audio without creating a new Transcript Session.
_Avoid_: Session, transcript, call

**Recording Consent**:
An explicit user action on a visible **Start Recording** or **Resume** control that authorizes the corresponding capture attempt. It is distinct from Call Detection and from persistent macOS Capture Permissions.
_Avoid_: Capture Permission, detection, implicit consent

**Capture Permissions**:
The macOS authorizations for LocalScribe to access microphone input and system audio capture. They are prerequisites for Recording, not Recording Consent.
_Avoid_: Consent, start approval

**Call Detection**:
A best-effort inference from local metadata that a supported Call may be present. It may produce a Call Proposal but never creates a Transcript Session, grants Recording Consent, or starts Recording.
_Avoid_: Call confirmation, automatic recording

**Call Proposal**:
A visible, temporary offer to begin a Transcript Session for a possible Call. The proposal itself records nothing.
_Avoid_: Consent, recording, session

**Call-End Warning**:
A visible countdown shown when the Call Provider associated with a Call-Linked Session appears to be absent. It offers a choice to keep recording or stop and is not proof that the Call ended.
_Avoid_: Confirmed call end, silent auto-stop

### Audio and transcription

**Audio Source**:
An independently tracked origin of captured audio: Microphone or System Audio. Sources retain distinct availability, timing, and speaker-attribution meaning.
_Avoid_: Channel, device, speaker

**Required Source**:
An Audio Source whose permanent loss or excessive absence changes the terminal Transcript Status to `incomplete_sources`.
_Avoid_: Mandatory device, required channel

**Source Gap**:
An interval in which a Required Source was unavailable. It is disclosed separately and is never converted into invented speech.
_Avoid_: Silence, Final Segment, failed session

**Capture Event**:
A timestamped, non-dialogue record of an Audio Source change or discontinuity that accompanies a Transcript.
_Avoid_: Final Segment, error message

**Final Segment**:
A stable, time-bounded unit of recognized speech attributed to one Audio Source and one Speaker Label. It is the only speech unit eligible to appear as a Transcript dialogue row.
_Avoid_: Chunk, partial hypothesis, speaker turn, row

**Transcript Language Mode**:
The per-session choice of Russian, English, or automatic selection between Russian and English. A default mode only preselects this choice for a new Transcript Session.
_Avoid_: Meeting language, model language, unrestricted auto-detect

**Local Transcription Model**:
A user-selected, on-device speech-recognition model required to begin a Transcript Session. LocalScribe never downloads model files.
_Avoid_: Cloud model, downloaded model

### Speakers and voice profiles

**Local Speaker**:
The person whose speech comes from the Microphone Audio Source and is labelled `Me` by default. The Local Speaker is never matched to a Voice Profile.
_Avoid_: User, microphone, participant identity

**Remote Speaker**:
A speaker heard through the System Audio Source. A Remote Speaker may remain an Anonymous Speaker or receive the name from a compatible Voice Profile.
_Avoid_: Remote audio, attendee, identity

**Speaker Label**:
The display attribution attached to a Final Segment: the Local Speaker name, an anonymous `Speaker N`, or a Voice Profile name. It is a convenience label, not proof of identity.
_Avoid_: Identity, authentication result, participant account

**Transcript Participant**:
A distinct Speaker Label that appears on at least one Final Segment in a Transcript. It is not a roster of everyone present in the Call.
_Avoid_: Attendee list, contact, call roster

**Anonymous Speaker**:
A session-scoped Remote Speaker label such as `Speaker 1` used when no Voice Profile can be applied confidently. It does not guarantee a one-to-one match with a physical person.
_Avoid_: Unknown person, identity, unnamed profile

**Voice Descriptor**:
Compact, sensitive voice-derived evidence retained locally for speaker matching. It is excluded from Transcripts, is neither raw audio nor proof of identity, and may remain in historical Transcript Session data after a Voice Profile is deleted.
_Avoid_: Voice recording, voiceprint, identity

**Voice Profile**:
An explicitly saved, on-device association between a display name and compatible Voice Descriptors used to label future Remote Speaker segments cautiously. It is not proof of identity; renaming or deleting it changes future matching without rewriting historical Transcripts.
_Avoid_: Contact, account, voiceprint, biometric identity

**Voice-Profile Enrollment**:
The explicit action that creates or enriches a Voice Profile from an Anonymous Speaker and relabels that speaker in the selected current or most recently completed Transcript Session.
_Avoid_: Automatic learning, recognition, simple rename

### Transcripts, publication, and recovery

**Transcript**:
The local Markdown document produced for one Transcript Session, containing metadata, Final Segments, and Capture Events. It may also contain user-authored content that LocalScribe preserves across updates.
_Avoid_: Session, recording, summary, raw audio

**Transcript Status**:
The lifecycle value written into a Transcript: `recording`, `complete`, `incomplete_sources`, or `interrupted`. `recording` means still in progress; `complete` means terminal with acceptable Required Sources; `incomplete_sources` means a Required Source was permanently lost or absent beyond the completeness policy; `interrupted` means the Transcript Session could not finish normally.
_Avoid_: Internal phase, publication status, save result

**Transcript Folder**:
The user-selected local folder for primary Transcript files. It may be inside an Obsidian vault or be an ordinary folder.
_Avoid_: Vault, Obsidian output, output directory

**Transcript Publication**:
The local saving of a particular Transcript version to a known destination. It is independent of Transcript Status, so a complete Transcript Session can remain recoverable until its current version is published.
_Avoid_: Network publishing, export, session completion

**Local Safety Storage**:
A private on-device location used when the Transcript Folder cannot be reached or updated safely. A Transcript stored there is durable recovery material, not a temporary file.
_Avoid_: Staging, cache, temporary directory, backup

**Recovery Copy**:
A separately named Transcript saved in the Transcript Folder when an externally changed target cannot be updated safely. It preserves the existing user-edited file rather than overwriting it.
_Avoid_: Recovered Transcript, Local Safety Storage, backup

**Recoverable Session**:
A durable Transcript Session with remaining terminal or publication work. It either did not reach a terminal Transcript Status or its current terminal Transcript has not been published successfully.
_Avoid_: Crashed session, interrupted session, incomplete session

**Session Recovery**:
The startup processing of Recoverable Sessions that publishes an unfinished Transcript Session as `interrupted` from its durable Final Segments or republishes an already terminal Transcript, without resuming Recording. It never recovers unfinalized speech, supplies Recording Consent, or starts an Audio Source.
_Avoid_: Resume Recording, source recovery, Recovery Copy
