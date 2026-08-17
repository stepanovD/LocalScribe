import Foundation

enum CallPlatform: String, CaseIterable, Sendable {
    case zoom
    case yandexTelemost
    case googleMeet
    case skype

    static let proposalPriority: [CallPlatform] = [
        .zoom,
        .yandexTelemost,
        .googleMeet,
        .skype,
    ]

    var displayName: String {
        switch self {
        case .zoom:
            "Zoom"
        case .yandexTelemost:
            "Yandex Telemost"
        case .googleMeet:
            "Google Meet"
        case .skype:
            "Skype"
        }
    }
}

struct CallProcessObservation: Sendable, Equatable, Hashable {
    let processIdentifier: Int32
    let bundleIdentifier: String
}

struct CallWindowObservation: Sendable, Equatable {
    let ownerProcessIdentifier: Int32
    let ownerBundleIdentifier: String?
    let title: String?
    let isOnScreen: Bool

    init(
        ownerProcessIdentifier: Int32,
        ownerBundleIdentifier: String?,
        title: String?,
        isOnScreen: Bool = true
    ) {
        self.ownerProcessIdentifier = ownerProcessIdentifier
        self.ownerBundleIdentifier = ownerBundleIdentifier
        self.title = title
        self.isOnScreen = isOnScreen
    }
}

/// A short-lived, metadata-only view of the current desktop environment.
/// Window titles are used only by `CallDetectionMatcher` and never enter an
/// emitted event or durable application state.
struct CallEnvironmentSnapshot: Sendable, Equatable {
    let runningApplications: [CallProcessObservation]
    let audioInputProcesses: [CallProcessObservation]
    let windows: [CallWindowObservation]
}

enum CallDetectionEvent: Sendable, Equatable {
    case began(DetectedCallProposal)
    case ended(DetectedCallProposal)
}

/// A privacy-preserving call-presence sample emitted alongside episode events.
/// `known` contains only reduced platform identities; raw process and window
/// metadata never crosses the matcher boundary. `unknown` means the system
/// provider could not produce a trustworthy sample and must never be treated as
/// evidence that a call ended.
enum CallPresenceObservation: Sendable, Equatable {
    case known(Set<CallPlatform>)
    case unknown
}

struct CallDetectionEvidence: Sendable, Equatable {
    let beginningPlatforms: Set<CallPlatform>
    let sustainingPlatforms: Set<CallPlatform>
}

/// Turns conservative call-presence samples into an explicit auto-stop flow.
///
/// The reducer is intentionally independent of tasks and wall-clock reads. Its
/// owner supplies a monotonic `now` value and is responsible for protecting
/// timer callbacks with its own session/proposal generation. The target
/// platform is fixed at initialization so activity from another provider can
/// neither keep a recording alive nor cancel its pending stop.
struct DetectedCallAutoStopReducer: Sendable {
    struct Configuration: Sendable, Equatable {
        let requiredNegativeSamples: Int
        let countdownDuration: Duration
        let snoozeDuration: Duration

        init(
            requiredNegativeSamples: Int = 10,
            countdownDuration: Duration = .seconds(10),
            snoozeDuration: Duration = .seconds(300)
        ) {
            precondition(requiredNegativeSamples > 0)
            precondition(countdownDuration > .zero)
            precondition(snoozeDuration > .zero)
            self.requiredNegativeSamples = requiredNegativeSamples
            self.countdownDuration = countdownDuration
            self.snoozeDuration = snoozeDuration
        }
    }

    enum State: Sendable, Equatable {
        case monitoring
        case confirming(negativeSampleCount: Int)
        case countdown(deadline: ContinuousClock.Instant)
        case snoozed(deadline: ContinuousClock.Instant)
        case stopRequested
    }

    enum Effect: Sendable, Equatable {
        case monitoringResumed
        case countdownStarted(deadline: ContinuousClock.Instant)
        case snoozed(deadline: ContinuousClock.Instant)
        case stopRequested
    }

    private enum LatestPresence: Sendable, Equatable {
        case present
        case absent
        case unknown
    }

    let platform: CallPlatform
    private(set) var state: State = .monitoring

    private let configuration: Configuration
    private var latestPresence: LatestPresence = .unknown

