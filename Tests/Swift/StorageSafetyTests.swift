import Foundation
import XCTest

@testable import LocalScribeApp

final class StorageSafetyTests: XCTestCase {
    func testFilenameTemplatesCannotEscapeSelectedDirectory() {
        let sessionID = UUID(
            uuidString: "11111111-2222-3333-4444-555555555555"
        )!
        let filename = SafeFilename.markdownFilename(
            stem: "../../private\\secret:\u{0}\nCall",
            sessionID: sessionID,
            forceStableSuffix: true
        )

        XCTAssertTrue(SafeFilename.isSingleMarkdownComponent(filename))
        XCTAssertFalse(filename.contains("/"))
        XCTAssertFalse(filename.contains("\\"))
        XCTAssertFalse(filename.contains(":"))
        XCTAssertTrue(filename.hasSuffix(".md"))
    }

    func testManagedMergePreservesUserFieldsAndProse() throws {
        let existing = Data(
            """
            ---
            # prefix user comment
            type: "call-transcript"
            schema_version: 1
            status: "recording"
            # inter-key user comment
            session_id: "session-1"
            custom_user_key: "keep me"
            # suffix user comment
            ---

            User prose before.

            <!-- transcript:start -->

            ## 00:00:01 — Me

            Old transcript.

            <!-- transcript:end -->

            <!-- capture-events:start -->

            ## Capture events

            - Old event.

            <!-- capture-events:end -->

            User prose after.
            """.utf8
        )
        let snapshot = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 2
            status: "complete"
            session_id: "session-1"
            ended: "2026-07-29T12:00:00Z"
            ---

            # Call

            <!-- transcript:start -->

            **00:00:01 — Me :** Final transcript.

            <!-- transcript:end -->

            <!-- capture-events:start -->

            ## Capture events

            - New event.

            <!-- capture-events:end -->
            """.utf8
        )

        let merged = try ManagedMarkdownMerger.merge(
            existing: existing,
            renderedSnapshot: snapshot
        )
        let text = try XCTUnwrap(String(data: merged, encoding: .utf8))

        XCTAssertTrue(text.contains("custom_user_key: \"keep me\""))
        XCTAssertTrue(text.contains("# prefix user comment"))
        XCTAssertTrue(text.contains("# inter-key user comment"))
        XCTAssertTrue(text.contains("# suffix user comment"))
        XCTAssertTrue(text.contains("status: \"complete\""))
        XCTAssertTrue(text.contains("schema_version: 2"))
        XCTAssertTrue(text.contains("ended: \"2026-07-29T12:00:00Z\""))
        XCTAssertTrue(text.contains("User prose before."))
        XCTAssertTrue(text.contains("User prose after."))
        XCTAssertTrue(text.contains("Final transcript."))
        XCTAssertFalse(text.contains("Old transcript."))
        XCTAssertTrue(text.contains("New event."))
        XCTAssertFalse(text.contains("Old event."))
        XCTAssertEqual(
            text.components(separatedBy: "<!-- transcript:start -->").count,
            2
        )
        XCTAssertEqual(
            text.components(separatedBy: "<!-- transcript:end -->").count,
            2
        )
    }

    func testMalformedOwnershipMarkersAreRejected() {
        let malformed = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 1
            status: "recording"
            ---
            <!-- transcript:start -->
            <!-- transcript:start -->
            <!-- transcript:end -->
            """.utf8
        )

        XCTAssertThrowsError(
            try ManagedMarkdownMerger.merge(
                existing: malformed,
                renderedSnapshot: malformed
            )
        )
    }

    func testRepeatedManagedUpdatesKeepEarlierUserContent() throws {
        let existing = Data(
            """
            ---
            # prefix user comment
            type: "call-transcript"
            schema_version: 1
            status: "recording"
            # inter-key user comment
            session_id: "session-1"
            custom_user_key: "keep me"
            # suffix user comment
            ---

            User prose.

            <!-- transcript:start -->
            First.
            <!-- transcript:end -->
            """.utf8
        )
        let second = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 1
            status: "recording"
            session_id: "session-1"
            ---
            <!-- transcript:start -->
            Second.
            <!-- transcript:end -->
            """.utf8
        )
        let third = Data(
            """
            ---
            type: "call-transcript"
            schema_version: 1
            status: "complete"
            session_id: "session-1"
            ---
            <!-- transcript:start -->
            Third.
            <!-- transcript:end -->
            """.utf8
        )

        let once = try ManagedMarkdownMerger.merge(
            existing: existing,
            renderedSnapshot: second
        )
        let twice = try ManagedMarkdownMerger.merge(
            existing: once,
            renderedSnapshot: third
        )
        let text = try XCTUnwrap(String(data: twice, encoding: .utf8))
        XCTAssertTrue(text.contains("custom_user_key: \"keep me\""))
        XCTAssertTrue(text.contains("# prefix user comment"))
        XCTAssertTrue(text.contains("# inter-key user comment"))
        XCTAssertTrue(text.contains("# suffix user comment"))
        XCTAssertTrue(text.contains("User prose."))
        XCTAssertTrue(text.contains("Third."))
        XCTAssertFalse(text.contains("Second."))
    }

    func testAtomicPublisherReplacesACompleteDocument() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "LocalScribe-Atomic-\(UUID().uuidString)",
                isDirectory: true
            )
        try FileManager.default.createDirectory(
            at: root,
            withIntermediateDirectories: false
        )
        defer { try? FileManager.default.removeItem(at: root) }

        let target = root.appendingPathComponent("Call.md")
        try AtomicFilePublisher.publish(Data("first".utf8), to: target)
        try AtomicFilePublisher.publish(Data("second".utf8), to: target)

        XCTAssertEqual(try Data(contentsOf: target), Data("second".utf8))
        let leftovers = try FileManager.default.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: nil
        )
        XCTAssertEqual(leftovers.map(\.lastPathComponent), ["Call.md"])
    }

    func testAtomicPublisherRejectsChangedExpectedTarget() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "LocalScribe-Atomic-Conflict-\(UUID().uuidString)",
                isDirectory: true
            )
        try FileManager.default.createDirectory(
            at: root,
            withIntermediateDirectories: false
        )
        defer { try? FileManager.default.removeItem(at: root) }

        let target = root.appendingPathComponent("Call.md")
        let expected = Data("expected".utf8)
        let external = Data("external".utf8)
        try expected.write(to: target)
        try external.write(to: target)

        XCTAssertThrowsError(
            try AtomicFilePublisher.publish(
                Data("replacement".utf8),
                to: target,
                expectation: .bytes(expected)
            )
        ) { error in
            XCTAssertEqual(
                error as? VaultWriterError,
                .externalEditConflict
            )
        }
        XCTAssertEqual(try Data(contentsOf: target), external)
    }

    func testStagingUsesPrivateStablePerSessionDirectory() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "LocalScribe-Staging-\(UUID().uuidString)",
                isDirectory: true
            )
        defer { try? FileManager.default.removeItem(at: root) }

        let staging = StagingDirectory(rootURL: root)
        let sessionID = UUID(
            uuidString: "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
        )!
        let url = try await staging.publish(
            data: Data("safe".utf8),
            safeFilename: "Call.md",
            sessionID: sessionID
        )

        XCTAssertEqual(
            url.deletingLastPathComponent().lastPathComponent,
            sessionID.uuidString.lowercased()
        )
        XCTAssertEqual(try Data(contentsOf: url), Data("safe".utf8))
    }
}
