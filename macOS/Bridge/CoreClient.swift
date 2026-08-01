import CLocalScribeCore
import Foundation

final class CoreClient: @unchecked Sendable, CoreClientProtocol {
    private let lock = NSLock()
    private var handle: OpaquePointer?

    init(journalURL: URL, allowTestBackends: Bool = false) throws {
        var createdHandle: OpaquePointer?
        var error = ls_error_v1()

        let status: ls_status_code_t = withUTF8Views([journalURL.path]) { views in
            var configuration = ls_core_config_v1()
            initializeABIHeader(&configuration)
            configuration.flags = allowTestBackends
                ? UInt32(LS_CORE_CONFIG_ALLOW_TEST_BACKENDS)
                : 0
            configuration.journal_path = views[0]
            return ls_core_create_v1(
                &configuration,
                &createdHandle,
                &error
            )
        }

        try check(status)
        guard let createdHandle else {
            throw CoreBridgeError.malformedCoreValue
        }
        handle = createdHandle
    }

    deinit {
        lock.lock()
        let current = handle
        handle = nil
        lock.unlock()
        if let current {
            ls_core_destroy(current)
        }
    }

    func createSessionAfterConsent(
        configuration: CoreSessionConfiguration
    ) throws -> any CoreSessionProtocol {
        let core = try currentHandle()
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]

        let values = [
            configuration.sessionID.uuidString.lowercased(),
            configuration.journalURL.path,
            configuration.sourceApplication,
            configuration.localSpeakerName,
            configuration.asrBackendID,
            configuration.asrModelURL.path,
            configuration.diarizationBackendID,
            formatter.string(from: configuration.createdAt),
        ]

        var sessionHandle: OpaquePointer?
        var error = ls_error_v1()
        let status: ls_status_code_t = withUTF8Views(values) { views in
            var input = ls_session_config_v1()
            initializeABIHeader(&input)
            input.session_id = views[0]
            input.journal_path = views[1]
            input.source_app = views[2]
            input.local_speaker_name = views[3]
            input.asr_backend_id = views[4]
            input.asr_model_path = views[5]
            input.diarization_backend_id = views[6]
            input.created_at_iso8601 = views[7]
            input.language_mode = configuration.languageMode.rawValue
            input.audio_queue_capacity_frames =
                configuration.audioQueueCapacityFrames
            input.microphone_source_id = configuration.microphoneSourceID
            input.system_audio_source_id = configuration.systemAudioSourceID
            input.required_source_mask =
                UInt32(LS_REQUIRED_SOURCE_MICROPHONE)
                | UInt32(LS_REQUIRED_SOURCE_SYSTEM_AUDIO)
            input.source_completeness_threshold_ns =
                configuration.sourceCompletenessThresholdNanoseconds

            return ls_session_create_after_consent_v1(
                core,
                &input,
                &sessionHandle,
                &error
            )
        }

        try check(status)
        guard let sessionHandle else {
            throw CoreBridgeError.malformedCoreValue
        }