    init(
        platform: CallPlatform,
        configuration: Configuration = Configuration()
    ) {
        self.platform = platform
        self.configuration = configuration
    }

    /// Reduces one poll result. Unknown observations freeze confirmation and
    /// deadline state; they never count as absence and never request a stop.
    mutating func observe(
        _ observation: CallPresenceObservation,
        now: ContinuousClock.Instant
    ) -> [Effect] {
        switch observation {
        case let .known(platforms):
            if platforms.contains(platform) {
                latestPresence = .present
                return observePresent()
            }

            let wasUnknown = latestPresence == .unknown
            latestPresence = .absent
            return observeAbsent(now: now, wasUnknown: wasUnknown)

        case .unknown:
            latestPresence = .unknown
            return []
        }
    }

    /// Advances an already-established countdown or snooze deadline. A due
    /// deadline remains frozen while the latest observation is unknown.
    mutating func tick(now: ContinuousClock.Instant) -> [Effect] {
        switch state {
        case let .countdown(deadline):
            guard now >= deadline, latestPresence == .absent else {
                return []
            }
            state = .stopRequested
            return [.stopRequested]

        case let .snoozed(deadline):
            guard now >= deadline, latestPresence == .absent else {
                return []
            }
            return startCountdown(now: now)

        case .monitoring, .confirming, .stopRequested:
            return []
        }
    }

    /// Keeps the current recording alive for the configured snooze duration.
    /// The action is accepted only while the warning countdown is active.
    mutating func keepRecording(
        now: ContinuousClock.Instant
    ) -> [Effect] {
        guard case .countdown = state else {
            return []
        }
        let deadline = now.advanced(by: configuration.snoozeDuration)
        state = .snoozed(deadline: deadline)
        return [.snoozed(deadline: deadline)]
    }

    private mutating func observePresent() -> [Effect] {
        switch state {
        case .monitoring, .stopRequested:
            return []
        case .confirming, .countdown, .snoozed:
            state = .monitoring
            return [.monitoringResumed]
        }
    }

    private mutating func observeAbsent(
        now: ContinuousClock.Instant,
        wasUnknown: Bool
    ) -> [Effect] {
        switch state {
        case .monitoring:
            return confirmAbsence(sampleCount: 1, now: now)

        case let .confirming(sampleCount):
            return confirmAbsence(sampleCount: sampleCount + 1, now: now)

        case let .countdown(deadline):
            // If an unknown sample covered the deadline, a later known absence
            // starts a full new countdown instead of stopping retroactively.
            if wasUnknown, now >= deadline {
                return startCountdown(now: now)
            }
            return []

        case let .snoozed(deadline):
            // The same rule applies at the end of a snooze: uncertainty never
            // consumes the user's warning interval.
            if now >= deadline {
                return startCountdown(now: now)
            }
            return []

        case .stopRequested:
            return []
        }
    }

    private mutating func confirmAbsence(
        sampleCount: Int,
        now: ContinuousClock.Instant
    ) -> [Effect] {
        if sampleCount >= configuration.requiredNegativeSamples {
            return startCountdown(now: now)
        }
        state = .confirming(negativeSampleCount: sampleCount)
        return []
    }

    private mutating func startCountdown(
        now: ContinuousClock.Instant
    ) -> [Effect] {
        let deadline = now.advanced(by: configuration.countdownDuration)
        state = .countdown(deadline: deadline)
        return [.countdownStarted(deadline: deadline)]
    }
}

/// Keeps stable call episodes available while the session UI is temporarily
/// busy. Reserving an episode suppresses duplicate proposals until its matching
/// end; a rejected reservation can be released and retried after a later shell
/// state change.
struct CallDetectionOfferLedger: Sendable {
    private var activeByPlatform:
        [CallPlatform: DetectedCallProposal] = [:]
    private var reservedOrHandledIDs = Set<UUID>()

    mutating func observeBegan(
        _ proposal: DetectedCallProposal,
        suppress: Bool
    ) {
        if let replaced = activeByPlatform[proposal.platform] {
            reservedOrHandledIDs.remove(replaced.id)
        }
        activeByPlatform[proposal.platform] = proposal
        if suppress {
            reservedOrHandledIDs.insert(proposal.id)
        }
    }

