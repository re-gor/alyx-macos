#include "process_tap_backend.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

struct dmn_audio_tap {
    dmn_audio::Backend* backend = nullptr;
    uint32_t tap_id = 0;
    uint32_t aggregate_id = 0;
    bool ready = false;
    dmn_audio::UID uid{};
    size_t uid_bytes = 0;
    dmn_audio_format format{};
};
static_assert(sizeof(dmn_audio_format) == 48, "public format ABI drift");
static_assert(sizeof(dmn_audio_error) == 16, "public error ABI drift");

namespace dmn_audio {
bool valid_uid(const UID& uid, size_t& bytes) noexcept {
    bytes = 0;
    size_t n = 0;
    while (n < uid.size() && uid[n]) ++n;
    if (!n || n == uid.size()) return false;
    // Bounded UTF-8, excluding control characters, overlong sequences/surrogates.
    for (size_t i = 0; i < n;) {
        const uint8_t lead = uint8_t(uid[i++]);
        if (lead < 0x80) { if (lead < 0x20 || lead == 0x7f) return false; continue; }
        unsigned extra = 0; uint32_t value = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { extra = 1; value = lead & 31; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { extra = 2; value = lead & 15; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { extra = 3; value = lead & 7; minimum = 0x10000; }
        else return false;
        if (extra > n - i) return false;
        while (extra--) {
            const uint8_t next = uint8_t(uid[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    bytes = n + 1; return true;
}
bool valid_format(const dmn_audio_format& f) noexcept {
    constexpr uint32_t lpcm = 0x6c70636d, floating = 1, big_endian = 2;
    constexpr uint32_t signed_integer = 4, packed = 8, planar = 32;
    if (f.format_id != lpcm || f.channels != 2 || !std::isfinite(f.sample_rate) ||
        f.sample_rate < 8000 || f.sample_rate > 192000 || std::floor(f.sample_rate) != f.sample_rate ||
        f.frames_per_packet != 1 || (f.format_flags & big_endian) || !(f.format_flags & packed)) return false;
    const bool is_float = f.format_flags & floating, is_integer = f.format_flags & signed_integer;
    if (is_float == is_integer || (is_float && f.bits_per_channel != 32) ||
        (is_integer && f.bits_per_channel != 16 && f.bits_per_channel != 32)) return false;
    const uint32_t frame_bytes = (f.bits_per_channel / 8) * ((f.format_flags & planar) ? 1 : f.channels);
    return f.bytes_per_frame == frame_bytes && f.bytes_per_packet == frame_bytes;
}
} // namespace dmn_audio

namespace {
dmn_audio_status cleanup(dmn_audio_tap& tap, dmn_audio_error& error) noexcept {
    tap.ready = false;
    if (tap.aggregate_id) {
        const int32_t status = tap.backend->destroy_aggregate(tap.aggregate_id);
        if (status) {
            error.cleanup_stage = DMN_AUDIO_STAGE_DESTROY_AGGREGATE;
            error.cleanup_os_status = status; return DMN_AUDIO_CLEANUP_FAILED;
        }
        tap.aggregate_id = 0;
    }
    if (tap.tap_id) {
        const int32_t status = tap.backend->destroy_tap(tap.tap_id);
        if (status) {
            error.cleanup_stage = DMN_AUDIO_STAGE_DESTROY_TAP;
            error.cleanup_os_status = status; return DMN_AUDIO_CLEANUP_FAILED;
        }
        tap.tap_id = 0;
    }
    return DMN_AUDIO_OK;
}
bool same_format(const dmn_audio_format& a, const dmn_audio_format& b) noexcept {
    return a.sample_rate == b.sample_rate && a.format_id == b.format_id &&
           a.format_flags == b.format_flags && a.bytes_per_packet == b.bytes_per_packet &&
           a.frames_per_packet == b.frames_per_packet && a.bytes_per_frame == b.bytes_per_frame &&
           a.channels == b.channels && a.bits_per_channel == b.bits_per_channel;
}
} // namespace

extern "C" dmn_audio_status dmn_audio_tap_create(int32_t pid, dmn_audio_tap** out,
                                                 dmn_audio_error* supplied) {
    dmn_audio_error error{};
    const auto finish = [&](dmn_audio_status status) { if (supplied) *supplied = error; return status; };
    if (!out || *out || pid <= 0) {
        error.stage = DMN_AUDIO_STAGE_GATE; return finish(DMN_AUDIO_INVALID_ARGUMENT);
    }
    const char* enabled = std::getenv("DMN_AUDIO_TAP");
    if (!enabled || std::strcmp(enabled, "1")) {
        error.stage = DMN_AUDIO_STAGE_GATE; return finish(DMN_AUDIO_DISABLED);
    }
    auto* tap = new(std::nothrow) dmn_audio_tap;
    if (!tap) { error.stage = DMN_AUDIO_STAGE_ALLOCATION; return finish(DMN_AUDIO_OUT_OF_MEMORY); }
    tap->backend = &dmn_audio::native_backend();
    const auto fail = [&](dmn_audio_status status) {
        if (cleanup(*tap, error) != DMN_AUDIO_OK) *out = tap;
        else delete tap;
        return finish(status);
    };
    error.stage = DMN_AUDIO_STAGE_OS;
    if (!tap->backend->supported()) return fail(DMN_AUDIO_UNSUPPORTED_OS);
    uint32_t process = 0;
    error.stage = DMN_AUDIO_STAGE_PID_LOOKUP;
    error.os_status = tap->backend->lookup_process(pid, process);
    if (error.os_status) return fail(DMN_AUDIO_HAL_FAILURE);
    if (!process) return fail(DMN_AUDIO_PRODUCER_NOT_FOUND);
    error.stage = DMN_AUDIO_STAGE_CREATE_TAP;
    error.os_status = tap->backend->create_tap({process}, tap->tap_id);
    if (error.os_status || !tap->tap_id) return fail(DMN_AUDIO_HAL_FAILURE);
    dmn_audio::UID tap_uid{}; size_t tap_uid_bytes = 0;
    error.stage = DMN_AUDIO_STAGE_TAP_UID;
    error.os_status = tap->backend->tap_uid(tap->tap_id, tap_uid);
    if (error.os_status) return fail(DMN_AUDIO_HAL_FAILURE);
    if (!dmn_audio::valid_uid(tap_uid, tap_uid_bytes)) return fail(DMN_AUDIO_BAD_UID);
    dmn_audio_format tap_format{};
    error.stage = DMN_AUDIO_STAGE_TAP_FORMAT;
    error.os_status = tap->backend->tap_format(tap->tap_id, tap_format);
    if (error.os_status) return fail(DMN_AUDIO_HAL_FAILURE);
    if (!dmn_audio::valid_format(tap_format)) return fail(DMN_AUDIO_UNSUPPORTED_FORMAT);
    error.stage = DMN_AUDIO_STAGE_CREATE_AGGREGATE;
    error.os_status = tap->backend->create_aggregate(tap_uid, {}, tap->aggregate_id);
    if (error.os_status || !tap->aggregate_id) return fail(DMN_AUDIO_HAL_FAILURE);
    error.stage = DMN_AUDIO_STAGE_AGGREGATE_UID;
    error.os_status = tap->backend->aggregate_uid(tap->aggregate_id, tap->uid);
    if (error.os_status) return fail(DMN_AUDIO_HAL_FAILURE);
    if (!dmn_audio::valid_uid(tap->uid, tap->uid_bytes)) return fail(DMN_AUDIO_BAD_UID);
    error.stage = DMN_AUDIO_STAGE_AGGREGATE_FORMAT;
    error.os_status = tap->backend->aggregate_format(tap->aggregate_id, tap->format);
    if (error.os_status) return fail(DMN_AUDIO_HAL_FAILURE);
    if (!dmn_audio::valid_format(tap->format) || !same_format(tap_format, tap->format))
        return fail(DMN_AUDIO_UNSUPPORTED_FORMAT);
    tap->format.struct_size = sizeof(dmn_audio_format);
    tap->format.version = DMN_AUDIO_FORMAT_VERSION;
    tap->format.reserved = 0;
    tap->ready = true; *out = tap; error = {};
    return finish(DMN_AUDIO_OK);
}

extern "C" dmn_audio_status dmn_audio_tap_get_uid(const dmn_audio_tap* tap, char* buffer,
                                                   size_t capacity, size_t* required) {
    if (!required) return DMN_AUDIO_INVALID_ARGUMENT;
    *required = 0;
    if (!tap || (!buffer && capacity)) return DMN_AUDIO_INVALID_ARGUMENT;
    if (!tap->ready) return DMN_AUDIO_NOT_READY;
    *required = tap->uid_bytes;
    if (!buffer || capacity < tap->uid_bytes) return DMN_AUDIO_BUFFER_TOO_SMALL;
    std::memcpy(buffer, tap->uid.data(), tap->uid_bytes); return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_get_format(const dmn_audio_tap* tap,
                                                      dmn_audio_format* out) {
    if (!out) return DMN_AUDIO_INVALID_ARGUMENT;
    *out = {};
    if (!tap) return DMN_AUDIO_INVALID_ARGUMENT;
    if (!tap->ready) return DMN_AUDIO_NOT_READY;
    *out = tap->format; return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_destroy(dmn_audio_tap** inout, dmn_audio_error* supplied) {
    dmn_audio_error error{};
    if (supplied) *supplied = error;
    if (!inout) return DMN_AUDIO_INVALID_ARGUMENT;
    if (!*inout) return DMN_AUDIO_OK;
    if (cleanup(**inout, error) != DMN_AUDIO_OK) {
        if (supplied) *supplied = error;
        return DMN_AUDIO_CLEANUP_FAILED;
    }
    delete *inout; *inout = nullptr; return DMN_AUDIO_OK;
}
extern "C" const char* dmn_audio_status_name(dmn_audio_status status) {
    switch (status) {
#define DMN_NAME(x) case x: return #x
        DMN_NAME(DMN_AUDIO_OK); DMN_NAME(DMN_AUDIO_DISABLED); DMN_NAME(DMN_AUDIO_INVALID_ARGUMENT);
        DMN_NAME(DMN_AUDIO_UNSUPPORTED_OS); DMN_NAME(DMN_AUDIO_PRODUCER_NOT_FOUND);
        DMN_NAME(DMN_AUDIO_HAL_FAILURE); DMN_NAME(DMN_AUDIO_BAD_UID);
        DMN_NAME(DMN_AUDIO_UNSUPPORTED_FORMAT); DMN_NAME(DMN_AUDIO_BUFFER_TOO_SMALL);
        DMN_NAME(DMN_AUDIO_NOT_READY); DMN_NAME(DMN_AUDIO_CLEANUP_FAILED);
        DMN_NAME(DMN_AUDIO_OUT_OF_MEMORY); DMN_NAME(DMN_AUDIO_INTERNAL_ERROR);
#undef DMN_NAME
        default: return "DMN_AUDIO_UNKNOWN_STATUS";
    }
}
