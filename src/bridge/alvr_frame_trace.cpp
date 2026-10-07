// Opt-in, bounded observation of ALVR 20.14.1's direct-mode callbacks.
// Only data vtable slots are replaced. Originals and rendering arguments are
// forwarded unchanged; no code patching, GPU readback or Map interception.
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
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <crt_externs.h>

void alvr_install_frame_trace();

namespace {
#define MSABI __attribute__((ms_abi))
constexpr size_t kFileBytes = 14497792;
constexpr char kHash[] = "5b2dc0012254fa3c45268ed655c3f589b2d460a62907c620670cfb5d22a48fa8";
constexpr uint32_t kVtable = 0xbb61d0;
constexpr std::array<uint32_t, 8> kMethods = {
    0xa8b0d0, 0xa8b740, 0xa8b5b0, 0xa8b860,
    0xa8bb90, 0xa8b8d0, 0xa8b8b0, 0xa65ab0
};
constexpr unsigned kRecordLimit = 512;
constexpr unsigned kPairLimit = 128;
constexpr unsigned kObjectLimit = 8;
constexpr unsigned kHandleLimit = 256;

struct Eye {
    uint64_t texture, depth;
    float bounds[4];
    float projection[16];
    float pose[12];
};
using Pair = std::array<Eye, 2>;
struct SwapDesc { uint32_t width, height, format, samples; };
struct SwapOut { uint64_t handles[3]; uint32_t flags; };
static_assert(sizeof(Eye) == 144 && sizeof(Pair) == 288, "ALVR eye ABI drift");
static_assert(offsetof(Eye, bounds) == 16 && offsetof(Eye, projection) == 32 &&
              offsetof(Eye, pose) == 96, "ALVR member ABI drift");
static_assert(sizeof(SwapDesc) == 16 && sizeof(SwapOut) == 32 && alignof(SwapOut) == 8 &&
              offsetof(SwapOut, flags) == 24, "swap ABI drift");

using Create = void (MSABI *)(void*, uint32_t, const SwapDesc*, SwapOut*);
using Submit = void (MSABI *)(void*, const Pair*);
using Present = void (MSABI *)(void*, uint64_t);
std::atomic<Create> original_create{};
std::atomic<Submit> original_submit{};
std::atomic<Present> original_present{};
std::atomic<uint64_t> sequence{0};
std::atomic<bool> capped{false};

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint64_t thread_id() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
}
bool enabled(const char* value) { return value && strcmp(value, "1") == 0; }
bool server_argument(const char* value) {
    std::string s = value ? value : "";
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    const size_t slash = s.find_last_of("/\\");
    return s.substr(slash == std::string::npos ? 0 : slash + 1) == "vrserver.exe";
}
bool server_process() {
    for (int i = 0; i < *_NSGetArgc(); ++i)
        if (server_argument((*_NSGetArgv())[i])) return true;
    return false;
}
std::string driver_path(const char* bridge) {
    const std::string path = bridge ? bridge : "";
    const std::string suffix = "/Contents/Frameworks/UTMWineAdapter/libwine-utm-bridge.dylib";
    if (path.size() <= suffix.size() ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
        return {};
    return path.substr(0, path.size() - suffix.size()) +
           "/Contents/drive_c/ALVR/bin/win64/driver_alvr_server.dll";
}

template<class T> T get(const uint8_t* b, size_t at) {
    T v{}; memcpy(&v, b + at, sizeof(v)); return v;
}
bool disk_table(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4096 || get<uint16_t>(bytes.data(), 0) != 0x5a4d) return false;
    const uint32_t pe = get<uint32_t>(bytes.data(), 0x3c);
    if (pe > 1024 || get<uint32_t>(bytes.data(), pe) != 0x4550 ||
        get<uint16_t>(bytes.data(), pe + 4) != 0x8664 ||
        get<uint16_t>(bytes.data(), pe + 24) != 0x20b) return false;
    const uint16_t count = get<uint16_t>(bytes.data(), pe + 6);
    const uint16_t optional = get<uint16_t>(bytes.data(), pe + 20);
    const size_t sections = pe + 24 + optional;
    if (count > 32 || sections + size_t(count) * 40 > bytes.size()) return false;
    const uint64_t preferred = get<uint64_t>(bytes.data(), pe + 24 + 24);
    for (unsigned i = 0; i < count; ++i) {
        const size_t s = sections + i * 40;
        const uint32_t rva = get<uint32_t>(bytes.data(), s + 12);
        const uint32_t size = get<uint32_t>(bytes.data(), s + 16);
        const uint32_t raw = get<uint32_t>(bytes.data(), s + 20);
        if (kVtable < rva || uint64_t(kVtable - rva) + 64 > size) continue;
        const uint64_t offset = uint64_t(raw) + kVtable - rva;
        if (offset + 64 > bytes.size()) return false;
        for (unsigned j = 0; j < kMethods.size(); ++j)
            if (get<uint64_t>(bytes.data(), offset + j * 8) != preferred + kMethods[j])
                return false;
        return true;
    }
    return false;
}
bool disk_guard(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    std::vector<uint8_t> bytes(kFileBytes);
    const size_t got = fread(bytes.data(), 1, bytes.size(), file);
    const bool exact = got == bytes.size() && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!exact) return false;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest);
    char hex[65]{};
    for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return strcmp(hex, kHash) == 0 && disk_table(bytes);
}

