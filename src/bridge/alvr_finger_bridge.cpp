// Opt-in ALVR20.14.1: ring/pinky curl follows grip, not thumb contact.
// Change one Jcc opcode to the same-target JMP. Thumb bones/API/ABI unchanged.
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <crt_externs.h>

void alvr_install_finger_grip_only();
namespace {
constexpr size_t kFileBytes=14497792;
constexpr char kHash[]="5b2dc0012254fa3c45268ed655c3f589b2d460a62907c620670cfb5d22a48fa8";
constexpr uint32_t kVtable=0xbb0628;
constexpr std::array<uint32_t,8> kMethods={
    0xa5ee40,0xa5ed30,0xa5ee20,0xa5f090,0xa5f1d0,0xa5ee10,0xa5f080,0xa5ee30
};
constexpr uint32_t kProvider=0xd6dee0;
constexpr uint32_t kFunction=0xa756c0;
constexpr size_t kFunctionBytes=3182; // full OnPoseUpdate through0xa7632e; no PE relocations
constexpr char kFunctionHash[]="00e22fff476a8dcdf6db9eac8c9c9ba715fec19c082417a13a5563e77ba802ee";
constexpr size_t kContextOffset=0xa761a4-kFunction;
constexpr size_t kBranchOffset=0xa761c3-kFunction;
constexpr std::array<uint8_t,33> kContext={
  0x80,0xbf,0x10,0x02,0x00,0x00,0x00,0x48,0x8d,0x54,0x24,0x30,0x48,0x8b,0x06,0x49,
  0x8b,0xce,0x48,0xc7,0x44,0x24,0x30,0x02,0x00,0x00,0x00,0x48,0x8b,0x58,0x18,0x74,0x36
};
static_assert(kBranchOffset+2+0x36 == 0xa761fb-kFunction,"same-target grip branch");
template<class T>T get(const uint8_t* p,size_t at) {T v{};memcpy(&v,p+at,sizeof(v));return v;}
bool enabled(const char* value) { return value && strcmp(value, "1") == 0; }
bool server_argument(const char* value) {
    if (!value) return false;
    const char* name = value;
    for (const char* at = value; *at; ++at)
        if (*at == '/' || *at == '\\') name = at + 1;
    constexpr char expected[] = "vrserver.exe";
    for (size_t i = 0; i < sizeof(expected); ++i) {
        unsigned char c = name[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != expected[i]) return false;
        if (!c) return true;
    }
    return false;
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


bool digest_matches(const uint8_t* bytes, size_t length, const char* expected) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256(bytes, CC_LONG(length), digest);
    char hex[65]{};
    for (unsigned i=0;i<sizeof(digest);++i) snprintf(hex+i*2,3,"%02x",digest[i]);
    return strcmp(hex,expected)==0;
}
bool offset_of(const std::vector<uint8_t>& bytes, uint32_t requested,
               size_t length, size_t& output) {
    if (bytes.size()<4096 || get<uint16_t>(bytes.data(),0)!=0x5a4d) return false;
    const uint32_t pe=get<uint32_t>(bytes.data(),0x3c);
    if (pe>1024 || get<uint32_t>(bytes.data(),pe)!=0x4550 ||
        get<uint16_t>(bytes.data(),pe+4)!=0x8664 ||
        get<uint16_t>(bytes.data(),pe+24)!=0x20b) return false;
    const uint16_t count=get<uint16_t>(bytes.data(),pe+6);
    const size_t sections=pe+24+get<uint16_t>(bytes.data(),pe+20);
    if (count>32 || sections+size_t(count)*40>bytes.size()) return false;
    for(unsigned i=0;i<count;++i) {
        const size_t s=sections+i*40;
        const uint32_t rva=get<uint32_t>(bytes.data(),s+12);
        const uint32_t size=get<uint32_t>(bytes.data(),s+16);
        const uint32_t raw=get<uint32_t>(bytes.data(),s+20);
        if(requested<rva || uint64_t(requested-rva)+length>size) continue;
        const uint64_t at=uint64_t(raw)+requested-rva;
        if(at>bytes.size() || length>bytes.size()-at) return false;
        output=size_t(at);return true;
    }
    return false;
}
bool disk_table(const std::vector<uint8_t>& bytes) {
    size_t function=0,table=0;
    if(!offset_of(bytes,kFunction,kFunctionBytes,function) ||
       !offset_of(bytes,kVtable,kMethods.size()*8,table) ||
       !digest_matches(bytes.data()+function,kFunctionBytes,kFunctionHash) ||
       memcmp(bytes.data()+function+kContextOffset,kContext.data(),kContext.size())!=0)
        return false;
    const uint32_t pe=get<uint32_t>(bytes.data(),0x3c);
    const uint64_t preferred=get<uint64_t>(bytes.data(),pe+48);
    for(unsigned i=0;i<kMethods.size();++i)
        if(get<uint64_t>(bytes.data(),table+i*8)!=preferred+kMethods[i]) return false;
    return true;
}
bool disk_guard(const char* path) {
    std::vector<uint8_t> bytes(kFileBytes);
    FILE* file = fopen(path, "rb");
    if (!file) return false;
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
    if (image != 0xdd7000 || exports >= image ||
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



bool function_guard(const uint8_t* function) {
    return readable(function,kFunctionBytes) &&
           digest_matches(function,kFunctionBytes,kFunctionHash) &&
           memcmp(function+kContextOffset,kContext.data(),kContext.size())==0;
}
bool patch_function(uint8_t* function) {
    if(!function_guard(function)) return false;
    auto* opcode=function+kBranchOffset;
    const mach_vm_address_t at=reinterpret_cast<mach_vm_address_t>(opcode);
    mach_vm_address_t begin=0;mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info{};
    if(!region_info(at,begin,size,info) || !(info.protection&VM_PROT_READ) ||
       !(info.protection&VM_PROT_EXECUTE)) return false;
    const mach_vm_size_t page_size=std::max<mach_vm_size_t>(16384,getpagesize());
    const mach_vm_address_t page=at&~(page_size-1);
    if(page<begin || page_size>begin+size-page) return false;
    if(mach_vm_protect(mach_task_self(),page,page_size,false,
                      info.protection|VM_PROT_WRITE|VM_PROT_COPY)!=KERN_SUCCESS) return false;
    uint8_t expected=0x74;
    const bool changed=__atomic_compare_exchange_n(opcode,&expected,uint8_t(0xeb),false,
                                                   __ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    if(changed) sys_icache_invalidate(opcode,1);
    const kern_return_t restore=mach_vm_protect(mach_task_self(),page,page_size,false,info.protection);
    if(restore!=KERN_SUCCESS) {
        if(changed) {__atomic_store_n(opcode,uint8_t(0x74),__ATOMIC_SEQ_CST);sys_icache_invalidate(opcode,1);}
        const kern_return_t retry=mach_vm_protect(mach_task_self(),page,page_size,false,info.protection);
        fprintf(stderr,"ALVR-FINGER declined: original opcode restored; protection restore=%d retry=%d\n",restore,retry);
        return false;
    }
    return changed;
}
bool startup_state(uint8_t* base) {
    if(!readable(base+kProvider,0x51) ||
       get<uint64_t>(base,kProvider)!=reinterpret_cast<uint64_t>(base+kVtable) ||
       get<uint8_t>(base,kProvider+0x50)!=0) return false;
    for(uint32_t offset:{0x18u,0x20u,0x28u,0x30u})
        if(get<uint64_t>(base,kProvider+offset)!=0) return false;
    return true;
}
} // namespace

void alvr_install_finger_grip_only() {
    if(!enabled(getenv("DMN_ALVR_FINGER_GRIP_ONLY")) || !server_process()) return;
    static std::atomic_flag busy=ATOMIC_FLAG_INIT;
    static std::atomic<bool> done{false};
    if(done.load() || busy.test_and_set()) return;
    try {
        Dl_info owner{};dladdr(reinterpret_cast<const void*>(&alvr_install_finger_grip_only),&owner);
        const std::string path=driver_path(owner.dli_fname);
        if(path.empty() || !disk_guard(path.c_str())) {
            fprintf(stderr,"ALVR-FINGER declined: exact bridge path/DLL/hash/function/provider guard\n");
            done.store(true);busy.clear();return;
        }
        uint8_t* base=loaded_driver();
        if(!base) {busy.clear();return;}
        if(!memory_table(base) || !startup_state(base) || !function_guard(base+kFunction)) {
            fprintf(stderr,"ALVR-FINGER declined: loaded table/startup/full-function/context guard\n");
            done.store(true);busy.clear();return;
        }
        const bool installed=patch_function(base+kFunction);
        fprintf(stderr,"ALVR-FINGER %s pid=%d base=%p: RP curl=grip, opcodea761c3 74->EB; thumb bones/controlflow tail unchanged\n",
                installed?"installed":"declined",getpid(),base);
        done.store(true);
    } catch(...) {
        fprintf(stderr,"ALVR-FINGER declined: native guard allocation/error\n");done.store(true);
    }
    busy.clear();
}

#ifdef ALVR_FINGER_TEST_MAIN
int main(int argc,char** argv) {
    if(argc!=2 || !disk_guard(argv[1]) || disk_guard("/dev/null")) return 1;
    if(!server_argument("C:/Steam/vrserver.exe") || server_argument("vrcompositor.exe") ||
       enabled(nullptr) || enabled("true") || !enabled("1")) return 1;
    std::vector<uint8_t> disk(kFileBytes);
    FILE* file=fopen(argv[1],"rb");if(!file)return 1;
    const size_t got=fread(disk.data(),1,disk.size(),file);fclose(file);if(got!=disk.size())return 1;
    size_t offset=0;if(!offset_of(disk,kFunction,kFunctionBytes,offset))return 1;
    const mach_vm_size_t page_size=std::max<mach_vm_size_t>(16384,getpagesize());
    mach_vm_address_t page=0;
    if(mach_vm_map(mach_task_self(),&page,page_size,page_size-1,VM_FLAGS_ANYWHERE,MACH_PORT_NULL,0,false,
                   VM_PROT_READ|VM_PROT_WRITE,VM_PROT_READ|VM_PROT_WRITE|VM_PROT_EXECUTE,VM_INHERIT_NONE)!=KERN_SUCCESS)return 1;
    auto* function=reinterpret_cast<uint8_t*>(page+0x100);
    memcpy(function,disk.data()+offset,kFunctionBytes);
    // A malformed function is rejected before any opcode write.
    function[0]^=1;if(function_guard(function)||patch_function(function))return 1;function[0]^=1;
    if(mach_vm_protect(mach_task_self(),page,page_size,false,VM_PROT_READ|VM_PROT_EXECUTE)!=KERN_SUCCESS)return 1;
    if(!patch_function(function) || function_guard(function) || patch_function(function))return 1;
    unsigned differences=0;
    for(size_t i=0;i<kFunctionBytes;++i)if(function[i]!=disk[offset+i]) {
        if(i!=kBranchOffset || function[i]!=0xeb || disk[offset+i]!=0x74)return 1;++differences;
    }
    if(differences!=1 || function[kBranchOffset+1]!=0x36)return 1;
    mach_vm_address_t begin=0;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info{};
    if(!region_info(page,begin,size,info)||info.protection!=(VM_PROT_READ|VM_PROT_EXECUTE))return 1;
    mach_vm_deallocate(mach_task_self(),page,page_size);
    puts("ALVR finger exact/full-function/context, single-byte same-target patch, RX restore guards PASS; no Wine/runtime mutation");
}
#endif
