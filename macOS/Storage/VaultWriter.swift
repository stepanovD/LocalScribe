import CryptoKit
import Darwin
import Foundation

enum PublicationDestination: Int32, Sendable {
    case vault = 1
    case staging = 2
    case recoveryCopy = 3
}

struct PublicationFingerprint: Sendable, Equatable {
    let sha256Hex: String
    let fileIdentity: String?
    let byteCount: Int
}

struct MarkdownPublicationRequest: Sendable {
    let sessionID: UUID
    let preferredFilenameStem: String
    let markdown: Data
    let highestSegmentRevision: UInt64
    /// Strictly increasing within a process/session. The sink rejects a
    /// request superseded by a newer snapshot even if async URL resolution
    /// completes in reverse order.
    let publicationSequence: UInt64
    let expectedPrevious: PublicationFingerprint?
}

struct MarkdownPublicationReceipt: Sendable, Equatable {
    let destination: PublicationDestination
    let fingerprint: PublicationFingerprint
    let highestSegmentRevision: UInt64
    let publishedAt: Date
    let filename: String
    /// User-facing location retained only in Swift memory.
    let url: URL
}

enum VaultWriterError: Error, Sendable, Equatable {
    case invalidUTF8
    case malformedManagedDocument
    case unsafeDestination
    case symbolicLinkDestination
    case externalEditConflict
    case publicationFailed
    case stagingFailed
    case publicationSuperseded
}

protocol MarkdownPublishing: Sendable {
    func publish(
        _ request: MarkdownPublicationRequest
    ) async throws -> MarkdownPublicationReceipt

    func latestReceipt(
        for sessionID: UUID
    ) async -> MarkdownPublicationReceipt?
}

