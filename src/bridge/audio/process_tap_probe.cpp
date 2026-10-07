#include "process_tap.h"
#include <cstdio>
#include <cstdlib>

int main() {
    // This smoke binary deliberately has no live-create option, even if its
    // parent's environment enables the future Wine callback.
    setenv("DMN_AUDIO_TAP", "0", 1);
    dmn_audio_tap* handle = nullptr; dmn_audio_error error{};
    const auto status = dmn_audio_tap_create(123, &handle, &error);
    if (status != DMN_AUDIO_DISABLED || handle) return 1;
    if (dmn_audio_tap_create(0, &handle, &error) != DMN_AUDIO_INVALID_ARGUMENT || handle) return 1;
    if (dmn_audio_tap_destroy(&handle, &error) != DMN_AUDIO_OK) return 1;
    puts("PASS: native-linked smoke is disabled; no backend/HAL create, IO or permission call.");
    puts("Live tap/aggregate visibility and TCC remain deliberately untested.");
}
