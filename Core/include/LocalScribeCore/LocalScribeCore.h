#ifndef LOCALSCRIBE_CORE_LOCALSCRIBECORE_H
#define LOCALSCRIBE_CORE_LOCALSCRIBECORE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(LS_CORE_BUILDING)
#    define LS_CORE_API __declspec(dllexport)
#  else
#    define LS_CORE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define LS_CORE_API __attribute__((visibility("default")))
#else
#  define LS_CORE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LS_CORE_ABI_VERSION 1u
#define LS_ERROR_MESSAGE_CAPACITY 256u

typedef struct ls_core ls_core_t;
typedef struct ls_session ls_session_t;
typedef struct ls_event ls_event_t;
typedef struct ls_owned_bytes ls_owned_bytes_t;
typedef struct ls_recovery_list ls_recovery_list_t;
typedef struct ls_voice_profile_list ls_voice_profile_list_t;

/*
 * Public enum values use an explicit int32_t carrier so their ABI does not
 * depend on a C compiler's enum representation. Zero always means unknown.
 */
typedef int32_t ls_status_code_t;
enum {
    LS_STATUS_UNKNOWN = -1,
    LS_OK = 0,

    LS_INVALID_ARGUMENT = 1001,
    LS_INVALID_ABI_VERSION = 1002,
    LS_INVALID_STRUCT_SIZE = 1003,
    LS_INVALID_STATE = 1004,
    LS_BUFFER_TOO_SMALL = 1005,
    LS_NOT_FOUND = 1006,
    LS_CONFLICT = 1007,

    LS_CONSENT_REQUIRED = 2001,

    LS_BACKEND_UNAVAILABLE = 3001,
    LS_MODEL_UNAVAILABLE = 3002,
    LS_BACKEND_FAILURE = 3003,

    LS_AUDIO_FORMAT_ERROR = 4001,
    LS_AUDIO_SEQUENCE_ERROR = 4002,
    LS_BACKPRESSURE = 4003,

    LS_SQLITE_ERROR = 5001,
    LS_SCHEMA_TOO_NEW = 5002,
    LS_RECOVERY_ERROR = 5003,

    LS_RENDER_ERROR = 6001,
    LS_ENCODING_ERROR = 6002,

    LS_CLOSED = 7001,
    LS_CANCELLED = 7002,
    LS_TIMEOUT = 7003,

    LS_INTERNAL_ERROR = 9001
};

typedef int32_t ls_phase_t;
enum {
    LS_PHASE_UNKNOWN = 0,
    LS_PHASE_PREPARING = 1,
    LS_PHASE_RECORDING = 2,
    LS_PHASE_PAUSED = 3,
    LS_PHASE_FINALIZING = 4,
    LS_PHASE_RECOVERY_REQUIRED = 5,
    LS_PHASE_COMPLETE = 6,
    LS_PHASE_INCOMPLETE_SOURCES = 7,
    LS_PHASE_INTERRUPTED = 8,
    LS_PHASE_FAILED_TO_START = 9
};

typedef int32_t ls_published_status_t;
enum {
    LS_PUBLISHED_STATUS_UNKNOWN = 0,
    LS_PUBLISHED_STATUS_RECORDING = 1,
    LS_PUBLISHED_STATUS_COMPLETE = 2,
    LS_PUBLISHED_STATUS_INTERRUPTED = 3,
    LS_PUBLISHED_STATUS_INCOMPLETE_SOURCES = 4
};

typedef int32_t ls_event_kind_t;
enum {
    LS_EVENT_UNKNOWN = 0,
    LS_EVENT_STATE_CHANGED = 1,
    LS_EVENT_FINAL_SEGMENT = 2,
    LS_EVENT_SOURCE_CHANGED = 3,
    LS_EVENT_METRICS = 4,
    LS_EVENT_TERMINAL = 5,
    LS_EVENT_ERROR = 6
};

typedef int32_t ls_finalize_reason_t;
enum {
    LS_FINALIZE_REASON_UNKNOWN = 0,
    LS_FINALIZE_REASON_USER_STOP = 1,
    LS_FINALIZE_REASON_CALL_ENDED = 2,
    LS_FINALIZE_REASON_RECOVERY = 3,
    LS_FINALIZE_REASON_CANCELLED = 4,
    LS_FINALIZE_REASON_PROCESS_INTERRUPTED = 5
};

