import AppKit
import CoreAudio
import CoreGraphics
import Foundation

protocol CallEnvironmentProviding: Sendable {
    func snapshot() async -> CallEnvironmentSnapshot?
}

struct SystemCallEnvironmentProvider: CallEnvironmentProviding {
    func snapshot() async -> CallEnvironmentSnapshot? {
        let workspace = await MainActor.run {
            Self.workspaceSnapshot()
        }
        guard let workspace,
              let activeInputProcesses = Self.activeAudioInputProcesses()
        else {
            return nil
        }

        let runningBundleIDByPID = Dictionary(
            workspace.runningApplications.map {
                ($0.processIdentifier, $0.bundleIdentifier)
            },
            uniquingKeysWith: { first, _ in first }
        )
        let resolvedInputProcesses = activeInputProcesses.map { process in
            guard process.bundleIdentifier.isEmpty,
                  let bundleIdentifier =
                      runningBundleIDByPID[process.processIdentifier]
            else {
                return process
            }
            return CallProcessObservation(
                processIdentifier: process.processIdentifier,
                bundleIdentifier: bundleIdentifier
            )
        }

        return CallEnvironmentSnapshot(
            runningApplications: workspace.runningApplications,
            audioInputProcesses: resolvedInputProcesses,
            windows: workspace.windows
        )
    }

    private struct WorkspaceSnapshot: Sendable {
        let runningApplications: [CallProcessObservation]
        let windows: [CallWindowObservation]
    }

    @MainActor
    private static func workspaceSnapshot() -> WorkspaceSnapshot? {
        let runningApplications = NSWorkspace.shared.runningApplications
            .compactMap { application -> CallProcessObservation? in
                guard let bundleIdentifier = application.bundleIdentifier
                else {
                    return nil
                }
                return CallProcessObservation(
                    processIdentifier: application.processIdentifier,
                    bundleIdentifier: bundleIdentifier
                )
            }
        let runningBundleIDByPID = Dictionary(
            runningApplications.map {
                ($0.processIdentifier, $0.bundleIdentifier)
            },
            uniquingKeysWith: { first, _ in first }
        )

        guard let windowDictionaries = CGWindowListCopyWindowInfo(
            [.optionAll, .excludeDesktopElements],
            kCGNullWindowID
        ) as? [[String: Any]] else {
            return nil
        }

        let windows = windowDictionaries.compactMap {
            dictionary -> CallWindowObservation? in
            guard let ownerPIDNumber = dictionary[
                kCGWindowOwnerPID as String
            ] as? NSNumber else {
                return nil
            }
            if let layer = dictionary[kCGWindowLayer as String] as? NSNumber,
               layer.intValue != 0
            {
                return nil
            }
            let ownerPID = ownerPIDNumber.int32Value
            let title = (dictionary[kCGWindowName as String] as? String)
                .map { String($0.prefix(512)) }
            let isOnScreen = (
                dictionary[kCGWindowIsOnscreen as String] as? NSNumber
            )?.boolValue ?? false
            return CallWindowObservation(
                ownerProcessIdentifier: ownerPID,
                ownerBundleIdentifier: runningBundleIDByPID[ownerPID],
                title: title,
                isOnScreen: isOnScreen
            )
        }

        return WorkspaceSnapshot(
            runningApplications: runningApplications,
            windows: windows
        )
    }

    private static func activeAudioInputProcesses()
        -> [CallProcessObservation]?
    {
        var listAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyProcessObjectList,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var byteCount: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &listAddress,
            0,
            nil,
            &byteCount
        ) == noErr else {
            return nil
        }
        guard byteCount > 0 else {
            return []
        }

        let objectCount = Int(byteCount)
            / MemoryLayout<AudioObjectID>.stride
        guard objectCount > 0,
              objectCount * MemoryLayout<AudioObjectID>.stride
                == Int(byteCount)
        else {
            return nil
        }
        var processObjectIDs = [AudioObjectID](
            repeating: kAudioObjectUnknown,
            count: objectCount
        )
        let listStatus = processObjectIDs.withUnsafeMutableBytes { buffer in
            guard let baseAddress = buffer.baseAddress else {
                return kAudioHardwareUnspecifiedError
            }
            return AudioObjectGetPropertyData(
                AudioObjectID(kAudioObjectSystemObject),
                &listAddress,
                0,
                nil,
                &byteCount,
                baseAddress
            )
        }
        guard listStatus == noErr else {
            return nil
        }