        return CoreSession(
            sessionID: configuration.sessionID,
            handle: sessionHandle,
            owner: self
        )
    }

    func recoverableSessionIDs() throws -> [String] {
        let core = try currentHandle()
        var list: OpaquePointer?
        try check(ls_core_list_recoverable_sessions_v1(core, &list))
        guard let list else {
            return []
        }
        defer { ls_recovery_list_destroy(list) }

        let count = ls_recovery_list_count(list)
        return try (0..<count).map { index in
            var value = ls_utf8_view_v1()
            initializeABIHeader(&value)
            try check(ls_recovery_list_session_id_v1(list, index, &value))
            return try copyUTF8(value)
        }
    }

    func openRecoverableSession(id: String) throws -> any CoreSessionProtocol {
        let core = try currentHandle()
        var sessionHandle: OpaquePointer?
        let status: ls_status_code_t = withUTF8Views([id]) { views in
            ls_core_open_recoverable_session_v1(
                core,
                views[0],
                &sessionHandle
            )
        }
        try check(status)
        guard let sessionHandle, let sessionID = UUID(uuidString: id) else {
            throw CoreBridgeError.malformedCoreValue
        }
        return CoreSession(
            sessionID: sessionID,
            handle: sessionHandle,
            owner: self
        )
    }

    func listVoiceProfiles() throws -> [CoreVoiceProfile] {
        let core = try currentHandle()
        var list: OpaquePointer?
        var error = ls_error_v1()
        initializeABIHeader(&error)
        try check(
            ls_core_list_voice_profiles_v1(
                core,
                &list,
                &error
            )
        )
        guard let list else {
            return []
        }
        defer { ls_voice_profile_list_destroy(list) }

        let count = ls_voice_profile_list_count(list)
        return try (0..<count).map { index in
            var value = ls_voice_profile_copy_v1()
            initializeABIHeader(&value)
            try check(
                ls_voice_profile_list_copy_v1(
                    list,
                    index,
                    &value
                )
            )
            return CoreVoiceProfile(
                profileID: value.profile_id,
                displayName: try copyUTF8(value.display_name),
                sampleCount: value.observation_count
            )
        }
    }

    func enrollVoiceProfile(
        sessionID: UUID,
        speakerID: UInt64,
        displayName: String
    ) throws -> CoreVoiceProfileEnrollment {
        let core = try currentHandle()
        var enrollment = ls_voice_profile_enrollment_v1()
        initializeABIHeader(&enrollment)
        var error = ls_error_v1()
        initializeABIHeader(&error)
        let status: ls_status_code_t = withUTF8Views(
            [sessionID.uuidString.lowercased(), displayName]
        ) { views in
            ls_core_enroll_voice_profile_v1(
                core,
                views[0],
                speakerID,
                views[1],
                &enrollment,
                &error
            )
        }
        try check(status)
        return CoreVoiceProfileEnrollment(
            profileID: enrollment.profile_id,
            speakerID: enrollment.speaker_id,
            sampleCount: enrollment.observation_count,
            relabeledSegments: enrollment.relabeled_segments,
            journalCheckpoint: enrollment.journal_checkpoint,
            highestSegmentRevision: enrollment.highest_segment_revision
        )
    }

    func renameVoiceProfile(
        profileID: UInt64,
        displayName: String
    ) throws {
        let core = try currentHandle()
        var error = ls_error_v1()
        initializeABIHeader(&error)
        let status: ls_status_code_t = withUTF8Views([displayName]) { views in
            ls_core_rename_voice_profile_v1(
                core,
                profileID,
                views[0],
                &error
            )
        }
        try check(status)
    }

    func deleteVoiceProfile(profileID: UInt64) throws {
        let core = try currentHandle()
        var error = ls_error_v1()
        initializeABIHeader(&error)
        try check(
            ls_core_delete_voice_profile_v1(
                core,
                profileID,
                &error
            )
        )
    }

    private func currentHandle() throws -> OpaquePointer {
        lock.lock()
        defer { lock.unlock() }
        guard let handle else {
            throw CoreBridgeError.closed
        }
        return handle
    }
}

private final class CoreSession: @unchecked Sendable, CoreSessionProtocol {
    let sessionID: UUID

    private let condition = NSCondition()
    private var handle: OpaquePointer?
    private var isClosing = false
    private var activeCalls = 0

    // Retaining the client guarantees the core outlives this session handle.
    private let owner: CoreClient

    init(sessionID: UUID, handle: OpaquePointer, owner: CoreClient) {
        self.sessionID = sessionID
        self.handle = handle
        self.owner = owner
    }

    deinit {
        close()
    }

    func markSourcesReady() throws {
        try withHandle { handle in
            try check(ls_session_mark_sources_ready_v1(handle))
        }
    }

    func pushAudio(_ frame: CapturedAudioFrame) -> AudioFrameDisposition {
        do {
            return try withHandle { handle in
                frame.interleavedSamples.withUnsafeBufferPointer { samples in
                    var input = ls_audio_frame_v1()
                    initializeABIHeader(&input)
                    input.source_id = frame.sourceID
                    input.sequence_number = frame.sequenceNumber
                    input.monotonic_time_ns = frame.monotonicTimeNanoseconds
                    input.sample_rate_hz = frame.sampleRateHz
                    input.channel_count = frame.channelCount
                    input.sample_format = UInt16(
                        LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED
                    )
                    input.frame_count = frame.frameCount
                    input.flags = frame.flags.rawValue
                    input.samples = samples.baseAddress

                    switch ls_session_push_audio_v1(handle, &input) {
                    case ls_status_code_t(LS_OK):
                        return .accepted
                    case ls_status_code_t(LS_BACKPRESSURE):
                        return .backpressure
                    case ls_status_code_t(LS_CLOSED):
                        return .closed
                    default:
                        return .rejected
                    }
                }
            }
        } catch {
            return .closed
        }
    }

