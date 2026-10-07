// Diagnostic workaround for ALVR 20.14.1 GetErrorStr assigning a null
// FormatMessageW result to std::wstring. Preserve the HRESULT failure.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>

#define MSABI __attribute__((ms_abi))
using FormatMessage = uint32_t (MSABI *)(uint32_t, const void*, uint32_t,
    uint32_t, uint16_t*, uint32_t, void*);
static std::atomic<FormatMessage> original_format{};

static uint32_t MSABI safe_format(uint32_t flags, const void* source,
    uint32_t id, uint32_t language, uint16_t* buffer, uint32_t size, void* args) {
    auto original = original_format.load();
    uint32_t result = original(flags, source, id, language, buffer, size, args);
    constexpr uint32_t allocate = 0x100, from_string = 0x400;
    constexpr uint32_t from_module = 0x800, from_system = 0x1000;
    // Restrict this to ALVR's allocating system-error formatter. Other calls
    // retain their original return value and Windows error behavior.
    if (!result && (flags & (allocate | from_system)) == (allocate | from_system)) {
        static const char16_t fallback[] = u"Windows error text unavailable";
        result = original((flags & ~(from_system | from_module)) | from_string,
                          fallback, 0, language, buffer, size, args);
        fprintf(stderr, "WINE-UTM: ALVR FormatMessageW id=0x%08x fallback=%u\n", id, result);
    }
    return result;
}

template<class T> static T read_at(const uint8_t* base, uint32_t offset) {
    T value;
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

static bool patch_driver(uint8_t* base, mach_vm_size_t header_size) {
    if (header_size < 4096 || read_at<uint16_t>(base, 0) != 0x5a4d) return false;
    uint32_t pe = read_at<uint32_t>(base, 0x3c);
    if (pe > 1024 || read_at<uint32_t>(base, pe) != 0x4550 ||
        read_at<uint16_t>(base, pe + 24) != 0x20b) return false;
    uint32_t image_size = read_at<uint32_t>(base, pe + 24 + 56);
    auto valid = [image_size](uint32_t rva, uint32_t size) {
        return rva && rva < image_size && size <= image_size - rva;
    };
    uint32_t exports = read_at<uint32_t>(base, pe + 24 + 112);
    if (!valid(exports, 40)) return false;
    uint32_t name = read_at<uint32_t>(base, exports + 12);
    if (!valid(name, 23) || strcmp(reinterpret_cast<char*>(base + name),
                                   "alvr_server_openvr.dll") != 0) return false;
    uint32_t imports = read_at<uint32_t>(base, pe + 24 + 120);
    uint32_t import_size = read_at<uint32_t>(base, pe + 24 + 124);
    if (!valid(imports, import_size)) return false;
    for (uint32_t offset = 0; offset + 20 <= import_size; offset += 20) {
        uint32_t original = read_at<uint32_t>(base, imports + offset);
        uint32_t iat = read_at<uint32_t>(base, imports + offset + 16);
        if (!original && !iat) break;
        if (!original) continue;
        for (uint32_t i = 0; i < 4096; ++i) {
            if (!valid(original + i * 8, 8) || !valid(iat + i * 8, 8)) break;
            uint64_t item = read_at<uint64_t>(base, original + i * 8);
            if (!item) break;
            if (item >> 63 || item > UINT32_MAX || !valid(uint32_t(item), 17)) continue;
            if (strcmp(reinterpret_cast<char*>(base + item + 2), "FormatMessageW") != 0) continue;
            auto slot = reinterpret_cast<uint64_t*>(base + iat + i * 8);
            FormatMessage old = reinterpret_cast<FormatMessage>(*slot);
            mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(slot);
            mach_vm_address_t page = address & ~(mach_vm_address_t(getpagesize()) - 1);
            mach_vm_size_t region_size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            mach_vm_address_t region = address;
            kern_return_t kr = mach_vm_region(mach_task_self(), &region, &region_size,
                VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
            if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
            if (kr != KERN_SUCCESS || region > address) return false;
            if (mach_vm_protect(mach_task_self(), page, getpagesize(), false,
                info.protection | VM_PROT_WRITE) != KERN_SUCCESS) return false;
            original_format.store(old);
            __atomic_store_n(slot, reinterpret_cast<uint64_t>(&safe_format), __ATOMIC_RELEASE);
            mach_vm_protect(mach_task_self(), page, getpagesize(), false, info.protection);
            fprintf(stderr, "WINE-UTM: installed ALVR error-format fallback pid=%d\n", getpid());
            return true;
        }
    }
    return false;
}

void alvr_install_error_fallback() {
    static std::atomic_flag lock = ATOMIC_FLAG_INIT;
    if (original_format.load() || lock.test_and_set()) return;
    // Wine's loaded 64-bit PE images live in this reservation. Inspect only
    // readable region headers, and patch only the specifically named driver.
    mach_vm_address_t address = 0x6fff00000000ull;
    while (address < 0x700000000000ull) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size,
            VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || address >= 0x700000000000ull) break;
        if ((info.protection & VM_PROT_READ) &&
            patch_driver(reinterpret_cast<uint8_t*>(address), size)) break;
        if (address + size <= address) break;
        address += size;
    }
    lock.clear();
}