        return processObjectIDs.compactMap { objectID in
            guard let isRunningInput = readUInt32(
                objectID: objectID,
                selector: kAudioProcessPropertyIsRunningInput
            ),
                isRunningInput != 0,
                let processIdentifier = readProcessIdentifier(
                    objectID: objectID
                )
            else {
                return nil
            }
            return CallProcessObservation(
                processIdentifier: processIdentifier,
                bundleIdentifier: readBundleIdentifier(objectID: objectID)
                    ?? ""
            )
        }
    }

    private static func readUInt32(
        objectID: AudioObjectID,
        selector: AudioObjectPropertySelector
    ) -> UInt32? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: UInt32 = 0
        var byteCount = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(
            objectID,
            &address,
            0,
            nil,
            &byteCount,
            &value
        ) == noErr else {
            return nil
        }
        return value
    }

    private static func readProcessIdentifier(
        objectID: AudioObjectID
    ) -> Int32? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioProcessPropertyPID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var processIdentifier = pid_t()
        var byteCount = UInt32(MemoryLayout<pid_t>.size)
        guard AudioObjectGetPropertyData(
            objectID,
            &address,
            0,
            nil,
            &byteCount,
            &processIdentifier
        ) == noErr else {
            return nil
        }
        return Int32(processIdentifier)
    }

    private static func readBundleIdentifier(
        objectID: AudioObjectID
    ) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioProcessPropertyBundleID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: Unmanaged<CFString>?
        var byteCount = UInt32(
            MemoryLayout<Unmanaged<CFString>?>.size
        )
        guard AudioObjectGetPropertyData(
            objectID,
            &address,
            0,
            nil,
            &byteCount,
            &value
        ) == noErr,
            let value
        else {
            return nil
        }
        return value.takeRetainedValue() as String
    }
}

actor CallDetectionMonitor {
    nonisolated let events: AsyncStream<CallDetectionEvent>

    private let eventContinuation:
        AsyncStream<CallDetectionEvent>.Continuation
    private let provider: any CallEnvironmentProviding
    private let matcher: CallDetectionMatcher
    private let pollingInterval: Duration
    private var reducer: CallDetectionReducer
    private var pollingTask: Task<Void, Never>?
    private var pollingGeneration: UInt64 = 0

    init(
        provider: any CallEnvironmentProviding =
            SystemCallEnvironmentProvider(),
        matcher: CallDetectionMatcher = CallDetectionMatcher(),
        reducer: CallDetectionReducer = CallDetectionReducer(),
        pollingInterval: Duration = .seconds(1)
    ) {
        let pair = AsyncStream<CallDetectionEvent>.makeStream(
            bufferingPolicy: .bufferingNewest(16)
        )
        events = pair.stream
        eventContinuation = pair.continuation
        self.provider = provider
        self.matcher = matcher
        self.reducer = reducer
        self.pollingInterval = pollingInterval
    }

    deinit {
        pollingTask?.cancel()
        eventContinuation.finish()
    }

    func start() {
        guard pollingTask == nil else {
            return
        }
        pollingGeneration &+= 1
        let generation = pollingGeneration
        let interval = pollingInterval
        pollingTask = Task.detached(priority: .utility) { [weak self] in
            while !Task.isCancelled {
                do {
                    guard let self,
                          await self.pollOnce(generation: generation)
                    else {
                        return
                    }
                }

                do {
                    try await Task<Never, Never>.sleep(for: interval)
                } catch {
                    return
                }
            }
        }
    }

    func stop() {
        pollingGeneration &+= 1
        pollingTask?.cancel()
        pollingTask = nil
    }

    private func pollOnce(generation: UInt64) async -> Bool {
        let snapshot = await provider.snapshot()
        guard !Task.isCancelled,
              generation == pollingGeneration
        else {
            return false
        }
        let evidence = matcher.evidence(in: snapshot)
        let emittedEvents = reducer.reduceEvidence(evidence)
        for event in emittedEvents {
            eventContinuation.yield(event)
        }
        return true
    }
}
