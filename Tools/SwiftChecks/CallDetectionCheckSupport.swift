import Foundation

private enum CallDetectionCheckError: Error {
    case invariant(String)
}

func runCallDetectionCheck() async throws {
    let matcher = CallDetectionMatcher()

    try requireCallDetection(
        CallPlatform.proposalPriority.count == CallPlatform.allCases.count
            && Set(CallPlatform.proposalPriority)
                == Set(CallPlatform.allCases),
        "the proposal priority does not cover every call platform exactly once"
    )

    let idleZoom = CallProcessObservation(
        processIdentifier: 10,
        bundleIdentifier: "us.zoom.xos"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [idleZoom],
                audioInputProcesses: [],
                windows: []
            )
        ) == [],
        "an idle Zoom client was classified as a call"
    )

    let zoomMeetingHost = CallProcessObservation(
        processIdentifier: 11,
        bundleIdentifier: "us.zoom.zCCIMeetingHost"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [idleZoom, zoomMeetingHost],
                audioInputProcesses: [],
                windows: []
            )
        ) == [.zoom],
        "the Zoom meeting host was not classified as a call"
    )

    let telemost = CallProcessObservation(
        processIdentifier: 20,
        bundleIdentifier: "ru.yandex.desktop.telemost"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [telemost],
                audioInputProcesses: [telemost],
                windows: []
            )
        ) == [.yandexTelemost],
        "active native Telemost input was not classified as a call"
    )

    let skype = CallProcessObservation(
        processIdentifier: 21,
        bundleIdentifier: "com.skype.skype"
    )
    let skypeHelper = CallProcessObservation(
        processIdentifier: 22,
        bundleIdentifier: "com.skype.skype.helper"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [skype],
                audioInputProcesses: [skypeHelper],
                windows: []
            )
        ) == [.skype],
        "active native Skype input was not classified as a call"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [skype],
                audioInputProcesses: [],
                windows: []
            )
        ) == [],
        "an idle Skype client was classified as a call"
    )
    let skypeForBusiness = CallProcessObservation(
        processIdentifier: 23,
        bundleIdentifier: "com.microsoft.SkypeForBusiness"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [skypeForBusiness],
                audioInputProcesses: [skypeForBusiness],
                windows: []
            )
        ) == [.skype],
        "active Skype for Business input was not classified as a call"
    )
    let skypeUpdater = CallProcessObservation(
        processIdentifier: 24,
        bundleIdentifier: "com.skype.skype.updater"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [skypeUpdater],
                audioInputProcesses: [skypeUpdater],
                windows: []
            )
        ) == [],
        "an unrecognized Skype bundle prefix was classified as a call"
    )

    let chromeHelper = CallProcessObservation(
        processIdentifier: 31,
        bundleIdentifier: "com.google.Chrome.helper"
    )
    let chrome = CallProcessObservation(
        processIdentifier: 30,
        bundleIdentifier: "com.google.Chrome"
    )
    let telemostWindow = CallWindowObservation(
        ownerProcessIdentifier: chrome.processIdentifier,
        ownerBundleIdentifier: chrome.bundleIdentifier,
        title: "Project room — Yandex Telemost"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [chrome, chromeHelper],
                audioInputProcesses: [chromeHelper],
                windows: [telemostWindow]
            )
        ) == [.yandexTelemost],
        "same-family browser Telemost evidence was not matched"
    )

    let meetPWA = CallProcessObservation(
        processIdentifier: 32,
        bundleIdentifier: "com.google.Chrome.app.meet.google.com"
    )
    let meetWindow = CallWindowObservation(
        ownerProcessIdentifier: meetPWA.processIdentifier,
        ownerBundleIdentifier: meetPWA.bundleIdentifier,
        title: "Meet - abc-defg-hij"
    )
    let skypeDialPadWindow = CallWindowObservation(
        ownerProcessIdentifier: chrome.processIdentifier,
        ownerBundleIdentifier: chrome.bundleIdentifier,
        title: "Skype Dial Pad"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [chrome, chromeHelper, meetPWA],
                audioInputProcesses: [chromeHelper],
                windows: [meetWindow, skypeDialPadWindow]
            )
        ) == [.googleMeet],
        "Google Meet PWA was missed or idle Skype Dial Pad was matched"
    )

    let telemostLandingWindow = CallWindowObservation(
        ownerProcessIdentifier: chrome.processIdentifier,
        ownerBundleIdentifier: chrome.bundleIdentifier,
        title: "Яндекс Телемост — чаты и видеозвонки"
    )
    let backgroundZoomWindow = CallWindowObservation(
        ownerProcessIdentifier: chrome.processIdentifier,
        ownerBundleIdentifier: chrome.bundleIdentifier,
        title: "Weekly Zoom Meeting",
        isOnScreen: false
    )
    let meetLandingWindow = CallWindowObservation(
        ownerProcessIdentifier: chrome.processIdentifier,
        ownerBundleIdentifier: chrome.bundleIdentifier,
        title: "Google Meet: Online Video Calls"
    )
    let skypeSupportWindow = CallWindowObservation(
        ownerProcessIdentifier: chrome.processIdentifier,
        ownerBundleIdentifier: chrome.bundleIdentifier,
        title: "Skype is retiring — Microsoft Support"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [chrome, chromeHelper],
                audioInputProcesses: [chromeHelper],
                windows: [
                    telemostLandingWindow,
                    backgroundZoomWindow,
                    meetLandingWindow,
                    skypeSupportWindow,
                ]
            )
        ) == [],
        "a landing, support, or background window was classified as a call"
    )

    let titleOnlyMeetEvidence = matcher.evidence(
        in: CallEnvironmentSnapshot(
            runningApplications: [meetPWA],
            audioInputProcesses: [],
            windows: [meetWindow]
        )
    )
    try requireCallDetection(
        titleOnlyMeetEvidence?.beginningPlatforms.isEmpty == true
            && titleOnlyMeetEvidence?.sustainingPlatforms == [.googleMeet],
        "a recognized browser title did not sustain without beginning an episode"
    )

    let safariInput = CallProcessObservation(
        processIdentifier: 40,
        bundleIdentifier: "com.apple.WebKit.WebContent"
    )
    try requireCallDetection(
        matcher.matchingPlatforms(
            in: CallEnvironmentSnapshot(
                runningApplications: [chrome, safariInput],
                audioInputProcesses: [safariInput],
                windows: [telemostWindow]
            )
        ) == [],
        "audio evidence from a different browser family was combined"
    )

    var reducer = CallDetectionReducer(
        beginConfirmationCount: 2,
        endGraceCount: 3
    )
    try requireCallDetection(
        reducer.reduce([.zoom]).isEmpty,
        "one transient Zoom sample began an episode"
    )
    let began = reducer.reduce([.zoom])
    guard case let .began(proposal)? = began.first,
          began.count == 1,
          proposal.platform == .zoom
    else {
        throw CallDetectionCheckError.invariant(
            "two stable Zoom samples did not begin one episode"
        )
    }
    let hiddenZoomEvidence = matcher.evidence(
        in: CallEnvironmentSnapshot(
            runningApplications: [chrome, chromeHelper],
            audioInputProcesses: [chromeHelper],
            windows: [backgroundZoomWindow]
        )
    )
    for _ in 0..<5 {
        try requireCallDetection(
            reducer.reduceEvidence(hiddenZoomEvidence).isEmpty,
            "a hidden browser-call window ended an active episode"
        )
    }
    try requireCallDetection(
        reducer.reduce([.zoom]).isEmpty && reducer.reduce(nil).isEmpty,
        "an active or unknown sample duplicated an episode"
    )
    try requireCallDetection(
        reducer.reduce([]).isEmpty && reducer.reduce([]).isEmpty,
        "the end grace period was shorter than configured"
    )
    let ended = reducer.reduce([])
    try requireCallDetection(
        ended == [.ended(proposal)],
        "the stable call end did not close the original episode"
    )
    try requireCallDetection(
        reducer.reduce([.zoom]).isEmpty,
        "a new episode ignored onset debounce"
    )
    guard case .began = reducer.reduce([.zoom]).first else {
        throw CallDetectionCheckError.invariant(
            "a new call after a confirmed end was not detected"
        )
    }

    var independentReducer = CallDetectionReducer(
        beginConfirmationCount: 2,
        endGraceCount: 3
    )
    try requireCallDetection(
        independentReducer.reduce([.googleMeet, .skype]).isEmpty,
        "Meet or Skype ignored onset debounce"
    )
    let simultaneousBegan = independentReducer.reduce([
        .googleMeet,
        .skype,
    ])
    let simultaneousProposals = simultaneousBegan.compactMap { event in
        if case let .began(proposal) = event {
            return proposal
        }
        return nil
    }
    guard simultaneousProposals.count == 2,
          let meetProposal = simultaneousProposals.first(where: {
              $0.platform == .googleMeet
          }),
          let skypeProposal = simultaneousProposals.first(where: {
              $0.platform == .skype
          })
    else {
        throw CallDetectionCheckError.invariant(
            "simultaneous Meet and Skype calls did not begin independently"
        )
    }
    try requireCallDetection(
        independentReducer.reduce([.skype]).isEmpty
            && independentReducer.reduce(nil).isEmpty
            && independentReducer.reduce([.skype]).isEmpty,
        "Meet end grace or unknown-sample freeze was not preserved"
    )
    try requireCallDetection(
        independentReducer.reduce([.skype]) == [.ended(meetProposal)],
        "ending Meet affected the simultaneous Skype episode"
    )
    try requireCallDetection(
        independentReducer.reduce([]).isEmpty
            && independentReducer.reduce([]).isEmpty
            && independentReducer.reduce([]) == [.ended(skypeProposal)],
        "the independent Skype episode did not end after its grace period"
    )

    let autoStopNow = ContinuousClock().now
    var autoStop = DetectedCallAutoStopReducer(platform: .zoom)
    for sampleCount in 1..<10 {
        try requireCallDetection(
            autoStop.observe(.known([]), now: autoStopNow).isEmpty
                && autoStop.state
                    == .confirming(negativeSampleCount: sampleCount),
            "auto-stop did not require ten consecutive known-negative samples"
        )
    }
    try requireCallDetection(
        autoStop.observe(.unknown, now: autoStopNow).isEmpty
            && autoStop.state == .confirming(negativeSampleCount: 9),
        "an unknown presence sample advanced auto-stop confirmation"
    )
    let autoStopDeadline = autoStopNow.advanced(by: .seconds(10))
    try requireCallDetection(
        autoStop.observe(.known([]), now: autoStopNow)
            == [.countdownStarted(deadline: autoStopDeadline)]
            && autoStop.state == .countdown(deadline: autoStopDeadline),
        "ten known-negative samples did not start the auto-stop countdown"
    )
    try requireCallDetection(
        autoStop.tick(
            now: autoStopNow.advanced(by: .seconds(9))
        ).isEmpty
            && autoStop.tick(now: autoStopDeadline) == [.stopRequested]
            && autoStop.tick(
                now: autoStopDeadline.advanced(by: .seconds(1))
            ).isEmpty
            && autoStop.state == .stopRequested,
        "the auto-stop countdown did not request exactly one due stop"
    )

    let shortAutoStopConfiguration =
        DetectedCallAutoStopReducer.Configuration(
            requiredNegativeSamples: 1,
            countdownDuration: .seconds(10),
            snoozeDuration: .seconds(300)
        )
    var uncertainAutoStop = DetectedCallAutoStopReducer(
        platform: .zoom,
        configuration: shortAutoStopConfiguration
    )
    let uncertainDeadline = autoStopNow.advanced(by: .seconds(10))
    try requireCallDetection(
        uncertainAutoStop.observe(.known([]), now: autoStopNow)
            == [.countdownStarted(deadline: uncertainDeadline)],
        "the injected auto-stop threshold was ignored"
    )
    _ = uncertainAutoStop.observe(
        .unknown,
        now: autoStopNow.advanced(by: .seconds(5))
    )
    try requireCallDetection(
        uncertainAutoStop.tick(now: uncertainDeadline).isEmpty,
        "an unknown presence sample allowed a due auto-stop"
    )
    let knownAgainAt = autoStopNow.advanced(by: .seconds(20))
    let restartedDeadline = knownAgainAt.advanced(by: .seconds(10))
    try requireCallDetection(
        uncertainAutoStop.observe(.known([]), now: knownAgainAt)
            == [.countdownStarted(deadline: restartedDeadline)]
            && uncertainAutoStop.state
                == .countdown(deadline: restartedDeadline),
        "known absence after uncertainty did not restart a full countdown"
    )
    try requireCallDetection(
        uncertainAutoStop.observe(.known([.zoom]), now: knownAgainAt)
            == [.monitoringResumed]
            && uncertainAutoStop.state == .monitoring,
        "same-platform recovery did not cancel and rearm auto-stop"
    )

    var snoozedAutoStop = DetectedCallAutoStopReducer(
        platform: .zoom,
        configuration: shortAutoStopConfiguration
    )
    _ = snoozedAutoStop.observe(.known([.googleMeet]), now: autoStopNow)
    let snoozeDeadline = autoStopNow.advanced(by: .seconds(300))
    try requireCallDetection(
        snoozedAutoStop.keepRecording(now: autoStopNow)
            == [.snoozed(deadline: snoozeDeadline)]
            && snoozedAutoStop.state == .snoozed(deadline: snoozeDeadline),
        "Keep Recording did not snooze auto-stop for five minutes"
    )
    _ = snoozedAutoStop.observe(
        .unknown,
        now: snoozeDeadline.advanced(by: .seconds(-1))
    )
    try requireCallDetection(
        snoozedAutoStop.tick(now: snoozeDeadline).isEmpty,
        "unknown presence ended the auto-stop snooze"
    )
    let snoozeKnownAt = snoozeDeadline.advanced(by: .seconds(20))
    let snoozeRestartedDeadline = snoozeKnownAt.advanced(by: .seconds(10))
    try requireCallDetection(
        snoozedAutoStop.observe(.known([]), now: snoozeKnownAt)
            == [.countdownStarted(deadline: snoozeRestartedDeadline)]
            && snoozedAutoStop.state
                == .countdown(deadline: snoozeRestartedDeadline),
        "known absence after a frozen snooze did not retrigger the warning"
    )

    var offerLedger = CallDetectionOfferLedger()
    let meetPriorityProbe = DetectedCallProposal(
        id: UUID(
            uuidString: "10101010-1111-2222-3333-444444444444"
        )!,
        platform: .googleMeet
    )
    let skypePriorityProbe = DetectedCallProposal(
        id: UUID(
            uuidString: "20202020-1111-2222-3333-444444444444"
        )!,
        platform: .skype
    )
    let deferred = DetectedCallProposal(
        id: UUID(
            uuidString: "40404040-1111-2222-3333-444444444444"
        )!,
        platform: .zoom
    )
    offerLedger.observeBegan(skypePriorityProbe, suppress: false)
    offerLedger.observeBegan(meetPriorityProbe, suppress: false)
    offerLedger.observeBegan(deferred, suppress: false)
    try requireCallDetection(
        offerLedger.nextOffer() == deferred
            && offerLedger.reserve(deferred)
            && offerLedger.nextOffer() == meetPriorityProbe,
        "the highest-priority call proposal was not reserved exactly once"
    )
    offerLedger.releaseRejectedReservation(deferred)
    try requireCallDetection(
        offerLedger.nextOffer() == deferred,
        "a proposal rejected while consent UI was busy could not retry"
    )
    try requireCallDetection(
        offerLedger.reserve(deferred),
        "a retried proposal could not be reserved"
    )
    offerLedger.observeEnded(deferred)
    try requireCallDetection(
        offerLedger.nextOffer() == meetPriorityProbe
            && offerLedger.reserve(meetPriorityProbe)
            && offerLedger.nextOffer() == skypePriorityProbe,
        "the stable provider proposal priority was not preserved"
    )
    offerLedger.observeEnded(meetPriorityProbe)
    offerLedger.observeEnded(skypePriorityProbe)
    try requireCallDetection(
        offerLedger.nextOffer() == nil,
        "ended provider calls remained in the proposal backlog"
    )

    weak var weakMonitor: CallDetectionMonitor?
    do {
        let monitor = CallDetectionMonitor(
            provider: EmptyCallEnvironmentCheckProvider(),
            pollingInterval: .milliseconds(10)
        )
        weakMonitor = monitor
        await monitor.start()
    }
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: .seconds(1))
    while weakMonitor != nil, clock.now < deadline {
        try? await Task.sleep(for: .milliseconds(10))
    }
    try requireCallDetection(
        weakMonitor == nil,
        "the call monitor retained itself after its owner released it"
    )
}

private struct EmptyCallEnvironmentCheckProvider:
    CallEnvironmentProviding
{
    func snapshot() async -> CallEnvironmentSnapshot? {
        CallEnvironmentSnapshot(
            runningApplications: [],
            audioInputProcesses: [],
            windows: []
        )
    }
}

private func requireCallDetection(
    _ condition: @autoclosure () -> Bool,
    _ message: String
) throws {
    if !condition() {
        throw CallDetectionCheckError.invariant(message)
    }
}