actor VaultWriter: MarkdownPublishing {
    private let directoryStore: any VaultDirectoryResolving
    private let stagingDirectory: StagingDirectory
    private let beforeAtomicVaultRename: (@Sendable (URL) throws -> Void)?
    private let afterVaultSwapConflict: (@Sendable (URL) throws -> Void)?
    private var latestReceipts: [UUID: MarkdownPublicationReceipt] = [:]
    private var latestPublicationSequence: [UUID: UInt64] = [:]

    init(
        directoryStore: any VaultDirectoryResolving,
        stagingDirectory: StagingDirectory,
        beforeAtomicVaultRename: (@Sendable (URL) throws -> Void)? = nil,
        afterVaultSwapConflict: (@Sendable (URL) throws -> Void)? = nil
    ) {
        self.directoryStore = directoryStore
        self.stagingDirectory = stagingDirectory
        self.beforeAtomicVaultRename = beforeAtomicVaultRename
        self.afterVaultSwapConflict = afterVaultSwapConflict
    }

    func publish(_ request: MarkdownPublicationRequest) async throws -> MarkdownPublicationReceipt {
        guard String(data: request.markdown, encoding: .utf8) != nil else {
            throw VaultWriterError.invalidUTF8
        }
        guard request.publicationSequence > 0 else {
            throw VaultWriterError.publicationFailed
        }
        let latest = latestPublicationSequence[request.sessionID] ?? 0
        guard request.publicationSequence >= latest else {
            throw VaultWriterError.publicationSuperseded
        }
        latestPublicationSequence[request.sessionID] =
            request.publicationSequence

        let defaultFilename = SafeFilename.markdownFilename(
            stem: request.preferredFilenameStem,
            sessionID: request.sessionID,
            forceStableSuffix: false
        )
        let priorReceipt = latestReceipts[request.sessionID]
        let filename = priorReceipt?.destination == .staging
            ? defaultFilename
            : (priorReceipt?.filename ?? defaultFilename)

        let lease: any SecurityScopedResourceLeasing
        do {
            lease = try await directoryStore.resolveLease()
        } catch {
            try ensureCurrent(request)
            return try await stage(request, filename: filename)
        }
        defer { lease.release() }
        try ensureCurrent(request)

        do {
            return try publishToVault(
                request,
                filename: filename,
                directory: lease.url
            )
        } catch VaultWriterError.publicationSuperseded {
            throw VaultWriterError.publicationSuperseded
        } catch VaultWriterError.externalEditConflict {
            try ensureCurrent(request)
            do {
                return try publishRecoveryCopy(
                    request,
                    directory: lease.url
                )
            } catch {
                try ensureCurrent(request)
                return try await stage(request, filename: filename)
            }
        } catch VaultWriterError.symbolicLinkDestination {
            try ensureCurrent(request)
            return try await stage(request, filename: filename)
        } catch VaultWriterError.unsafeDestination {
            try ensureCurrent(request)
            return try await stage(request, filename: filename)
        } catch {
            try ensureCurrent(request)
            return try await stage(request, filename: filename)
        }
    }

    func latestReceipt(
        for sessionID: UUID
    ) async -> MarkdownPublicationReceipt? {
        latestReceipts[sessionID]
    }

    private func publishToVault(
        _ request: MarkdownPublicationRequest,
        filename: String,
        directory: URL
    ) throws -> MarkdownPublicationReceipt {
        try ensureCurrent(request)
        let fileManager = FileManager.default
        let target = try safeTarget(filename: filename, directory: directory)
        let targetExists = fileManager.fileExists(atPath: target.path)

        var bytesToPublish = request.markdown
        var chosenTarget = target
        var targetExpectation: AtomicTargetExpectation = .absent

        if targetExists {
            try rejectSymbolicLink(target)

            let rememberedReceipt = latestReceipts[request.sessionID]
            let expected: PublicationFingerprint?
            if let rememberedReceipt,
               rememberedReceipt.destination != .staging,
               rememberedReceipt.filename == target.lastPathComponent
            {
                expected = rememberedReceipt.fingerprint
            } else if rememberedReceipt == nil {
                expected = request.expectedPrevious
            } else {
                expected = nil
            }

            if expected != nil {
                let existing = try Data(contentsOf: target, options: .mappedIfSafe)
                targetExpectation = .bytes(existing)
                do {
                    // Always merge an existing owned note, even when its
                    // fingerprint still equals the previous receipt. A prior
                    // merge may already contain user prose/frontmatter; raw
                    // replacement on the next periodic snapshot would erase
                    // that content.
                    bytesToPublish = try ManagedMarkdownMerger.merge(
                        existing: existing,
                        renderedSnapshot: request.markdown
                    )
                } catch {
                    throw VaultWriterError.externalEditConflict
                }
            } else {
                let existing = try Data(contentsOf: target, options: .mappedIfSafe)
                targetExpectation = .bytes(existing)
                do {
                    // A process may die after the atomic file replacement but
                    // before its receipt reaches SQLite. Matching session_id
                    // and unambiguous managed markers prove that this is the
                    // same LocalScribe document, so recovery can update it
                    // idempotently instead of creating a duplicate.
                    bytesToPublish = try ManagedMarkdownMerger.merge(
                        existing: existing,
                        renderedSnapshot: request.markdown
                    )
                } catch {
                    let suffixedName = SafeFilename.markdownFilename(
                        stem: request.preferredFilenameStem,
                        sessionID: request.sessionID,
                        forceStableSuffix: true
                    )
                    chosenTarget = try safeTarget(
                        filename: suffixedName,
                        directory: directory
                    )
                    targetExpectation = .absent

                    if fileManager.fileExists(atPath: chosenTarget.path) {
                        try rejectSymbolicLink(chosenTarget)
                        let suffixedExisting = try Data(
                            contentsOf: chosenTarget,
                            options: .mappedIfSafe
                        )
                        targetExpectation = .bytes(suffixedExisting)
                        do {
                            bytesToPublish = try ManagedMarkdownMerger.merge(
                                existing: suffixedExisting,
                                renderedSnapshot: request.markdown
                            )
                        } catch {
                            // Neither candidate proves ownership. Never
                            // overwrite an unrelated or malformed note.
                            throw VaultWriterError.externalEditConflict
                        }
                    }
                }
            }
        }

        try rejectSymbolicLinkIfPresent(chosenTarget)
        let publishedFingerprint: PublicationFingerprint
        do {
            publishedFingerprint = try AtomicFilePublisher.publish(
                bytesToPublish,
                to: chosenTarget,
                expectation: targetExpectation,
                beforeAtomicRename: beforeAtomicVaultRename,
                afterSwapConflict: afterVaultSwapConflict
            )
        } catch {
            if error as? VaultWriterError == .externalEditConflict {
                throw VaultWriterError.externalEditConflict
            }
            throw VaultWriterError.publicationFailed
        }

        let receipt = MarkdownPublicationReceipt(
            destination: .vault,
            // This fingerprint is derived from the exact bytes passed to the
            // atomic replacement, never from a later pathname read that an
            // external editor could race.
            fingerprint: publishedFingerprint,
            highestSegmentRevision: request.highestSegmentRevision,
            publishedAt: Date(),
            filename: chosenTarget.lastPathComponent,
            url: chosenTarget
        )
        latestReceipts[request.sessionID] = receipt
        return receipt
    }

    private func publishRecoveryCopy(
        _ request: MarkdownPublicationRequest,
        directory: URL
    ) throws -> MarkdownPublicationReceipt {
        try ensureCurrent(request)
        let filename = SafeFilename.recoveryFilename(
            stem: request.preferredFilenameStem,
            sessionID: request.sessionID
        )
        let target = try safeTarget(filename: filename, directory: directory)

        guard !FileManager.default.fileExists(atPath: target.path) else {
            // A second conflict must never overwrite the earlier recovery
            // artifact. Staging will preserve the exact current snapshot.
            throw VaultWriterError.externalEditConflict
        }

        let publishedFingerprint: PublicationFingerprint
        do {
            publishedFingerprint = try AtomicFilePublisher.publish(
                request.markdown,
                to: target,
                expectation: .absent
            )
        } catch {
            throw VaultWriterError.publicationFailed
        }

        let receipt = MarkdownPublicationReceipt(
            destination: .recoveryCopy,
            fingerprint: publishedFingerprint,
            highestSegmentRevision: request.highestSegmentRevision,
            publishedAt: Date(),
            filename: filename,
            url: target
        )
        latestReceipts[request.sessionID] = receipt
        return receipt
    }

    private func stage(
        _ request: MarkdownPublicationRequest,
        filename: String
    ) async throws -> MarkdownPublicationReceipt {
        try ensureCurrent(request)
        let url: URL
        do {
            url = try await stagingDirectory.publish(
                data: request.markdown,
                safeFilename: filename,
                sessionID: request.sessionID,
                publicationSequence: request.publicationSequence
            )
        } catch StagingDirectoryError.publicationSuperseded {
            throw VaultWriterError.publicationSuperseded
        } catch {
            throw VaultWriterError.stagingFailed
        }
        try ensureCurrent(request)

        let receipt = MarkdownPublicationReceipt(
            destination: .staging,
            fingerprint: try Self.fingerprint(of: url),
            highestSegmentRevision: request.highestSegmentRevision,
            publishedAt: Date(),
            filename: filename,
            url: url
        )
        latestReceipts[request.sessionID] = receipt
        return receipt
    }

    private func ensureCurrent(
        _ request: MarkdownPublicationRequest
    ) throws {
        guard latestPublicationSequence[request.sessionID]
                == request.publicationSequence
        else {
            throw VaultWriterError.publicationSuperseded
        }
    }

    private func safeTarget(filename: String, directory: URL) throws -> URL {
        guard SafeFilename.isSingleMarkdownComponent(filename) else {
            throw VaultWriterError.unsafeDestination
        }

        let standardizedDirectory = directory.standardizedFileURL
        let target = standardizedDirectory
            .appendingPathComponent(filename, isDirectory: false)
            .standardizedFileURL

        guard target.deletingLastPathComponent() == standardizedDirectory else {
            throw VaultWriterError.unsafeDestination
        }
        return target
    }

    private func rejectSymbolicLinkIfPresent(_ url: URL) throws {
        if FileManager.default.fileExists(atPath: url.path) {
            try rejectSymbolicLink(url)
        }
    }

    private func rejectSymbolicLink(_ url: URL) throws {
        let values = try url.resourceValues(forKeys: [.isSymbolicLinkKey])
        guard values.isSymbolicLink != true else {
            throw VaultWriterError.symbolicLinkDestination
        }
    }

    private static func fingerprint(of url: URL) throws -> PublicationFingerprint {
        let data = try Data(contentsOf: url, options: .mappedIfSafe)
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)

        let device = (attributes[.systemNumber] as? NSNumber)?.uint64Value
        let inode = (attributes[.systemFileNumber] as? NSNumber)?.uint64Value
        let identity: String?
        if let device, let inode {
            identity = "\(device):\(inode)"
        } else {
            identity = nil
        }

        return PublicationFingerprint(
            sha256Hex: digest,
            fileIdentity: identity,
            byteCount: data.count
        )
    }
}

