// Opt-in Wine10 CoreAudio loopback: selected native process -> private tap UID.
// The stock ALVR/Wine capture and ALVR audio transport remain the consumers.
#include "audio/process_tap.h"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <crt_externs.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {
using UnixCall = int32_t (*)(void*); // Wine Unix callbacks use SysV, not ms_abi.
struct LoopbackParams {
    const uint16_t* name;
    const char* device;
    char* ret_device;
    uint32_t ret_device_len;
    int32_t result;
};
struct CreateStreamParams {
    const uint16_t* name;
    const char* device;
    int32_t flow;
    int32_t share;
    uint32_t flags;
    int64_t duration;
    int64_t period;
    const void* format;
    int32_t result;
    uint32_t* channel_count;
    uint64_t* stream;
};
struct ReleaseStreamParams {uint64_t stream;void* timer_thread;int32_t result;};
struct GetCaptureParams {
    uint64_t stream;int32_t result;uint8_t** data;uint32_t* frames;uint32_t* flags;
    uint64_t* devpos;uint64_t* qpcpos;
};
static_assert(sizeof(ReleaseStreamParams)==24 && offsetof(ReleaseStreamParams,result)==16);
static_assert(sizeof(GetCaptureParams)==56 && alignof(GetCaptureParams)==8);
static_assert(offsetof(GetCaptureParams,result)==8 && offsetof(GetCaptureParams,data)==16);
static_assert(offsetof(GetCaptureParams,frames)==24 && offsetof(GetCaptureParams,flags)==32);
static_assert(offsetof(GetCaptureParams,devpos)==40 && offsetof(GetCaptureParams,qpcpos)==48);
static_assert(sizeof(CreateStreamParams)==80 && alignof(CreateStreamParams)==8);
static_assert(offsetof(CreateStreamParams,duration)==32);
static_assert(offsetof(CreateStreamParams,period)==40);
static_assert(offsetof(CreateStreamParams,format)==48);
static_assert(offsetof(CreateStreamParams,result)==56);
static_assert(offsetof(CreateStreamParams,stream)==72);
static_assert(sizeof(LoopbackParams) == 32 && alignof(LoopbackParams) == 8);
static_assert(offsetof(LoopbackParams, device) == 8);
static_assert(offsetof(LoopbackParams, ret_device) == 16);
static_assert(offsetof(LoopbackParams, ret_device_len) == 24);
static_assert(offsetof(LoopbackParams, result) == 28);
constexpr int32_t kNotImplemented = int32_t(0x80004001u);
constexpr int32_t kFailure = int32_t(0x80004005u);
constexpr int32_t kOutOfMemory = int32_t(0x8007000eu);
// mmdevapi checks this NTSTATUS stored in the result field, not Win32 HRESULT122.
constexpr int32_t kBufferTooSmall = int32_t(0xc0000023u);
constexpr size_t kLoopback = 15;
constexpr size_t kTableOffset = 0xa020;
constexpr size_t kFileBytes = 73888;
constexpr char kFileHash[] = "2a91cdb8e62cd637c98ff5045ecc491aa8e1e672da7d48efbb2a1a3ce58e8f2c";
constexpr std::array<uintptr_t,36> kOffsets = {
    0x900,0x950,0x960,0x980,0x1020,0x1440,0x1550,0x15a0,0x15e0,
    0x1660,0x1720,0x18e0,0x1a80,0x1c30,0x1cb0,0x950,0x1e20,
    0x25c0,0x25f0,0x2630,0x29e0,0x2a30,0x2a80,0x2ac0,0x2b80,
    0x2cc0,0x950,0x950,0x2d30,0x2d50,0x4290,0x4d10,0x4d70,
    0x5d80,0x67d0,0x950
};
constexpr std::array<uint8_t,8> kStub = {0x55,0x48,0x89,0xe5,0x31,0xc0,0x5d,0xc3};
constexpr std::array<uint8_t,16> kReleasePrefix = {
    0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x83,0xec,0x10,0x48,0x89,0xfb,0x4c,0x8b};
