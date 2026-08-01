import Foundation

enum SecurityScopedResourceError: Error, Sendable, Equatable {
    case noSelection
    case bookmarkCreationFailed
    case bookmarkResolutionFailed
    case staleBookmarkCouldNotBeRefreshed
    case securityScopeUnavailable
    case expectedDirectory
    case expectedRegularFile
    case resourceNotWritable
    case unsupportedModelProfile
}

protocol SecurityScopedResourceLeasing: AnyObject, Sendable {
    var url: URL { get }
    func release()
}

protocol VaultSelectionProviding: Sendable {
    func hasSelection() async -> Bool
}

protocol VaultDirectoryResolving: Sendable {
    func resolveLease() async throws -> any SecurityScopedResourceLeasing
}

protocol ModelSelectionProviding: Sendable {
    func hasSelection() async -> Bool
    func resolveLease() async throws -> any SecurityScopedResourceLeasing
}

final class SecurityScopedResourceLease:
    SecurityScopedResourceLeasing,
    @unchecked Sendable
{
    let url: URL

    private let lock = NSLock()
    private var isReleased = false

    init(url: URL) throws {
        guard url.startAccessingSecurityScopedResource() else {
            throw SecurityScopedResourceError.securityScopeUnavailable
        }
        self.url = url
    }

    func release() {
        lock.lock()
        let shouldRelease = !isReleased
        isReleased = true
        lock.unlock()

        if shouldRelease {
            url.stopAccessingSecurityScopedResource()
        }
    }

    deinit {
        release()
    }
}

enum SecurityScopedBookmarkRefresh {
    static func finish<Lease: SecurityScopedResourceLeasing>(
        lease: Lease,
        isStale: Bool,
        validate: () throws -> Void,
        refresh: () throws -> Void
    ) throws -> Lease {
        do {
            try validate()
            if isStale {
                do {
                    try refresh()
                } catch {
                    throw SecurityScopedResourceError
                        .staleBookmarkCouldNotBeRefreshed
                }
            }
            return lease
        } catch {
            lease.release()
            throw error
        }
    }
}

enum LocalASRModelProfile {
    static func accepts(filename: String, byteCount: Int) -> Bool {
        let normalized = filename.lowercased()
        guard normalized.hasPrefix("ggml-"),
              normalized.hasSuffix(".bin"),
              byteCount > 0
        else {
            return false
        }

        let modelName = normalized
            .dropFirst("ggml-".count)
            .dropLast(".bin".count)
        return !modelName.isEmpty
    }

    static func validate(_ url: URL) throws {
        let values: URLResourceValues
        do {
            values = try url.resourceValues(
                forKeys: [.isRegularFileKey, .fileSizeKey]
            )
        } catch {
            throw SecurityScopedResourceError.bookmarkResolutionFailed
        }
        guard values.isRegularFile == true,
              let size = values.fileSize,
              accepts(filename: url.lastPathComponent, byteCount: size)
        else {
            throw SecurityScopedResourceError.unsupportedModelProfile
        }
    }
}

private enum SecurityScopedResourceKind: Sendable {
    case directory
    case regularFile
}

private struct SendableUserDefaults: @unchecked Sendable {
    let value: UserDefaults
}