enum SafeFilename {
    static func markdownFilename(
        stem: String,
        sessionID: UUID,
        forceStableSuffix: Bool
    ) -> String {
        let sanitized = sanitizedStem(stem)
        let suffix = String(
            sessionID.uuidString
                .replacingOccurrences(of: "-", with: "")
                .lowercased()
                .prefix(8)
        )
        let base = forceStableSuffix ? "\(sanitized) — \(suffix)" : sanitized
        return String(base.prefix(180)) + ".md"
    }

    static func recoveryFilename(stem: String, sessionID: UUID) -> String {
        let sanitized = sanitizedStem(stem)
        let suffix = String(
            sessionID.uuidString
                .replacingOccurrences(of: "-", with: "")
                .lowercased()
                .prefix(8)
        )
        return String("\(sanitized) — recovery-\(suffix)".prefix(180)) + ".md"
    }

    static func isSingleMarkdownComponent(_ value: String) -> Bool {
        guard value.hasSuffix(".md"),
              !value.isEmpty,
              value != ".",
              value != "..",
              !value.contains("/"),
              !value.contains("\\"),
              !value.contains(":"),
              !value.unicodeScalars.contains(where: {
                  $0.value == 0 || CharacterSet.controlCharacters.contains($0)
              })
        else {
            return false
        }

        return (value as NSString).lastPathComponent == value
    }

