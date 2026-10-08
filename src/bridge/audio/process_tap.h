#ifndef DMN_AUDIO_PROCESS_TAP_H
#define DMN_AUDIO_PROCESS_TAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dmn_audio_tap dmn_audio_tap;

typedef enum dmn_audio_status {
    DMN_AUDIO_OK = 0,
    DMN_AUDIO_DISABLED = 1,
    DMN_AUDIO_INVALID_ARGUMENT = 2,
    DMN_AUDIO_UNSUPPORTED_OS = 3,
    DMN_AUDIO_PRODUCER_NOT_FOUND = 4,
    DMN_AUDIO_HAL_FAILURE = 5,
    DMN_AUDIO_BAD_UID = 6,
    DMN_AUDIO_UNSUPPORTED_FORMAT = 7,
    DMN_AUDIO_BUFFER_TOO_SMALL = 8,
    DMN_AUDIO_NOT_READY = 9,
    DMN_AUDIO_CLEANUP_FAILED = 10,
    DMN_AUDIO_OUT_OF_MEMORY = 11,
    DMN_AUDIO_INTERNAL_ERROR = 12,
    DMN_AUDIO_TARGET_UNSAFE = 13
} dmn_audio_status;

typedef enum dmn_audio_stage {
    DMN_AUDIO_STAGE_NONE = 0,
    DMN_AUDIO_STAGE_GATE = 1,
    DMN_AUDIO_STAGE_OS = 2,
    DMN_AUDIO_STAGE_PID_LOOKUP = 3,
    DMN_AUDIO_STAGE_CREATE_TAP = 4,
    DMN_AUDIO_STAGE_TAP_UID = 5,
    DMN_AUDIO_STAGE_TAP_FORMAT = 6,
    DMN_AUDIO_STAGE_CREATE_AGGREGATE = 7,
    DMN_AUDIO_STAGE_AGGREGATE_UID = 8,
    DMN_AUDIO_STAGE_AGGREGATE_FORMAT = 9,
    DMN_AUDIO_STAGE_DESTROY_AGGREGATE = 10,
    DMN_AUDIO_STAGE_DESTROY_TAP = 11,
    DMN_AUDIO_STAGE_ALLOCATION = 12,
    DMN_AUDIO_STAGE_DETACH_TARGET = 13,
    DMN_AUDIO_STAGE_SET_TARGET = 14,
    DMN_AUDIO_STAGE_VALIDATE_TARGET = 15
} dmn_audio_stage;

typedef struct dmn_audio_error {
    uint32_t stage;
    int32_t os_status;
    uint32_t cleanup_stage;
    int32_t cleanup_os_status;
} dmn_audio_error;

/* Stable ASBD summary, not an Apple-framework type in the public ABI. */
typedef struct dmn_audio_format {
    uint32_t struct_size;
    uint32_t version;
    double sample_rate;
    uint32_t format_id;
    uint32_t format_flags;
    uint32_t bytes_per_packet;
    uint32_t frames_per_packet;
    uint32_t bytes_per_frame;
    uint32_t channels;
    uint32_t bits_per_channel;
    uint32_t reserved;
} dmn_audio_format;

enum { DMN_AUDIO_FORMAT_VERSION = 1, DMN_AUDIO_UID_MAX_BYTES = 256 };

/* Explicit create requires DMN_AUDIO_TAP exactly "1" and native_pid > 0.
 * Disabled/invalid requests invoke no backend/HAL operation. No constructor,
 * IO callback, AudioDeviceStart, global tap, microphone/default-device fallback,
 * producer mute, or default-device change is performed by this library.
 *
 * *out must initially be NULL. On success it owns a ready private tap aggregate.
 * On failure it is normally NULL. If rollback fails, it owns a cleanup-only
 * handle: NEVER advertise its UID, and retry destroy before another create.
 * error is optional and preserves both primary and rollback OSStatus/stages.
 * Calls must be serialized by the owner; handles are not shared across threads.
 */
dmn_audio_status dmn_audio_tap_create(int32_t native_pid, dmn_audio_tap **out,
                                     dmn_audio_error *error);

/* Explicit game-only idle input: private stereo tap with EMPTY process list,
 * exclusive=false. Requires the same exact DMN_AUDIO_TAP=1 gate; no global or
 * microphone fallback. UID/format/lifecycle contract is identical to create.
 */
dmn_audio_status dmn_audio_tap_create_idle(dmn_audio_tap **out, dmn_audio_error *error);

/* Caller MUST suspend/reset capture and borrowed buffers before retarget.
 * native_pid=0 selects nobody; positive selects only that process. The old
 * source is detached first, and the same tap/aggregate UID and ASBD are checked.
 * Failure never silently keeps the old source: a verified empty rollback leaves
 * the handle ready/idle, while TARGET_UNSAFE or NOT_READY means keep capture
 * muted/suspended. Do not destroy the handle while Wine owns its AudioUnit.
 * error.cleanup_* report failed empty rollback or validation. No IO is started.
 */
dmn_audio_status dmn_audio_tap_retarget_pid(dmn_audio_tap *tap, int32_t native_pid,
                                          dmn_audio_error *error);

/* UTF-8 UID size includes the NUL. Query with buffer=NULL, capacity=0:
 * returns BUFFER_TOO_SMALL and the required size. A cleanup-only handle returns
 * NOT_READY. No HAL call is made; these getters use validated creation data.
 */
dmn_audio_status dmn_audio_tap_get_uid(const dmn_audio_tap *tap, char *buffer,
                                      size_t capacity, size_t *required_bytes);
dmn_audio_status dmn_audio_tap_get_format(const dmn_audio_tap *tap,
                                         dmn_audio_format *out);

/* Stop consumer IO and release Wine AudioUnit/client references first.
 * Deletes aggregate before tap. Cleanup failure retains *inout for a later
 * explicit retry; successful resources are not destroyed twice. On success
 * *inout becomes NULL. Destroy(NULL handle) is an idempotent successful close.
 * This API does not sleep/retry indefinitely or start capture to close it.
 */
dmn_audio_status dmn_audio_tap_destroy(dmn_audio_tap **inout,
                                      dmn_audio_error *error);

const char *dmn_audio_status_name(dmn_audio_status status);

#ifdef __cplusplus
}
#endif
#endif