typedef int32_t ls_source_kind_t;
enum {
    LS_SOURCE_KIND_UNKNOWN = 0,
    LS_SOURCE_KIND_MICROPHONE = 1,
    LS_SOURCE_KIND_SYSTEM_AUDIO = 2
};

typedef int32_t ls_source_health_t;
enum {
    LS_SOURCE_HEALTH_UNKNOWN = 0,
    LS_SOURCE_HEALTH_READY = 1,
    LS_SOURCE_HEALTH_ACTIVE = 2,
    LS_SOURCE_HEALTH_TEMPORARILY_UNAVAILABLE = 3,
    LS_SOURCE_HEALTH_PERMANENTLY_LOST = 4
};

typedef int32_t ls_source_event_kind_t;
enum {
    LS_SOURCE_EVENT_UNKNOWN = 0,
    LS_SOURCE_EVENT_READY = 1,
    LS_SOURCE_EVENT_ACTIVE = 2,
    LS_SOURCE_EVENT_UNAVAILABLE = 3,
    LS_SOURCE_EVENT_RECOVERED = 4,
    LS_SOURCE_EVENT_PERMANENTLY_LOST = 5,
    LS_SOURCE_EVENT_DISCONTINUITY = 6
};

typedef int32_t ls_publication_destination_t;
enum {
    LS_PUBLICATION_DESTINATION_UNKNOWN = 0,
    LS_PUBLICATION_DESTINATION_VAULT = 1,
    LS_PUBLICATION_DESTINATION_STAGING = 2,
    LS_PUBLICATION_DESTINATION_RECOVERY_COPY = 3
};

typedef int32_t ls_language_mode_t;
enum {
    LS_LANGUAGE_MODE_UNKNOWN = 0,
    LS_LANGUAGE_MODE_RUSSIAN = 1,
    LS_LANGUAGE_MODE_ENGLISH = 2,
    LS_LANGUAGE_MODE_RUSSIAN_ENGLISH = 3
};

enum {
    LS_CORE_CONFIG_ALLOW_TEST_BACKENDS = 1u << 0
};

enum {
    LS_REQUIRED_SOURCE_MICROPHONE = 1u << 0,
    LS_REQUIRED_SOURCE_SYSTEM_AUDIO = 1u << 1
};

enum {
    LS_SAMPLE_FORMAT_FLOAT32_INTERLEAVED = 1u
};

enum {
    LS_AUDIO_FLAG_DISCONTINUITY = 1u << 0,
    LS_AUDIO_FLAG_END_OF_STREAM = 1u << 1
};

enum {
    LS_SEGMENT_FLAG_FINAL = 1u << 0,
    LS_SEGMENT_FLAG_UNINTELLIGIBLE = 1u << 1
};

enum {
    LS_SOURCE_EVENT_FLAG_TEST_INJECTED = 1u << 0
};

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    const uint8_t *data;
    size_t size;
} ls_utf8_view_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint8_t bytes[16];
} ls_uuid_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    ls_status_code_t code;
    uint32_t reserved;
    size_t message_size;
    uint8_t message[LS_ERROR_MESSAGE_CAPACITY];
} ls_error_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t reserved;
    ls_utf8_view_v1 journal_path;
} ls_core_config_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    ls_utf8_view_v1 session_id;
    ls_utf8_view_v1 journal_path;
    ls_utf8_view_v1 source_app;
    ls_utf8_view_v1 local_speaker_name;
    ls_utf8_view_v1 asr_backend_id;
    ls_utf8_view_v1 asr_model_path;
    ls_utf8_view_v1 diarization_backend_id;
    ls_utf8_view_v1 created_at_iso8601;
    uint32_t language_mode;
    uint32_t audio_queue_capacity_frames;
    uint64_t microphone_source_id;
    uint64_t system_audio_source_id;
    uint32_t required_source_mask;
    uint32_t reserved;
    int64_t source_completeness_threshold_ns;
} ls_session_config_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t source_id;
    uint64_t sequence_number;
    int64_t monotonic_time_ns;
    uint32_t sample_rate_hz;
    uint16_t channel_count;
    uint16_t sample_format;
    uint32_t frame_count;
    uint32_t flags;
    const float *samples;
} ls_audio_frame_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t source_id;
    ls_source_kind_t source_kind;
    ls_source_event_kind_t event_kind;
    ls_source_health_t health;
    uint32_t flags;
    int64_t start_time_ns;
    int64_t end_time_ns;
    ls_utf8_view_v1 reason;
} ls_source_event_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    ls_utf8_view_v1 title;
    ls_utf8_view_v1 created_at_iso8601;
    ls_utf8_view_v1 ended_at_iso8601;
    int64_t duration_seconds;
    uint32_t microphone_captured;
    uint32_t system_audio_captured;
} ls_markdown_options_v1;