constexpr std::array<uint8_t,16> kGetCapturePrefix = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x20,0x49};

bool enabled() {
    const char* value = getenv("DMN_AUDIO_TAP");
    return value && strcmp(value,"1") == 0;
}
bool diagnostics_enabled() {
    const char* value=getenv("DMN_AUDIO_DIAGNOSTICS");
    return value && strcmp(value,"1")==0;
}
bool readable(const void* pointer,size_t bytes) {
    if (!pointer || !bytes) return false;
    const uintptr_t begin=reinterpret_cast<uintptr_t>(pointer);
    if (bytes>UINTPTR_MAX-begin) return false;
    const uintptr_t end=begin+bytes;
    uintptr_t cursor=begin;
    while (cursor<end) {
        mach_vm_address_t address=cursor;
        mach_vm_size_t size=0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object=MACH_PORT_NULL;
        const kern_return_t result=mach_vm_region(mach_task_self(),&address,&size,
            VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&object);
        if (object!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
        if (result!=KERN_SUCCESS || address>cursor || !size ||
            size>UINTPTR_MAX-address || !(info.protection&VM_PROT_READ)) return false;
        const uintptr_t next=uintptr_t(address+size);
        if (next<=cursor) return false;
        cursor=std::min(next,end);
    }
    return true;
}
bool diagnostics_code_matches(uintptr_t base) {
    const auto* release=reinterpret_cast<const void*>(base+kOffsets[5]);
    const auto* get=reinterpret_cast<const void*>(base+kOffsets[12]);
    return readable(release,kReleasePrefix.size()) && readable(get,kGetCapturePrefix.size()) &&
        memcmp(release,kReleasePrefix.data(),kReleasePrefix.size())==0 &&
        memcmp(get,kGetCapturePrefix.data(),kGetCapturePrefix.size())==0;
}
uint64_t signal_now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct SignalFormat {uint32_t rate=0;uint64_t capacity_frames=0;};
bool signal_format(const CreateStreamParams& params,SignalFormat& out) {
    if (!readable(params.format,18) || params.duration<=0) return false;
    const auto* bytes=static_cast<const uint8_t*>(params.format);
    uint16_t tag=0,channels=0,bits=0,block=0,extra=0;
    uint32_t rate=0;
    memcpy(&tag,bytes,2);memcpy(&channels,bytes+2,2);memcpy(&rate,bytes+4,4);
    memcpy(&block,bytes+12,2);memcpy(&bits,bytes+14,2);memcpy(&extra,bytes+16,2);
    constexpr uint8_t ieee_float_guid[]={3,0,0,0,0,0,16,0,128,0,0,170,0,56,155,113};
    if (tag==0xfffe) {
        if (extra<22 || !readable(bytes,40) || memcmp(bytes+24,ieee_float_guid,16)!=0)
            return false;
    } else if (tag!=3) return false;
    if (channels!=2 || bits!=32 || block!=8 || rate<8000 || rate>192000 ||
        uint64_t(params.duration)>UINT64_MAX/rate) return false;
    out.rate=rate;
    // One extra frame allows native MulDiv rounding; the sample-read limit is separate.
    out.capacity_frames=uint64_t(params.duration)*rate/10000000+1;
    return out.capacity_frames>0;
}
bool server_argument(const char* value) {
    if (!value) return false;
    const char* name = value;
    for (const char* at=value; *at; ++at)
        if (*at=='/' || *at=='\\') name=at+1;
    constexpr char expected[]="vrserver.exe";
    for (size_t i=0; i<sizeof(expected); ++i) {
        unsigned char c=name[i];
        if (c>='A' && c<='Z') c+= 'a'-'A';
        if (c!=expected[i]) return false;
        if (!c) return true;
    }
    return false;
}
bool server_process() {
    for (int i=0; i<*_NSGetArgc(); ++i)
        if (server_argument((*_NSGetArgv())[i])) return true;
    return false;
}
int32_t producer_pid(const char* value) {
    if (!value || !*value) return 0;
    // Reject whitespace, signs, suffixes and zero; never infer a global source.
    for (const char* at=value; *at; ++at) if (*at<'0' || *at>'9') return 0;
    char* end=nullptr;
    errno=0;
    const unsigned long n=strtoul(value,&end,10);
    if (errno || !end || *end || !n || n>uint32_t(INT32_MAX)) return 0;
    return int32_t(n);
}
bool producer_arguments(const char* bytes, size_t size, const char* prefix) {
    if (!bytes || size<sizeof(int) || !prefix || !*prefix) return false;
    int argc=0;
    memcpy(&argc,bytes,sizeof(argc));
    if (argc<1 || argc>256) return false;
    size_t at=sizeof(argc);
    auto next=[&](const char*& text) {
        if (at>=size) return false;
        text=bytes+at;
        const size_t n=strnlen(text,size-at);
        if (n==size-at) return false;
        at+=n+1;
        return true;
    };
    const char* text=nullptr;
    if (!next(text)) return false; // Kernel executable pathname.
    while (at<size && !bytes[at]) ++at; // Kernel padding before argv[0].
    bool alyx=false;
    for (int i=0; i<argc; ++i) {
        if (!next(text)) return false;
        const char* name=text;
        for (const char* p=text; *p; ++p) if (*p=='/' || *p=='\\') name=p+1;
        if (strcasecmp(name,"hlvr.exe")==0) alyx=true;
    }
    bool same_prefix=false;
    while (at<size) {
        if (!next(text)) return false;
        if (strncmp(text,"WINEPREFIX=",11)==0 && strcmp(text+11,prefix)==0)
            same_prefix=true;
    }
    return alyx && same_prefix;
}
bool same_game_process(int32_t pid) {
    proc_bsdinfo info{};
    if (proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof(info))!=sizeof(info) ||
        info.pbi_uid!=getuid()) return false;
    int mib[]={CTL_KERN,KERN_PROCARGS2,pid};
    std::array<char,32768> args{};
    size_t size=args.size();
    if (sysctl(mib,3,args.data(),&size,nullptr,0)!=0) return false;
    return producer_arguments(args.data(),size,getenv("WINEPREFIX"));
}
int32_t source_pid() {
    // Explicit manual override is trusted; a late-selected PID file additionally
    // verifies the current native game+prefix so stale/reused PIDs are rejected.
    if (const char* value=getenv("DMN_AUDIO_SOURCE_PID")) return producer_pid(value);
    const char* directory=getenv("DMN_WINE_SOCKET_DIR");
    if (!directory || !*directory) return 0;
    const int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir<0) return 0;
    struct stat st{};
    if (fstat(dir,&st)!=0 || st.st_uid!=getuid() || (st.st_mode&077)!=0) {
        close(dir); return 0;
    }
    const int file=openat(dir,"audio-source.pid",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    close(dir);
    if (file<0) return 0;
    std::array<char,25> text{};
    const bool safe=fstat(file,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() &&
        (st.st_mode&077)==0 && st.st_size>0 && st.st_size<ssize_t(text.size());
    const ssize_t bytes=safe ? read(file,text.data(),text.size()-1) : -1;
    close(file);
    if (bytes<=0 || bytes!=st.st_size) return 0;
    if (text[size_t(bytes)-1]=='\n') text[size_t(bytes)-1]=0;
    const int32_t pid=producer_pid(text.data());
    return pid && same_game_process(pid) ? pid : 0;
}
bool digest_matches(const void* bytes, size_t size) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256(bytes,CC_LONG(size),digest);
    char hex[65]{};
    for (unsigned i=0; i<sizeof(digest); ++i) snprintf(hex+i*2,3,"%02x",digest[i]);
    return strcmp(hex,kFileHash)==0;
}
bool file_matches(const char* path) {
    if (!path) return false;
    const char* basename=strrchr(path,'/');
    if (!basename || strcmp(basename+1,"winecoreaudio.so")!=0) return false;
    FILE* file=fopen(path,"rb");
    if (!file) return false;
    std::array<uint8_t,kFileBytes> bytes{};
    const size_t size=fread(bytes.data(),1,bytes.size(),file);
    const bool exact=size==bytes.size() && fgetc(file)==EOF && !ferror(file);
    fclose(file);
    return exact && digest_matches(bytes.data(),bytes.size());
}
bool table_matches(uintptr_t base, const UnixCall* table) {
    for (size_t i=0; i<kOffsets.size(); ++i)
        if (reinterpret_cast<uintptr_t>(table[i]) != base+kOffsets[i]) return false;
    return true;
}
void write_uid(LoopbackParams& params, const char* uid) {
    const size_t length=strnlen(uid,DMN_AUDIO_UID_MAX_BYTES);
    if (!length || length==DMN_AUDIO_UID_MAX_BYTES) {
        params.result=kFailure;
        return;
    }
    const uint32_t required=uint32_t(length+1);
    if (!params.ret_device && params.ret_device_len) {
        // A failed caller malloc must not become an endless two-pass retry.
        params.result=kOutOfMemory;
        return;
    }
    if (params.ret_device_len<required) {
        params.ret_device_len=required;
        params.result=kBufferTooSmall;
        return;
    }
    memcpy(params.ret_device,uid,required); // Caller owns the buffer, including NUL.
    params.result=0;
}