bool region_info(mach_vm_address_t at, mach_vm_address_t& begin,
                 mach_vm_size_t& size, vm_region_basic_info_data_64_t& info) {
    begin = at;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const kern_return_t kr = mach_vm_region(mach_task_self(), &begin, &size,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return kr == KERN_SUCCESS && size && begin <= at;
}
bool readable(const void* data, size_t bytes) {
    mach_vm_address_t at = reinterpret_cast<mach_vm_address_t>(data);
    if (!at || bytes > UINT64_MAX - at) return false;
    const mach_vm_address_t end = at + bytes;
    while (at < end) {
        mach_vm_address_t begin = 0; mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        if (!region_info(at, begin, size, info) || !(info.protection & VM_PROT_READ) ||
            size > UINT64_MAX - begin) return false;
        at = std::min(end, begin + size);
    }
    return true;
}
uint8_t* named_driver(uint8_t* base, mach_vm_size_t header_size) {
    if (header_size < 4096 || get<uint16_t>(base, 0) != 0x5a4d) return nullptr;
    const uint32_t pe = get<uint32_t>(base, 0x3c);
    if (pe > 1024 || get<uint32_t>(base, pe) != 0x4550 ||
        get<uint16_t>(base, pe + 4) != 0x8664 ||
        get<uint16_t>(base, pe + 24) != 0x20b) return nullptr;
    const uint32_t image = get<uint32_t>(base, pe + 24 + 56);
    const uint32_t exports = get<uint32_t>(base, pe + 24 + 112);
    if (image < kVtable + 64 || image > (64u << 20) || exports >= image ||
        image - exports < 40 || !readable(base + exports, 40)) return nullptr;
    const uint32_t name = get<uint32_t>(base, exports + 12);
    constexpr char expected[] = "alvr_server_openvr.dll";
    if (name >= image || image - name < sizeof(expected) ||
        !readable(base + name, sizeof(expected)) ||
        memcmp(base + name, expected, sizeof(expected)) != 0) return nullptr;
    return base;
}
uint8_t* loaded_driver() {
    mach_vm_address_t address = 0x6fff00000000ull;
    while (address < 0x700000000000ull) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size,
            VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || address >= 0x700000000000ull) break;
        if ((info.protection & VM_PROT_READ) &&
            named_driver(reinterpret_cast<uint8_t*>(address), size))
            return reinterpret_cast<uint8_t*>(address);
        if (size > UINT64_MAX - address) break;
        address += size;
    }
    return nullptr;
}
bool memory_table(uint8_t* base) {
    if (!readable(base + kVtable, 64)) return false;
    for (unsigned i = 0; i < kMethods.size(); ++i)
        if (get<uint64_t>(base, kVtable + i * 8) !=
            reinterpret_cast<uint64_t>(base + kMethods[i])) return false;
    return true;
}

