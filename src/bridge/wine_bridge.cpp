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
#include <array>
#include <vector>
#include <mutex>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <CommonCrypto/CommonDigest.h>
#include "wine_process_scope.h"

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
namespace {
constexpr size_t kArgLimit = 32768, kNameLimit = 512;
constexpr size_t kClientBytes = 5064856;
constexpr const char* kClientHash =
    "8ac73c05c919dd82e6614aac8492a0a768240adba7faba8e6c45b1fe13d712e5";
constexpr uint32_t kClientTable = 0x3b5360;
constexpr std::array<uint32_t, 8> kClientMethods = {
    0xa9f50, 0xaa0e0, 0xaa590, 0xaa200, 0xaa510, 0xaa690, 0xaa680, 0xaa1b0
};
std::atomic<bool> enable_hooks{false};
std::once_flag hooks_once;
thread_local bool inside_scope = false;
thread_local bool inside_install = false;
#ifdef DMN_GUI_SCOPE_TEST
bool (*test_procargs)(char*, size_t&) = nullptr;
bool (*test_client_disk_scope)() = nullptr;
bool (*test_client_scope)() = nullptr;
#endif
struct FlagGuard {
    bool& flag;
    explicit FlagGuard(bool& value): flag(value) { flag = true; }
    ~FlagGuard() { flag = false; }
};
template<class T> T field(const uint8_t* bytes, size_t offset) {
    T value{}; memcpy(&value, bytes + offset, sizeof(value)); return value;
}
std::string basename(const char* text) {
    if (!text) return {};
    const size_t size = strnlen(text, kArgLimit);
    if (!size || size == kArgLimit) return {};
    std::string value(text, size);
    const auto slash = value.find_last_of("/\\");
    if (slash != std::string::npos) value.erase(0, slash + 1);
    if (value.empty() || value.size() >= kNameLimit) return {};
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool pe_name(const std::string& name) {
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0 &&
        name[0] != '-' && name.find_first_of("\"'\r\n\t=") == std::string::npos &&
        name.find(".exe") == name.size() - 4;
}
bool wine_loader(const std::string& name) {
    return name == "wine" || name == "wine64" || name == "wine-preloader" ||
        name == "wine64-preloader";
}
bool kernel_pe_name(const char* bytes, size_t size, std::string& name) {
    name.clear();
    if (!bytes || size <= sizeof(int) || size > kArgLimit) return false;
    int argc = 0; memcpy(&argc, bytes, sizeof(argc));
    if (argc <= 0 || argc > 512) return false;
    size_t at = sizeof(argc);
    auto next = [&](std::string& out) {
        if (at >= size) return false;
        const auto* end = static_cast<const char*>(memchr(bytes + at, 0, size - at));
        if (!end) return false;
        out.assign(bytes + at, end - (bytes + at));
        at += out.size() + 1;
        return true;
    };
    std::string executable, first;
    if (!next(executable) || executable.empty()) return false;
    while (at < size && !bytes[at]) ++at;
    if (!next(first)) return false;
    name = basename(first.c_str());
    for (unsigned loaders = 0; wine_loader(name) && loaders < 2; ++loaders) {
        if (argc <= int(loaders + 1) || !next(first)) { name.clear(); return false; }
        name = basename(first.c_str());
    }
    if (!pe_name(name)) { name.clear(); return false; }
    return true;
}
bool blocked_process(const std::string& name) {
    for (const char* blocked : {"steam.exe", "steamwebhelper.exe", "gameoverlayui.exe",
             "alvr_dashboard.exe", "alvr_launcher.exe", "vrwebhelper.exe", "vrdashboard.exe",
             "vrmonitor.exe", "vrstartup.exe", "wineboot.exe", "explorer.exe", "services.exe",
             "rpcss.exe", "winedevice.exe", "svchost.exe", "rundll32.exe", "cmd.exe",
             "regedit.exe", "powershell.exe", "steamsetup.exe", "setup.exe", "unins000.exe",
             "audioscenewatcher.exe", "active_scene_watch.exe", "active_game_watcher.exe",
             "alvr_scene_watcher.exe"})
        if (name == blocked) return true;
    return false;
}
bool known_renderer(const std::string& name) {
    for (const char* allowed : {"vrserver.exe", "vrcompositor.exe", "hlvr.exe",
             "wine_producer.exe", "wine_consumer.exe", "buffer_probe.exe"})
        if (name == allowed) return true;
    return false;
}
bool memory_read(uintptr_t at, void* out, size_t size) {
    if (!at || !size || size > 65536 || size > UINTPTR_MAX - at) return false;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, size,
        reinterpret_cast<mach_vm_address_t>(out), &copied) == KERN_SUCCESS && copied == size;
}
bool loaded_client_matches(uintptr_t base) {
    std::array<uint8_t, 4096> header{};
    if (!memory_read(base, header.data(), header.size()) || field<uint16_t>(header.data(), 0) != 0x5a4d)
        return false;
    const auto pe = field<uint32_t>(header.data(), 0x3c);
    if (pe > 1024 || field<uint32_t>(header.data(), pe) != 0x4550 ||
        field<uint16_t>(header.data(), pe + 4) != 0x8664 ||
        field<uint16_t>(header.data(), pe + 24) != 0x20b) return false;
    const auto size = field<uint32_t>(header.data(), pe + 80);
    const auto exports = field<uint32_t>(header.data(), pe + 136);
    if (size < kClientTable + 64 || size > (64u << 20) || exports >= size ||
        size - exports < 40 || size > UINTPTR_MAX - base) return false;
    std::array<uint8_t, 40> directory{};
    if (!memory_read(base + exports, directory.data(), directory.size())) return false;
    const auto name = field<uint32_t>(directory.data(), 12);
    constexpr char expected[] = "vrclient_x64.dll";
    std::array<char, sizeof(expected)> actual{};
    if (name >= size || size - name < actual.size() ||
        !memory_read(base + name, actual.data(), actual.size()) ||
        memcmp(actual.data(), expected, actual.size())) return false;
    std::array<uint64_t, 8> methods{};
    if (!memory_read(base + kClientTable, methods.data(), sizeof(methods))) return false;
    for (unsigned i = 0; i < methods.size(); ++i)
        if (methods[i] != base + kClientMethods[i]) return false;
    return true;
}
bool loaded_client() {
    // Bounded known Wine64 DLL reservations and normal preferred PE bases.
    unsigned regions = 0;
    for (const auto bounds : {std::array<uintptr_t, 2>{0x180000000ull, 0x200000000ull},
                              std::array<uintptr_t, 2>{0x6fff00000000ull, 0x700000000000ull}}) {
        mach_vm_address_t at = bounds[0];
        while (at < bounds[1] && regions++ < 4096) {
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            const auto status = mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
                reinterpret_cast<vm_region_info_t>(&info), &count, &object);
            if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
            if (status != KERN_SUCCESS || !size || at >= bounds[1] || size > UINTPTR_MAX - at) break;
            if ((info.protection & VM_PROT_READ) && loaded_client_matches(at)) return true;
            at += size;
        }
    }
    return false;
}
bool client_disk_data_matches(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != kClientBytes) return false;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest);
    char hex[65]{};
    for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (strcmp(hex, kClientHash) || field<uint16_t>(bytes.data(), 0) != 0x5a4d) return false;
    const auto pe = field<uint32_t>(bytes.data(), 0x3c);
    return pe <= 1024 && field<uint32_t>(bytes.data(), pe) == 0x4550 &&
        field<uint16_t>(bytes.data(), pe + 4) == 0x8664 && field<uint16_t>(bytes.data(), pe + 24) == 0x20b;
}
bool client_disk_matches(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    std::vector<uint8_t> bytes(kClientBytes);
    const bool exact = fread(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
        fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return exact && client_disk_data_matches(bytes);
}
bool generic_client_disk_scope() {
#ifdef DMN_GUI_SCOPE_TEST
    if (test_client_disk_scope) return test_client_disk_scope();
#endif
    const char* sharing = getenv("DMN_WINE_SHARING");
    if (!sharing || strcmp(sharing, "1")) return false;
    static const bool disk_ok = [] {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void*>(&generic_client_disk_scope), &info) || !info.dli_fname)
            return false;
        std::string owner = info.dli_fname;
        const std::string suffix = "/Contents/Frameworks/UTMWineAdapter/libwine-utm-bridge.dylib";
        if (owner.size() <= suffix.size() ||
            owner.compare(owner.size() - suffix.size(), suffix.size(), suffix)) return false;
        const std::string app = owner.substr(0, owner.size() - suffix.size());
        char prefix[4096]{}, expected[4096]{};
        const char* configured = getenv("WINEPREFIX");
        if (!configured || !realpath(configured, prefix) ||
            !realpath((app + "/Contents/SharedSupport/prefix").c_str(), expected) ||
            strcmp(prefix, expected)) return false;
        struct stat prefix_info{};
        if (stat(prefix, &prefix_info) || !S_ISDIR(prefix_info.st_mode) ||
            prefix_info.st_uid != getuid()) return false;
        return client_disk_matches((app +
            "/Contents/drive_c/Program Files (x86)/Steam/steamapps/common/SteamVR/bin/vrclient_x64.dll").c_str());
    }();
    return disk_ok;
}
bool generic_client_scope() {
    if (!generic_client_disk_scope()) return false;
#ifdef DMN_GUI_SCOPE_TEST
    if (test_client_scope) return test_client_scope();
#endif
    return loaded_client();
}
bool process_scope(const std::string& name, bool client_ok) {
    return pe_name(name) && !blocked_process(name) && (known_renderer(name) || client_ok);
}
bool activate_hooks(const std::string& name, const char* phase) {
    if (inside_install) return false;
    try {
        std::call_once(hooks_once, [&] {
            FlagGuard guard(inside_install);
            compositor_start_frame_trace();
            dmn_share_install_swizzles();
            enable_hooks.store(true, std::memory_order_release);
            fprintf(stderr, "WINE-UTM: installed Metal resource hooks pid=%d pe=%s phase=%s\n",
                getpid(), name.c_str(), phase);
        });
    } catch (...) {
        static std::atomic<unsigned> failures{0};
        if (failures.fetch_add(1) < 4)
            fprintf(stderr, "WINE-UTM: hook installation failed pid=%d phase=%s\n", getpid(), phase);
        return false;
    }
    return enable_hooks.load(std::memory_order_acquire);
}
bool late_scope(const char* phase, bool allow_generic) {
    if (enable_hooks.load(std::memory_order_acquire)) return true;
    if (inside_scope || inside_install) return false;
    FlagGuard guard(inside_scope);
    try {
        std::array<char, kNameLimit> name{};
        if (!wine_bridge_own_pe_name(name.data(), name.size()) || blocked_process(name.data())) return false;
        const bool known = known_renderer(name.data());
        const bool client_ok = !known && allow_generic && generic_client_scope();
        if (!process_scope(name.data(), client_ok)) {
            static std::atomic<unsigned> declined{0};
            if (allow_generic && declined.fetch_add(1) < 4)
                fprintf(stderr, "WINE-UTM: scope pending pid=%d pe=%s phase=%s vrclient_guard=0\n",
                    getpid(), name.data(), phase);
            return false; // A negative lookup is not cached: retry on the next D3D lookup.
        }
        return activate_hooks(name.data(), phase);
    } catch (...) {
        return false; // Allocation/filesystem failure must not cross dlsym's C ABI.
    }
}
bool pending_factory_scope() {
    if (inside_scope || inside_install) return false;
    FlagGuard guard(inside_scope);
    try {
        std::array<char, kNameLimit> name{};
        if (!wine_bridge_own_pe_name(name.data(), name.size()) || blocked_process(name.data())) return false;
        // Capture only a forwarding factory while import-time Wine caches its
        // pointer. Resource/device hooks still require call-time client proof.
        return known_renderer(name.data()) || generic_client_disk_scope();
    } catch (...) { return false; }
}
} // namespace