    func pause() throws {
        try withHandle { handle in
            try check(ls_session_pause_v1(handle))
        }
    }

    func resumeAfterConsent() throws {
        try withHandle { handle in
            try check(ls_session_resume_after_consent_v1(handle))
        }
    }

    func sourceEvent(
        sourceID: UInt64,
        kind: CaptureSourceKind,
        event: CoreSourceEventKind,
        health: CoreSourceHealth,
        startTimeNanoseconds: Int64,
        endTimeNanoseconds: Int64,
        reasonCode: String
    ) throws {
        try withHandle { handle in
            let status: ls_status_code_t = withUTF8Views([reasonCode]) { views in
                var input = ls_source_event_v1()
                initializeABIHeader(&input)
                input.source_id = sourceID
                input.source_kind = kind == .microphone
                    ? ls_source_kind_t(LS_SOURCE_KIND_MICROPHONE)
                    : ls_source_kind_t(LS_SOURCE_KIND_SYSTEM_AUDIO)
                input.event_kind = event.rawValue
                input.health = health.rawValue
                input.start_time_ns = startTimeNanoseconds
                input.end_time_ns = endTimeNanoseconds
                input.reason = views[0]
                return ls_session_source_event_v1(handle, &input)
            }
            try check(status)
        }
    }

    func finalize(reason: CoreFinalizeReason) throws {
        try withHandle { handle in
            try check(ls_session_finalize_v1(handle, reason.rawValue))
        }
    }

    func nextEvent(timeoutMilliseconds: UInt32) throws -> CoreEvent? {
        try withHandle { handle -> CoreEvent? in
            var event: OpaquePointer?
            let status = ls_session_next_event_v1(
                handle,
                timeoutMilliseconds,
                &event
            )
            if status == ls_status_code_t(LS_TIMEOUT) {
                return nil
            }
            if status == ls_status_code_t(LS_CLOSED) {
                throw CoreBridgeError.closed
            }
            try check(status)
            guard let event else {
                throw CoreBridgeError.malformedCoreValue
            }
            defer { ls_event_destroy(event) }

            switch ls_event_kind(event) {
            case ls_event_kind_t(LS_EVENT_STATE_CHANGED):
                return .stateChanged(try copyState(event))
            case ls_event_kind_t(LS_EVENT_FINAL_SEGMENT):
                return .finalSegment(try copySegment(event))
            case ls_event_kind_t(LS_EVENT_SOURCE_CHANGED):
                return .sourceChanged(try copySource(event))
            case ls_event_kind_t(LS_EVENT_METRICS):
                return .metrics(try copyMetrics(event))
            case ls_event_kind_t(LS_EVENT_TERMINAL):
                return .terminal(try copyState(event))
            case ls_event_kind_t(LS_EVENT_ERROR):
                return .error(code: "core_event")
            default:
                throw CoreBridgeError.malformedCoreValue
            }
        }
    }

    func metrics() throws -> CorePipelineMetrics {
        try withHandle { handle in
            var value = ls_pipeline_metrics_v1()
            initializeABIHeader(&value)
            try check(ls_session_copy_metrics_v1(handle, &value))
            return mapMetrics(value)
        }
    }

    func currentState() throws -> CoreStateEvent {
        try withHandle { handle in
            var value = ls_state_event_copy_v1()
            initializeABIHeader(&value)
            try check(ls_session_copy_state_v1(handle, &value))
            return try decodeState(value)
        }
    }

    func renderMarkdown(
        options: CoreMarkdownOptions
    ) throws -> CoreRenderedMarkdown {
        try withHandle { handle in
            let formatter = ISO8601DateFormatter()
            formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
            let created = options.createdAt.map(formatter.string(from:)) ?? ""
            let ended = options.endedAt.map(formatter.string(from:)) ?? ""
            let values = [
                options.title,
                created,
                ended,
            ]

            var bytes: OpaquePointer?
            var snapshot = ls_render_snapshot_v1()
            initializeABIHeader(&snapshot)
            var error = ls_error_v1()
            let status: ls_status_code_t = withUTF8Views(values) { views in
                var input = ls_markdown_options_v1()
                initializeABIHeader(&input)
                input.title = views[0]
                input.created_at_iso8601 = views[1]
                input.ended_at_iso8601 = views[2]
                input.duration_seconds = options.durationSeconds
                input.microphone_captured = options.microphoneCaptured ? 1 : 0
                input.system_audio_captured = options.systemAudioCaptured ? 1 : 0
                return ls_session_render_markdown_with_snapshot_v1(
                    handle,
                    &input,
                    &bytes,
                    &snapshot,
                    &error
                )
            }
            try check(status)
            guard let bytes else {
                throw CoreBridgeError.malformedCoreValue
            }
            defer { ls_owned_bytes_destroy(bytes) }

            let count = ls_owned_bytes_size(bytes)
            let data: Data
            if count == 0 {
                data = Data()
            } else {
                guard let pointer = ls_owned_bytes_data(bytes) else {
                    throw CoreBridgeError.malformedCoreValue
                }
                data = Data(bytes: pointer, count: count)
            }
            return CoreRenderedMarkdown(
                data: data,
                journalCheckpoint: snapshot.journal_checkpoint,
                highestSegmentRevision: snapshot.highest_segment_revision
            )
        }
    }

