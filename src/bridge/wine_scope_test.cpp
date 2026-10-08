// Offline: no Wine/VR processes, Metal, installed hooks or audio permissions.
#include <dlfcn.h>
#include <stdexcept>
#include <thread>
#include <fstream>
#include <iostream>
extern "C" void* gui_test_dlsym(void*, const char*);
extern "C" int gui_test_dladdr(const void*, Dl_info*);
#define DMN_GUI_SCOPE_TEST
#define dlsym gui_test_dlsym
#define dladdr gui_test_dladdr
#include "wine_bridge.cpp"
#undef dlsym
#undef dladdr

static std::vector<char> args;
static std::atomic<unsigned> installs{}, trace_starts{}, device_hooks{}, device_calls{}, swap_calls{}, audio_calls{};
static std::atomic<bool> throw_first{true}, client_present{false}, client_disk_present{true};
static int original_audio, cloned_audio;
static const char* owner_path = "/fake/D3DMetal.framework/Versions/A/D3DMetal";
static bool read_args(char* out, size_t& size) {
    if (args.size() > size) return false;
    memcpy(out, args.data(), args.size()); size = args.size(); return true;
}
static std::vector<char> blob(std::initializer_list<const char*> argv) {
    const int argc = int(argv.size());
    std::vector<char> out(sizeof(argc)); memcpy(out.data(), &argc, sizeof(argc));
    auto append = [&](const char* s) { out.insert(out.end(), s, s + strlen(s) + 1); };
    append("/loader/wine64"); out.insert(out.end(), 4, 0);
    for (const auto* s : argv) append(s);
    append("WINEPREFIX=/private/not-an-executable");
    append("DONT_TRUST_ENV=vrserver.exe");
    return out;
}
static void require(bool ok, const char* label) {
    if (!ok) throw std::runtime_error(label);
}
static bool client_scope_fake() { return client_present.load(); }
static bool client_disk_scope_fake() { return client_disk_present.load(); }
extern "C" void dmn_share_install_swizzles() {
    ++installs;
    require(inside_install, "installation recursion flag");
    require(!late_scope("recursive", true), "recursive activation declines, no deadlock");
    require(bridge_dlsym(nullptr, "D3D11CreateDevice") != reinterpret_cast<void*>(&create_device),
        "recursive dlsym forwards original");
    if (throw_first.exchange(false)) throw std::runtime_error("fake installer failure");
}
extern "C" void dmn_hooks_after_d3d11_device(void*) { ++device_hooks; }
void alvr_install_error_fallback() {}
void alvr_install_frame_trace() {}
void alvr_install_activation_wait() {}
void alvr_install_finger_grip_only() {}
void compositor_start_frame_trace() { ++trace_starts; }
void client_install_frame_trace() {}
void* wine_audio_interpose_table(void* value, const char* name) {
    ++audio_calls;
    require(value == &original_audio && !strcmp(name, "__wine_unix_call_funcs"), "audio forwards identity");
    return &cloned_audio;
}
static int32_t MSABI device(void* adapter, uint32_t driver, void* module, uint32_t flags,
    const uint32_t* levels, uint32_t count, uint32_t sdk, void** out, uint32_t* level, void** ctx) {
    if (client_present.load())
        require(enable_hooks.load() && installs.load() == 2, "hooks ready before original device");
    else require(!enable_hooks && !installs, "pre-VR factory forwards without resource hooks");
    require(adapter == reinterpret_cast<void*>(11) && driver == 12 && module == reinterpret_cast<void*>(13) &&
        flags == 14 && levels && *levels == 15 && count == 16 && sdk == 17 && out && level && ctx,
        "device MSABI arguments unchanged");
    ++device_calls; *out = reinterpret_cast<void*>(101); *level = 102; *ctx = reinterpret_cast<void*>(103); return 0;
}
static int32_t MSABI swapchain(void*, uint32_t, void*, uint32_t, const uint32_t*, uint32_t,
    uint32_t, const void* desc, void** swap, void** out, uint32_t* level, void** ctx) {
    require(desc == reinterpret_cast<void*>(201), "swap descriptor unchanged");
    ++swap_calls; *swap = reinterpret_cast<void*>(202); *out = reinterpret_cast<void*>(203);
    *level = 204; *ctx = reinterpret_cast<void*>(205); return 0;
}
extern "C" void* gui_test_dlsym(void*, const char* name) {
    if (!strcmp(name, "D3D11CreateDevice")) return reinterpret_cast<void*>(&device);
    if (!strcmp(name, "D3D11CreateDeviceAndSwapChain")) return reinterpret_cast<void*>(&swapchain);
    if (!strcmp(name, "__wine_unix_call_funcs")) return &original_audio;
    return nullptr;
}
extern "C" int gui_test_dladdr(const void*, Dl_info* info) { *info = {}; info->dli_fname = owner_path; return 1; }

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected exact installed vrclient path for read-only SHA control");
        for (const auto name : {"vrserver.exe", "vrcompositor.exe", "hlvr.exe", "wine_producer.exe"}) {
            std::string out;
            auto b = blob({"/path/wine64", name, "steam.exe"});
            require(kernel_pe_name(b.data(), b.size(), out) && out == name, "known argv1 exact");
            require(process_scope(out, false), "known renderer preserved");
        }
        for (const auto name : {"steam.exe", "SteamWebHelper.EXE", "alvr_dashboard.exe", "AudioSceneWatcher.exe",
                               "wineboot.exe", "setup.exe", "vrwebhelper.exe", "vrdashboard.exe"}) {
            auto b = blob({"wine64", name, "hlvr.exe", "vrserver.exe"}); std::string out;
            require(kernel_pe_name(b.data(), b.size(), out) && !process_scope(out, true), "desktop blocked despite target args");
        }
        for (const auto name : {"--game=vrserver.exe", "hlvr.exe.bak", "\"vrserver.exe\"", "ignored hlvr.exe --arg"}) {
            auto b = blob({"wine64", name, "vrcompositor.exe"}); std::string out;
            require(!kernel_pe_name(b.data(), b.size(), out), "quoted/option/substring spoof declined");
        }
        std::string out;
        auto two = blob({"wine64-preloader", "/path/wine64", "C:\\Games\\VR Game.exe"});
        require(kernel_pe_name(two.data(), two.size(), out) && out == "vr game.exe", "two loader slots and spaces");
        auto three = blob({"wine64-preloader", "wine64", "wine", "hlvr.exe"});
        require(!kernel_pe_name(three.data(), three.size(), out), "loader search bounded");
        auto short_blob = blob({"vrserver.exe"}); short_blob.resize(sizeof(int) + 4);
        require(!kernel_pe_name(short_blob.data(), short_blob.size(), out), "truncation rejected");
        auto huge_count = blob({"vrserver.exe"}); int invalid = 513; memcpy(huge_count.data(), &invalid, 4);
        require(!kernel_pe_name(huge_count.data(), huge_count.size(), out), "unbounded argc rejected");
        require(!process_scope("another_vr_game.exe", false) && process_scope("another_vr_game.exe", true), "generic requires client guard");

        std::ifstream file(argv[1], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        require(client_disk_data_matches(bytes), "exact installed SHA/PE passes");
        bytes.back() ^= 1; require(!client_disk_data_matches(bytes), "one-byte disk drift rejected");
        bytes.pop_back(); require(!client_disk_data_matches(bytes), "wrong size rejected");
        std::vector<uint8_t> image(kClientTable + 128);
        auto put = [&](size_t at, auto value) { memcpy(image.data() + at, &value, sizeof(value)); };
        put(0, uint16_t(0x5a4d)); put(0x3c, uint32_t(128)); put(128, uint32_t(0x4550));
        put(132, uint16_t(0x8664)); put(152, uint16_t(0x20b)); put(208, uint32_t(image.size()));
        put(264, uint32_t(4096)); put(4108, uint32_t(4140)); memcpy(image.data() + 4140, "vrclient_x64.dll", 16);
        const auto base = reinterpret_cast<uintptr_t>(image.data());
        for (unsigned i = 0; i < kClientMethods.size(); ++i) put(kClientTable + i * 8, uint64_t(base + kClientMethods[i]));
        require(loaded_client_matches(base), "loaded PE/name/all8 method guard passes");
        image[kClientTable + 56] ^= 1; require(!loaded_client_matches(base), "last slot drift rejected"); image[kClientTable + 56] ^= 1;
        image[4140] = 'X'; require(!loaded_client_matches(base), "loaded DLL name spoof rejected");
        require(!loaded_client_matches(1), "unmapped VM read fails safely");

        test_procargs = read_args; test_client_scope = client_scope_fake;
        test_client_disk_scope = client_disk_scope_fake;
        args = blob({"wine64"});
        char name[512] = "stale";
        require(!wine_bridge_own_pe_name(name, sizeof(name)) && !name[0], "early unknown not stale");
        require(!late_scope("constructor", false), "early unknown no install");
        require(bridge_dlsym(nullptr, "__wine_unix_call_funcs") == &cloned_audio && audio_calls == 1 && !installs,
            "audio table registration independent of graphics/early PE identity");
        client_present = true;
        for (const auto* helper : {"steam.exe", "steamwebhelper.exe", "alvr_dashboard.exe", "AudioSceneWatcher.exe"}) {
            args = blob({"wine64", helper, "hlvr.exe"});
            require(bridge_dlsym(nullptr, "D3D11CreateDevice") == reinterpret_cast<void*>(&device) && !installs,
                "actual late lookup keeps desktop/watcher unhooked even with client");
        }
        client_present = false;
        args = blob({"wine64", "another_vr_game.exe"});
        client_disk_present = false;
        require(bridge_dlsym(nullptr, "D3D11CreateDevice") == reinterpret_cast<void*>(&device) && !installs, "bad disk scope keeps factory unchanged");
        client_disk_present = true;
        auto cached = reinterpret_cast<CreateDevice>(bridge_dlsym(nullptr, "D3D11CreateDevice"));
        require(cached == &create_device && !installs, "validated disk captures pending factory without resource mutation");
        uint32_t pre_levels = 15, pre_level = 0; void *pre_dev = nullptr, *pre_ctx = nullptr;
        require(cached(reinterpret_cast<void*>(11), 12, reinterpret_cast<void*>(13), 14, &pre_levels, 16, 17,
            &pre_dev, &pre_level, &pre_ctx) == 0 && device_calls == 1 && !device_hooks && !installs,
            "cached pre-VR factory calls original once without device hooks");
        client_present = true;
        require(!late_scope("failed-install", true) && !enable_hooks && !inside_install && !inside_scope, "failed install retries safely");
        std::array<bool, 16> results{};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < results.size(); ++i) threads.emplace_back([&, i] {
            uint32_t levels = 15, level = 0; void *dev = nullptr, *ctx = nullptr;
            results[i] = cached(reinterpret_cast<void*>(11), 12, reinterpret_cast<void*>(13), 14,
                &levels, 16, 17, &dev, &level, &ctx) == 0;
        });
        for (auto& t : threads) t.join();
        for (bool r : results) require(r, "concurrent callers see complete installation");
        require(installs == 2 && trace_starts == 2 && enable_hooks, "one successful activation after one error");
        args = blob({"wine64", "another_vr_game.exe"});
        char tiny[2] = "x"; require(!wine_bridge_own_pe_name(tiny, sizeof(tiny)) && !tiny[0], "small UID buffer fails closed");
        auto wrapped = reinterpret_cast<CreateDevice>(bridge_dlsym(nullptr, "D3D11CreateDevice"));
        require(wrapped == &create_device, "late dlsym wrapped");
        uint32_t levels = 15, level = 0; void *dev = nullptr, *ctx = nullptr;
        require(wrapped(reinterpret_cast<void*>(11), 12, reinterpret_cast<void*>(13), 14, &levels, 16, 17, &dev, &level, &ctx) == 0 &&
            device_calls == 18 && device_hooks == 17 && dev == reinterpret_cast<void*>(101), "device original once and successful hook");
        void* swap = nullptr;
        auto wrapped_swap = reinterpret_cast<CreateSwapChain>(bridge_dlsym(nullptr, "D3D11CreateDeviceAndSwapChain"));
        require(wrapped_swap(nullptr, 0, nullptr, 0, nullptr, 0, 0, reinterpret_cast<void*>(201), &swap, &dev, &level, &ctx) == 0 &&
            swap_calls == 1 && device_hooks == 18 && swap == reinterpret_cast<void*>(202), "swap original once and successful hook");
        owner_path = "/fake/other-framework";
        require(bridge_dlsym(nullptr, "D3D11CreateDevice") == reinterpret_cast<void*>(&device), "non-D3DMetal owner unchanged");
        std::cout << "PASS: scope/guard/late activation/early audio/concurrency/recursion/MSABI original-once; no real hooks or VR\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