extern "C" bool wine_bridge_own_pe_name(char* out, size_t capacity) {
    if (out && capacity) out[0] = 0;
    if (!out || !capacity) return false;
    try {
        int mib[] = {CTL_KERN, KERN_PROCARGS2, getpid()};
        std::array<char, kArgLimit> bytes{};
        size_t size = bytes.size();
        std::string name;
        bool read_ok = false;
    #ifdef DMN_GUI_SCOPE_TEST
        if (test_procargs) read_ok = test_procargs(bytes.data(), size);
        else
    #endif
        read_ok = sysctl(mib, 3, bytes.data(), &size, nullptr, 0) == 0;
        if (!read_ok ||
            !kernel_pe_name(bytes.data(), size, name) || name.size() + 1 > capacity) return false;
        memcpy(out, name.c_str(), name.size() + 1);
        return true;
    } catch (...) {
        return false;
    }
}

static int32_t MSABI create_device(void* adapter, uint32_t driver, void* module,
    uint32_t flags, const uint32_t* levels, uint32_t count, uint32_t sdk,
    void** dev, uint32_t* level, void** ctx) {
    auto original = original_device.load();
    if (!original) return int32_t(0x80004005);
    const bool hooks = late_scope("d3d-create", true);
    if (hooks) {
        alvr_install_error_fallback();
        alvr_install_activation_wait();
        alvr_install_finger_grip_only();
        alvr_install_frame_trace();
        client_install_frame_trace();
    }
    int32_t hr = original(adapter, driver, module, flags, levels, count, sdk,
                          dev, level, ctx);
    if (hooks && hr >= 0 && dev && *dev) {
        dmn_hooks_after_d3d11_device(*dev);
        static std::atomic<unsigned> notices{0};
        if (notices.fetch_add(1) < 16)
            fprintf(stderr, "WINE-UTM: attached device %p pid=%d\n", *dev, getpid());
    }
    return hr;
}