struct PairSeen { uintptr_t object = 0; uint64_t left = 0, right = 0; unsigned records = 0; };
struct ObjectSeen { uintptr_t object = 0; uint64_t presents = 0, until = 0; };
struct HandleSeen { uint64_t handle = 0; uint32_t pid = 0; SwapDesc desc{}; };
struct TraceState {
    std::mutex mutex;
    unsigned records = 0, routine_records = 0, pairs = 0, objects = 0, handles = 0;
    std::array<PairSeen, kPairLimit> pair{};
    std::array<ObjectSeen, kObjectLimit> object{};
    std::array<HandleSeen, kHandleLimit> handle{};
};
TraceState& state() { static auto* s = new TraceState(); return *s; }
struct StderrRecord {
    StderrRecord() { flockfile(stderr); }
    ~StderrRecord() { funlockfile(stderr); }
};
ObjectSeen* object_state(TraceState& s, void* self) {
    const auto id = reinterpret_cast<uintptr_t>(self);
    for (unsigned i = 0; i < s.objects; ++i) if (s.object[i].object == id) return &s.object[i];
    if (s.objects == s.object.size()) return nullptr;
    auto& o = s.object[s.objects++]; o.object = id; return &o;
}
void floats(const float* p, unsigned count) {
    fputc('[', stderr);
    for (unsigned i = 0; i < count; ++i) {
        if (i) fputc(',', stderr);
        if (std::isfinite(p[i])) fprintf(stderr, "%.9g", double(p[i])); else fputs("null", stderr);
    }
    fputc(']', stderr);
}
void eye(const TraceState& s, const Eye& e) {
    fprintf(stderr, "{\"texture\":\"0x%016llx\",\"depth\":\"0x%016llx\"",
        (unsigned long long)e.texture, (unsigned long long)e.depth);
    for (unsigned i = 0; i < s.handles; ++i) if (s.handle[i].handle == e.texture) {
        const auto& h = s.handle[i];
        fprintf(stderr, ",\"width\":%u,\"height\":%u,\"format\":%u,\"samples\":%u,\"client_pid\":%u",
            h.desc.width, h.desc.height, h.desc.format, h.desc.samples, h.pid); break;
    }
    fputs(",\"bounds\":", stderr); floats(e.bounds, 4);
    fputs(",\"projection\":", stderr); floats(e.projection, 16);
    fputs(",\"pose\":", stderr); floats(e.pose, 12); fputc('}', stderr);
}
void pair_json(const TraceState& s, const Pair& p) {
    fputc('[', stderr); eye(s, p[0]); fputc(',', stderr); eye(s, p[1]); fputc(']', stderr);
}

