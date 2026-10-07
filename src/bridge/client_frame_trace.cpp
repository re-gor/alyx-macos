// Opt-in private DirectInternal publication trace, before SteamVR server IPC.
// vrcompositor only; one guarded data-vtable slot; originals called unchanged.
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <crt_externs.h>

void client_install_frame_trace();
namespace {
#define MSABI __attribute__((ms_abi))
constexpr size_t kFileBytes = 5064856;
constexpr char kHash[] = "8ac73c05c919dd82e6614aac8492a0a768240adba7faba8e6c45b1fe13d712e5";
constexpr uint32_t kTable = 0x3b5360;
constexpr std::array<uint32_t, 8> kMethods = {
    0xa9f50, 0xaa0e0, 0xaa590, 0xaa200, 0xaa510, 0xaa690, 0xaa680, 0xaa1b0
};
constexpr unsigned kRecords = 384, kIdentities = 128;
struct Eye {
    uint64_t texture, depth;
    float bounds[4], projection[16], pose[12];
    float prediction_offset;
    uint32_t padding;
};
using Bounds = std::array<float, 4>;
static_assert(sizeof(Eye) == 152 && alignof(Eye) == 8, "private eye ABI drift");
static_assert(offsetof(Eye, bounds) == 16 && offsetof(Eye, projection) == 32 &&
              offsetof(Eye, pose) == 96 && offsetof(Eye, prediction_offset) == 144,
              "private eye field ABI drift");
using Submit = void (MSABI *)(void*, uint32_t, uint32_t, const Eye*, double, const Bounds*);
std::atomic<Submit> original{};
std::atomic<bool> capped{false};
std::atomic<uint64_t> sequence{0};

template<class T> T get(const uint8_t* b, size_t at) {
    T value{}; memcpy(&value, b + at, sizeof(value)); return value;
}
bool enabled(const char* value) { return value && strcmp(value, "1") == 0; }
bool compositor_argument(const char* value) {
    std::string s = value ? value : "";
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const auto slash = s.find_last_of("/\\");
    return s.substr(slash == std::string::npos ? 0 : slash + 1) == "vrcompositor.exe";
}
bool compositor_process() {
    for (int i = 0; i < *_NSGetArgc(); ++i)
        if (compositor_argument((*_NSGetArgv())[i])) return true;
    return false;
}
std::string dll_path(const char* owner) {
    const std::string s = owner ? owner : "";
    const std::string suffix = "/Contents/Frameworks/UTMWineAdapter/libwine-utm-bridge.dylib";
    if (s.size() <= suffix.size() || s.compare(s.size() - suffix.size(), suffix.size(), suffix)) return {};
    return s.substr(0, s.size() - suffix.size()) +
        "/Contents/drive_c/Program Files (x86)/Steam/steamapps/common/SteamVR/bin/vrclient_x64.dll";
}
bool disk_guard(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    std::vector<uint8_t> b(kFileBytes);
    const size_t n = fread(b.data(), 1, b.size(), file);
    const bool exact = n == b.size() && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!exact) return false;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256(b.data(), CC_LONG(b.size()), digest);
    char hex[65]{};
    for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcmp(hex, kHash) || get<uint16_t>(b.data(), 0) != 0x5a4d) return false;
    const uint32_t pe = get<uint32_t>(b.data(), 0x3c);
    if (pe > 1024 || get<uint32_t>(b.data(), pe) != 0x4550 ||
        get<uint16_t>(b.data(), pe + 4) != 0x8664 || get<uint16_t>(b.data(), pe + 24) != 0x20b) return false;
    const auto count = get<uint16_t>(b.data(), pe + 6);
    const auto preferred = get<uint64_t>(b.data(), pe + 48);
    const size_t sections = pe + 24 + get<uint16_t>(b.data(), pe + 20);
    if (count > 32 || sections + size_t(count) * 40 > b.size()) return false;
    for (unsigned i = 0; i < count; ++i) {
        const size_t at = sections + i * 40;
        const auto rva = get<uint32_t>(b.data(), at + 12);
        const auto bytes = get<uint32_t>(b.data(), at + 16);
        const auto raw = get<uint32_t>(b.data(), at + 20);
        if (kTable < rva || uint64_t(kTable - rva) + 64 > bytes) continue;
        const uint64_t offset = uint64_t(raw) + kTable - rva;
        if (offset + 64 > b.size()) return false;
        for (unsigned j = 0; j < kMethods.size(); ++j)
            if (get<uint64_t>(b.data(), offset + j * 8) != preferred + kMethods[j]) return false;
        return true;
    }
    return false;
}
bool region(uintptr_t at, mach_vm_address_t& begin, mach_vm_size_t& bytes,
            vm_region_basic_info_data_64_t& info) {
    begin = at; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto kr = mach_vm_region(mach_task_self(), &begin, &bytes, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return kr == KERN_SUCCESS && bytes && begin <= at && bytes <= UINTPTR_MAX - begin &&
        at < begin + bytes;
}
bool read(uintptr_t at, void* out, size_t bytes) {
    if (!at || !bytes || bytes > 65536 || bytes > UINTPTR_MAX - at) return false;
    const uintptr_t end = at + bytes;
    for (uintptr_t next = at; next < end;) {
        mach_vm_address_t begin = 0; mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        if (!region(next, begin, size, info) || !(info.protection & VM_PROT_READ)) return false;
        next = std::min(end, uintptr_t(begin + size));
    }
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, bytes,
        reinterpret_cast<mach_vm_address_t>(out), &got) == KERN_SUCCESS && got == bytes;
}
uintptr_t named_dll(uintptr_t base) {
    std::array<uint8_t, 4096> h{};
    if (!read(base, h.data(), h.size()) || get<uint16_t>(h.data(), 0) != 0x5a4d) return 0;
    const auto pe = get<uint32_t>(h.data(), 0x3c);
    if (pe > 1024 || get<uint32_t>(h.data(), pe) != 0x4550 ||
        get<uint16_t>(h.data(), pe + 4) != 0x8664 || get<uint16_t>(h.data(), pe + 24) != 0x20b) return 0;
    const auto size = get<uint32_t>(h.data(), pe + 80);
    const auto exports = get<uint32_t>(h.data(), pe + 136);
    std::array<uint8_t, 40> directory{};
    if (size < kTable + 64 || size > (64u << 20) || exports >= size ||
        size - exports < directory.size() || !read(base + exports, directory.data(), directory.size())) return 0;
    const auto name = get<uint32_t>(directory.data(), 12);
    constexpr char expected[] = "vrclient_x64.dll";
    std::array<char, sizeof(expected)> actual{};
    return name < size && size - name >= actual.size() &&
        read(base + name, actual.data(), actual.size()) && !memcmp(actual.data(), expected, actual.size()) ? base : 0;
}
uintptr_t loaded_dll() {
    mach_vm_address_t at = 0x6fff00000000ull;
    while (at < 0x700000000000ull) {
        mach_vm_size_t size = 0; vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto kr = mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || at >= 0x700000000000ull || size > UINTPTR_MAX - at) break;
        if ((info.protection & VM_PROT_READ) && named_dll(at)) return at;
        at += size;
    }
    return 0;
}
struct Identity { uintptr_t self = 0; uint64_t texture = 0, depth = 0; uint32_t layer = 0, eye = 0, records = 0; };
struct State {
    std::mutex mutex;
    unsigned count = 0, records = 0;
    std::array<Identity, kIdentities> seen{};
};
State& state() { static auto* value = new State(); return *value; }
void numbers(std::ostream& out, const float* values, unsigned count) {
    out << '[';
    for (unsigned i = 0; i < count; ++i) {
        if (i) out << ',';
        if (std::isfinite(values[i])) out << double(values[i]); else out << "null";
    }
    out << ']';
}
void pointer(std::ostream& out, uintptr_t value) { out << "\"0x" << std::hex << value << std::dec << '"'; }
void MSABI trace_submit(void* self, uint32_t layer, uint32_t index,
                        const Eye* incoming, double time, const Bounds* bounds) {
    if (capped.load(std::memory_order_relaxed)) {
        original.load()(self, layer, index, incoming, time, bounds); return;
    }
    const uint64_t seq = sequence.fetch_add(1);
    const auto entered = std::chrono::steady_clock::now();
    Eye eye{}; Bounds clipping{};
    const bool eye_ok = read(reinterpret_cast<uintptr_t>(incoming), &eye, sizeof(eye));
    const bool bounds_ok = read(reinterpret_cast<uintptr_t>(bounds), clipping.data(), sizeof(clipping));
    original.load()(self, layer, index, incoming, time, bounds);
    if (!eye_ok) return;
    try {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (s.records >= kRecords) return;
    Identity* found = nullptr;
    for (unsigned i = 0; i < s.count; ++i) {
        auto& x = s.seen[i];
        if (x.self == uintptr_t(self) && x.texture == eye.texture && x.depth == eye.depth &&
            x.layer == layer && x.eye == index) { found = &x; break; }
    }
    if (!found) {
        if (s.count == s.seen.size()) return;
        found = &s.seen[s.count++]; *found = {uintptr_t(self), eye.texture, eye.depth, layer, index, 0};
    }
    if (found->records++ >= 3) return; // Reserve budget for later/new application handles.
    if (++s.records == kRecords) capped.store(true, std::memory_order_relaxed);
    std::ostringstream out; out.precision(9);
    out << "{\"event\":\"private_submit_returned\",\"pid\":" << getpid() << ",\"sequence\":" << seq
        << ",\"entered_ns\":" << std::chrono::duration_cast<std::chrono::nanoseconds>(entered.time_since_epoch()).count()
        << ",\"layer\":" << layer << ",\"eye\":" << index << ",\"self\":"; pointer(out, uintptr_t(self));
    out << ",\"texture\":"; pointer(out, eye.texture);
    out << ",\"depth\":"; pointer(out, eye.depth);
    out << ",\"timestamp\":"; if (std::isfinite(time)) out << time; else out << "null";
    out << ",\"eye_bounds\":"; numbers(out, eye.bounds, 4);
    out << ",\"clip_bounds_readable\":" << (bounds_ok ? "true" : "false");
    if (bounds_ok) { out << ",\"clip_bounds\":"; numbers(out, clipping.data(), 4); }
    out << ",\"projection\":"; numbers(out, eye.projection, 16);
    out << ",\"pose\":"; numbers(out, eye.pose, 12);
    out << ",\"prediction_offset\":";
    if (std::isfinite(eye.prediction_offset)) out << eye.prediction_offset; else out << "null";
    out << '}'; fprintf(stderr, "CLIENT-FRAME %s\n", out.str().c_str());
    } catch (...) {
        // Keep optional logging failures from unwinding through a Windows caller.
        // The original publication has already completed exactly once.
        capped.store(true, std::memory_order_relaxed);
        fputs("CLIENT-FRAME disabled: diagnostic logging exception\n", stderr);
    }
}
bool patch(uintptr_t base) {
    std::array<uint64_t, 8> slots{};
    if (!read(base + kTable, slots.data(), sizeof(slots))) return false;
    for (unsigned i = 0; i < slots.size(); ++i) if (slots[i] != base + kMethods[i]) return false;
    mach_vm_address_t begin = 0; mach_vm_size_t size = 0; vm_region_basic_info_data_64_t info{};
    if (!region(base + kTable, begin, size, info) || !(info.protection & VM_PROT_READ)) return false;
    const auto granularity = std::max<mach_vm_size_t>(16384, getpagesize());
    const auto page = (base + kTable) & ~(granularity - 1);
    if (page < begin || granularity > begin + size - page ||
        mach_vm_protect(mach_task_self(), page, granularity, false, info.protection | VM_PROT_WRITE) != KERN_SUCCESS) return false;
    original.store(reinterpret_cast<Submit>(slots[2]));
    auto* target = reinterpret_cast<uint64_t*>(base + kTable + 16);
    __atomic_store_n(target, reinterpret_cast<uint64_t>(&trace_submit), __ATOMIC_RELEASE);
    if (mach_vm_protect(mach_task_self(), page, granularity, false, info.protection) != KERN_SUCCESS) {
        __atomic_store_n(target, slots[2], __ATOMIC_RELEASE);
        if (mach_vm_protect(mach_task_self(), page, granularity, false, info.protection) != KERN_SUCCESS)
            fputs("CLIENT-FRAME declined: page protection restoration retry failed\n", stderr);
        return false;
    }
    return true;
}
} // namespace

