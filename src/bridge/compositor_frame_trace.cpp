// Triggered, own-process observation of SteamVR 2.17.10 CPU frame metadata.
// No graphics calls, Map interception, or memory/code patches.
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <crt_externs.h>

void compositor_start_frame_trace();

namespace {
constexpr size_t kFileBytes = 3782808;
constexpr char kHash[] = "8f518bf734b7303aec4e39da8c2f0c83cc6f36f066a38d1a28c4b8aefe3cc7d8";
constexpr uintptr_t kPreferred = 0x140000000ull;
constexpr uint32_t kGlobal = 0x36ce30, kCompositorVtable = 0x2b0418;
constexpr uint32_t kRendererVtable = 0x2c0d88;
constexpr size_t kRingBytes = 2800;
constexpr unsigned kSamples = 64, kIntervalMs = 150;
using Ring = std::array<uint8_t, kRingBytes>;
static_assert(16 + 3 * 928 == kRingBytes, "ring ABI drift");

template<class T> T get(const uint8_t* p, size_t offset) {
    T value{}; memcpy(&value, p + offset, sizeof(value)); return value;
}
bool enabled(const char* s) { return s && strcmp(s, "1") == 0; }
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
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
std::string exe_path(const char* owner) {
    const std::string path = owner ? owner : "";
    const std::string suffix = "/Contents/Frameworks/UTMWineAdapter/libwine-utm-bridge.dylib";
    if (path.size() <= suffix.size() ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    return path.substr(0, path.size() - suffix.size()) +
        "/Contents/drive_c/Program Files (x86)/Steam/steamapps/common/SteamVR/bin/win64/vrcompositor.exe";
}

struct ImageGuard {
    uint32_t image_bytes = 0;
    std::array<uint8_t, 7> assign{}, gate{};
    std::array<uint8_t, 30> graphics_chain{};
};
bool disk_guard(const char* path, ImageGuard& guard) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    std::vector<uint8_t> bytes(kFileBytes);
    const size_t n = fread(bytes.data(), 1, bytes.size(), file);
    const bool exact = n == bytes.size() && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!exact) return false;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest);
    char hex[65]{};
    for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcmp(hex, kHash) != 0 || get<uint16_t>(bytes.data(), 0) != 0x5a4d) return false;
    const uint32_t pe = get<uint32_t>(bytes.data(), 0x3c);
    if (pe > 1024 || get<uint32_t>(bytes.data(), pe) != 0x4550 ||
        get<uint16_t>(bytes.data(), pe + 4) != 0x8664 ||
        get<uint16_t>(bytes.data(), pe + 24) != 0x20b ||
        get<uint64_t>(bytes.data(), pe + 48) != kPreferred) return false;
    guard.image_bytes = get<uint32_t>(bytes.data(), pe + 80);
    if (guard.image_bytes < kGlobal + 8 || guard.image_bytes > (64u << 20)) return false;
    const uint16_t count = get<uint16_t>(bytes.data(), pe + 6);
    const size_t table = pe + 24 + get<uint16_t>(bytes.data(), pe + 20);
    if (count > 32 || table + size_t(count) * 40 > bytes.size()) return false;
    auto copy_rva = [&](uint32_t rva, auto& out) {
        for (unsigned i = 0; i < count; ++i) {
            const size_t s = table + i * 40;
            const uint32_t va = get<uint32_t>(bytes.data(), s + 12);
            const uint32_t raw_size = get<uint32_t>(bytes.data(), s + 16);
            const uint32_t raw = get<uint32_t>(bytes.data(), s + 20);
            if (rva < va || uint64_t(rva - va) + out.size() > raw_size) continue;
            const uint64_t offset = uint64_t(raw) + rva - va;
            if (offset + out.size() > bytes.size()) return false;
            memcpy(out.data(), bytes.data() + offset, out.size()); return true;
        }
        return false;
    };
    return copy_rva(0x101e5a, guard.assign) && copy_rva(0x064672, guard.gate) &&
        copy_rva(0x1255b3, guard.graphics_chain);
}