    func acknowledgePublication(
        receipt: MarkdownPublicationReceipt,
        journalCheckpoint: UInt64
    ) throws {
        try withHandle { handle in
            let identity = receipt.fingerprint.fileIdentity ?? ""
            let values = [receipt.fingerprint.sha256Hex, identity]
            let timestamp = receipt.publishedAt.timeIntervalSince1970 * 1_000_000_000
            guard timestamp.isFinite,
                  timestamp >= Double(Int64.min),
                  timestamp <= Double(Int64.max)
            else {
                throw CoreBridgeError.malformedCoreValue
            }

            let status: ls_status_code_t = withUTF8Views(values) { views in
                var input = ls_publication_receipt_v1()
                initializeABIHeader(&input)
                input.journal_checkpoint = journalCheckpoint
                input.highest_segment_revision = UInt32(
                    clamping: receipt.highestSegmentRevision
                )
                input.destination = receipt.destination.rawValue
                input.published_at_unix_ns = Int64(timestamp.rounded())
                input.sha256_hex = views[0]
                input.file_identity = views[1]
                return ls_session_ack_publication_v1(handle, &input)
            }
            try check(status)
        }
    }

    func close() {
        condition.lock()
        guard !isClosing else {
            condition.unlock()
            return
        }
        isClosing = true
        while activeCalls > 0 {
            condition.wait()
        }
        let current = handle
        handle = nil
        condition.unlock()

        if let current {
            ls_session_destroy(current)
        }
    }

    private func withHandle<Result>(
        _ operation: (OpaquePointer) throws -> Result
    ) throws -> Result {
        condition.lock()
        guard !isClosing, let handle else {
            condition.unlock()
            throw CoreBridgeError.closed
        }
        activeCalls += 1
        condition.unlock()

        defer {
            condition.lock()
            activeCalls -= 1
            if activeCalls == 0 {
                condition.broadcast()
            }
            condition.unlock()
        }
        return try operation(handle)
    }

    private func copyState(_ event: OpaquePointer) throws -> CoreStateEvent {
        var value = ls_state_event_copy_v1()
        initializeABIHeader(&value)
        try check(ls_event_copy_state_v1(event, &value))
        return try decodeState(value)
    }

    private func decodeState(
        _ value: ls_state_event_copy_v1
    ) throws -> CoreStateEvent {
        guard let phase = CorePhase(rawValue: value.phase),
              let status = CorePublishedStatus(rawValue: value.published_status),
              let reason = CoreFinalizeReason(rawValue: value.finalize_reason)
        else {
            throw CoreBridgeError.malformedCoreValue
        }
        return CoreStateEvent(
            phase: phase,
            publishedStatus: status,
            finalizeReason: reason
        )
    }

    private func copySegment(_ event: OpaquePointer) throws -> CoreTranscriptSegment {
        var value = ls_transcript_segment_copy_v1()
        initializeABIHeader(&value)
        try check(ls_event_copy_segment_v1(event, &value))

        return CoreTranscriptSegment(
            stableID: copyUUID(value.stable_id),
            sourceID: value.source_id,
            startTimeNanoseconds: value.start_time_ns,
            endTimeNanoseconds: value.end_time_ns,
            speakerID: value.speaker_id,
            speakerLabel: try copyUTF8(value.speaker_label),
            text: try copyUTF8(value.text),
            language: try copyUTF8(value.language),
            confidence: value.confidence,
            revision: value.revision,
            isFinal: value.flags & UInt32(LS_SEGMENT_FLAG_FINAL) != 0,
            isUnintelligible:
                value.flags & UInt32(LS_SEGMENT_FLAG_UNINTELLIGIBLE) != 0
        )
    }