    mutating func observeEnded(_ proposal: DetectedCallProposal) {
        guard activeByPlatform[proposal.platform]?.id == proposal.id else {
            return
        }
        activeByPlatform[proposal.platform] = nil
        reservedOrHandledIDs.remove(proposal.id)
    }

    func nextOffer() -> DetectedCallProposal? {
        for platform in CallPlatform.proposalPriority {
            guard let proposal = activeByPlatform[platform],
                  !reservedOrHandledIDs.contains(proposal.id)
            else {
                continue
            }
            return proposal
        }
        return nil
    }

    mutating func reserve(_ proposal: DetectedCallProposal) -> Bool {
        guard activeByPlatform[proposal.platform]?.id == proposal.id,
              !reservedOrHandledIDs.contains(proposal.id)
        else {
            return false
        }
        reservedOrHandledIDs.insert(proposal.id)
        return true
    }

    mutating func releaseRejectedReservation(
        _ proposal: DetectedCallProposal
    ) {
        guard activeByPlatform[proposal.platform]?.id == proposal.id else {
            return
        }
        reservedOrHandledIDs.remove(proposal.id)
    }

    mutating func suppressAllActive() {
        reservedOrHandledIDs.formUnion(
            activeByPlatform.values.map(\.id)
        )
    }

    func contains(_ proposal: DetectedCallProposal) -> Bool {
        activeByPlatform[proposal.platform]?.id == proposal.id
    }

    mutating func removeAll() {
        activeByPlatform.removeAll()
        reservedOrHandledIDs.removeAll()
    }
}

struct CallDetectionMatcher: Sendable {
    private enum BundleID {
        static let zoom = "us.zoom.xos"
        static let zoomMeetingHost = "us.zoom.zccimeetinghost"
        static let zoomCaptureHost = "us.zoom.cpthost"
        static let yandexTelemost = "ru.yandex.desktop.telemost"
        static let skype = "com.skype.skype"
        static let skypeHelper = "com.skype.skype.helper"
        static let skypeForBusiness = "com.microsoft.skypeforbusiness"
    }

    private enum BrowserFamily: Hashable, Sendable {
        case safari
        case chrome
        case yandex
        case edge
        case firefox
        case chromium

        init?(bundleIdentifier: String) {
            let identifier = bundleIdentifier.lowercased()
            switch identifier {
            case let value where value == "com.apple.safari"
                || value.hasPrefix("com.apple.safari.")
                || value.hasPrefix("com.apple.webkit."):
                self = .safari
            case let value where value == "com.google.chrome"
                || value.hasPrefix("com.google.chrome."):
                self = .chrome
            case let value where value == "ru.yandex.desktop.yandex-browser"
                || value.hasPrefix("ru.yandex.desktop.yandex-browser."):
                self = .yandex
            case let value where value == "com.microsoft.edgemac"
                || value.hasPrefix("com.microsoft.edgemac."):
                self = .edge
            case let value where value == "org.mozilla.firefox"
                || value.hasPrefix("org.mozilla.firefox"):
                self = .firefox
            case let value where value == "org.chromium.chromium"
                || value.hasPrefix("org.chromium.chromium."):
                self = .chromium
            default:
                return nil
            }
        }
    }

    /// Returns `nil` when the provider could not produce a trustworthy
    /// snapshot. An unknown sample is intentionally different from a known
    /// sample with no matching call.
    func matchingPlatforms(
        in snapshot: CallEnvironmentSnapshot?
    ) -> Set<CallPlatform>? {
        evidence(in: snapshot)?.beginningPlatforms
    }

