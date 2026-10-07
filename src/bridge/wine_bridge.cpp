// Attach UTM's resource hooks to devices created by Wine's D3DMetal host.
// The native GFXT host must remain uninitialized in this path.
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <crt_externs.h>
#include <string>
#include <algorithm>

extern "C" void dmn_share_install_swizzles();
extern "C" void dmn_hooks_after_d3d11_device(void*);
void alvr_install_error_fallback();
void alvr_install_frame_trace();
void alvr_install_activation_wait();
void alvr_install_finger_grip_only();
void compositor_start_frame_trace();
void client_install_frame_trace();
void* wine_audio_interpose_table(void*, const char*);

#define MSABI __attribute__((ms_abi))
using CreateDevice = int32_t (MSABI *)(void*, uint32_t, void*, uint32_t,
    const uint32_t*, uint32_t, uint32_t, void**, uint32_t*, void**);
using CreateSwapChain = int32_t (MSABI *)(void*, uint32_t, void*, uint32_t,
    const uint32_t*, uint32_t, uint32_t, const void*, void**, void**,
    uint32_t*, void**);
static std::atomic<CreateDevice> original_device{};
static std::atomic<CreateSwapChain> original_swapchain{};
static bool enable_hooks = false;

static int32_t MSABI create_device(void* adapter, uint32_t driver, void* module,
    uint32_t flags, const uint32_t* levels, uint32_t count, uint32_t sdk,
    void** dev, uint32_t* level, void** ctx) {
    auto original = original_device.load();
    if (!original) return int32_t(0x80004005);
    alvr_install_error_fallback();
    alvr_install_activation_wait();
    alvr_install_finger_grip_only();
    alvr_install_frame_trace();
    client_install_frame_trace();
    int32_t hr = original(adapter, driver, module, flags, levels, count, sdk,
                          dev, level, ctx);
    if (hr >= 0 && dev && *dev) {
        dmn_hooks_after_d3d11_device(*dev);
        fprintf(stderr, "WINE-UTM: attached device %p pid=%d\n", *dev, getpid());
    }
    return hr;
}

static int32_t MSABI create_swapchain(void* adapter, uint32_t driver, void* module,
    uint32_t flags, const uint32_t* levels, uint32_t count, uint32_t sdk,
    const void* desc, void** swap, void** dev, uint32_t* level, void** ctx) {
    auto original = original_swapchain.load();
    if (!original) return int32_t(0x80004005);
    alvr_install_error_fallback();
    alvr_install_activation_wait();
    alvr_install_finger_grip_only();
    alvr_install_frame_trace();
    client_install_frame_trace();
    int32_t hr = original(adapter, driver, module, flags, levels, count, sdk,
                          desc, swap, dev, level, ctx);
    if (hr >= 0 && dev && *dev) dmn_hooks_after_d3d11_device(*dev);
    return hr;
}

static void* bridge_dlsym(void* handle, const char* name) {
    void* symbol = dlsym(handle, name);
    if (!enable_hooks) return symbol;
    if (!symbol || !name) return symbol;
    symbol = wine_audio_interpose_table(symbol, name);
    bool device = strcmp(name, "D3D11CreateDevice") == 0;
    bool swapchain = strcmp(name, "D3D11CreateDeviceAndSwapChain") == 0;
    if (!device && !swapchain) return symbol;
    Dl_info owner{};
    if (!dladdr(symbol, &owner) || !owner.dli_fname ||
        !strstr(owner.dli_fname, "D3DMetal.framework/")) return symbol;
    if (device) {
        original_device.store(reinterpret_cast<CreateDevice>(symbol));
        fprintf(stderr, "WINE-UTM: intercepted D3D11CreateDevice pid=%d\n", getpid());
        return reinterpret_cast<void*>(&create_device);
    }
    original_swapchain.store(reinterpret_cast<CreateSwapChain>(symbol));
    return reinterpret_cast<void*>(&create_swapchain);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void* replacement; const void* original; } interpose = {
    reinterpret_cast<const void*>(&bridge_dlsym),
    reinterpret_cast<const void*>(&dlsym)
};

__attribute__((constructor)) static void attach() {
    const char* name = getprogname();
    if (name && strcmp(name, "wineserver") == 0) return;
    // The ALVR dashboard uses OpenGL. Install Metal interop only in the VR
    // renderer processes and diagnostics, not in the desktop/client processes.
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        std::transform(arg.begin(), arg.end(), arg.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        for (const char* target : {"vrserver.exe", "vrcompositor.exe", "hlvr.exe",
                                  "wine_producer.exe", "wine_consumer.exe", "buffer_probe.exe"})
            if (arg.find(target) != std::string::npos) enable_hooks = true;
    }
    if (!enable_hooks) return;
    compositor_start_frame_trace();
    dmn_share_install_swizzles();
    fprintf(stderr, "WINE-UTM: installed Metal resource hooks pid=%d\n", getpid());
}
