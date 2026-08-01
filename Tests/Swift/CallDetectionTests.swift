import XCTest

@testable import LocalScribeApp

final class CallDetectionMatcherTests: XCTestCase {
    private let matcher = CallDetectionMatcher()

    func testNativeZoomTelemostAndSkypeWithActiveInputAreDetected() {
        let zoom = process(101, "us.zoom.xos")
        let telemost = process(202, "ru.yandex.desktop.telemost")
        let skype = process(203, "com.skype.skype")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [zoom, telemost, skype],
            audioInputProcesses: [zoom, telemost, skype],
            windows: []
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set([.zoom, .yandexTelemost, .skype])
        )
    }

    func testIdleSkypeClientIsNotDetected() {
        let skype = process(204, "com.skype.skype")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [skype],
            audioInputProcesses: [],
            windows: []
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testConsumerSkypeHelperIsDetectedWithInput() {
        let skype = process(205, "com.skype.skype")
        let skypeHelper = process(206, "com.skype.skype.helper")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [skype],
            audioInputProcesses: [skypeHelper],
            windows: []
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set([.skype])
        )
    }

    func testSkypeForBusinessIsDetectedWithInput() {
        let business = process(
            207,
            "com.microsoft.SkypeForBusiness"
        )
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [business],
            audioInputProcesses: [business],
            windows: []
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set([.skype])
        )
    }

    func testUnrecognizedSkypeBundlePrefixIsIgnored() {
        let updater = process(208, "com.skype.skype.updater")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [updater],
            audioInputProcesses: [updater],
            windows: []
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testIdleZoomBaseProcessIsNotDetected() {
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [process(101, "us.zoom.xos")],
            audioInputProcesses: [],
            windows: []
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testMeetingSpecificZoomHelperIsDetectedWithoutInput() {
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [
                process(102, "us.zoom.zCCIMeetingHost"),
            ],
            audioInputProcesses: [],
            windows: []
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set([.zoom])
        )
    }

    func testSameBrowserFamilyInputAndMeetingTitleAreDetected() {
        let chrome = process(301, "com.google.Chrome")
        let chromeInputHelper = process(302, "com.google.Chrome.helper")
        let yandexBrowser = process(
            401,
            "ru.yandex.desktop.yandex-browser"
        )
        let yandexInputHelper = process(
            402,
            "ru.yandex.desktop.yandex-browser.helper"
        )
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [chrome, yandexBrowser],
            audioInputProcesses: [chromeInputHelper, yandexInputHelper],
            windows: [
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Weekly Zoom Meeting"
                ),
                window(
                    ownerPID: yandexBrowser.processIdentifier,
                    ownerBundleID: nil,
                    title: "Командная встреча — Телемост"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set([.zoom, .yandexTelemost])
        )
    }

    func testGoogleMeetBrowserCallIsDetectedButSkypeDialPadIsNot() {
        let chrome = process(410, "com.google.Chrome")
        let chromeHelper = process(411, "com.google.Chrome.helper")
        let edge = process(420, "com.microsoft.edgemac")
        let edgeHelper = process(421, "com.microsoft.edgemac.helper")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [chrome, edge],
            audioInputProcesses: [chromeHelper, edgeHelper],
            windows: [
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Meet - abc-defg-hij"
                ),
                window(
                    ownerPID: edge.processIdentifier,
                    ownerBundleID: edge.bundleIdentifier,
                    title: "Skype Dial Pad"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set([.googleMeet])
        )
    }

    func testGoogleMeetPWAUsesChromeBrowserFamily() {
        let chrome = process(430, "com.google.Chrome")
        let meetPWA = process(
            431,
            "com.google.Chrome.app.meet.google.com"
        )
        let chromeHelper = process(432, "com.google.Chrome.helper")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [chrome, meetPWA],
            audioInputProcesses: [chromeHelper],
            windows: [
                window(
                    ownerPID: meetPWA.processIdentifier,
                    ownerBundleID: meetPWA.bundleIdentifier,
                    title: "abc-defg-hij – Google Meet"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set([.googleMeet])
        )
    }

    func testMeetAndSkypeTitlesWithoutBrowserInputAreIgnored() {
        let chrome = process(440, "com.google.Chrome")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [chrome],
            audioInputProcesses: [],
            windows: [
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Meet"
                ),
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Skype Dial Pad"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testMeetAndSkypeNonCallPagesAreIgnoredWithBrowserInput() {
        let chrome = process(450, "com.google.Chrome")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [chrome],
            audioInputProcesses: [chrome],
            windows: [
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Google Meet: Online Video Calls"
                ),
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Skype is retiring — Microsoft Support"
                ),
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Meet the team"
                ),
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Meet"
                ),
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Meet - Help"
                ),
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Help - Google Meet"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testMeetAndSkypeCrossBrowserEvidenceIsIgnored() {
        let safari = process(460, "com.apple.Safari")
        let chrome = process(461, "com.google.Chrome")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [safari, chrome],
            audioInputProcesses: [chrome],
            windows: [
                window(
                    ownerPID: safari.processIdentifier,
                    ownerBundleID: safari.bundleIdentifier,
                    title: "Meet - abc-defg-hij"
                ),
                window(
                    ownerPID: safari.processIdentifier,
                    ownerBundleID: safari.bundleIdentifier,
                    title: "Skype Dial Pad"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testCrossBrowserFamilyEvidenceIsIgnored() {
        let safari = process(501, "com.apple.Safari")
        let chrome = process(502, "com.google.Chrome")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [safari, chrome],
            audioInputProcesses: [chrome],
            windows: [
                // Zoom-looking evidence belongs to Safari, while input belongs
                // to Chrome, so the two browser families must not be joined.
                window(
                    ownerPID: safari.processIdentifier,
                    ownerBundleID: safari.bundleIdentifier,
                    title: "Zoom Meeting"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testUnrelatedBrowserTitleIsIgnored() {
        let chrome = process(502, "com.google.Chrome")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [chrome],
            audioInputProcesses: [chrome],
            windows: [
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Project notes"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testActiveInputWithTelemostLandingWindowIsIgnored() {
        let yandexBrowser = process(
            503,
            "ru.yandex.desktop.yandex-browser"
        )
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [yandexBrowser],
            audioInputProcesses: [yandexBrowser],
            windows: [
                window(
                    ownerPID: yandexBrowser.processIdentifier,
                    ownerBundleID: yandexBrowser.bundleIdentifier,
                    title: "Яндекс Телемост — чаты и видеозвонки"
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testBackgroundMeetingWindowIsIgnored() {
        let chrome = process(504, "com.google.Chrome")
        let snapshot = CallEnvironmentSnapshot(
            runningApplications: [chrome],
            audioInputProcesses: [chrome],
            windows: [
                window(
                    ownerPID: chrome.processIdentifier,
                    ownerBundleID: chrome.bundleIdentifier,
                    title: "Weekly Zoom Meeting",
                    isOnScreen: false
                ),
            ]
        )

        XCTAssertEqual(
            matcher.matchingPlatforms(in: snapshot),
            Set<CallPlatform>()
        )
    }

    func testUnavailableSnapshotRemainsUnknown() {
        XCTAssertNil(matcher.matchingPlatforms(in: nil))
    }

    private func process(
        _ processIdentifier: Int32,
        _ bundleIdentifier: String
    ) -> CallProcessObservation {
        CallProcessObservation(
            processIdentifier: processIdentifier,
            bundleIdentifier: bundleIdentifier
        )
    }

    private func window(
        ownerPID: Int32,
        ownerBundleID: String?,
        title: String?,
        isOnScreen: Bool = true
    ) -> CallWindowObservation {
        CallWindowObservation(
            ownerProcessIdentifier: ownerPID,
            ownerBundleIdentifier: ownerBundleID,
            title: title,
            isOnScreen: isOnScreen
        )
    }
}

final class CallDetectionReducerTests: XCTestCase {
    func testTwoPositiveSamplesBeginOnceAndFurtherSamplesDoNotDuplicate() throws {
        var reducer = CallDetectionReducer(
            beginConfirmationCount: 2,
            endGraceCount: 3
        )

        XCTAssertEqual(reducer.reduce([.zoom]), [])
        let began = try beganProposal(from: reducer.reduce([.zoom]))
        XCTAssertEqual(began.platform, .zoom)
        XCTAssertEqual(reducer.reduce([.zoom]), [])
        XCTAssertEqual(reducer.reduce([.zoom]), [])
    }

    func testUnknownSamplesFreezeEndGraceAndConfirmedEndAllowsNewEpisode() throws {
        var reducer = CallDetectionReducer(
            beginConfirmationCount: 2,
            endGraceCount: 3
        )
        XCTAssertEqual(reducer.reduce([.yandexTelemost]), [])
        let first = try beganProposal(
            from: reducer.reduce([.yandexTelemost])
        )

        XCTAssertEqual(reducer.reduce([]), [])
        XCTAssertEqual(reducer.reduce(nil), [])
        XCTAssertEqual(reducer.reduce([]), [])
        XCTAssertEqual(reducer.reduce(nil), [])
        let ended = try endedProposal(from: reducer.reduce([]))
        XCTAssertEqual(ended, first)

        XCTAssertEqual(reducer.reduce([]), [])
        XCTAssertEqual(reducer.reduce([.yandexTelemost]), [])
        let second = try beganProposal(
            from: reducer.reduce([.yandexTelemost])
        )
        XCTAssertEqual(second.platform, .yandexTelemost)
        XCTAssertNotEqual(second.id, first.id)
    }

    func testHiddenGoogleMeetWindowSustainsButCannotBeginEpisode() throws {
        let matcher = CallDetectionMatcher()
        let chrome = CallProcessObservation(
            processIdentifier: 700,
            bundleIdentifier: "com.google.Chrome"
        )
        let visible = CallEnvironmentSnapshot(
            runningApplications: [chrome],
            audioInputProcesses: [chrome],
            windows: [
                CallWindowObservation(
                    ownerProcessIdentifier: chrome.processIdentifier,
                    ownerBundleIdentifier: chrome.bundleIdentifier,
                    title: "Meet - abc-defg-hij",
                    isOnScreen: true
                ),
            ]
        )
        let hidden = CallEnvironmentSnapshot(
            runningApplications: [chrome],
            audioInputProcesses: [chrome],
            windows: [
                CallWindowObservation(
                    ownerProcessIdentifier: chrome.processIdentifier,
                    ownerBundleIdentifier: chrome.bundleIdentifier,
                    title: "Meet - abc-defg-hij",
                    isOnScreen: false
                ),
            ]
        )
        var reducer = CallDetectionReducer(
            beginConfirmationCount: 2,
            endGraceCount: 3
        )

        XCTAssertEqual(
            reducer.reduceEvidence(matcher.evidence(in: hidden)),
            []
        )
        XCTAssertEqual(
            reducer.reduceEvidence(matcher.evidence(in: visible)),
            []
        )
        let began = try beganProposal(
            from: reducer.reduceEvidence(matcher.evidence(in: visible))
        )
        XCTAssertEqual(began.platform, .googleMeet)
        for _ in 0..<5 {
            XCTAssertEqual(
                reducer.reduceEvidence(matcher.evidence(in: hidden)),
                []
            )
        }
    }

    func testGoogleMeetAndSkypeEpisodesRemainIndependent() throws {
        var reducer = CallDetectionReducer(
            beginConfirmationCount: 2,
            endGraceCount: 3
        )

        XCTAssertEqual(reducer.reduce([.googleMeet, .skype]), [])
        let began = reducer.reduce([.googleMeet, .skype])
        let beganProposals = began.compactMap { event in
            if case let .began(proposal) = event {
                return proposal
            }
            return nil
        }
        XCTAssertEqual(
            Set(beganProposals.map(\.platform)),
            Set([.googleMeet, .skype])
        )

        XCTAssertEqual(reducer.reduce([.skype]), [])
        XCTAssertEqual(reducer.reduce(nil), [])
        XCTAssertEqual(reducer.reduce([.skype]), [])
        let meetEnded = reducer.reduce([.skype])
        XCTAssertEqual(meetEnded.count, 1)
        guard case let .ended(endedMeet)? = meetEnded.first else {
            throw UnexpectedCallDetectionEvents(events: meetEnded)
        }
        XCTAssertEqual(endedMeet.platform, .googleMeet)

        XCTAssertEqual(reducer.reduce([]), [])
        XCTAssertEqual(reducer.reduce([]), [])
        let skypeEnded = try endedProposal(from: reducer.reduce([]))
        XCTAssertEqual(skypeEnded.platform, .skype)
    }

    private func beganProposal(
        from events: [CallDetectionEvent]
    ) throws -> DetectedCallProposal {
        guard events.count == 1, case let .began(proposal) = events[0] else {
            throw UnexpectedCallDetectionEvents(events: events)
        }
        return proposal
    }

    private func endedProposal(
        from events: [CallDetectionEvent]
    ) throws -> DetectedCallProposal {
        guard events.count == 1, case let .ended(proposal) = events[0] else {
            throw UnexpectedCallDetectionEvents(events: events)
        }
        return proposal
    }
}

final class CallDetectionOfferLedgerTests: XCTestCase {
    func testRejectedBusyReservationCanRetryButHandledEpisodeDoesNotRepeat() {
        var ledger = CallDetectionOfferLedger()
        let proposal = DetectedCallProposal(id: UUID(), platform: .zoom)
        ledger.observeBegan(proposal, suppress: false)

        XCTAssertEqual(ledger.nextOffer(), proposal)
        XCTAssertTrue(ledger.reserve(proposal))
        XCTAssertNil(ledger.nextOffer())

        ledger.releaseRejectedReservation(proposal)
        XCTAssertEqual(ledger.nextOffer(), proposal)
        XCTAssertTrue(ledger.reserve(proposal))
        XCTAssertNil(ledger.nextOffer())

        ledger.observeEnded(proposal)
        XCTAssertNil(ledger.nextOffer())
    }

    func testEpisodeBeginningDuringCaptureIsSuppressedUntilItsEnd() {
        var ledger = CallDetectionOfferLedger()
        let first = DetectedCallProposal(
            id: UUID(),
            platform: .yandexTelemost
        )
        ledger.observeBegan(first, suppress: true)
        XCTAssertNil(ledger.nextOffer())

        ledger.observeEnded(first)
        let second = DetectedCallProposal(
            id: UUID(),
            platform: .yandexTelemost
        )
        ledger.observeBegan(second, suppress: false)
        XCTAssertEqual(ledger.nextOffer(), second)
    }

    func testSimultaneousPlatformsUseStablePriority() {
        var ledger = CallDetectionOfferLedger()
        let telemost = DetectedCallProposal(
            id: UUID(),
            platform: .yandexTelemost
        )
        let zoom = DetectedCallProposal(id: UUID(), platform: .zoom)
        let meet = DetectedCallProposal(
            id: UUID(),
            platform: .googleMeet
        )
        let skype = DetectedCallProposal(id: UUID(), platform: .skype)
        ledger.observeBegan(skype, suppress: false)
        ledger.observeBegan(meet, suppress: false)
        ledger.observeBegan(telemost, suppress: false)
        ledger.observeBegan(zoom, suppress: false)

        XCTAssertEqual(ledger.nextOffer(), zoom)
        XCTAssertTrue(ledger.reserve(zoom))
        XCTAssertEqual(ledger.nextOffer(), telemost)
        XCTAssertTrue(ledger.reserve(telemost))
        XCTAssertEqual(ledger.nextOffer(), meet)
        XCTAssertTrue(ledger.reserve(meet))
        XCTAssertEqual(ledger.nextOffer(), skype)
    }

    func testNewPlatformDisplayNamesFlowIntoProposals() {
        XCTAssertEqual(
            DetectedCallProposal(id: UUID(), platform: .googleMeet)
                .applicationName,
            "Google Meet"
        )
        XCTAssertEqual(
            DetectedCallProposal(id: UUID(), platform: .skype)
                .applicationName,
            "Skype"
        )
    }
}

final class CallDetectionMonitorLifecycleTests: XCTestCase {
    func testMonitorCanDeallocateWithoutExplicitStop() async {
        weak var weakMonitor: CallDetectionMonitor?

        do {
            let monitor = CallDetectionMonitor(
                provider: EmptyCallEnvironmentProvider(),
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
        XCTAssertNil(weakMonitor)
    }
}

private struct EmptyCallEnvironmentProvider: CallEnvironmentProviding {
    func snapshot() async -> CallEnvironmentSnapshot? {
        CallEnvironmentSnapshot(
            runningApplications: [],
            audioInputProcesses: [],
            windows: []
        )
    }
}

private struct UnexpectedCallDetectionEvents: Error {
    let events: [CallDetectionEvent]
}