    private static func sanitizedStem(_ value: String) -> String {
        let normalized = value.precomposedStringWithCanonicalMapping
        let scalars = normalized.unicodeScalars.map { scalar -> Character in
            if scalar == "/" || scalar == "\\" || scalar == ":"
                || scalar.value == 0
                || CharacterSet.controlCharacters.contains(scalar)
            {
                return " "
            }
            return Character(scalar)
        }

        let collapsed = String(scalars)
            .split(whereSeparator: \.isWhitespace)
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)

        if collapsed.isEmpty || collapsed == "." || collapsed == ".." {
            return "Call"
        }
        return collapsed
    }
}

enum AtomicFilePublisher {
    @discardableResult
    static func publish(
        _ data: Data,
        to target: URL,
        expectation: AtomicTargetExpectation = .unchecked,
        beforeAtomicRename: (@Sendable (URL) throws -> Void)? = nil,
        afterSwapConflict: (@Sendable (URL) throws -> Void)? = nil
    ) throws -> PublicationFingerprint {
        let directory = target.deletingLastPathComponent()
        let targetName = target.lastPathComponent
        var scratchName =
            ".localscribe-\(UUID().uuidString.lowercased()).tmp"
        let directoryDescriptor: Int32 =
            directory.withUnsafeFileSystemRepresentation { path in
                guard let path else {
                    errno = EINVAL
                    return -1
                }
                return Darwin.open(
                    path,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
                )
            }
        guard directoryDescriptor >= 0,
              !targetName.isEmpty,
              !targetName.contains("/")
        else {
            throw VaultWriterError.publicationFailed
        }
        defer { Darwin.close(directoryDescriptor) }

        var shouldRemoveTemporary = true
        defer {
            if shouldRemoveTemporary {
                scratchName.withCString {
                    _ = Darwin.unlinkat(directoryDescriptor, $0, 0)
                }
            }
        }

        let temporaryDescriptor: Int32 = scratchName.withCString {
            Darwin.openat(
                directoryDescriptor,
                $0,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                mode_t(0o600)
            )
        }
        guard temporaryDescriptor >= 0 else {
            throw VaultWriterError.publicationFailed
        }
        let handle = FileHandle(
            fileDescriptor: temporaryDescriptor,
            closeOnDealloc: true
        )
        do {
            try handle.write(contentsOf: data)
            try handle.synchronize()
            try handle.close()
        } catch {
            try? handle.close()
            throw error
        }

        // This is only an early rejection and a deterministic test seam.
        // The authoritative check happens against the exact inode displaced
        // by the atomic rename below, so a later external save cannot be
        // overwritten unnoticed.
        switch expectation {
        case .unchecked:
            break
        case .absent:
            guard try pathIsAbsent(
                named: targetName,
                at: directoryDescriptor
            ) else {
                throw VaultWriterError.externalEditConflict
            }
        case let .bytes(expected):
            guard (try? readRegularFileNoFollow(
                named: targetName,
                at: directoryDescriptor
            )) == expected else {
                throw VaultWriterError.externalEditConflict
            }
        }

        if case .bytes = expectation {
            guard let visibleScratchName =
                moveScratchToVisibleArtifact(
                    scratchName: scratchName,
                    targetName: targetName,
                    at: directoryDescriptor
                )
            else {
                throw VaultWriterError.publicationFailed
            }
            scratchName = visibleScratchName
        }

        try beforeAtomicRename?(target)

        switch expectation {
        case .unchecked:
            let failure = atomicRename(
                from: scratchName,
                to: targetName,
                at: directoryDescriptor,
                flags: UInt32(RENAME_NOFOLLOW_ANY)
            )
            guard failure == 0 else {
                throw VaultWriterError.publicationFailed
            }
            shouldRemoveTemporary = false

        case .absent:
            let failure = atomicRename(
                from: scratchName,
                to: targetName,
                at: directoryDescriptor,
                flags: UInt32(RENAME_EXCL | RENAME_NOFOLLOW_ANY)
            )
            guard failure == 0 else {
                if failure == EEXIST || failure == ELOOP {
                    throw VaultWriterError.externalEditConflict
                }
                throw VaultWriterError.publicationFailed
            }
            shouldRemoveTemporary = false

        case let .bytes(expected):
            let swapFlags = UInt32(RENAME_SWAP | RENAME_NOFOLLOW_ANY)
            let failure = atomicRename(
                from: scratchName,
                to: targetName,
                at: directoryDescriptor,
                flags: swapFlags
            )
            guard failure == 0 else {
                if failure == ENOENT || failure == ELOOP {
                    throw VaultWriterError.externalEditConflict
                }
                throw VaultWriterError.publicationFailed
            }

            // The old target now lives at a visible conflict path; retain it
            // until its exact bytes have been checked. Even a crash at this
            // point cannot strand a user edit under a hidden temp name.
            shouldRemoveTemporary = false
            let displaced = try? readRegularFileNoFollow(
                named: scratchName,
                at: directoryDescriptor
            )
            guard let displaced else {
                throw VaultWriterError.externalEditConflict
            }
            guard displaced == expected else {
                // Keep an independent, fsynced E1 copy before any restore can
                // expose E1 at a pathname that another atomic save may
                // replace. The visible swap slot already closes the earlier
                // crash window; this copy closes the E2/E3 restore window.
                do {
                    try preserveExternalBytesAsVisibleArtifact(
                        displaced,
                        targetName: targetName,
                        at: directoryDescriptor
                    )
                } catch {
                    // `scratchName` still visibly contains E1.
                    throw VaultWriterError.externalEditConflict
                }

                do {
                    try afterSwapConflict?(target)
                } catch {
                    throw VaultWriterError.externalEditConflict
                }

                // If E2 already replaced our uncommitted snapshot, it is the
                // newest user version and must remain at the original target.
                // E1 is already visible in its conflict artifact.
                guard (try? readRegularFileNoFollow(
                    named: targetName,
                    at: directoryDescriptor
                )) == data else {
                    throw VaultWriterError.externalEditConflict
                }

                let restoreFailure = atomicRename(
                    from: scratchName,
                    to: targetName,
                    at: directoryDescriptor,
                    flags: swapFlags
                )
                guard restoreFailure == 0 else {
                    // E1 remains at its visible artifact if the filesystem
                    // refuses the swap-back.
                    throw VaultWriterError.externalEditConflict
                }

                let displacedDuringRestore = try? readRegularFileNoFollow(
                    named: scratchName,
                    at: directoryDescriptor
                )
                guard let displacedDuringRestore else {
                    throw VaultWriterError.externalEditConflict
                }

                if displacedDuringRestore != data {
                    // E2 landed after the target check but before swap-back.
                    // It is already visible at `scratchName`. Put it back at
                    // the original path if E1 is still there. If an E3 has
                    // replaced E1, leave E3 at target and E2 visible here.
                    let targetBeforeCorrection =
                        try? readRegularFileNoFollow(
                            named: targetName,
                            at: directoryDescriptor
                        )
                    if targetBeforeCorrection == displaced {
                        _ = atomicRename(
                            from: scratchName,
                            to: targetName,
                            at: directoryDescriptor,
                            flags: swapFlags
                        )
                    }
                } else {
                    // Normal restore: target is E1 and the visible scratch
                    // contains only our uncommitted snapshot.
                    shouldRemoveTemporary = true
                }
                throw VaultWriterError.externalEditConflict
            }

            // Only the already-expected previous LocalScribe snapshot is
            // discarded after a successful publication.
            shouldRemoveTemporary = true
        }

        let digest = SHA256.hash(data: data)
            .map { String(format: "%02x", $0) }
            .joined()
        return PublicationFingerprint(
            sha256Hex: digest,
            // A pathname identity sampled after replacement could already
            // belong to an external atomic save. Digest + byte count remain
            // exact; identity is therefore deliberately left unknown.
            fileIdentity: nil,
            byteCount: data.count
        )
    }