struct SignalCounters {
    uint64_t calls=0,empty=0,errors=0,frames=0,sampled_frames=0;
    uint64_t finite=0,nonfinite=0,nonzero=0,silent_frames=0,invalid=0;
    double peak=0,squares=0;
};
struct SignalEntry {
    uint64_t stream=0,generation=0;
    SignalFormat format{};
};
struct SignalDiagnostics {
    std::mutex mutex;
    std::array<SignalEntry,8> entries{};
    SignalCounters window{};
    uint64_t next_generation=1,registered=0,retired=0,full=0;
    uint64_t deadline_ns=0;
    int32_t source_pid=0;
    bool started=false,active=false;
};
struct State {
    std::mutex mutex;
    std::array<UnixCall,36> table{};
    const UnixCall* original_table=nullptr;
    UnixCall original_loopback=nullptr;
    dmn_audio_tap* tap=nullptr;
    int32_t pid=0;
    bool ready=false;
    unsigned errors=0;
    unsigned capture_logs=0;
    bool diagnostics_installed=false;
    SignalDiagnostics diagnostics{};
};
#ifdef DMN_AUDIO_DIAGNOSTICS_TESTING
void (*diagnostics_test_after_current)()=nullptr;
void (*diagnostics_test_before_retire_lock)()=nullptr;
#endif
State* state() {
    // Process-lifetime private objects. Do not run a C++ destructor while Wine
    // may still own running AudioUnits. HAL removes private objects on exit.
    // This holds at most one tap+aggregate and never swaps an advertised UID.
    static State* value=new(std::nothrow) State;
    return value;
}
unsigned signal_active_streams(const SignalDiagnostics& diagnostics) {
    unsigned count=0;
    for (const auto& entry:diagnostics.entries) if (entry.stream) ++count;
    return count;
}
void signal_worker(State* value) {
    // Fixed30s/6 summaries, even when capture never returns a buffer. No HAL calls.
    for (unsigned i=0;i<6;++i) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        SignalCounters counters{};unsigned active=0;int32_t pid=0;
        uint64_t registered=0,retired=0,full=0;
        bool report=false;
        {
            std::lock_guard<std::mutex> lock(value->diagnostics.mutex);
            auto& d=value->diagnostics;
            report=d.active && enabled() && diagnostics_enabled();
            counters=d.window;d.window={};active=signal_active_streams(d);pid=d.source_pid;
            registered=d.registered;retired=d.retired;full=d.full;
            if (i==5) d.active=false;
        }
        if (report) {
            const double rms=counters.finite ? std::sqrt(counters.squares/double(counters.finite)) : 0;
            fprintf(stderr,"WINE-AUDIO: signal source=%d window=5s active=%u registered=%llu retired=%llu full=%llu calls=%llu empty=%llu errors=%llu frames=%llu sampled_frames=%llu finite=%llu nonfinite=%llu nonzero=%llu silent_frames=%llu invalid=%llu sampled_peak=%.6g sampled_rms=%.6g\n",
                pid,active,(unsigned long long)registered,(unsigned long long)retired,
                (unsigned long long)full,(unsigned long long)counters.calls,
                (unsigned long long)counters.empty,(unsigned long long)counters.errors,
                (unsigned long long)counters.frames,(unsigned long long)counters.sampled_frames,
                (unsigned long long)counters.finite,(unsigned long long)counters.nonfinite,
                (unsigned long long)counters.nonzero,(unsigned long long)counters.silent_frames,
                (unsigned long long)counters.invalid,counters.peak,rms);
        }
    }
}
void signal_register(State* value,uint64_t stream,const SignalFormat& format,int32_t pid) {
    bool start=false;
    {
        std::lock_guard<std::mutex> lock(value->diagnostics.mutex);
        auto& d=value->diagnostics;
        auto* slot=static_cast<SignalEntry*>(nullptr);
        for (auto& entry:d.entries) if (entry.stream==stream) {slot=&entry;break;}
        if (!slot) for (auto& entry:d.entries) if (!entry.stream) {slot=&entry;break;}
        if (!slot) {++d.full;return;}
        *slot={stream,d.next_generation++,format};++d.registered;d.source_pid=pid;
        if (!d.started) {
            d.started=true;d.active=true;d.deadline_ns=signal_now_ns()+30000000000ULL;start=true;
        }
    }
#ifndef DMN_AUDIO_DIAGNOSTICS_TESTING
    if (start) {
        try {std::thread(signal_worker,value).detach();}
        catch (...) {
            {std::lock_guard<std::mutex> lock(value->diagnostics.mutex);value->diagnostics.active=false;}
            fprintf(stderr,"WINE-AUDIO: diagnostics worker unavailable\n");
        }
    }
#else
    (void)start;
#endif
}
int32_t release_stream_observed(void* raw) {
    State* value=state();
    const auto original=value && value->original_table ? value->original_table[5] : nullptr;
    if (!original) return 0; // Cannot be vended without a validated original table.
    if (value->diagnostics_installed && readable(raw,sizeof(ReleaseStreamParams))) {
        ReleaseStreamParams params{};memcpy(&params,raw,sizeof(params));
#ifdef DMN_AUDIO_DIAGNOSTICS_TESTING
        if (diagnostics_test_before_retire_lock) diagnostics_test_before_retire_lock();
#endif
        std::lock_guard<std::mutex> lock(value->diagnostics.mutex);
        for (auto& entry:value->diagnostics.entries) if (entry.stream==params.stream && entry.stream) {
            entry={};++value->diagnostics.retired;
        }
    }
    return original(raw); // Retire BEFORE native free; never hold our lock across it.
}
int32_t get_capture_observed(void* raw) {
    State* value=state();
    const auto original=value && value->original_table ? value->original_table[12] : nullptr;
    if (!original) return 0;
    SignalEntry scope{};
    if (value->diagnostics_installed && enabled() && diagnostics_enabled() &&
        readable(raw,sizeof(uint64_t))) {
        uint64_t stream=0;memcpy(&stream,raw,sizeof(stream));
        std::lock_guard<std::mutex> lock(value->diagnostics.mutex);
        if (value->diagnostics.active && signal_now_ns()>=value->diagnostics.deadline_ns)
            value->diagnostics.active=false;
        if (value->diagnostics.active)
            for (const auto& entry:value->diagnostics.entries)
                if (entry.stream && entry.stream==stream) {scope=entry;break;}
    }
    const int32_t status=original(raw); // No metadata lock spans the native call.
    if (!scope.stream) return status;
    // Pin the tracked generation until the bounded borrowed-sample read and
    // counter commit finish. ReleaseStream must take this same lock before it
    // retires the stream and calls native free. No original call, logging or
    // HAL operation runs under this lock.
    std::lock_guard<std::mutex> sample_lifetime(value->diagnostics.mutex);
    {
        bool current=false;
        for (const auto& entry:value->diagnostics.entries)
            if (entry.stream==scope.stream && entry.generation==scope.generation) current=true;
        if (signal_now_ns()>=value->diagnostics.deadline_ns) value->diagnostics.active=false;
        if (!current || !value->diagnostics.active) return status;
    }
#ifdef DMN_AUDIO_DIAGNOSTICS_TESTING
    if (diagnostics_test_after_current) diagnostics_test_after_current();
#endif
    if (!readable(raw,sizeof(GetCaptureParams))) return status;
    GetCaptureParams params{};memcpy(&params,raw,sizeof(params));
    SignalCounters observation{};observation.calls=1;
    if (status!=0 || params.result!=0) {
        if (status==0 && uint32_t(params.result)==0x08890001u) ++observation.empty;
        else ++observation.errors;
    } else if (!readable(params.frames,sizeof(uint32_t))) ++observation.invalid;
    else {
        uint32_t frames=0;memcpy(&frames,params.frames,sizeof(frames));
        observation.frames=frames;
        if (!frames) ++observation.empty;
        else if (frames>scope.format.capacity_frames || !readable(params.flags,sizeof(uint32_t)))
            ++observation.invalid;
        else {
            uint32_t flags=0;memcpy(&flags,params.flags,sizeof(flags));
            if (flags&2u) observation.silent_frames=frames;
            else if (!readable(params.data,sizeof(uint8_t*))) ++observation.invalid;
            else {
                uint8_t* data=nullptr;memcpy(&data,params.data,sizeof(data));
                const size_t sampled=std::min<size_t>(frames,4096);
                if (!readable(data,sampled*8)) ++observation.invalid;
                else {
                    observation.sampled_frames=sampled;
                    for (size_t i=0;i<sampled*2;++i) {
                        float sample=0;memcpy(&sample,data+i*sizeof(sample),sizeof(sample));
                        if (!std::isfinite(sample)) {++observation.nonfinite;continue;}
                        ++observation.finite;if (sample!=0) ++observation.nonzero;
                        const double x=sample;
                        observation.peak=std::max(observation.peak,std::fabs(x));
                        observation.squares+=x*x;
                    }
                }
            }
        }
    }
    {
        auto& d=value->diagnostics;
        bool current=false;
        for (const auto& entry:d.entries)
            if (entry.stream==scope.stream && entry.generation==scope.generation) current=true;
        if (d.active && current) {
            auto& c=d.window;
            c.calls+=observation.calls;c.empty+=observation.empty;c.errors+=observation.errors;
            c.frames+=observation.frames;c.sampled_frames+=observation.sampled_frames;
            c.finite+=observation.finite;c.nonfinite+=observation.nonfinite;
            c.nonzero+=observation.nonzero;c.silent_frames+=observation.silent_frames;
            c.invalid+=observation.invalid;c.peak=std::max(c.peak,observation.peak);
            c.squares+=observation.squares;
        }
    }
    return status; // Borrowed samples inspected only; never saved or altered.
}
int32_t get_loopback(void*);
int32_t create_capture(void*);
void prepare_table(State& value,const UnixCall* original,bool diagnostics) {
    std::copy(original,original+value.table.size(),value.table.begin());
    value.original_table=original;value.original_loopback=original[kLoopback];
    value.table[kLoopback]=&get_loopback;value.table[4]=&create_capture;
    value.diagnostics_installed=diagnostics;
    if (diagnostics) {
        value.table[5]=&release_stream_observed;value.table[12]=&get_capture_observed;
    }
}
int32_t fallback(State* value, void* raw) {
    if (value && value->original_loopback) return value->original_loopback(raw);
    if (raw) static_cast<LoopbackParams*>(raw)->result=kNotImplemented;
    return 0;
}
void report_error(State& value, dmn_audio_status status, const dmn_audio_error& error) {
    if (value.errors++ >= 8) return;
    fprintf(stderr,"WINE-AUDIO: create failed status=%s stage=%u os=%d cleanup=%u/%d\n",
        dmn_audio_status_name(status),error.stage,error.os_status,
        error.cleanup_stage,error.cleanup_os_status);
}
int32_t get_loopback(void* raw) {
    if (!raw) return 0; // The original stub returns success without dereferencing.
    State* value=state();
    if (!value || !enabled()) return fallback(value,raw);
    const int32_t pid=source_pid();
    if (!pid) return fallback(value,raw);
    std::lock_guard<std::mutex> lock(value->mutex);
    auto& params=*static_cast<LoopbackParams*>(raw);
    if (value->ready && value->pid!=pid) {
        // Existing Wine consumers can still hold this UID: require cold restart.
        params.result=kFailure;
        return 0;
    }
    if (!value->ready) {
        dmn_audio_error error{};
        if (value->tap) {
            const auto status=dmn_audio_tap_destroy(&value->tap,&error);
            if (status!=DMN_AUDIO_OK) {
                report_error(*value,status,error);
                params.result=kFailure;
                return 0;
            }
        }
        const auto status=dmn_audio_tap_create(pid,&value->tap,&error);
        if (status!=DMN_AUDIO_OK) {
            report_error(*value,status,error);
            params.result=kFailure;
            return 0;
        }
        dmn_audio_format format{};
        const auto format_status=dmn_audio_tap_get_format(value->tap,&format);
        if (format_status!=DMN_AUDIO_OK || format.version!=DMN_AUDIO_FORMAT_VERSION ||
            format.struct_size!=sizeof(format) || format.channels!=2 ||
            !std::isfinite(format.sample_rate) || format.sample_rate<8000 ||
            format.sample_rate>192000) {
            report_error(*value,DMN_AUDIO_UNSUPPORTED_FORMAT,error);
            // UID has not been handed to Wine yet; destroy is safe at next retry.
            params.result=kFailure;
            return 0;
        }
        value->pid=pid;
        value->ready=true;
        fprintf(stderr,"WINE-AUDIO: private stereo source pid=%d rate=%.0f host=%d\n",
            pid,format.sample_rate,getpid());
    }
    std::array<char,DMN_AUDIO_UID_MAX_BYTES> uid{};
    size_t required=0;
    const auto status=dmn_audio_tap_get_uid(value->tap,uid.data(),uid.size(),&required);
    if (status!=DMN_AUDIO_OK || !required || required>uid.size()) {
        params.result=kFailure;
        return 0;
    }
    write_uid(params,uid.data());
    return 0; // Unix-call status; params.result is the Windows initialization result.
}
int32_t create_capture(void* raw) {
    if (!raw) return 0;
    State* value=state();
    if (!value || !value->original_table) {
        static_cast<CreateStreamParams*>(raw)->result=kFailure;
        return 0;
    }
    const auto original=value->original_table[4];
    auto& params=*static_cast<CreateStreamParams*>(raw);
    bool owned=false;
    int32_t selected_pid=0;
    unsigned log_index=32;
    {
        std::lock_guard<std::mutex> lock(value->mutex);
        std::array<char,DMN_AUDIO_UID_MAX_BYTES> uid{};
        size_t required=0;
        owned=enabled() && value->ready && value->tap && params.flow==1 &&
            params.share==0 && params.device &&
            dmn_audio_tap_get_uid(value->tap,uid.data(),uid.size(),&required)==DMN_AUDIO_OK &&
            strcmp(params.device,uid.data())==0;
        if (owned) log_index=value->capture_logs++;
        if (owned) selected_pid=value->pid;
    }
    if (!owned) return original(raw);
    // Wine reserves two periods before conversion. Its usual three-period
    // capture ring can overflow before another full period can be converted.
    // Increase CAPACITY only; preserve the period, format and caller-owned args.
    CreateStreamParams forwarded=params;
    const int32_t buffer_ms=producer_pid(getenv("DMN_AUDIO_CAPTURE_BUFFER_MS"));
    if (buffer_ms>=30 && buffer_ms<=200 && params.period>0 &&
        params.duration>=params.period && params.period<=int64_t(buffer_ms)*10000) {
        forwarded.duration=std::max(params.duration,int64_t(buffer_ms)*10000);
    }
    SignalFormat observed_format{};
    const bool observe=value->diagnostics_installed && diagnostics_enabled() &&
        signal_format(forwarded,observed_format);
    const int32_t status=original(&forwarded);
    params.result=forwarded.result;
    if (observe && status==0 && params.result==0 &&
        readable(params.stream,sizeof(uint64_t))) {
        uint64_t stream=0;memcpy(&stream,params.stream,sizeof(stream));
        if (stream) signal_register(value,stream,observed_format,selected_pid);
    }
    if (log_index<16) {
        uint16_t tag=0,channels=0,bits=0,block=0;
        uint32_t rate=0,subtype=0;
        if (params.format) {
            const auto* bytes=static_cast<const uint8_t*>(params.format);
            memcpy(&tag,bytes,2); memcpy(&channels,bytes+2,2);
            memcpy(&rate,bytes+4,4); memcpy(&block,bytes+12,2); memcpy(&bits,bytes+14,2);
            uint16_t extra=0; memcpy(&extra,bytes+16,2);
            if (tag==0xfffe && extra>=22) memcpy(&subtype,bytes+24,4);
        }
        fprintf(stderr,"WINE-AUDIO: capture fmt=%04x subtype=%u rate=%u channels=%u bits=%u block=%u period=%.1fms capacity=%.1f->%.1fms result=%08x unix=%08x\n",
            tag,subtype,rate,channels,bits,block,params.period/10000.0,
            params.duration/10000.0,forwarded.duration/10000.0,
            uint32_t(params.result),uint32_t(status));
    }
    return status;
}
}