private actor SecurityScopedBookmarkStore {
    private let defaults: UserDefaults
    private let defaultsKey: String
    private let kind: SecurityScopedResourceKind

    init(
        defaults: SendableUserDefaults,
        defaultsKey: String,
        kind: SecurityScopedResourceKind
    ) {
        self.defaults = defaults.value
        self.defaultsKey = defaultsKey
        self.kind = kind
    }

    func hasSelection() -> Bool {
        defaults.data(forKey: defaultsKey) != nil
    }

    func save(_ url: URL) throws {
        try validate(url)
        do {
            let data = try url.bookmarkData(
                options: [.withSecurityScope],
                includingResourceValuesForKeys: [
                    .isDirectoryKey,
                    .isRegularFileKey,
                    .isWritableKey,
                ],
                relativeTo: nil
            )
            defaults.set(data, forKey: defaultsKey)
        } catch {
            throw SecurityScopedResourceError.bookmarkCreationFailed
        }
    }

    func resolveLease() throws -> SecurityScopedResourceLease {
        guard let data = defaults.data(forKey: defaultsKey) else {
            throw SecurityScopedResourceError.noSelection
        }

        var isStale = false
        let url: URL
        do {
            url = try URL(
                resolvingBookmarkData: data,
                options: [.withSecurityScope, .withoutUI],
                relativeTo: nil,
                bookmarkDataIsStale: &isStale
            )
        } catch {
            throw SecurityScopedResourceError.bookmarkResolutionFailed
        }

        let lease = try SecurityScopedResourceLease(url: url)
        return try SecurityScopedBookmarkRefresh.finish(
            lease: lease,
            isStale: isStale,
            validate: { try validate(url) },
            refresh: {
                // A moved external directory/model may only permit resource
                // inspection and bookmark regeneration while the resolved
                // security scope is active.
                try save(url)
            }
        )
    }

    func clear() {
        defaults.removeObject(forKey: defaultsKey)
    }

    private func validate(_ url: URL) throws {
        let values: URLResourceValues
        do {
            values = try url.resourceValues(
                forKeys: [.isDirectoryKey, .isRegularFileKey, .isWritableKey]
            )
        } catch {
            throw SecurityScopedResourceError.bookmarkResolutionFailed
        }

        switch kind {
        case .directory:
            guard values.isDirectory == true else {
                throw SecurityScopedResourceError.expectedDirectory
            }
            guard values.isWritable != false else {
                throw SecurityScopedResourceError.resourceNotWritable
            }
        case .regularFile:
            guard values.isRegularFile == true else {
                throw SecurityScopedResourceError.expectedRegularFile
            }
        }
    }
}

actor SecurityScopedDirectoryStore:
    VaultSelectionProviding,
    VaultDirectoryResolving
{
    private let store: SecurityScopedBookmarkStore

    init(
        defaults: UserDefaults = .standard,
        defaultsKey: String = "LocalScribe.vaultDirectoryBookmark.v1"
    ) {
        store = SecurityScopedBookmarkStore(
            defaults: SendableUserDefaults(value: defaults),
            defaultsKey: defaultsKey,
            kind: .directory
        )
    }

    func hasSelection() async -> Bool {
        await store.hasSelection()
    }

    func saveDirectory(_ url: URL) async throws {
        try await store.save(url)
    }

    func resolveLease() async throws -> any SecurityScopedResourceLeasing {
        try await store.resolveLease()
    }

    func clear() async {
        await store.clear()
    }
}

/// A local model selected outside the app container needs its own bookmark.
/// Keeping it separate prevents a model URL from ever being confused with the
/// user-selected vault sink.
actor SecurityScopedModelStore: ModelSelectionProviding {
    private let store: SecurityScopedBookmarkStore

    init(
        defaults: UserDefaults = .standard,
        defaultsKey: String = "LocalScribe.asrModelBookmark.v1"
    ) {
        store = SecurityScopedBookmarkStore(
            defaults: SendableUserDefaults(value: defaults),
            defaultsKey: defaultsKey,
            kind: .regularFile
        )
    }

    func hasSelection() async -> Bool {
        await store.hasSelection()
    }

    func saveModel(_ url: URL) async throws {
        try LocalASRModelProfile.validate(url)
        try await store.save(url)
    }

    func resolveLease() async throws -> any SecurityScopedResourceLeasing {
        let lease = try await store.resolveLease()
        do {
            try LocalASRModelProfile.validate(lease.url)
            return lease
        } catch {
            lease.release()
            throw error
        }
    }

    func clear() async {
        await store.clear()
    }
}