// Read through the kernel, not a naked dereference after a VM check. A region
// disappearing between checks yields a declined sample instead of a fault.
bool copy_read(uintptr_t address, void* out, size_t bytes) {
    if (!address || !bytes || bytes > 65536 || bytes > UINTPTR_MAX - address) return false;
    const uintptr_t end = address + bytes;
    for (uintptr_t at = address; at < end;) {
        mach_vm_address_t begin = at; mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto kr = mach_vm_region(mach_task_self(), &begin, &size,
            VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || begin > at ||
            size > UINTPTR_MAX - begin || !(info.protection & VM_PROT_READ)) return false;
        const uintptr_t next = std::min(end, uintptr_t(begin + size));
        if (next <= at) return false;
        at = next;
    }
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, bytes,
        reinterpret_cast<mach_vm_address_t>(out), &got) == KERN_SUCCESS && got == bytes;
}
template<class T> bool value_at(uintptr_t at, T& out) { return copy_read(at, &out, sizeof(out)); }
bool memory_guard(const ImageGuard& guard) {
    std::array<uint8_t, 4096> header{};
    if (!copy_read(kPreferred, header.data(), header.size()) ||
        get<uint16_t>(header.data(), 0) != 0x5a4d) return false;
    const auto pe = get<uint32_t>(header.data(), 0x3c);
    if (pe > 1024 || get<uint32_t>(header.data(), pe) != 0x4550 ||
        get<uint16_t>(header.data(), pe + 4) != 0x8664 ||
        get<uint16_t>(header.data(), pe + 24) != 0x20b ||
        get<uint64_t>(header.data(), pe + 48) != kPreferred ||
        get<uint32_t>(header.data(), pe + 80) != guard.image_bytes) return false;
    std::array<uint8_t, 7> assign{}, gate{};
    std::array<uint8_t, 30> graphics_chain{};
    return copy_read(kPreferred + 0x101e5a, assign.data(), assign.size()) &&
        copy_read(kPreferred + 0x064672, gate.data(), gate.size()) &&
        copy_read(kPreferred + 0x1255b3, graphics_chain.data(), graphics_chain.size()) &&
        assign == guard.assign && gate == guard.gate && graphics_chain == guard.graphics_chain;
}