    private static func preserveExternalBytesAsVisibleArtifact(
        _ data: Data,
        targetName: String,
        at directoryDescriptor: Int32
    ) throws {
        for _ in 0..<8 {
            let artifactName = visibleConflictArtifactName(
                for: targetName
            )
            let artifactDescriptor: Int32 = artifactName.withCString {
                Darwin.openat(
                    directoryDescriptor,
                    $0,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    mode_t(0o600)
                )
            }
            if artifactDescriptor < 0 {
                if errno == EEXIST {
                    continue
                }
                throw VaultWriterError.publicationFailed
            }

            let handle = FileHandle(
                fileDescriptor: artifactDescriptor,
                closeOnDealloc: true
            )
            do {
                try handle.write(contentsOf: data)
                try handle.synchronize()
                try handle.close()
                guard (try? readRegularFileNoFollow(
                    named: artifactName,
                    at: directoryDescriptor
                )) == data else {
                    artifactName.withCString {
                        _ = Darwin.unlinkat(directoryDescriptor, $0, 0)
                    }
                    continue
                }
                return
            } catch {
                try? handle.close()
                artifactName.withCString {
                    _ = Darwin.unlinkat(directoryDescriptor, $0, 0)
                }
                throw VaultWriterError.publicationFailed
            }
        }
        throw VaultWriterError.publicationFailed
    }