    private func copySource(_ event: OpaquePointer) throws -> CoreSourceEvent {
        var value = ls_source_event_copy_v1()
        initializeABIHeader(&value)
        try check(ls_event_copy_source_v1(event, &value))
        guard let eventKind = CoreSourceEventKind(rawValue: value.event_kind),
              let health = CoreSourceHealth(rawValue: value.health)
        else {
            throw CoreBridgeError.malformedCoreValue
        }

        let sourceKind: CaptureSourceKind
        switch value.source_kind {
        case ls_source_kind_t(LS_SOURCE_KIND_MICROPHONE):
            sourceKind = .microphone
        case ls_source_kind_t(LS_SOURCE_KIND_SYSTEM_AUDIO):
            sourceKind = .systemAudio
        default:
            throw CoreBridgeError.malformedCoreValue
        }

        return CoreSourceEvent(
            sourceID: value.source_id,
            sourceKind: sourceKind,
            eventKind: eventKind,
            health: health,
            startTimeNanoseconds: value.start_time_ns,
            endTimeNanoseconds: value.end_time_ns
        )
    }

    private func copyMetrics(_ event: OpaquePointer) throws -> CorePipelineMetrics {
        var value = ls_pipeline_metrics_v1()
        initializeABIHeader(&value)
        try check(ls_event_copy_metrics_v1(event, &value))
        return mapMetrics(value)
    }
}

private func mapMetrics(_ value: ls_pipeline_metrics_v1) -> CorePipelineMetrics {
    CorePipelineMetrics(
        framesOffered: value.frames_offered,
        framesAccepted: value.frames_accepted,
        framesRejected: value.frames_rejected,
        discontinuities: value.discontinuities,
        finalSegmentsCommitted: value.final_segments_committed,
        partialEventsCoalesced: value.partial_events_coalesced,
        audioQueueDepth: value.audio_queue_depth,
        audioQueueHighWater: value.audio_queue_high_water,
        journalCheckpoint: value.journal_checkpoint,
        highestSegmentRevision: value.highest_segment_revision
    )
}

private func copyUUID(_ value: ls_uuid_v1) -> UUID {
    var copy = value.bytes
    return withUnsafeBytes(of: &copy) { raw in
        let bytes = Array(raw.prefix(16))
        return UUID(uuid: (
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]
        ))
    }
}

private func copyUTF8(_ value: ls_utf8_view_v1) throws -> String {
    guard value.size == 0 || value.data != nil else {
        throw CoreBridgeError.malformedCoreValue
    }
    guard let data = value.data else {
        return ""
    }
    let bytes = Data(bytes: data, count: value.size)
    guard let string = String(data: bytes, encoding: .utf8) else {
        throw CoreBridgeError.invalidUTF8
    }
    return string
}

private func check(_ status: ls_status_code_t) throws {
    guard status == ls_status_code_t(LS_OK) else {
        if status == ls_status_code_t(LS_CLOSED) {
            throw CoreBridgeError.closed
        }
        throw CoreBridgeError.status(status)
    }
}

private func initializeABIHeader<Value>(_ value: inout Value) {
    withUnsafeMutableBytes(of: &value) { bytes in
        bytes.initializeMemory(as: UInt8.self, repeating: 0)
        bytes.storeBytes(
            of: UInt32(MemoryLayout<Value>.size),
            toByteOffset: 0,
            as: UInt32.self
        )
        bytes.storeBytes(
            of: UInt32(LS_CORE_ABI_VERSION),
            toByteOffset: MemoryLayout<UInt32>.size,
            as: UInt32.self
        )
    }
}

private func withUTF8Views<Result>(
    _ strings: [String],
    _ operation: ([ls_utf8_view_v1]) throws -> Result
) rethrows -> Result {
    let data = strings.map { Data($0.utf8) }

    func pin(
        _ index: Int,
        _ views: inout [ls_utf8_view_v1]
    ) throws -> Result {
        if index == data.count {
            return try operation(views)
        }

        return try data[index].withUnsafeBytes { rawBuffer in
            var view = ls_utf8_view_v1()
            initializeABIHeader(&view)
            view.data = rawBuffer.bindMemory(to: UInt8.self).baseAddress
            view.size = rawBuffer.count
            views.append(view)
            defer { views.removeLast() }
            return try pin(index + 1, &views)
        }
    }

    var views: [ls_utf8_view_v1] = []
    views.reserveCapacity(strings.count)
    return try pin(0, &views)
}