void MSABI trace_create(void* self, uint32_t pid, const SwapDesc* desc, SwapOut* out) {
    if (capped.load(std::memory_order_relaxed)) {
        original_create.load()(self, pid, desc, out); return;
    }
    SwapDesc before{};
    const bool desc_ok = readable(desc, sizeof(before));
    if (desc_ok) memcpy(&before, desc, sizeof(before));
    original_create.load()(self, pid, desc, out);
    if (!desc_ok || !readable(out, 28)) return;
    SwapOut result{}; memcpy(&result, out, 28);
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (s.records >= kRecordLimit) return;
    for (uint64_t handle : result.handles) {
        if (!handle) continue;
        auto existing = std::find_if(s.handle.begin(), s.handle.begin() + s.handles,
                                    [handle](const auto& v) { return v.handle == handle; });
        if (existing != s.handle.begin() + s.handles) *existing = {handle, pid, before};
        else if (s.handles < s.handle.size()) s.handle[s.handles++] = {handle, pid, before};
    }
    if (auto* o = object_state(s, self)) o->until = o->presents + 12;
    ++s.records;
    if (s.records == kRecordLimit) capped.store(true, std::memory_order_relaxed);
    StderrRecord record;
    fprintf(stderr, "ALVR-FRAME {\"event\":\"create_swap_returned\",\"sequence\":%llu,\"self\":\"%p\",\"client_pid\":%u,\"width\":%u,\"height\":%u,\"format\":%u,\"samples\":%u,\"flags\":%u,\"handles\":[\"0x%016llx\",\"0x%016llx\",\"0x%016llx\"]}\n",
        (unsigned long long)sequence.fetch_add(1), self, pid, before.width, before.height,
        before.format, before.samples, result.flags, (unsigned long long)result.handles[0],
        (unsigned long long)result.handles[1], (unsigned long long)result.handles[2]);
}
void MSABI trace_submit(void* self, const Pair* args) {
    if (capped.load(std::memory_order_relaxed)) {
        original_submit.load()(self, args); return;
    }
    const uint64_t seq = sequence.fetch_add(1), entered = now_ns(), thread = thread_id();
    Pair snapshot{}; const bool ok = readable(args, sizeof(snapshot));
    if (ok) memcpy(&snapshot, args, sizeof(snapshot));
    original_submit.load()(self, args);
    if (!ok) return;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (s.records >= kRecordLimit) return;
    const uintptr_t id = reinterpret_cast<uintptr_t>(self);
    PairSeen* seen = nullptr;
    for (unsigned i = 0; i < s.pairs; ++i) {
        auto& p = s.pair[i];
        if (p.object == id && p.left == snapshot[0].texture && p.right == snapshot[1].texture) { seen = &p; break; }
    }
    if (!seen) {
        if (s.pairs == s.pair.size()) return;
        seen = &s.pair[s.pairs++]; *seen = {id, snapshot[0].texture, snapshot[1].texture, 0};
        if (auto* o = object_state(s, self)) o->until = o->presents + 12;
    }
    if (seen->records++ >= 3) return;
    ++s.records;
    if (s.records == kRecordLimit) capped.store(true, std::memory_order_relaxed);
    StderrRecord record;
    fprintf(stderr, "ALVR-FRAME {\"event\":\"submit_returned\",\"sequence\":%llu,\"native_thread\":%llu,\"entered_ns\":%llu,\"self\":\"%p\",\"eyes\":",
        (unsigned long long)seq, (unsigned long long)thread, (unsigned long long)entered, self);
    pair_json(s, snapshot); fputs("}\n", stderr);
}
void MSABI trace_present(void* self, uint64_t sync) {
    if (capped.load(std::memory_order_relaxed)) {
        original_present.load()(self, sync); return;
    }
    const uint64_t seq = sequence.fetch_add(1), entered = now_ns(), thread = thread_id();
    int count = -1; std::array<Pair, 10> queued{};
    // These private fields are an entry observation before ALVR takes its
    // mutex. Label them unsynchronized; concurrent SubmitLayer may race them.
    const auto* bytes = static_cast<const uint8_t*>(self);
    if (readable(bytes + 0x48, 4)) memcpy(&count, bytes + 0x48, 4);
    const bool queue_ok = count >= 0 && count <= 10 && readable(bytes + 0x50, size_t(count) * sizeof(Pair));
    if (queue_ok && count) memcpy(queued.data(), bytes + 0x50, size_t(count) * sizeof(Pair));
    original_present.load()(self, sync);
    const uint64_t exited = now_ns();
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    auto* o = object_state(s, self);
    if (!o) return;
    const uint64_t serial = ++o->presents;
    // Reserve the budget for new swap sets/pairs even if the user activates
    // the scene much later. Only sixteen routine/sparse samples may be logged.
    if (s.records >= kRecordLimit) return;
    if (serial > o->until) {
        if ((serial > 12 && serial % 128 != 0) || s.routine_records >= 16) return;
        ++s.routine_records;
    }
    ++s.records;
    if (s.records == kRecordLimit) capped.store(true, std::memory_order_relaxed);
    StderrRecord record;
    fprintf(stderr, "ALVR-FRAME {\"event\":\"present_returned\",\"sequence\":%llu,\"native_thread\":%llu,\"entered_ns\":%llu,\"self\":\"%p\",\"serial\":%llu,\"sync\":\"0x%016llx\",\"duration_ms\":%.6f,\"entry_queue_unsynchronized\":true,\"entry_layer_count\":%d,\"entry_layers\":[",
        (unsigned long long)seq, (unsigned long long)thread, (unsigned long long)entered, self,
        (unsigned long long)serial, (unsigned long long)sync, double(exited - entered) / 1e6, count);
    if (queue_ok) for (int i = 0; i < count; ++i) { if (i) fputc(',', stderr); pair_json(s, queued[i]); }
    fputs("]}\n", stderr);
}