    private static func moveScratchToVisibleArtifact(
        scratchName: String,
        targetName: String,
        at directoryDescriptor: Int32
    ) -> String? {
        for _ in 0..<8 {
            let artifactName = visibleConflictArtifactName(
                for: targetName
            )
            let failure = atomicRename(
                from: scratchName,
                to: artifactName,
                at: directoryDescriptor,
                flags: UInt32(RENAME_EXCL | RENAME_NOFOLLOW_ANY)
            )
            if failure == 0 {
                return artifactName
            }
            if failure != EEXIST {
                return nil
            }
        }
        return nil
    }

    private static func visibleConflictArtifactName(
        for targetName: String
    ) -> String {
        let targetTag = SHA256.hash(data: Data(targetName.utf8))
            .prefix(4)
            .map { String(format: "%02x", $0) }
            .joined()
        let uniqueTag = String(
            UUID().uuidString
                .replacingOccurrences(of: "-", with: "")
                .lowercased()
                .prefix(8)
        )
        return "LocalScribe — external-edit-\(targetTag)-\(uniqueTag).md"
    }

    private static func pathIsAbsent(
        named name: String,
        at directoryDescriptor: Int32
    ) throws -> Bool {
        var information = stat()
        let result: Int32 = name.withCString {
            Darwin.fstatat(
                directoryDescriptor,
                $0,
                &information,
                AT_SYMLINK_NOFOLLOW
            )
        }
        if result == 0 {
            return false
        }
        if errno == ENOENT {
            return true
        }
        throw VaultWriterError.publicationFailed
    }

    private static func readRegularFileNoFollow(
        named name: String,
        at directoryDescriptor: Int32
    ) throws -> Data {
        let descriptor: Int32 = name.withCString {
            Darwin.openat(
                directoryDescriptor,
                $0,
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC
            )
        }
        guard descriptor >= 0 else {
            throw VaultWriterError.externalEditConflict
        }

        let handle = FileHandle(
            fileDescriptor: descriptor,
            closeOnDealloc: true
        )
        var information = stat()
        guard Darwin.fstat(descriptor, &information) == 0,
              (information.st_mode & S_IFMT) == S_IFREG
        else {
            try? handle.close()
            throw VaultWriterError.externalEditConflict
        }
        return try handle.readToEnd() ?? Data()
    }

    /// Returns zero on success or the captured POSIX error number. Directory
    /// relative single-component names bind every operation to the opened
    /// selected directory; `RENAME_NOFOLLOW_ANY` rejects substituted links.
    private static func atomicRename(
        from sourceName: String,
        to destinationName: String,
        at directoryDescriptor: Int32,
        flags: UInt32
    ) -> Int32 {
        var failure: Int32 = 0
        let result: Int32 = sourceName.withCString { sourcePath in
            destinationName.withCString { destinationPath in
                let result = renameatx_np(
                    directoryDescriptor,
                    sourcePath,
                    directoryDescriptor,
                    destinationPath,
                    flags
                )
                if result != 0 {
                    failure = errno
                }
                return result
            }
        }
        return result == 0 ? 0 : failure
    }
}

enum AtomicTargetExpectation {
    case unchecked
    case absent
    case bytes(Data)
}

