import Foundation

enum StagingDirectoryError: Error, Sendable, Equatable {
    case applicationSupportUnavailable
    case invalidFilename
    case createDirectoryFailed
    case publicationFailed
    case publicationSuperseded
}

actor StagingDirectory {
    private let rootURL: URL
    private var latestPublicationSequence: [UUID: UInt64] = [:]

    init(fileManager: FileManager = .default) throws {
        guard let applicationSupport = fileManager.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        ).first else {
            throw StagingDirectoryError.applicationSupportUnavailable
        }

        rootURL = applicationSupport
            .appendingPathComponent("LocalScribe", isDirectory: true)
            .appendingPathComponent("Staging", isDirectory: true)
    }

    init(rootURL: URL) {
        self.rootURL = rootURL
    }

    func publish(
        data: Data,
        safeFilename: String,
        sessionID: UUID,
        publicationSequence: UInt64 = 0
    ) throws -> URL {
        guard SafeFilename.isSingleMarkdownComponent(safeFilename) else {
            throw StagingDirectoryError.invalidFilename
        }
        if publicationSequence > 0 {
            let latest = latestPublicationSequence[sessionID] ?? 0
            guard publicationSequence >= latest else {
                throw StagingDirectoryError.publicationSuperseded
            }
            latestPublicationSequence[sessionID] = publicationSequence
        }

        let fileManager = FileManager.default
        do {
            try fileManager.createDirectory(
                at: rootURL,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
        } catch {
            throw StagingDirectoryError.createDirectoryFailed
        }

        let sessionDirectory = rootURL.appendingPathComponent(
            sessionID.uuidString.lowercased(),
            isDirectory: true
        )
        do {
            try fileManager.createDirectory(
                at: sessionDirectory,
                withIntermediateDirectories: false,
                attributes: [.posixPermissions: 0o700]
            )
        } catch CocoaError.fileWriteFileExists {
            // The stable per-session directory is intentionally reused.
        } catch {
            throw StagingDirectoryError.createDirectoryFailed
        }

        do {
            let values = try sessionDirectory.resourceValues(
                forKeys: [.isDirectoryKey, .isSymbolicLinkKey]
            )
            guard values.isDirectory == true, values.isSymbolicLink != true else {
                throw StagingDirectoryError.publicationFailed
            }
        } catch let error as StagingDirectoryError {
            throw error
        } catch {
            throw StagingDirectoryError.publicationFailed
        }

        let target = sessionDirectory.appendingPathComponent(
            safeFilename,
            isDirectory: false
        )
        if fileManager.fileExists(atPath: target.path) {
            do {
                let values = try target.resourceValues(forKeys: [.isSymbolicLinkKey])
                guard values.isSymbolicLink != true else {
                    throw StagingDirectoryError.publicationFailed
                }
            } catch let error as StagingDirectoryError {
                throw error
            } catch {
                throw StagingDirectoryError.publicationFailed
            }
        }
        do {
            try AtomicFilePublisher.publish(data, to: target)
            return target
        } catch {
            throw StagingDirectoryError.publicationFailed
        }
    }
}