void client_install_frame_trace() {
    if (!enabled(getenv("DMN_CLIENT_FRAME_TRACE")) || !compositor_process()) return;
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    static std::atomic<bool> done{false};
    if (done.load() || busy.test_and_set()) return;
    Dl_info owner{}; dladdr(reinterpret_cast<void*>(&client_install_frame_trace), &owner);
    const auto path = dll_path(owner.dli_fname);
    if (path.empty() || !disk_guard(path.c_str())) {
        fputs("CLIENT-FRAME declined: exact DLL/path/hash/all-slot guard\n", stderr);
        done.store(true); busy.clear(); return;
    }
    const auto base = loaded_dll();
    if (!base) { busy.clear(); return; }
    const bool installed = patch(base);
    fprintf(stderr, "CLIENT-FRAME %s pid=%d: private DirectInternal slot2; max384 records/3 per new identity\n",
            installed ? "installed" : "declined", getpid());
    done.store(true); busy.clear();
}

#ifdef CLIENT_FRAME_TRACE_TEST_MAIN
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (enabled(nullptr) || enabled("0") || !enabled("1") ||
        !compositor_argument("C:/Steam/vrcompositor.exe") || compositor_argument("vrserver.exe") ||
        compositor_argument("--flag=vrcompositor.exe")) return 1;
    if (dll_path("/app/Contents/Frameworks/UTMWineAdapter/libwine-utm-bridge.dylib").empty() ||
        !dll_path("/tmp/libwine-utm-bridge.dylib").empty()) return 1;
    if (!disk_guard(argv[1]) || disk_guard("/dev/null")) return 1;
    uint64_t value = 1, copy = 0;
    if (!read(uintptr_t(&value), &copy, sizeof(copy)) || value != copy || read(0, &copy, sizeof(copy))) return 1;
    puts("Private client trace ABI/hash/all-eight-slot/path/VM guards PASS; no runtime patch");
    return 0;
}
#endif
