#include "process_tap_backend.h"
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#include <sys/types.h>

namespace dmn_audio {
namespace {
dmn_audio_format summarize(const AudioStreamBasicDescription& value) noexcept {
    dmn_audio_format out{};
    out.struct_size = sizeof(out); out.version = DMN_AUDIO_FORMAT_VERSION;
    out.sample_rate = value.mSampleRate;
    out.format_id = value.mFormatID; out.format_flags = value.mFormatFlags;
    out.bytes_per_packet = value.mBytesPerPacket; out.frames_per_packet = value.mFramesPerPacket;
    out.bytes_per_frame = value.mBytesPerFrame; out.channels = value.mChannelsPerFrame;
    out.bits_per_channel = value.mBitsPerChannel;
    return out;
}
OSStatus read_uid(AudioObjectID object, AudioObjectPropertySelector selector, UID& out) noexcept {
    out.fill(0);
    const AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain};
    CFStringRef uid = nullptr; UInt32 bytes = sizeof(uid);
    const OSStatus status = AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, &uid);
    if (status != noErr) { if (uid) CFRelease(uid); return status; }
    if (bytes != sizeof(uid) || !uid || CFGetTypeID(uid) != CFStringGetTypeID()) {
        if (uid) CFRelease(uid);
        return kAudioHardwareUnspecifiedError;
    }
    const bool converted = CFStringGetCString(uid, out.data(), out.size(), kCFStringEncodingUTF8);
    CFRelease(uid);
    return converted ? noErr : kAudioHardwareUnspecifiedError;
}
OSStatus read_format(AudioObjectID object, AudioObjectPropertySelector selector,
                     AudioObjectPropertyScope scope, dmn_audio_format& out) noexcept {
    out = {};
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    AudioStreamBasicDescription format{}; UInt32 bytes = sizeof(format);
    const OSStatus status = AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, &format);
    if (status != noErr) return status;
    if (bytes != sizeof(format)) return kAudioHardwareUnspecifiedError;
    out = summarize(format); return noErr;
}
OSStatus read_description(AudioObjectID object, CATapDescription* __strong& out) noexcept {
    out = nil;
    const AudioObjectPropertyAddress address{kAudioTapPropertyDescription,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFTypeRef value = nullptr; UInt32 bytes = sizeof(value);
    const OSStatus status = AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, &value);
    id description = value ? CFBridgingRelease(value) : nil;
    if (status != noErr) return status;
    if (bytes != sizeof(value) || ![description isKindOfClass:[CATapDescription class]])
        return kAudioHardwareUnspecifiedError;
    out = description; return noErr;
}
bool selected_policy(CATapDescription* value) noexcept {
    return value && value.UUID && value.privateTap && !value.exclusive &&
           value.mixdown && !value.mono && value.muteBehavior == CATapUnmuted &&
           !value.deviceUID;
}
class NativeBackend final : public Backend {
public:
    bool supported() noexcept override {
        if (@available(macOS 14.2, *)) return true;
        return false;
    }
    int32_t lookup_process(int32_t input, uint32_t& object) noexcept override {
        object = kAudioObjectUnknown;
        const pid_t pid = input; UInt32 bytes = sizeof(AudioObjectID);
        const AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslatePIDToProcessObject,
                                                  kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMain};
        const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                           sizeof(pid), &pid, &bytes, &object);
        return status != noErr ? status : bytes == sizeof(object) ? noErr : kAudioHardwareUnspecifiedError;
    }
    int32_t create_tap(const TapPolicy& policy, uint32_t& object) noexcept override {
        object = kAudioObjectUnknown;
        if ((policy.empty_target ? policy.process_object != 0 : policy.process_object == 0) ||
            !policy.private_tap || !policy.stereo_mixdown ||
            policy.exclusive || !policy.playback_unmuted) return kAudioHardwareIllegalOperationError;
        @autoreleasepool {
            if (@available(macOS 14.2, *)) {
                @try {
                    CATapDescription* description = [[CATapDescription alloc]
                        initStereoMixdownOfProcesses:policy.empty_target ? @[] : @[@(policy.process_object)]];
                    if (!description) return kAudioHardwareUnspecifiedError;
                    description.name = @"Codex ALVR selected-process audio";
                    description.UUID = [NSUUID UUID];
                    description.privateTap = YES;
                    description.exclusive = NO;
                    description.mixdown = YES;
                    description.mono = NO;
                    description.muteBehavior = CATapUnmuted;
                    // No macOS26 bundleIDs/processRestore APIs, no global tap.
                    return AudioHardwareCreateProcessTap(description, &object);
                } @catch (NSException* exception) {
                    (void)exception; return kAudioHardwareUnspecifiedError;
                }
            }
        }
        return kAudioHardwareUnsupportedOperationError;
    }
    int32_t retarget_tap(uint32_t object, uint32_t process) noexcept override {
        @autoreleasepool {
            if (@available(macOS 14.2, *)) {
                @try {
                    const AudioObjectPropertyAddress address{kAudioTapPropertyDescription,
                        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
                    Boolean writable = false;
                    OSStatus status = AudioObjectIsPropertySettable(object, &address, &writable);
                    if (status != noErr) return status;
                    if (!writable) return kAudioHardwareUnsupportedOperationError;
                    CATapDescription* before = nil;
                    status = read_description(object, before);
                    if (status != noErr) return status;
                    if (!selected_policy(before)) return kAudioHardwareIllegalOperationError;
                    NSArray<NSNumber*>* processes = process ? @[@(process)] : @[];
                    CATapDescription* next = [[CATapDescription alloc] initStereoMixdownOfProcesses:processes];
                    if (!next) return kAudioHardwareUnspecifiedError;
                    next.name = before.name; next.UUID = before.UUID;
                    next.privateTap = YES; next.exclusive = NO;
                    next.mixdown = YES; next.mono = NO; next.muteBehavior = CATapUnmuted;
                    // Reuse the existing tap, without global or macOS26 restore APIs.
                    status = AudioObjectSetPropertyData(object, &address, 0, nullptr, sizeof(next), &next);
                    if (status != noErr) return status;
                    CATapDescription* after = nil;
                    status = read_description(object, after);
                    if (status != noErr) return status;
                    if (!selected_policy(after) || ![after.UUID isEqual:before.UUID] ||
                        ![after.processes isEqualToArray:processes]) return kAudioHardwareIllegalOperationError;
                    return noErr;
                } @catch (NSException* exception) {
                    (void)exception; return kAudioHardwareUnspecifiedError;
                }
            }
        }
        return kAudioHardwareUnsupportedOperationError;
    }
    int32_t tap_uid(uint32_t object, UID& out) noexcept override {
        return read_uid(object, kAudioTapPropertyUID, out);
    }
    int32_t tap_format(uint32_t object, dmn_audio_format& out) noexcept override {
        return read_format(object, kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal, out);
    }
    int32_t create_aggregate(const UID& uid, const AggregatePolicy& policy,
                             uint32_t& object) noexcept override {
        object = kAudioObjectUnknown;
        if (!policy.private_device || policy.stacked || policy.tap_auto_start ||
            !policy.drift_compensation) return kAudioHardwareIllegalOperationError;
        @autoreleasepool {
            @try {
                NSString* tapUID = [[NSString alloc] initWithUTF8String:uid.data()];
                NSString* aggregateUID = [@"org.codex.alvr.audio." stringByAppendingString:[NSUUID UUID].UUIDString];
                if (!tapUID || !aggregateUID) return kAudioHardwareUnspecifiedError;
                NSDictionary* description = @{
                    @kAudioAggregateDeviceNameKey: @"Codex ALVR private process-tap input",
                    @kAudioAggregateDeviceUIDKey: aggregateUID,
                    @kAudioAggregateDeviceIsPrivateKey: @YES,
                    @kAudioAggregateDeviceIsStackedKey: @NO,
                    @kAudioAggregateDeviceTapAutoStartKey: @NO,
                    @kAudioAggregateDeviceTapListKey: @[@{
                        @kAudioSubTapUIDKey: tapUID,
                        @kAudioSubTapDriftCompensationKey: @YES
                    }]
                };
                // Tap-only input. No physical output/mic subdevice/default-device query.
                // No AudioDeviceStart or IOProc is installed here.
                return AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description, &object);
            } @catch (NSException* exception) {
                (void)exception; return kAudioHardwareUnspecifiedError;
            }
        }
    }
    int32_t aggregate_uid(uint32_t object, UID& out) noexcept override {
        return read_uid(object, kAudioDevicePropertyDeviceUID, out);
    }
    int32_t aggregate_format(uint32_t object, dmn_audio_format& out) noexcept override {
        return read_format(object, kAudioDevicePropertyStreamFormat, kAudioObjectPropertyScopeInput, out);
    }
    int32_t destroy_aggregate(uint32_t object) noexcept override {
        const OSStatus status = AudioHardwareDestroyAggregateDevice(object);
        // Already absent after HAL/device teardown is an idempotent close.
        return status == kAudioHardwareBadObjectError ? noErr : status;
    }
    int32_t destroy_tap(uint32_t object) noexcept override {
        if (@available(macOS 14.2, *)) {
            const OSStatus status = AudioHardwareDestroyProcessTap(object);
            return status == kAudioHardwareBadObjectError ? noErr : status;
        }
        return kAudioHardwareUnsupportedOperationError;
    }
};
} // namespace
Backend& native_backend() noexcept { static NativeBackend instance; return instance; }
} // namespace dmn_audio