void* wine_audio_interpose_table(void* symbol, const char* name) {
    if (!enabled() || !symbol || !name || strcmp(name,"__wine_unix_call_funcs")!=0 ||
        !server_process()) return symbol;
    Dl_info owner{};
    if (!dladdr(symbol,&owner) || !owner.dli_fbase || !owner.dli_fname ||
        reinterpret_cast<uintptr_t>(symbol)-reinterpret_cast<uintptr_t>(owner.dli_fbase)
            != kTableOffset || !file_matches(owner.dli_fname)) return symbol;
    const auto* original=static_cast<const UnixCall*>(symbol);
    const uintptr_t base=reinterpret_cast<uintptr_t>(owner.dli_fbase);
    if (!table_matches(base,original) ||
        memcmp(reinterpret_cast<void*>(base+kOffsets[kLoopback]),kStub.data(),kStub.size())!=0)
        return symbol;
    State* value=state();
    if (!value) return symbol;
    std::lock_guard<std::mutex> lock(value->mutex);
    if (value->original_table && value->original_table!=original) return symbol;
    if (!value->original_table) {
        const bool diagnostics=diagnostics_enabled() && diagnostics_code_matches(base);
        prepare_table(*value,original,diagnostics);
        fprintf(stderr,"WINE-AUDIO: prepared guarded loopback table index15 pid=%d diagnostics=%u\n",
            getpid(),uint32_t(value->diagnostics_installed));
    }
    // Original read-only table, code pages and all other callbacks are unchanged.
    return value->table.data();
}