/*
 * Identifies the exact immutable journal snapshot used to produce rendered
 * bytes. Copy both values into the matching publication receipt.
 */
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t journal_checkpoint;
    uint32_t highest_segment_revision;
    uint32_t reserved;
} ls_render_snapshot_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t journal_checkpoint;
    uint32_t highest_segment_revision;
    ls_publication_destination_t destination;
    int64_t published_at_unix_ns;
    ls_utf8_view_v1 sha256_hex;
    ls_utf8_view_v1 file_identity;
} ls_publication_receipt_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t frames_offered;
    uint64_t frames_accepted;
    uint64_t frames_rejected;
    uint64_t discontinuities;
    uint64_t final_segments_committed;
    uint64_t partial_events_coalesced;
    uint32_t audio_queue_depth;
    uint32_t audio_queue_high_water;
    uint64_t journal_checkpoint;
    uint32_t highest_segment_revision;
    uint32_t reserved;
} ls_pipeline_metrics_v1;

/*
 * The UTF-8 views returned here point into the event and stay valid until
 * ls_event_destroy(). They are not NUL terminated.
 */
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    ls_uuid_v1 stable_id;
    uint64_t source_id;
    int64_t start_time_ns;
    int64_t end_time_ns;
    uint64_t speaker_id;
    ls_utf8_view_v1 speaker_label;
    ls_utf8_view_v1 text;
    ls_utf8_view_v1 language;
    float confidence;
    uint32_t revision;
    uint32_t flags;
    uint32_t reserved;
} ls_transcript_segment_copy_v1;

/* UTF-8 views remain valid until ls_voice_profile_list_destroy(). */
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t profile_id;
    ls_utf8_view_v1 display_name;
    ls_utf8_view_v1 embedding_model_id;
    uint64_t observation_count;
    int64_t created_at_unix_ns;
    int64_t updated_at_unix_ns;
} ls_voice_profile_copy_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t profile_id;
    uint64_t speaker_id;
    uint64_t observation_count;
    uint32_t relabeled_segments;
    uint64_t journal_checkpoint;
    uint32_t highest_segment_revision;
    uint32_t reserved;
} ls_voice_profile_enrollment_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    ls_phase_t phase;
    ls_published_status_t published_status;
    ls_finalize_reason_t finalize_reason;
    uint32_t reserved;
} ls_state_event_copy_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t source_id;
    ls_source_kind_t source_kind;
    ls_source_event_kind_t event_kind;
    ls_source_health_t health;
    uint32_t flags;
    int64_t start_time_ns;
    int64_t end_time_ns;
} ls_source_event_copy_v1;

LS_CORE_API ls_status_code_t ls_core_create_v1(
    const ls_core_config_v1 *config,
    ls_core_t **out_core,
    ls_error_v1 *out_error);

LS_CORE_API void ls_core_destroy(ls_core_t *core);

LS_CORE_API ls_status_code_t ls_session_create_after_consent_v1(
    ls_core_t *core,
    const ls_session_config_v1 *config,
    ls_session_t **out_session,
    ls_error_v1 *out_error);

LS_CORE_API ls_status_code_t
ls_session_mark_sources_ready_v1(ls_session_t *session);

/* Thread-safe; accepted sample bytes are copied before this call returns. */
LS_CORE_API ls_status_code_t ls_session_push_audio_v1(
    ls_session_t *session,
    const ls_audio_frame_v1 *frame);

LS_CORE_API ls_status_code_t ls_session_pause_v1(ls_session_t *session);

LS_CORE_API ls_status_code_t
ls_session_resume_after_consent_v1(ls_session_t *session);

LS_CORE_API ls_status_code_t ls_session_source_event_v1(
    ls_session_t *session,
    const ls_source_event_v1 *event);

LS_CORE_API ls_status_code_t ls_session_finalize_v1(
    ls_session_t *session,
    ls_finalize_reason_t reason);

LS_CORE_API void ls_session_destroy(ls_session_t *session);

/* Exactly one consumer may poll a session at a time. */
LS_CORE_API ls_status_code_t ls_session_next_event_v1(
    ls_session_t *session,
    uint32_t timeout_ms,
    ls_event_t **out_event);

