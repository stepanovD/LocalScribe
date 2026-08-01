import XCTest

@testable import LocalScribeApp

final class CaptureContractTests: XCTestCase {
    func testCaptureTimelineMarksEpochAndClockDiscontinuities() {
        let timeline = CaptureTimeline()
        timeline.resetEpoch()

        let first = timeline.next(
            timestampNanoseconds: 1_000_000_000,
            frameCount: 480,
            sampleRateHz: 48_000,
            channelCount: 1
        )
        XCTAssertEqual(first.sequenceNumber, 0)
        XCTAssertTrue(first.flags.contains(.discontinuity))

        let contiguous = timeline.next(
            timestampNanoseconds: 1_010_000_000,
            frameCount: 480,
            sampleRateHz: 48_000,
            channelCount: 1
        )
        XCTAssertEqual(contiguous.sequenceNumber, 1)
        XCTAssertFalse(contiguous.flags.contains(.discontinuity))

        let changedFormat = timeline.next(
            timestampNanoseconds: 1_020_000_000,
            frameCount: 480,
            sampleRateHz: 48_000,
            channelCount: 2
        )
        XCTAssertEqual(changedFormat.sequenceNumber, 2)
        XCTAssertTrue(changedFormat.flags.contains(.discontinuity))

        timeline.markDiscontinuity()
        let forced = timeline.next(
            timestampNanoseconds: 1_030_000_000,
            frameCount: 480,
            sampleRateHz: 48_000,
            channelCount: 2
        )
        XCTAssertEqual(forced.sequenceNumber, 3)
        XCTAssertTrue(forced.flags.contains(.discontinuity))
    }

    func testAudioFrameRejectsInconsistentSampleStorage() {
        XCTAssertThrowsError(
            try CapturedAudioFrame(
                sourceID: 1,
                sequenceNumber: 0,
                monotonicTimeNanoseconds: 1,
                sampleRateHz: 48_000,
                channelCount: 2,
                frameCount: 8,
                flags: [],
                interleavedSamples: [Float](repeating: 0, count: 8)
            )
        )
    }

    func testCaptureMailboxCoalescesPressureButRetainsHealth() async {
        let mailbox = CaptureEventMailbox()
        var iterator = mailbox.stream.makeAsyncIterator()
        mailbox.beginHealthAttempt()

        for _ in 0..<1_000 {
            mailbox.yield(.frameRejected(.microphone))
        }
        mailbox.yield(.ready(.systemAudio))
        mailbox.yield(.failed(.microphone, code: "route_lost"))
        mailbox.yield(.stopped(.systemAudio))

        var observed: [CaptureSourceEvent] = []
        for _ in 0..<4 {
            guard let event = await iterator.next() else {
                return XCTFail("capture mailbox ended unexpectedly")
            }
            observed.append(event)
            mailbox.didConsume(event)
        }
        XCTAssertEqual(
            observed,
            [
                .frameRejected(.microphone),
                .ready(.systemAudio),
                .failed(.microphone, code: "route_lost"),
                .stopped(.systemAudio),
            ]
        )
        XCTAssertFalse(mailbox.requiredSourcesAreReady)

        mailbox.yield(.ready(.microphone))
        mailbox.yield(.ready(.systemAudio))
        XCTAssertTrue(mailbox.requiredSourcesAreReady)
        mailbox.finish()
    }
}
