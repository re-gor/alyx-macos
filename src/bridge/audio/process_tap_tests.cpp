#include "process_tap_backend.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
void check(bool value, const char* label) {
    if (!value) { fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
enum Op { Supported, Lookup, CreateTap, RetargetTap, TapUID, TapFormat, CreateAggregate,
          AggregateUID, AggregateFormat, DestroyAggregate, DestroyTap, NoFailure };
dmn_audio_format stereo() {
    dmn_audio_format f{}; f.sample_rate = 48000; f.format_id = 0x6c70636d;
    f.format_flags = 9; f.bits_per_channel = 32; f.channels = 2;
    f.bytes_per_frame = f.bytes_per_packet = 8; f.frames_per_packet = 1; return f;
}
struct Fake final : dmn_audio::Backend {
    std::array<Op, 256> calls{}; size_t count = 0;
    Op failed = NoFailure; bool available = true, found = true, partial = false;
    bool active_tap = false, active_aggregate = false, invalid_uid = false;
    int fail_aggregate_close = 0, fail_tap_close = 0;
    int32_t last_pid = 0;
    uint32_t selected_process = 0;
    unsigned retarget_calls = 0, fail_retarget_call = 0;
    bool target_uid_drift = false, target_format_drift = false;
    dmn_audio_format first = stereo(), second = stereo();
    int32_t record(Op op) noexcept {
        if (count == calls.size()) std::abort();
        calls[count++] = op; return failed == op ? -1001 : 0;
    }
    bool supported() noexcept override { record(Supported); return available; }
    int32_t lookup_process(int32_t pid, uint32_t& object) noexcept override {
        last_pid = pid; object = found ? 42 : 0; return record(Lookup);
    }
    int32_t create_tap(const dmn_audio::TapPolicy& p, uint32_t& id) noexcept override {
        check((p.empty_target ? p.process_object == 0 : p.process_object == 42) &&
              p.private_tap && p.stereo_mixdown && !p.exclusive &&
              p.playback_unmuted, "specific private stereo unmuted tap policy");
        const auto status = record(CreateTap); id = (!status || partial) ? 100 : 0;
        selected_process = p.process_object;
        active_tap = id != 0; return status;
    }
    int32_t retarget_tap(uint32_t id, uint32_t process) noexcept override {
        check(id == 100 && active_tap && (process == 0 || process == 42), "owned tap selected-only target");
        ++retarget_calls; const auto status = record(RetargetTap);
        const bool failure = status || retarget_calls == fail_retarget_call;
        if (!failure || partial) selected_process = process;
        return failure ? -3001 : 0;
    }
    int32_t tap_uid(uint32_t id, dmn_audio::UID& uid) noexcept override {
        check(id == 100, "owned tap UID");
        std::strcpy(uid.data(), target_uid_drift && selected_process ? "changed-tap-uid" : "tap-uid");
        return record(TapUID);
    }
    int32_t tap_format(uint32_t id, dmn_audio_format& f) noexcept override {
        check(id == 100, "owned tap format"); f = first;
        if (target_format_drift && selected_process) f.sample_rate = 44100;
        return record(TapFormat);
    }
    int32_t create_aggregate(const dmn_audio::UID& uid, const dmn_audio::AggregatePolicy& p,
                             uint32_t& id) noexcept override {
        check(!std::strcmp(uid.data(), "tap-uid") && p.private_device && !p.stacked &&
              !p.tap_auto_start && p.drift_compensation, "private tap-only aggregate policy");
        const auto status = record(CreateAggregate); id = (!status || partial) ? 200 : 0;
        active_aggregate = id != 0; return status;
    }
    int32_t aggregate_uid(uint32_t id, dmn_audio::UID& uid) noexcept override {
        check(id == 200, "owned aggregate UID");
        if (invalid_uid) uid.fill('x'); else std::strcpy(uid.data(), "private-aggregate-uid");
        return record(AggregateUID);
    }
    int32_t aggregate_format(uint32_t id, dmn_audio_format& f) noexcept override {
        check(id == 200, "owned aggregate format"); f = second; return record(AggregateFormat);
    }
    int32_t destroy_aggregate(uint32_t id) noexcept override {
        check(id == 200 && active_aggregate, "aggregate never double-destroyed"); record(DestroyAggregate);
        if (fail_aggregate_close) { --fail_aggregate_close; return -2001; }
        active_aggregate = false; return 0;
    }
    int32_t destroy_tap(uint32_t id) noexcept override {
        check(id == 100 && active_tap && !active_aggregate, "tap destroyed only after aggregate"); record(DestroyTap);
        if (fail_tap_close) { --fail_tap_close; return -2002; }
        active_tap = false; return 0;
    }
} fake;
void fresh() { fake = Fake{}; }
void no_leaks() { check(!fake.active_tap && !fake.active_aggregate, "no fake resources leaked"); }
} // namespace

// Test-only provider replaces the native implementation. No CoreAudio framework
// is linked to this binary, so even enabled create cannot reach real HAL/TCC.
namespace dmn_audio { Backend& native_backend() noexcept { return fake; } }

int main() {
    dmn_audio_tap* tap = nullptr; dmn_audio_error error{};
    unsetenv("DMN_AUDIO_TAP");
    check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_DISABLED && fake.count == 0,
          "absent gate invokes zero backend/HAL operations");
    for (const char* value : {"0", "true", "1 ", ""}) {
        setenv("DMN_AUDIO_TAP", value, 1);
        check(dmn_audio_tap_create(123, &tap, nullptr) == DMN_AUDIO_DISABLED && fake.count == 0,
              "gate must match exactly1");
    }
    setenv("DMN_AUDIO_TAP", "1", 1);
    for (int32_t pid : {0, -1, INT32_MIN})
        check(dmn_audio_tap_create(pid, &tap, nullptr) == DMN_AUDIO_INVALID_ARGUMENT && fake.count == 0,
              "invalid producer never enters backend");
    check(dmn_audio_tap_create(123, nullptr, nullptr) == DMN_AUDIO_INVALID_ARGUMENT && fake.count == 0,
          "null output rejected before backend");
    tap = reinterpret_cast<dmn_audio_tap*>(1);
    check(dmn_audio_tap_create(123, &tap, nullptr) == DMN_AUDIO_INVALID_ARGUMENT && fake.count == 0,
          "live output never overwritten"); tap = nullptr;

    fresh(); fake.available = false;
    check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_UNSUPPORTED_OS && !tap && fake.count == 1,
          "unsupported OS reaches no resource/HAL operations");
    fresh(); fake.found = false;
    check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_PRODUCER_NOT_FOUND && !tap && fake.count == 2,
          "unknown PID is not global fallback");

    for (Op op : {Lookup, CreateTap, TapUID, TapFormat, CreateAggregate, AggregateUID, AggregateFormat}) {
        fresh(); fake.failed = op;
        check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_HAL_FAILURE && !tap &&
              error.os_status == -1001 && error.cleanup_os_status == 0, "primary failure and rollback");
        no_leaks();
    }
    for (Op op : {CreateTap, CreateAggregate}) {
        fresh(); fake.failed = op; fake.partial = true;
        check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_HAL_FAILURE && !tap,
              "failure with returned resource ID still rolls back"); no_leaks();
    }
    fresh(); fake.invalid_uid = true;
    check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_BAD_UID && !tap,
          "unterminated UID cannot be advertised"); no_leaks();
    fresh(); fake.second.channels = 1;
    check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_UNSUPPORTED_FORMAT && !tap,
          "nonstereo aggregate rejected before exposure"); no_leaks();
    fresh(); fake.second.sample_rate = 44100;
    check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_UNSUPPORTED_FORMAT && !tap,
          "tap/aggregate mismatch requires explicit conversion policy"); no_leaks();

    fresh();
    check(dmn_audio_tap_create(456, &tap, &error) == DMN_AUDIO_OK && tap && fake.last_pid == 456,
          "explicit positive PID success");
    const size_t before_getters = fake.count;
    size_t needed = 0; std::array<char, 64> output; output.fill('?');
    check(dmn_audio_tap_get_uid(tap, nullptr, 0, &needed) == DMN_AUDIO_BUFFER_TOO_SMALL &&
          needed == std::strlen("private-aggregate-uid") + 1, "UID size includes NUL");
    check(dmn_audio_tap_get_uid(tap, output.data(), needed - 1, &needed) == DMN_AUDIO_BUFFER_TOO_SMALL &&
          output[0] == '?', "undersized UID buffer untouched");
    check(dmn_audio_tap_get_uid(tap, output.data(), needed, &needed) == DMN_AUDIO_OK &&
          !std::strcmp(output.data(), "private-aggregate-uid") && output[needed] == '?', "exact bounded UID copy");
    dmn_audio_format format{};
    check(dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_OK && format.sample_rate == 48000 &&
          format.channels == 2 && format.struct_size == 48 && format.version == 1 && fake.count == before_getters,
          "validated cached getters make no HAL calls");
    fake.fail_aggregate_close = 1;
    check(dmn_audio_tap_destroy(&tap, &error) == DMN_AUDIO_CLEANUP_FAILED && tap && fake.active_tap &&
          fake.calls[fake.count-1] == DestroyAggregate && error.cleanup_os_status == -2001,
          "failed aggregate cleanup retains ownership and never destroys tap");
    check(dmn_audio_tap_get_uid(tap, output.data(), output.size(), &needed) == DMN_AUDIO_NOT_READY && needed == 0,
          "closing handle cannot expose UID");
    check(dmn_audio_tap_destroy(&tap, &error) == DMN_AUDIO_OK && !tap, "explicit close retry"); no_leaks();
    check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK, "idempotent complete close");

    fresh(); check(dmn_audio_tap_create(123, &tap, nullptr) == DMN_AUDIO_OK, "second valid tap");
    fake.fail_tap_close = 1;
    check(dmn_audio_tap_destroy(&tap, &error) == DMN_AUDIO_CLEANUP_FAILED && tap && !fake.active_aggregate,
          "tap close failure keeps only pending tap");
    const auto retry_start = fake.count;
    check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK && !tap &&
          fake.count == retry_start + 1 && fake.calls[retry_start] == DestroyTap,
          "retry never destroys completed aggregate twice"); no_leaks();

    fresh(); fake.failed = AggregateFormat; fake.fail_aggregate_close = 1;
    check(dmn_audio_tap_create(123, &tap, &error) == DMN_AUDIO_HAL_FAILURE && tap &&
          error.os_status == -1001 && error.cleanup_os_status == -2001,
          "create primary failure and failed rollback both preserved");
    check(dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_NOT_READY && format.channels == 0,
          "cleanup-only handle cannot expose format");
    check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK && !tap, "cleanup-only explicit retry"); no_leaks();

    fresh(); unsetenv("DMN_AUDIO_TAP");
    check(dmn_audio_tap_create_idle(&tap, &error) == DMN_AUDIO_DISABLED && fake.count == 0,
          "idle exact opt-in gate invokes no backend");
    setenv("DMN_AUDIO_TAP", "1", 1);
    check(dmn_audio_tap_create_idle(&tap, &error) == DMN_AUDIO_OK && tap &&
          fake.last_pid == 0 && fake.selected_process == 0 && fake.calls[1] == CreateTap,
          "idle empty process list skips PID lookup, never global");
    std::array<char, 64> idle_uid{};
    check(dmn_audio_tap_get_uid(tap, idle_uid.data(), idle_uid.size(), &needed) == DMN_AUDIO_OK,
          "idle advertised stable private UID");
    const auto before_invalid = fake.count;
    check(dmn_audio_tap_retarget_pid(tap, -1, &error) == DMN_AUDIO_INVALID_ARGUMENT &&
          fake.count == before_invalid, "negative target rejected before HAL");
    setenv("DMN_AUDIO_TAP", "0", 1);
    check(dmn_audio_tap_retarget_pid(tap, 789, &error) == DMN_AUDIO_DISABLED &&
          fake.count == before_invalid, "retarget disabled gate invokes zero backend operations");
    setenv("DMN_AUDIO_TAP", "1", 1);
    check(dmn_audio_tap_retarget_pid(tap, 789, &error) == DMN_AUDIO_OK &&
          fake.last_pid == 789 && fake.selected_process == 42 && fake.retarget_calls == 2,
          "switch detaches old first then selects exact new process");
    check(dmn_audio_tap_get_uid(tap, output.data(), output.size(), &needed) == DMN_AUDIO_OK &&
          !std::strcmp(output.data(), idle_uid.data()) && dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_OK &&
          format.sample_rate == 48000, "target switch preserves advertised aggregate UID and ASBD");
    check(dmn_audio_tap_retarget_pid(tap, 0, &error) == DMN_AUDIO_OK && fake.selected_process == 0,
          "zero target detaches without any new PID lookup");
    fake.found = false;
    check(dmn_audio_tap_retarget_pid(tap, 999, &error) == DMN_AUDIO_PRODUCER_NOT_FOUND &&
          fake.selected_process == 0 && dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_OK,
          "missing target remains validated idle, not prior/global capture");
    check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK, "idle switched lifecycle close"); no_leaks();

    fresh(); check(dmn_audio_tap_create_idle(&tap, nullptr) == DMN_AUDIO_OK, "detach-failure setup");
    fake.fail_retarget_call = 1;
    check(dmn_audio_tap_retarget_pid(tap, 789, &error) == DMN_AUDIO_TARGET_UNSAFE &&
          error.stage == DMN_AUDIO_STAGE_DETACH_TARGET && error.os_status == -3001 &&
          dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_NOT_READY,
          "unconfirmed detach is unsafe and not advertised");
    check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK, "unsafe close after caller stops IO"); no_leaks();

    fresh(); check(dmn_audio_tap_create_idle(&tap, nullptr) == DMN_AUDIO_OK, "set-failure setup");
    fake.fail_retarget_call = 2; fake.partial = true;
    check(dmn_audio_tap_retarget_pid(tap, 789, &error) == DMN_AUDIO_HAL_FAILURE &&
          error.stage == DMN_AUDIO_STAGE_SET_TARGET && error.os_status == -3001 &&
          fake.selected_process == 0 && fake.retarget_calls == 3 &&
          dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_OK,
          "partially successful set rolls back to verified empty target");
    check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK, "failed set lifecycle close"); no_leaks();

    for (bool uid_drift : {true, false}) {
        fresh(); check(dmn_audio_tap_create_idle(&tap, nullptr) == DMN_AUDIO_OK, "drift setup");
        fake.target_uid_drift = uid_drift; fake.target_format_drift = !uid_drift;
        check(dmn_audio_tap_retarget_pid(tap, 789, &error) ==
              (uid_drift ? DMN_AUDIO_BAD_UID : DMN_AUDIO_UNSUPPORTED_FORMAT) &&
              error.stage == DMN_AUDIO_STAGE_VALIDATE_TARGET && fake.selected_process == 0 &&
              dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_OK,
              "UID or ASBD drift rejected, verified empty rollback preserves original format");
        check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK, "drift lifecycle close"); no_leaks();
    }
    fresh(); check(dmn_audio_tap_create_idle(&tap, nullptr) == DMN_AUDIO_OK, "rollback-failure setup");
    fake.target_format_drift = true; fake.fail_retarget_call = 3;
    check(dmn_audio_tap_retarget_pid(tap, 789, &error) == DMN_AUDIO_TARGET_UNSAFE &&
          error.stage == DMN_AUDIO_STAGE_VALIDATE_TARGET &&
          error.cleanup_stage == DMN_AUDIO_STAGE_DETACH_TARGET && error.cleanup_os_status == -3001 &&
          dmn_audio_tap_get_format(tap, &format) == DMN_AUDIO_NOT_READY,
          "failed empty rollback requires capture muted/suspended");
    check(dmn_audio_tap_destroy(&tap, nullptr) == DMN_AUDIO_OK, "rollback unsafe lifecycle close"); no_leaks();

    dmn_audio::UID uid{}; size_t bytes = 99;
    check(!dmn_audio::valid_uid(uid, bytes) && bytes == 0, "empty UID");
    std::strcpy(uid.data(), "good-\xc3\xa9"); check(dmn_audio::valid_uid(uid, bytes), "valid bounded UTF8");
    for (const char* invalid : {"bad\nuid", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"}) {
        uid.fill(0); std::strcpy(uid.data(), invalid); check(!dmn_audio::valid_uid(uid, bytes), "invalid UTF8/control UID");
    }
    for (unsigned i = 0; i < 7; ++i) {
        auto f = stereo();
        switch(i) {
            case 0: f.channels = 1; break;
            case 1: f.sample_rate = std::numeric_limits<double>::quiet_NaN(); break;
            case 2: f.sample_rate = 48000.5; break;
            case 3: f.format_flags |= 2; break;
            case 4: f.bits_per_channel = 64; break;
            case 5: f.bytes_per_packet = 7; break;
            case 6: f.frames_per_packet = 0; break;
        }
        check(!dmn_audio::valid_format(f), "unsupported ASBD cannot be advertised");
    }
    auto planar = stereo(); planar.format_flags |= 32; planar.bytes_per_packet = planar.bytes_per_frame = 4;
    check(dmn_audio::valid_format(planar), "stereo noninterleaved float32 accepted");
    auto pcm16 = stereo(); pcm16.format_flags = 12; pcm16.bits_per_channel = 16;
    pcm16.bytes_per_packet = pcm16.bytes_per_frame = 4;
    check(dmn_audio::valid_format(pcm16), "stereo packed signed PCM16 accepted");
    puts("PASS: exact gates, old-create lifecycle, empty-target retarget, stable UID/ASBD, rollback and unsafe-state tests.");
    puts("No CoreAudio/Foundation native backend linked. No HAL, IO, capture or TCC request occurred.");
}