struct Snapshot {
    uintptr_t compositor = 0, manager = 0, graphics = 0, renderer = 0;
    uintptr_t compositor_vptr = 0, renderer_vptr = 0, fallback_container = 0;
    uintptr_t failed_address = 0, expected_vptr = 0;
    const char* failed_stage = "none";
    const char* failure_reason = "none";
    uintptr_t immediate = 0, active = 0, imported = 0, local = 0, staging = 0;
    uint64_t shared_handle = 0;
    uint32_t expected = 0, fallback = 0, selected = 0, previous_selected = 0;
    uint8_t mismatch = 0, fresh = 0;
    Ring ring{};
};
bool snapshot(Snapshot& s) {
    auto read = [&](const char* stage, uintptr_t at, auto& value) {
        if (value_at(at, value)) return true;
        s.failed_stage = stage; s.failed_address = at; s.failure_reason = "vm_read_failed";
        return false;
    };
    auto nonnull = [&](const char* stage, uintptr_t pointer) {
        if (pointer) return true;
        s.failed_stage = stage; s.failed_address = pointer; s.failure_reason = "null_pointer";
        return false;
    };
    auto vptr_guard = [&](const char* stage, uintptr_t object, uintptr_t actual, uintptr_t expected) {
        if (actual == expected) return true;
        s.failed_stage = stage; s.failed_address = object;
        s.failure_reason = "vptr_mismatch"; s.expected_vptr = expected;
        return false;
    };
    if (!read("compositor_global", kPreferred + kGlobal, s.compositor) ||
        !nonnull("compositor_global", s.compositor) ||
        !read("compositor_vptr", s.compositor, s.compositor_vptr) ||
        !vptr_guard("compositor_vptr", s.compositor, s.compositor_vptr, kPreferred + kCompositorVtable)) return false;
    // +0x48 is a backpointer on the separate render worker, not this object.
    // Direct global getters (RVA1255b3 and others) use CVRCompositor+0x190.
    s.manager = s.compositor; // Owner alias retained for existing diagnostic JSON.
    if (!read("compositor_primary_graphics", s.compositor + 0x190, s.graphics)) return false;
    if (!s.graphics) {
        if (!read("compositor_fallback_container", s.compositor + 0x1a8, s.fallback_container) ||
            !nonnull("compositor_fallback_container", s.fallback_container) ||
            !read("fallback_graphics", s.fallback_container + 8, s.graphics) ||
            !nonnull("fallback_graphics", s.graphics)) return false;
    }
    const uintptr_t g = s.graphics;
    if (!read("graphics_renderer", g + 0x25290, s.renderer) ||
        !nonnull("graphics_renderer", s.renderer) ||
        !read("renderer_vptr", s.renderer, s.renderer_vptr) ||
        !vptr_guard("renderer_vptr", s.renderer, s.renderer_vptr, kPreferred + kRendererVtable)) return false;
    if (!(read("expected_id", g + 0x21f5c, s.expected) && read("fallback_ring", g + 0x21f58, s.fallback) &&
        read("selected_ring", g + 0x23cb0, s.selected) && read("previous_selected_ring", g + 0x23cb4, s.previous_selected) &&
        read("mismatch_flag", g + 0x25257, s.mismatch) && read("fresh_flag", g + 0x24600, s.fresh) &&
        read("shared_handle", g + 0x226b8, s.shared_handle) && read("local_buffer", g + 0x226b0, s.local) &&
        read("imported_buffer", g + 0x226c0, s.imported) && read("staging_buffer", g + 0x226c8, s.staging) &&
        read("immediate_context", s.renderer + 0x9f8, s.immediate) &&
        read("active_context", s.renderer + 0xa08, s.active))) return false;
    if (!copy_read(g + 0x226d0, s.ring.data(), s.ring.size())) {
        s.failed_stage = "cpu_ring"; s.failed_address = g + 0x226d0;
        s.failure_reason = "vm_read_failed"; return false;
    }
    return true;
}
bool same_controls(const Snapshot& a, const Snapshot& b) {
    return a.compositor == b.compositor && a.manager == b.manager && a.graphics == b.graphics &&
        a.renderer == b.renderer && a.immediate == b.immediate && a.active == b.active &&
        a.imported == b.imported && a.local == b.local && a.staging == b.staging &&
        a.shared_handle == b.shared_handle && a.expected == b.expected && a.fallback == b.fallback &&
        a.selected == b.selected && a.previous_selected == b.previous_selected &&
        a.mismatch == b.mismatch && a.fresh == b.fresh;
}
void pointer_json(std::ostream& out, uintptr_t ptr) { out << "\"0x" << std::hex << ptr << std::dec << '"'; }
void failure_json(std::ostream& out, const Snapshot& s) {
    out << "{\"stage\":\"" << s.failed_stage << "\",\"reason\":\"" << s.failure_reason << "\",\"address\":";
    pointer_json(out, s.failed_address);
    out << ",\"compositor\":"; pointer_json(out, s.compositor);
    out << ",\"compositor_vptr\":"; pointer_json(out, s.compositor_vptr);
    out << ",\"manager\":"; pointer_json(out, s.manager);
    out << ",\"fallback_container\":"; pointer_json(out, s.fallback_container);
    out << ",\"graphics\":"; pointer_json(out, s.graphics);
    out << ",\"renderer\":"; pointer_json(out, s.renderer);
    out << ",\"renderer_vptr\":"; pointer_json(out, s.renderer_vptr);
    out << ",\"expected_vptr\":"; pointer_json(out, s.expected_vptr);
    out << '}';
}
void controls(std::ostream& out, const Snapshot& s) {
    out << "{\"expected\":" << s.expected << ",\"selected\":" << s.selected
        << ",\"previous_selected\":" << s.previous_selected << ",\"fallback\":" << s.fallback
        << ",\"mismatch\":" << unsigned(s.mismatch) << ",\"fresh\":" << unsigned(s.fresh)
        << ",\"header_ring\":" << get<uint32_t>(s.ring.data(), 0) << ",\"rings\":[";
    for (unsigned i = 0; i < 3; ++i) {
        if (i) out << ',';
        const size_t at = 16 + i * 928;
        out << '[' << get<uint32_t>(s.ring.data(), at) << ','
            << get<uint32_t>(s.ring.data(), at + 4) << ',' << get<uint32_t>(s.ring.data(), at + 8) << ']';
    }
    out << "],\"compositor\":"; pointer_json(out, s.compositor);
    out << ",\"graphics\":"; pointer_json(out, s.graphics);
    out << ",\"renderer\":"; pointer_json(out, s.renderer);
    out << ",\"immediate\":"; pointer_json(out, s.immediate);
    out << ",\"active\":"; pointer_json(out, s.active);
    out << ",\"contexts_equal\":" << (s.immediate && s.immediate == s.active ? "true" : "false");
    out << ",\"incoming_handle\":"; pointer_json(out, s.shared_handle);
    out << ",\"imported_buffer\":"; pointer_json(out, s.imported);
    out << ",\"local_buffer\":"; pointer_json(out, s.local);
    out << ",\"staging_buffer\":"; pointer_json(out, s.staging);
    out << '}';
}
void log_line(const std::string& line) { fprintf(stderr, "COMPOSITOR-FRAME %s\n", line.c_str()); }
void string_json(std::ostream& out, const std::string& value) {
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 0x20) {
            char escaped[7]{}; snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(c)); out << escaped;
        } else out << char(c);
    }
    out << '"';
}