    /// Visible browser evidence can begin an episode. A matching hidden
    /// browser window can only sustain one that was already confirmed, so
    /// minimizing a call does not close its pending proposal and a background
    /// tab cannot create a new proposal by itself.
    func evidence(
        in snapshot: CallEnvironmentSnapshot?
    ) -> CallDetectionEvidence? {
        guard let snapshot else {
            return nil
        }

        let runningBundleIDs = Set(
            snapshot.runningApplications.map {
                $0.bundleIdentifier.lowercased()
            }
        )
        let inputBundleIDs = Set(
            snapshot.audioInputProcesses.map {
                $0.bundleIdentifier.lowercased()
            }
        )
        let inputProcessIDs = Set(
            snapshot.audioInputProcesses.map(\.processIdentifier)
        )
        let runningBundleIDByPID = Dictionary(
            snapshot.runningApplications.map {
                ($0.processIdentifier, $0.bundleIdentifier)
            },
            uniquingKeysWith: { first, _ in first }
        )

        var beginningPlatforms = Set<CallPlatform>()
        var sustainingPlatforms = Set<CallPlatform>()

        func insertStrong(_ platform: CallPlatform) {
            beginningPlatforms.insert(platform)
            sustainingPlatforms.insert(platform)
        }

        // zCCIMeetingHost has a meeting-specific lifetime and remains a
        // strong signal even when a user joins without microphone input.
        if runningBundleIDs.contains(BundleID.zoomMeetingHost) {
            insertStrong(.zoom)
        }

        // The main Zoom process can remain alive while idle, so it is only a
        // call signal while Zoom-owned input is actually running. CptHost is
        // treated the same way because it can outlive individual UI windows.
        let zoomHasActiveInput = inputBundleIDs.contains(BundleID.zoom)
            || inputBundleIDs.contains(BundleID.zoomMeetingHost)
            || inputBundleIDs.contains(BundleID.zoomCaptureHost)
        let zoomProcessIsPresent = runningBundleIDs.contains(BundleID.zoom)
            || runningBundleIDs.contains(BundleID.zoomMeetingHost)
            || runningBundleIDs.contains(BundleID.zoomCaptureHost)
        if zoomHasActiveInput && zoomProcessIsPresent {
            insertStrong(.zoom)
        }

        // Telemost's desktop process is not meeting-specific, so microphone
        // input is required before it becomes a call signal.
        if inputBundleIDs.contains(BundleID.yandexTelemost) {
            insertStrong(.yandexTelemost)
        }

        // Consumer Skype can remain installed after its service retirement,
        // while Skype for Business is a separate supported product. Both
        // clients can stay open when idle, so a known client family must be
        // present and actively using audio input before it becomes evidence.
        let skypeHasActiveInput = inputBundleIDs.contains(where: {
            isSkypeAudioBundleIdentifier($0)
        })
        let skypeProcessIsPresent = runningBundleIDs.contains(where: {
            isSkypeApplicationBundleIdentifier($0)
        })
        if skypeHasActiveInput && skypeProcessIsPresent {
            insertStrong(.skype)
        }

        let inputBrowserFamilies = Set(
            snapshot.audioInputProcesses.compactMap {
                BrowserFamily(bundleIdentifier: $0.bundleIdentifier)
            }
        )

        // A recognized browser title is safe sustaining evidence for an
        // already-active episode even if input temporarily disappears because
        // the participant muted or the browser reconfigured its audio route.
        // Beginning remains stricter: the title and active input must belong to
        // the same allowlisted browser family, and the window must be visible.
        for window in snapshot.windows {
            guard let titlePlatform = platform(forWindowTitle: window.title)
            else {
                continue
            }
            let ownerBundleIdentifier = window.ownerBundleIdentifier
                ?? runningBundleIDByPID[window.ownerProcessIdentifier]
            guard let ownerBundleIdentifier,
                  let browserFamily = BrowserFamily(
                      bundleIdentifier: ownerBundleIdentifier
                  )
            else {
                continue
            }
            sustainingPlatforms.insert(titlePlatform)
            let hasActiveInput = inputProcessIDs.contains(
                window.ownerProcessIdentifier
            ) || inputBrowserFamilies.contains(browserFamily)
            if window.isOnScreen && hasActiveInput {
                beginningPlatforms.insert(titlePlatform)
            }
        }

        return CallDetectionEvidence(
            beginningPlatforms: beginningPlatforms,
            sustainingPlatforms: sustainingPlatforms
        )
    }