LS_CORE_API ls_event_kind_t ls_event_kind(const ls_event_t *event);

LS_CORE_API ls_status_code_t ls_event_copy_segment_v1(
    const ls_event_t *event,
    ls_transcript_segment_copy_v1 *out_segment);

LS_CORE_API ls_status_code_t ls_event_copy_metrics_v1(
    const ls_event_t *event,
    ls_pipeline_metrics_v1 *out_metrics);

LS_CORE_API ls_status_code_t ls_event_copy_state_v1(
    const ls_event_t *event,
    ls_state_event_copy_v1 *out_state);

LS_CORE_API ls_status_code_t ls_event_copy_source_v1(
    const ls_event_t *event,
    ls_source_event_copy_v1 *out_source);

LS_CORE_API void ls_event_destroy(ls_event_t *event);

LS_CORE_API ls_status_code_t ls_session_copy_metrics_v1(
    const ls_session_t *session,
    ls_pipeline_metrics_v1 *out_metrics);

/*
 * Copies the authoritative current lifecycle state without consuming the
 * session event queue.
 */
LS_CORE_API ls_status_code_t ls_session_copy_state_v1(
    const ls_session_t *session,
    ls_state_event_copy_v1 *out_state);

LS_CORE_API ls_status_code_t ls_session_render_markdown_v1(
    ls_session_t *session,
    const ls_markdown_options_v1 *options,
    ls_owned_bytes_t **out_markdown,
    ls_error_v1 *out_error);

LS_CORE_API ls_status_code_t
ls_session_render_markdown_with_snapshot_v1(
    ls_session_t *session,
    const ls_markdown_options_v1 *options,
    ls_owned_bytes_t **out_markdown,
    ls_render_snapshot_v1 *out_snapshot,
    ls_error_v1 *out_error);

LS_CORE_API const uint8_t *
ls_owned_bytes_data(const ls_owned_bytes_t *bytes);

LS_CORE_API size_t ls_owned_bytes_size(const ls_owned_bytes_t *bytes);

LS_CORE_API void ls_owned_bytes_destroy(ls_owned_bytes_t *bytes);

LS_CORE_API ls_status_code_t ls_session_ack_publication_v1(
    ls_session_t *session,
    const ls_publication_receipt_v1 *receipt);

LS_CORE_API ls_status_code_t ls_core_list_recoverable_sessions_v1(
    ls_core_t *core,
    ls_recovery_list_t **out_list);

LS_CORE_API size_t
ls_recovery_list_count(const ls_recovery_list_t *list);

LS_CORE_API ls_status_code_t ls_recovery_list_session_id_v1(
    const ls_recovery_list_t *list,
    size_t index,
    ls_utf8_view_v1 *out_session_id);

LS_CORE_API void ls_recovery_list_destroy(ls_recovery_list_t *list);

LS_CORE_API ls_status_code_t ls_core_list_voice_profiles_v1(
    ls_core_t *core,
    ls_voice_profile_list_t **out_list,
    ls_error_v1 *out_error);

LS_CORE_API size_t
ls_voice_profile_list_count(const ls_voice_profile_list_t *list);

LS_CORE_API ls_status_code_t ls_voice_profile_list_copy_v1(
    const ls_voice_profile_list_t *list,
    size_t index,
    ls_voice_profile_copy_v1 *out_profile);

LS_CORE_API void
ls_voice_profile_list_destroy(ls_voice_profile_list_t *list);

LS_CORE_API ls_status_code_t ls_core_enroll_voice_profile_v1(
    ls_core_t *core,
    ls_utf8_view_v1 session_id,
    uint64_t speaker_id,
    ls_utf8_view_v1 display_name,
    ls_voice_profile_enrollment_v1 *out_enrollment,
    ls_error_v1 *out_error);

LS_CORE_API ls_status_code_t ls_core_rename_voice_profile_v1(
    ls_core_t *core,
    uint64_t profile_id,
    ls_utf8_view_v1 display_name,
    ls_error_v1 *out_error);

LS_CORE_API ls_status_code_t ls_core_delete_voice_profile_v1(
    ls_core_t *core,
    uint64_t profile_id,
    ls_error_v1 *out_error);

LS_CORE_API ls_status_code_t ls_core_open_recoverable_session_v1(
    ls_core_t *core,
    ls_utf8_view_v1 session_id,
    ls_session_t **out_session);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LOCALSCRIBE_CORE_LOCALSCRIBECORE_H */