enum ManagedMarkdownMerger {
    private static let startMarker = "<!-- transcript:start -->"
    private static let endMarker = "<!-- transcript:end -->"
    private static let captureStartMarker = "<!-- capture-events:start -->"
    private static let captureEndMarker = "<!-- capture-events:end -->"
    private static let managedKeys: Set<String> = [
        "type",
        "schema_version",
        "status",
        "session_id",
        "created",
        "ended",
        "duration_seconds",
        "source_app",
        "capture",
        "languages",
        "participants",
        "tags",
    ]

    static func merge(existing: Data, renderedSnapshot: Data) throws -> Data {
        guard let existingString = String(data: existing, encoding: .utf8),
              let snapshotString = String(data: renderedSnapshot, encoding: .utf8)
        else {
            throw VaultWriterError.invalidUTF8
        }

        let existingDocument = try ParsedDocument(existingString)
        let snapshotDocument = try ParsedDocument(snapshotString)

        let mergedFrontmatter = try mergeFrontmatter(
            existing: existingDocument.frontmatterLines,
            snapshot: snapshotDocument.frontmatterLines
        )
        let mergedBody = try mergeTranscriptBlock(
            existing: existingDocument.body,
            snapshot: snapshotDocument.body
        )

        let output = "---\n"
            + mergedFrontmatter.joined(separator: "\n")
            + "\n---\n"
            + mergedBody
        guard let data = output.data(using: .utf8) else {
            throw VaultWriterError.invalidUTF8
        }
        return data
    }

    private static func mergeFrontmatter(
        existing: [String],
        snapshot: [String]
    ) throws -> [String] {
        let existingLayout = try topLevelLayout(existing)
        let snapshotLayout = try topLevelLayout(snapshot)
        let existingBlocks = existingLayout.blocks
        let snapshotBlocks = snapshotLayout.blocks
        guard topLevelScalar("session_id", in: existingBlocks)
                == topLevelScalar("session_id", in: snapshotBlocks),
              topLevelScalar("session_id", in: snapshotBlocks) != nil
        else {
            throw VaultWriterError.malformedManagedDocument
        }
        let snapshotManaged = Dictionary(
            uniqueKeysWithValues: snapshotBlocks.compactMap { block in
                managedKeys.contains(block.key) ? (block.key, block.lines) : nil
            }
        )

        guard snapshotManaged["type"] != nil,
              snapshotManaged["schema_version"] != nil,
              snapshotManaged["status"] != nil,
              snapshotManaged["session_id"] != nil
        else {
            throw VaultWriterError.malformedManagedDocument
        }

        var result = existingLayout.prefixTrivia
        var replaced = Set<String>()
        for block in existingBlocks {
            if managedKeys.contains(block.key) {
                if let replacement = snapshotManaged[block.key] {
                    result.append(contentsOf: replacement)
                    replaced.insert(block.key)
                }
                // Comments and blank lines belong to the user's YAML layout,
                // not to LocalScribe's managed value. Keep them even when the
                // surrounding managed mapping is replaced or removed.
                result.append(contentsOf: block.preservedTrivia)
            } else {
                result.append(contentsOf: block.lines)
            }
        }

        for block in snapshotBlocks where managedKeys.contains(block.key) {
            if !replaced.contains(block.key) {
                result.append(contentsOf: block.lines)
            }
        }
        return result
    }

    private static func mergeTranscriptBlock(
        existing: String,
        snapshot: String
    ) throws -> String {
        let existingRange = try uniqueMarkerRange(
            in: existing,
            startMarker: startMarker,
            endMarker: endMarker
        )
        let snapshotRange = try uniqueMarkerRange(
            in: snapshot,
            startMarker: startMarker,
            endMarker: endMarker
        )
        var merged = existing.replacingCharacters(
            in: existingRange,
            with: String(snapshot[snapshotRange])
        )

        let existingCapture = try optionalUniqueMarkerRange(
            in: merged,
            startMarker: captureStartMarker,
            endMarker: captureEndMarker
        )
        let snapshotCapture = try optionalUniqueMarkerRange(
            in: snapshot,
            startMarker: captureStartMarker,
            endMarker: captureEndMarker
        )
        switch (existingCapture, snapshotCapture) {
        case let (.some(existingRange), .some(snapshotRange)):
            merged.replaceSubrange(
                existingRange,
                with: snapshot[snapshotRange]
            )
        case let (.some(existingRange), .none):
            merged.removeSubrange(existingRange)
        case let (.none, .some(snapshotRange)):
            let transcriptRange = try uniqueMarkerRange(
                in: merged,
                startMarker: startMarker,
                endMarker: endMarker
            )
            merged.insert(
                contentsOf: "\n\n" + String(snapshot[snapshotRange]),
                at: transcriptRange.upperBound
            )
        case (.none, .none):
            break
        }
        return merged
    }