bool patch_table(uint8_t* base) {
    if (!memory_table(base)) return false;
    const mach_vm_address_t table = reinterpret_cast<mach_vm_address_t>(base + kVtable);
    mach_vm_address_t begin = 0; mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    if (!region_info(table, begin, size, info) || !(info.protection & VM_PROT_READ)) return false;
    // Rosetta reports 4 KiB; kernel protection granularity can be 16 KiB.
    const mach_vm_size_t page_size = std::max<mach_vm_size_t>(16384, getpagesize());
    const mach_vm_address_t page = table & ~(page_size - 1);
    if (page < begin || page_size > begin + size - page) return false;
    if (mach_vm_protect(mach_task_self(), page, page_size, false,
                        info.protection | VM_PROT_WRITE) != KERN_SUCCESS) return false;
    auto* slots = reinterpret_cast<uint64_t*>(table);
    original_create.store(reinterpret_cast<Create>(slots[0]));
    original_submit.store(reinterpret_cast<Submit>(slots[4]));
    original_present.store(reinterpret_cast<Present>(slots[5]));
    __atomic_store_n(slots, reinterpret_cast<uint64_t>(&trace_create), __ATOMIC_RELEASE);
    __atomic_store_n(slots + 4, reinterpret_cast<uint64_t>(&trace_submit), __ATOMIC_RELEASE);
    __atomic_store_n(slots + 5, reinterpret_cast<uint64_t>(&trace_present), __ATOMIC_RELEASE);
    if (mach_vm_protect(mach_task_self(), page, page_size, false, info.protection) != KERN_SUCCESS) {
        // Decline rather than leave wrappers installed with a changed page permission.
        __atomic_store_n(slots, reinterpret_cast<uint64_t>(base + kMethods[0]), __ATOMIC_RELEASE);
        __atomic_store_n(slots + 4, reinterpret_cast<uint64_t>(base + kMethods[4]), __ATOMIC_RELEASE);
        __atomic_store_n(slots + 5, reinterpret_cast<uint64_t>(base + kMethods[5]), __ATOMIC_RELEASE);
        const kern_return_t restored =
            mach_vm_protect(mach_task_self(), page, page_size, false, info.protection);
        if (restored != KERN_SUCCESS)
            fprintf(stderr, "ALVR-FRAME declined: original vtable slots restored, but page protection restore failed (%d)\n", restored);
        return false;
    }
    return true;
}
} // namespace

void alvr_install_frame_trace() {
    if (!enabled(getenv("DMN_ALVR_FRAME_TRACE")) || !server_process()) return;
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    static std::atomic<bool> done{false};
    if (done.load() || busy.test_and_set()) return;
    Dl_info owner{};
    dladdr(reinterpret_cast<const void*>(&alvr_install_frame_trace), &owner);
    const std::string path = driver_path(owner.dli_fname);
    if (path.empty() || !disk_guard(path.c_str())) {
        fprintf(stderr, "ALVR-FRAME declined: bridge location or exact DLL/hash/vtable guard failed\n");
        done.store(true); busy.clear(); return;
    }
    uint8_t* base = loaded_driver();
    if (!base) { busy.clear(); return; } // A later D3D device may load the driver.
    const bool installed = patch_table(base);
    fprintf(stderr, "ALVR-FRAME %s pid=%d: data-only slots 0/4/5, maximum %u records\n",
            installed ? "installed" : "declined", getpid(), kRecordLimit);
    done.store(true); busy.clear();
}

#ifdef ALVR_FRAME_TRACE_TEST_MAIN
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (enabled(nullptr) || enabled("0") || enabled("true") || !enabled("1")) return 1;
    if (!server_argument("C:/Steam/vrserver.exe") || server_argument("vrcompositor.exe") ||
        server_argument("--flag=vrserver.exe")) return 1;
    if (!driver_path("/app/Contents/Frameworks/UTMWineAdapter/libwine-utm-bridge.dylib").size() ||
        driver_path("/tmp/libwine-utm-bridge.dylib").size()) return 1;
    if (!disk_guard(argv[1]) || disk_guard("/dev/null")) return 1;
    // The standalone guard check never enters installation or calls Windows methods.
    puts("ALVR frame trace: opt-in/process/path/hash/all-eight-slot disk guards PASS; no runtime patch");
    return 0;
}
#endif
