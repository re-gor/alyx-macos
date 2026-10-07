#pragma once
#include "process_tap.h"
#include <array>
#include <cstdint>

namespace dmn_audio {
using UID = std::array<char, DMN_AUDIO_UID_MAX_BYTES>;
struct TapPolicy {
    uint32_t process_object;
    bool private_tap = true;
    bool stereo_mixdown = true;
    bool exclusive = false;
    bool playback_unmuted = true;
};
struct AggregatePolicy {
    bool private_device = true;
    bool stacked = false;
    bool tap_auto_start = false;
    bool drift_compensation = true;
};

/* Pure lifecycle engine; test binary supplies a fake native_backend instead of
 * linking process_tap_native.mm or CoreAudio. No test can create a real tap.
 */
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool supported() noexcept = 0;
    virtual int32_t lookup_process(int32_t pid, uint32_t& object) noexcept = 0;
    virtual int32_t create_tap(const TapPolicy&, uint32_t& tap) noexcept = 0;
    virtual int32_t tap_uid(uint32_t tap, UID&) noexcept = 0;
    virtual int32_t tap_format(uint32_t tap, dmn_audio_format&) noexcept = 0;
    virtual int32_t create_aggregate(const UID& tap_uid, const AggregatePolicy&,
                                     uint32_t& device) noexcept = 0;
    virtual int32_t aggregate_uid(uint32_t device, UID&) noexcept = 0;
    virtual int32_t aggregate_format(uint32_t device, dmn_audio_format&) noexcept = 0;
    virtual int32_t destroy_aggregate(uint32_t device) noexcept = 0;
    virtual int32_t destroy_tap(uint32_t tap) noexcept = 0;
};
Backend& native_backend() noexcept;
bool valid_uid(const UID&, size_t& bytes) noexcept;
bool valid_format(const dmn_audio_format&) noexcept;
} // namespace dmn_audio