    private static func uniqueMarkerRange(
        in body: String,
        startMarker: String,
        endMarker: String
    ) throws -> Range<String.Index> {
        guard let range = try optionalUniqueMarkerRange(
            in: body,
            startMarker: startMarker,
            endMarker: endMarker
        )
        else {
            throw VaultWriterError.malformedManagedDocument
        }
        return range
    }

    private static func optionalUniqueMarkerRange(
        in body: String,
        startMarker: String,
        endMarker: String
    ) throws -> Range<String.Index>? {
        let firstStart = body.range(of: startMarker)
        let firstEnd = body.range(of: endMarker)
        guard firstStart != nil || firstEnd != nil else {
            return nil
        }
        guard let start = firstStart,
              let end = firstEnd,
              end.lowerBound >= start.upperBound,
              body.range(
                  of: startMarker,
                  range: start.upperBound..<body.endIndex
              ) == nil,
              body.range(
                  of: endMarker,
                  range: end.upperBound..<body.endIndex
              ) == nil
        else {
            throw VaultWriterError.malformedManagedDocument
        }
        return start.lowerBound..<end.upperBound
    }

    private struct YAMLBlock {
        let key: String
        let lines: [String]
        let preservedTrivia: [String]
    }

    private struct YAMLLayout {
        let prefixTrivia: [String]
        let blocks: [YAMLBlock]
    }

    private static func topLevelScalar(
        _ key: String,
        in blocks: [YAMLBlock]
    ) -> String? {
        guard let line = blocks.first(where: { $0.key == key })?.lines.first,
              let colon = line.firstIndex(of: ":")
        else {
            return nil
        }
        let value = line[line.index(after: colon)...]
            .trimmingCharacters(in: .whitespaces)
        return value.isEmpty ? nil : value
    }

    private static func topLevelLayout(_ lines: [String]) throws -> YAMLLayout {
        var starts: [(index: Int, key: String)] = []
        for (index, line) in lines.enumerated() {
            if line.first?.isWhitespace == true || isYAMLTrivia(line) {
                continue
            }
            guard let colon = line.firstIndex(of: ":") else {
                throw VaultWriterError.malformedManagedDocument
            }
            let key = String(line[..<colon])
            guard !key.isEmpty,
                  key.unicodeScalars.allSatisfy({
                      CharacterSet.alphanumerics.contains($0) || $0 == "_"
                  })
            else {
                throw VaultWriterError.malformedManagedDocument
            }
            starts.append((index, key))
        }

        guard starts.count == Set(starts.map(\.key)).count else {
            throw VaultWriterError.malformedManagedDocument
        }

        guard let firstStart = starts.first else {
            throw VaultWriterError.malformedManagedDocument
        }
        let prefixTrivia = Array(lines[..<firstStart.index])
        guard prefixTrivia.allSatisfy(isYAMLTrivia) else {
            throw VaultWriterError.malformedManagedDocument
        }

        let blocks = starts.enumerated().map { offset, start in
            let end = offset + 1 < starts.count ? starts[offset + 1].index : lines.count
            let blockLines = Array(lines[start.index..<end])
            return YAMLBlock(
                key: start.key,
                lines: blockLines,
                preservedTrivia: Array(blockLines.dropFirst().filter(isYAMLTrivia))
            )
        }
        return YAMLLayout(prefixTrivia: prefixTrivia, blocks: blocks)
    }

    private static func isYAMLTrivia(_ line: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        return trimmed.isEmpty || trimmed.hasPrefix("#")
    }

    private struct ParsedDocument {
        let frontmatterLines: [String]
        let body: String

        init(_ source: String) throws {
            let normalized = source.replacingOccurrences(of: "\r\n", with: "\n")
            guard normalized.hasPrefix("---\n"),
                  let end = normalized.range(
                      of: "\n---\n",
                      range: normalized.index(normalized.startIndex, offsetBy: 4)..<normalized.endIndex
                  )
            else {
                throw VaultWriterError.malformedManagedDocument
            }

            let frontmatterStart = normalized.index(normalized.startIndex, offsetBy: 4)
            frontmatterLines = String(normalized[frontmatterStart..<end.lowerBound])
                .split(separator: "\n", omittingEmptySubsequences: false)
                .map(String.init)
            body = String(normalized[end.upperBound...])
        }
    }
}