static int32_t MSABI create_swapchain(void* adapter, uint32_t driver, void* module,
    uint32_t flags, const uint32_t* levels, uint32_t count, uint32_t sdk,
    const void* desc, void** swap, void** dev, uint32_t* level, void** ctx) {
    auto original = original_swapchain.load();
    if (!original) return int32_t(0x80004005);
    const bool hooks = late_scope("d3d-create-swapchain", true);
    if (hooks) {
        alvr_install_error_fallback();
        alvr_install_activation_wait();
        alvr_install_finger_grip_only();
        alvr_install_frame_trace();
        client_install_frame_trace();
    }
    int32_t hr = original(adapter, driver, module, flags, levels, count, sdk,
                          desc, swap, dev, level, ctx);
    if (hooks && hr >= 0 && dev && *dev) dmn_hooks_after_d3d11_device(*dev);
    return hr;
}

static void* bridge_dlsym(void* handle, const char* name) {
    void* symbol = dlsym(handle, name);
    if (!symbol || !name) return symbol;
    if (inside_scope || inside_install) return symbol;
    if (strcmp(name, "__wine_unix_call_funcs") == 0) {
        // Audio registration has independent opt-in/SDK/hash/table guards and
        // checks server identity at callback time. Early registration must not
        // cache Wine's unsupported table just because graphics scope is pending.
        return wine_audio_interpose_table(symbol, name);
    }
    bool device = strcmp(name, "D3D11CreateDevice") == 0;
    bool swapchain = strcmp(name, "D3D11CreateDeviceAndSwapChain") == 0;
    if (!device && !swapchain) return symbol;
    Dl_info owner{};
    if (!dladdr(symbol, &owner) || !owner.dli_fname ||
        !strstr(owner.dli_fname, "D3DMetal.framework/")) return symbol;
    if (!late_scope("d3d-lookup", true) && !pending_factory_scope()) return symbol;
    if (device) {
        original_device.store(reinterpret_cast<CreateDevice>(symbol));
        static std::atomic<unsigned> notices{0};
        if (notices.fetch_add(1) < 8)
            fprintf(stderr, "WINE-UTM: intercepted D3D11CreateDevice pid=%d\n", getpid());
        return reinterpret_cast<void*>(&create_device);
    }
    original_swapchain.store(reinterpret_cast<CreateSwapChain>(symbol));
    return reinterpret_cast<void*>(&create_swapchain);
}

#ifndef DMN_GUI_SCOPE_TEST
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void* replacement; const void* original; } interpose = {
    reinterpret_cast<const void*>(&bridge_dlsym),
    reinterpret_cast<const void*>(&dlsym)
};

__attribute__((constructor)) static void attach() {
    const char* name = getprogname();
    if (name && strcmp(name, "wineserver") == 0) return;
    std::array<char, kNameLimit> pe{};
    const bool identity = wine_bridge_own_pe_name(pe.data(), pe.size());
    if (const char* sharing = getenv("DMN_WINE_SHARING"); sharing && !strcmp(sharing, "1")) {
        fprintf(stderr, "WINE-UTM: scope constructor pid=%d pe=%s known=%u blocked=%u\n",
            getpid(), identity ? pe.data() : "unknown",
            unsigned(identity && known_renderer(pe.data())),
            unsigned(identity && blocked_process(pe.data())));
    }
    // Preserve early installation for known renderers. Generic games require
    // the loaded client guard at their actual D3DMetal lookup, after Wine init.
    late_scope("constructor", false);
}
#endif