bool trigger_path(const char* value, std::string& path, std::string& parent) {
    if (!value || value[0] != '/') return false;
    const std::string requested(value);
    const auto slash = requested.find_last_of('/');
    if (slash == std::string::npos || slash + 1 == requested.size()) return false;
    const std::string leaf = requested.substr(slash + 1);
    if (leaf == "." || leaf == "..") return false;
    char resolved[PATH_MAX]{};
    if (!realpath(requested.substr(0, slash).c_str(), resolved)) return false;
    parent = resolved;
    const char* broker = getenv("DMN_WINE_SOCKET_DIR");
    char broker_path[PATH_MAX]{};
    if (!broker || !realpath(broker, broker_path)) return false;
    const std::string allowed(broker_path);
    if (parent != allowed && parent.compare(0, allowed.size() + 1, allowed + '/') != 0)
        return false;
    path = parent + '/' + leaf;
    return true;
}
bool write_ring(const std::string& dir, unsigned sample, const Ring& ring) {
    char leaf[48]{}; snprintf(leaf, sizeof(leaf), "/cpu-ring-%02u.bin", sample);
    const int fd = open((dir + leaf).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const ssize_t wrote = write(fd, ring.data(), ring.size());
    const int close_result = close(fd);
    return wrote == ssize_t(ring.size()) && close_result == 0;
}
void observe(ImageGuard guard, std::string trigger, std::string parent) {
    timespec last{}; ino_t last_inode = 0; unsigned run = 0; bool first_poll = true;
    for (;;) {
        struct stat info{};
        const int stat_result = lstat(trigger.c_str(), &info);
        const int stat_error = stat_result == 0 ? 0 : errno;
        const bool regular = stat_result == 0 && S_ISREG(info.st_mode);
        const bool changed = regular && (info.st_ino != last_inode ||
            info.st_mtimespec.tv_sec != last.tv_sec || info.st_mtimespec.tv_nsec != last.tv_nsec);
        if (first_poll) {
            first_poll = false;
            std::ostringstream line;
            line << "{\"event\":\"first_poll\",\"pid\":" << getpid() << ",\"trigger\":";
            string_json(line, trigger);
            line << ",\"lstat_result\":" << stat_result << ",\"errno\":" << stat_error
                 << ",\"mode\":" << unsigned(info.st_mode) << ",\"regular\":" << (regular ? "true" : "false")
                 << ",\"changed\":" << (changed ? "true" : "false")
                 << ",\"inode\":" << info.st_ino << ",\"mtime_sec\":" << info.st_mtimespec.tv_sec
                 << ",\"mtime_nsec\":" << info.st_mtimespec.tv_nsec << '}';
            log_line(line.str());
        }
        if (!changed) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        last = info.st_mtimespec; last_inode = info.st_ino; ++run;
        if (!memory_guard(guard)) {
            log_line("{\"event\":\"declined\",\"reason\":\"unknown loaded PE layout/base\"}"); continue;
        }
        const std::string dir = parent + "/compositor-observation-" + std::to_string(getpid()) + '-' + std::to_string(run);
        const bool dump_ok = mkdir(dir.c_str(), 0700) == 0;
        for (unsigned i = 0; i < kSamples; ++i) {
            // Root removes its marker at the end of the bounded scene window.
            if (lstat(trigger.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) break;
            Snapshot a{}, b{};
            const bool ok_a = snapshot(a), ok_b = snapshot(b);
            const bool consistent = ok_a && ok_b && same_controls(a, b) && a.ring == b.ring;
            std::ostringstream line;
            line << "{\"event\":\"sample\",\"pid\":" << getpid() << ",\"run\":" << run
                 << ",\"sample\":" << i << ",\"time_ns\":" << now_ns()
                 << ",\"read_a\":" << (ok_a ? "true" : "false")
                 << ",\"read_b\":" << (ok_b ? "true" : "false")
                 << ",\"consistent\":" << (consistent ? "true" : "false")
                 << ",\"observation_unsynchronized\":true";
            if (ok_a) { line << ",\"a\":"; controls(line, a); }
            else { line << ",\"a_failure\":"; failure_json(line, a); }
            if (ok_b) { line << ",\"b\":"; controls(line, b); }
            else { line << ",\"b_failure\":"; failure_json(line, b); }
            if (ok_b && dump_ok && write_ring(dir, i, b.ring)) {
                // Controlled numeric directory/leaf names need no JSON escaping.
                line << ",\"raw_directory_basename\":\"compositor-observation-" << getpid() << '-' << run << '"';
            }
            line << '}'; log_line(line.str());
            std::this_thread::sleep_for(std::chrono::milliseconds(kIntervalMs));
        }
    }
}
void observe_guarded(ImageGuard guard, std::string trigger, std::string parent) {
    // This entry notice precedes stream allocation and polling; it distinguishes
    // a started native worker from a compositor that exited before activation.
    constexpr char entry[] = "COMPOSITOR-FRAME {\"event\":\"worker_started\"}\n";
    const ssize_t ignored = write(STDERR_FILENO, entry, sizeof(entry) - 1);
    (void)ignored;
    try { observe(guard, std::move(trigger), std::move(parent)); }
    catch (...) { log_line("{\"event\":\"declined\",\"reason\":\"observer exception\"}"); }
}
} // namespace

void compositor_start_frame_trace() {
    if (!enabled(getenv("DMN_COMPOSITOR_FRAME_TRACE")) || !compositor_process()) return;
    static std::atomic_flag started = ATOMIC_FLAG_INIT;
    if (started.test_and_set()) return;
    std::string trigger, parent;
    Dl_info owner{}; dladdr(reinterpret_cast<void*>(&compositor_start_frame_trace), &owner);
    const auto path = exe_path(owner.dli_fname);
    ImageGuard guard{};
    if (path.empty() || !trigger_path(getenv("DMN_COMPOSITOR_TRACE_TRIGGER"), trigger, parent) ||
        !disk_guard(path.c_str(), guard)) {
        log_line("{\"event\":\"declined\",\"reason\":\"path/exact exe hash/RVA guard\"}"); return;
    }
    try {
        std::thread(observe_guarded, guard, trigger, parent).detach();
        std::ostringstream line;
        line << "{\"event\":\"armed\",\"pid\":" << getpid() << ",\"samples_per_trigger\":64,\"interval_ms\":150,\"trigger\":";
        string_json(line, trigger); line << '}'; log_line(line.str());
    } catch (...) { log_line("{\"event\":\"declined\",\"reason\":\"observer thread creation\"}"); }
}

#ifdef COMPOSITOR_FRAME_TRACE_TEST_MAIN
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (enabled(nullptr) || enabled("0") || !enabled("1") ||
        !compositor_argument("C:/Steam/vrcompositor.exe") || compositor_argument("vrserver.exe") ||
        compositor_argument("--flag=vrcompositor.exe")) return 1;
    ImageGuard guard{};
    if (!disk_guard(argv[1], guard) || disk_guard("/dev/null", guard)) return 1;
    uint64_t input = 0x12345678, output = 0;
    if (!copy_read(reinterpret_cast<uintptr_t>(&input), &output, 8) || input != output ||
        copy_read(0, &output, 8)) return 1;
    std::string path, parent;
    const char* broker = getenv("DMN_WINE_SOCKET_DIR");
    if (!broker || !trigger_path((std::string(broker) + "/test-trigger").c_str(), path, parent) ||
        trigger_path("/tmp/test-trigger", path, parent)) return 1;
    puts("Compositor observer guards PASS; no observer thread, graphics calls or runtime changes");
    return 0;
}
#endif