    private func platform(forWindowTitle title: String?) -> CallPlatform? {
        guard let title, !title.isEmpty else {
            return nil
        }
        let normalized = title.folding(
            options: [.caseInsensitive, .diacriticInsensitive],
            locale: Locale(identifier: "en_US_POSIX")
        ).lowercased()
        if normalized.contains("zoom meeting")
            || normalized.contains("zoom webinar")
        {
            return .zoom
        }
        if normalized.contains("telemost")
            || normalized.contains("телемост")
        {
            let landingPageSignatures = [
                "чаты и видеозвонки",
                "видеовстречи без регистрации",
                "telemost — видеовстречи",
                "free online chats and calls",
                "free video meetings",
            ]
            guard !landingPageSignatures.contains(where: {
                normalized.contains($0)
            }) else {
                return nil
            }
            return .yandexTelemost
        }
        if isGoogleMeetCallTitle(normalized) {
            return .googleMeet
        }
        return nil
    }

    private func isGoogleMeetCallTitle(_ normalized: String) -> Bool {
        let meetingCodePattern = #"\b[a-z]{3}-[a-z]{4}-[a-z]{3}\b"#
        guard normalized.range(
            of: meetingCodePattern,
            options: .regularExpression
        ) != nil else {
            return false
        }

        let meetingPrefixes = [
            "meet - ",
            "meet – ",
            "meet — ",
        ]
        if meetingPrefixes.contains(where: normalized.hasPrefix) {
            return true
        }

        let meetingSuffixes = [
            " - google meet",
            " – google meet",
            " — google meet",
        ]
        return meetingSuffixes.contains(where: normalized.hasSuffix)
    }

    private func isSkypeApplicationBundleIdentifier(
        _ identifier: String
    ) -> Bool {
        identifier == BundleID.skype
            || identifier == BundleID.skypeForBusiness
    }

    private func isSkypeAudioBundleIdentifier(_ identifier: String) -> Bool {
        isSkypeApplicationBundleIdentifier(identifier)
            || identifier == BundleID.skypeHelper
    }
}

struct CallDetectionReducer: Sendable {
    private struct PlatformState: Sendable {
        var positiveSamples = 0
        var negativeSamples = 0
        var activeProposal: DetectedCallProposal?
    }

    private let beginConfirmationCount: Int
    private let endGraceCount: Int
    private var states: [CallPlatform: PlatformState] = [:]

    init(
        beginConfirmationCount: Int = 2,
        endGraceCount: Int = 3
    ) {
        precondition(beginConfirmationCount > 0)
        precondition(endGraceCount > 0)
        self.beginConfirmationCount = beginConfirmationCount
        self.endGraceCount = endGraceCount
    }

    /// Reduces one matcher result into stable episode events. Passing `nil`
    /// freezes all counters: a provider failure is not evidence that a call
    /// ended.
    mutating func reduce(
        _ matchingPlatforms: Set<CallPlatform>?
    ) -> [CallDetectionEvent] {
        guard let matchingPlatforms else {
            return []
        }

        return reduceEvidence(
            CallDetectionEvidence(
                beginningPlatforms: matchingPlatforms,
                sustainingPlatforms: matchingPlatforms
            )
        )
    }

    mutating func reduceEvidence(
        _ evidence: CallDetectionEvidence?
    ) -> [CallDetectionEvent] {
        guard let evidence else {
            return []
        }

        var events: [CallDetectionEvent] = []
        for platform in CallPlatform.proposalPriority {
            var state = states[platform] ?? PlatformState()
            if state.activeProposal != nil,
               evidence.sustainingPlatforms.contains(platform)
            {
                state.negativeSamples = 0
                state.positiveSamples = 0
            } else if state.activeProposal == nil,
                      evidence.beginningPlatforms.contains(platform)
            {
                state.negativeSamples = 0
                state.positiveSamples += 1
                if state.positiveSamples >= beginConfirmationCount {
                    let proposal = DetectedCallProposal(
                        id: UUID(),
                        platform: platform
                    )
                    state.activeProposal = proposal
                    state.positiveSamples = 0
                    events.append(.began(proposal))
                }
            } else {
                state.positiveSamples = 0
                if let proposal = state.activeProposal {
                    state.negativeSamples += 1
                    if state.negativeSamples >= endGraceCount {
                        state.activeProposal = nil
                        state.negativeSamples = 0
                        events.append(.ended(proposal))
                    }
                } else {
                    state.negativeSamples = 0
                }
            }
            states[platform] = state
        }
        return events
    }
}
