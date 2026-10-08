#include "wine_scene_source.h"
#include "../scene/scene_wire.h"
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "../scene/expected_native.h"

extern char** environ;
namespace {
using Clock = std::chrono::steady_clock;
using ServerCall = uint32_t(*)(void*);
using CurrentTeb = void*(*)();
// Exact Wine10 server.h ABI. Generic request/reply union is64 bytes.
struct Iovec { const void* data; uint32_t size; };
struct Request {
    alignas(8) uint8_t fixed[64];
    uint32_t count;
    void* reply_data;
    Iovec data[5];
};
struct ProcessInfo {
    int64_t start_time;
    uint32_t name_len;
    int32_t threads,priority;
    uint32_t pid,parent,session;
    int32_t handles,unix_pid;
};
static_assert(sizeof(Request)==160 && offsetof(Request,count)==64 &&
              offsetof(Request,reply_data)==72 && offsetof(Request,data)==80);
static_assert(sizeof(ProcessInfo)==40 && offsetof(ProcessInfo,unix_pid)==36);
constexpr uint32_t kListProcesses=76;
constexpr size_t kReplyLimit=1u<<20;
constexpr auto kPoll=std::chrono::milliseconds(200);
constexpr auto kStale=std::chrono::seconds(3);
struct Process { ProcessInfo info{}; std::string name; };
struct State {
    std::mutex mutex;
    ServerCall call=nullptr; CurrentTeb teb=nullptr;
    std::string prefix,engine;
    bool guard_ready=false;
    bool self_logged=false;
#ifdef DMN_SCENE_TESTING
    std::string expected_self="vrserver.exe";
#endif
    pid_t helper=0;
    int pipe=-1;
    std::array<uint8_t,sizeof(DmnSceneWire)> incoming{};
    size_t incoming_bytes=0;
    DmnSceneWire wire{};
    uint64_t last_sequence=0;
    Clock::time_point next_poll{},last_wire{},next_spawn{};
    uint32_t candidate_pid=0;
    uint64_t candidate_creation=0;
    unsigned candidate_count=0;
    DmnSceneSourceSnapshot selected{};
};
State& state() { static State value; return value; }
template<class T> T get(const void* data,size_t offset) {
    T value{}; memcpy(&value,static_cast<const uint8_t*>(data)+offset,sizeof(value)); return value;
}
template<class T> void put(void* data,size_t offset,T value) {
    memcpy(static_cast<uint8_t*>(data)+offset,&value,sizeof(value));
}
bool read_self(uintptr_t address,void* out,size_t size) {
    if (!address || !size || address>UINTPTR_MAX-size) return false;
    mach_vm_size_t read=0;
    return mach_vm_read_overwrite(mach_task_self(),address,size,
        reinterpret_cast<mach_vm_address_t>(out),&read)==KERN_SUCCESS && read==size;
}
std::string canonical(const char* path) {
    std::array<char,PATH_MAX> text{};
    if (!path || !*path || !realpath(path,text.data())) return {};
    return text.data();
}
std::string lower(std::string text) {
    for (char& c:text) if (c>='A'&&c<='Z') c+='a'-'A';
    return text;
}
std::string base(std::string text) {
    const auto p=text.find_last_of("/\\");
    if (p!=std::string::npos) text.erase(0,p+1);
    return lower(text);
}
bool pe_name(const std::string& name) {
    return name.size()>4 && name.size()<256 &&
        name.compare(name.size()-4,4,".exe")==0 && name[0]!='-' &&
        name.find_first_of("\"'\r\n\t=")==std::string::npos;
}
bool system_name(const std::string& name) {
    for (const char* value:{"steam.exe","steamwebhelper.exe","steamservice.exe",
        "vrserver.exe","vrcompositor.exe","vrmonitor.exe","vrwebhelper.exe",
        "vrdashboard.exe","vrstartup.exe","steamtours.exe","audioscenewatcher.exe",
        "explorer.exe","wineboot.exe","services.exe","rpcss.exe","winedevice.exe",
        "svchost.exe","cmd.exe","powershell.exe","gameoverlayui.exe"})
        if (name==value) return true;
    return false;
}
bool fixed_string(const char* s,size_t cap,std::string& out) {
    const char* end=static_cast<const char*>(memchr(s,0,cap));
    if (!end) return false;
    out.assign(s,end-s);
    // The helper writes zero-padded strings. Reject ambiguous hidden suffixes.
    for (const char* p=end+1;p<s+cap;++p) if (*p) return false;
    return true;
}
uint64_t utc_filetime() {
    timeval now{};
    if (gettimeofday(&now,nullptr)) return 0;
    return uint64_t(now.tv_sec)*10000000ULL+uint64_t(now.tv_usec)*10+116444736000000000ULL;
}
bool fresh(uint64_t observed) {
    const uint64_t now=utc_filetime();
    if (!now || !observed) return false;
    // Allow small cross-call clock skew; old queued records never become fresh
    // merely because the native reader resumed after a capture pause.
    return observed<=now+10000000ULL && (observed>=now || now-observed<=20000000ULL);
}
bool file_hash(const std::string& path,size_t wanted,const char* expected) {
    std::vector<uint8_t> bytes(wanted);
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return false;
    struct stat st{};
    bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() &&
        st.st_size==ssize_t(wanted) && !(st.st_mode&0022);
    size_t at=0;
    while (ok && at<bytes.size()) {
        const ssize_t n=read(fd,bytes.data()+at,bytes.size()-at);
        if (n>0) at+=size_t(n);
        else if (n<0 && errno==EINTR) continue;
        else ok=false;
    }
    close(fd);
    if (!ok) return false;
    uint8_t digest[32]{};char hex[65]{};
    CC_SHA256(bytes.data(),CC_LONG(bytes.size()),digest);
    for (unsigned i=0;i<32;++i) snprintf(hex+2*i,3,"%02x",digest[i]);
    return strcmp(hex,expected)==0;
}
bool runtime_guard(State& s) {
#if !defined(__x86_64__)
    (void)s;return false; // Exact pinned x86_64 Wine engine only.
#else
    if (s.guard_ready) return true;
    void* call=dlsym(RTLD_DEFAULT,"wine_server_call");
    void* teb=dlsym(RTLD_DEFAULT,"NtCurrentTeb");
    if (!call || !teb) return false;
    Dl_info owner{},teb_owner{};
    if (!dladdr(call,&owner)||!owner.dli_fname||!owner.dli_fbase||
        !dladdr(teb,&teb_owner)||teb_owner.dli_fbase!=owner.dli_fbase) return false;
    const std::string path=canonical(owner.dli_fname);
    const std::string suffix="/lib/wine/x86_64-unix/ntdll.so";
    if (path.size()<=suffix.size() ||
        path.compare(path.size()-suffix.size(),suffix.size(),suffix)!=0) return false;
    s.engine=path.substr(0,path.size()-suffix.size());
    const auto slash=s.engine.find_last_of('/');
    if (slash==std::string::npos) return false;
    s.prefix=canonical(getenv("WINEPREFIX"));
    if (s.prefix.empty() || s.prefix!=canonical((s.engine.substr(0,slash)+"/prefix").c_str()) ||
        canonical(getenv("WINESERVER"))!=canonical((s.engine+"/bin/wineserver").c_str())) return false;
    const uintptr_t image=reinterpret_cast<uintptr_t>(owner.dli_fbase);
    if (reinterpret_cast<uintptr_t>(call)-image!=kServerCallRva ||
        reinterpret_cast<uintptr_t>(teb)-image!=kCurrentTebRva ||
        !file_hash(path,kNtdllSize,kNtdllHash) ||
        !file_hash(s.engine+"/bin/wineserver",kServerSize,kServerHash)) return false;
    for (size_t i=0;i<3;++i) {
        std::array<uint8_t,32> code{};
        if (!read_self(image+kCodeRvas[i],code.data(),code.size()) ||
            memcmp(code.data(),kCodeBytes[i],code.size())) return false;
    }
    s.call=reinterpret_cast<ServerCall>(call);
    s.teb=reinterpret_cast<CurrentTeb>(teb);
    s.guard_ready=true;
    return true;
#endif
}
bool thread_pid(State& s,uint32_t& pid) {
    if (!runtime_guard(s)) return false;
    const uintptr_t teb=reinterpret_cast<uintptr_t>(s.teb());
    std::array<uint8_t,80> header{};
    if (!read_self(teb,header.data(),header.size()) || get<uint64_t>(header.data(),0x30)!=teb)
        return false;
    const uint64_t wine_pid=get<uint64_t>(header.data(),0x40);
    if (!wine_pid || wine_pid>UINT32_MAX) return false;
    pid=uint32_t(wine_pid);return true;
}
bool utf16_name(const uint8_t* data,size_t bytes,std::string& name) {
    if (bytes&1 || bytes>32768) return false;
    std::string text;
    for (size_t i=0;i<bytes;i+=2) {
        uint32_t c=get<uint16_t>(data,i);
        if (!c) return false;
        if (c>=0xd800&&c<=0xdbff) {
            if (i+4>bytes) return false;
            const uint32_t low=get<uint16_t>(data,i+2);
            if (low<0xdc00||low>0xdfff) return false;
            c=0x10000+((c-0xd800)<<10)+(low-0xdc00);i+=2;
        } else if (c>=0xdc00&&c<=0xdfff) return false;
        if (c<0x80) text+=char(c);
        else if (c<0x800) { text+=char(0xc0|(c>>6));text+=char(0x80|(c&63)); }
        else if (c<0x10000) { text+=char(0xe0|(c>>12));text+=char(0x80|((c>>6)&63));text+=char(0x80|(c&63)); }
        else { text+=char(0xf0|(c>>18));text+=char(0x80|((c>>12)&63));text+=char(0x80|((c>>6)&63));text+=char(0x80|(c&63)); }
    }
    name=base(text);return !name.empty();
}
bool parse_processes(const uint8_t* bytes,size_t size,uint32_t count,
                     uint32_t wanted,Process& result,bool& found) {
    found=false;size_t pos=0;
    if (!bytes || size>kReplyLimit || count>4096) return false;
    for (uint32_t i=0;i<count;++i) {
        pos=(pos+7)&~size_t(7);
        if (pos>size || size-pos<sizeof(ProcessInfo)) return false;
        ProcessInfo p{};memcpy(&p,bytes+pos,sizeof(p));pos+=sizeof(p);
        if (p.threads<0 || p.threads>16384 || p.name_len>size-pos || (p.name_len&1)) return false;
        if (p.pid==wanted) {
            if (found || p.unix_pid<=0 || p.start_time<=0 || !p.threads ||
                !utf16_name(bytes+pos,p.name_len,result.name)) return false;
            result.info=p;found=true;
        }
        pos+=p.name_len;pos=(pos+7)&~size_t(7);
        const size_t threads=size_t(p.threads)*40;
        if (pos>size || threads>size-pos) return false;
        pos+=threads;
    }
    return pos==size;
}
bool list(State& s,uint32_t wanted,Process& target,uint32_t self_pid) {
    std::vector<uint8_t> bytes(65536);
    Request request{};
    put<uint32_t>(request.fixed,0,kListProcesses);
    put<uint32_t>(request.fixed,8,uint32_t(bytes.size()));
    request.reply_data=bytes.data();
    uint32_t status=s.call(&request);
    uint32_t size=get<uint32_t>(request.fixed,8);
    if (status==0xc0000004 && size>bytes.size() && size<=kReplyLimit) {
        bytes.resize(size);request={};
        put<uint32_t>(request.fixed,0,kListProcesses);
        put<uint32_t>(request.fixed,8,uint32_t(bytes.size()));
        request.reply_data=bytes.data();status=s.call(&request);
    }
    if (status || get<uint32_t>(request.fixed,0)!=0) return false;
    const uint32_t received=get<uint32_t>(request.fixed,4);
    size=get<uint32_t>(request.fixed,8);
    const uint32_t count=get<uint32_t>(request.fixed,12);
    if (!size || size!=received || size>bytes.size()) return false;
    Process own{};bool found=false;
    if (!parse_processes(bytes.data(),size,count,self_pid,own,found) || !found ||
        own.info.unix_pid!=getpid()) return false;
#ifdef DMN_SCENE_TESTING
    if (own.name!=s.expected_self) return false;
#else
    if (own.name!="vrserver.exe") return false;
#endif
    if (!s.self_logged) {
        fprintf(stderr,"WINE-SCENE: self-map PASS wine_pid=%u native_pid=%d creation=%llu protocol=%u\n",
            self_pid,getpid(),static_cast<unsigned long long>(own.info.start_time),kListProcesses);
        s.self_logged=true;
    }
    return parse_processes(bytes.data(),size,count,wanted,target,found) && found;
}
bool native_identity(pid_t pid,const std::string& expected,const std::string& prefix,uint64_t& born) {
    proc_bsdinfo first{},second{};
    if (proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&first,sizeof(first))!=sizeof(first) ||
        first.pbi_uid!=getuid() || first.pbi_status==SZOMB || !first.pbi_start_tvsec) return false;
    std::array<char,32768> bytes{};size_t size=bytes.size();
    int mib[]={CTL_KERN,KERN_PROCARGS2,pid};
    if (sysctl(mib,3,bytes.data(),&size,nullptr,0) || size<sizeof(int)) return false;
    const int argc=get<int>(bytes.data(),0);
    if (argc<1||argc>512) return false;
    size_t at=sizeof(int);
    auto next=[&](std::string& out) {
        if (at>=size) return false;
        const char* end=static_cast<const char*>(memchr(bytes.data()+at,0,size-at));
        if (!end) return false;
        out.assign(bytes.data()+at,end-(bytes.data()+at));at+=out.size()+1;return true;
    };
    std::string text,name;
    if (!next(text)||text.empty()) return false;
    while (at<size&&!bytes[at]) ++at;
    for (int i=0;i<argc;++i) {
        if (!next(text)) return false;
        const auto part=base(text);
        if (i==0) name=part;
        if (i>0 && i<=2 && (name=="wine"||name=="wine64"||name=="wine-preloader"||name=="wine64-preloader")) name=part;
    }
    if (!pe_name(name)||name!=expected) return false;
    unsigned prefix_count=0;bool prefix_match=false;
    while (at<size) {
        if (!next(text)) return false;
        if (text.compare(0,11,"WINEPREFIX=")==0) {
            ++prefix_count;prefix_match=canonical(text.c_str()+11)==prefix;
        }
    }
    if (prefix_count!=1 || !prefix_match ||
        proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&second,sizeof(second))!=sizeof(second) ||
        second.pbi_uid!=first.pbi_uid || second.pbi_start_tvsec!=first.pbi_start_tvsec ||
        second.pbi_start_tvusec!=first.pbi_start_tvusec || second.pbi_status==SZOMB) return false;
    born=first.pbi_start_tvsec*1000000ULL+first.pbi_start_tvusec;
    return true;
}
void idle(State& s) {
    if (s.selected.ready || s.selected.native_pid || s.selected.wine_pid) ++s.selected.generation;
    s.selected.ready=false;s.selected.wine_pid=0;s.selected.native_pid=0;s.selected.start_time=0;
}
void close_pipe(State& s) {
    if (s.pipe>=0) close(s.pipe);
    s.pipe=-1;s.incoming_bytes=0;s.last_sequence=0;s.last_wire={};s.wire={};
    s.candidate_pid=0;s.candidate_creation=0;s.candidate_count=0;idle(s);
}
void spawn(State& s,Clock::time_point now) {
    if (s.helper) {
        int status=0;const pid_t result=waitpid(s.helper,&status,WNOHANG);
        if (result==0) return;
        if (result==s.helper || (result<0 && errno==ECHILD)) s.helper=0;
        else return;
        close_pipe(s);s.next_spawn=now+std::chrono::seconds(2);
    }
    if (s.pipe>=0 || now<s.next_spawn) return;
    s.next_spawn=now+std::chrono::seconds(5);
    const std::string helper=s.prefix+"/drive_c/ALVR/AudioSceneWatcher.exe";
    if (!file_hash(helper,kWatcherSize,kWatcherHash)) return;
    // Allocate C++ storage before opening descriptors: allocation failures must
    // not leak a pipe or spawn-file-actions resources on the Wine callback.
    std::vector<std::string> env;
    for (char** p=environ;*p;++p)
        if (strncmp(*p,"DYLD_INSERT_LIBRARIES=",22)!=0 && strncmp(*p,"WINEDEBUG=",10)!=0) env.emplace_back(*p);
    env.emplace_back("WINEDEBUG=-all");
    std::vector<char*> pointers;for (auto& value:env) pointers.push_back(value.data());pointers.push_back(nullptr);
    const std::string wine=s.engine+"/bin/wine";
    int pair[2]{};
    if (pipe(pair)) return;
    if (fcntl(pair[0],F_SETFD,FD_CLOEXEC)<0 || fcntl(pair[1],F_SETFD,FD_CLOEXEC)<0 ||
        fcntl(pair[0],F_SETFL,O_NONBLOCK)<0) { close(pair[0]);close(pair[1]);return; }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) { close(pair[0]);close(pair[1]);return; }
    int setup=posix_spawn_file_actions_adddup2(&actions,pair[1],STDOUT_FILENO);
    setup|=posix_spawn_file_actions_addclose(&actions,pair[0]);
    setup|=posix_spawn_file_actions_addclose(&actions,pair[1]);
    const int null=open("/dev/null",O_WRONLY|O_CLOEXEC);
    if (null>=0) { setup|=posix_spawn_file_actions_adddup2(&actions,null,STDERR_FILENO);setup|=posix_spawn_file_actions_addclose(&actions,null); }
    char* argv[]={const_cast<char*>(wine.c_str()),const_cast<char*>("C:\\ALVR\\AudioSceneWatcher.exe"),nullptr};
    pid_t child=0;
    const int error=setup?setup:posix_spawn(&child,wine.c_str(),&actions,nullptr,argv,pointers.data());
    posix_spawn_file_actions_destroy(&actions);close(pair[1]);if (null>=0) close(null);
    if (error) { close(pair[0]);return; }
    s.helper=child;s.pipe=pair[0];s.next_spawn={};
    fprintf(stderr,"WINE-SCENE: background watcher native_pid=%d started\n",child);
}
bool accept_wire(State& s,const DmnSceneWire& wire,uint32_t self_pid,Clock::time_point now) {
    std::string executable,key;
    if (wire.magic!=DmnSceneWireMagic || wire.version!=DmnSceneWireVersion || wire.size!=sizeof(wire) ||
        !wire.publisher_pid || !wire.publisher_creation || wire.server_pid!=self_pid ||
        !wire.server_creation || !wire.observed_time || !wire.sequence || wire.sequence<=s.last_sequence ||
        wire.flags&~uint32_t(7) || !fixed_string(wire.executable,sizeof(wire.executable),executable) ||
        !fixed_string(wire.app_key,sizeof(wire.app_key),key)) return false;
    s.wire=wire;s.last_sequence=wire.sequence;s.last_wire=now;return true;
}
bool pump(State& s,uint32_t self_pid,Clock::time_point now) {
    bool changed=false;
    unsigned records=0;
    // Bounded drain: discard old queued records, then use the newest observation.
    for (;s.pipe>=0 && records<32;) {
        const ssize_t n=read(s.pipe,s.incoming.data()+s.incoming_bytes,s.incoming.size()-s.incoming_bytes);
        if (n>0) {
            s.incoming_bytes+=size_t(n);
            if (s.incoming_bytes==s.incoming.size()) {
                DmnSceneWire wire{};memcpy(&wire,s.incoming.data(),sizeof(wire));s.incoming_bytes=0;++records;
                if (!accept_wire(s,wire,self_pid,now)) { close_pipe(s);return false; }
                changed=true;
            }
        } else if (n<0 && errno==EINTR) continue;
        else if (n<0 && (errno==EAGAIN||errno==EWOULDBLOCK)) break;
        else { close_pipe(s);return false; }
    }
    if (records==32) { idle(s);s.candidate_count=0;return false; }
    return changed;
}
bool map(State& s,uint32_t self_pid,DmnSceneSourceSnapshot& out) {
    const auto& w=s.wire;
    if (!fresh(w.observed_time) || (w.flags&3)!=3 || (w.flags&DmnSceneWireSystem) || !w.scene_pid || !w.scene_creation ||
        w.scene_pid==self_pid || w.scene_pid==w.publisher_pid) return false;
    std::string executable,key;
    if (!fixed_string(w.executable,sizeof(w.executable),executable) ||
        !fixed_string(w.app_key,sizeof(w.app_key),key) || key.empty()) return false;
    executable=lower(executable);
    if (!pe_name(executable)||system_name(executable)) return false;
    Process publisher{},server{},target{},again{};
    if (!list(s,w.publisher_pid,publisher,self_pid) || publisher.name!="audioscenewatcher.exe" ||
        publisher.info.unix_pid!=s.helper || uint64_t(publisher.info.start_time)!=w.publisher_creation ||
        !list(s,self_pid,server,self_pid) || uint64_t(server.info.start_time)!=w.server_creation ||
        !list(s,w.scene_pid,target,self_pid) || target.name!=executable ||
        uint64_t(target.info.start_time)!=w.scene_creation || target.info.unix_pid==getpid() ||
        target.info.unix_pid==s.helper) return false;
    uint64_t publisher_start=0,start=0;
    if (!native_identity(s.helper,"audioscenewatcher.exe",s.prefix,publisher_start) ||
        !native_identity(target.info.unix_pid,executable,s.prefix,start) ||
        !list(s,w.scene_pid,again,self_pid) || again.info.unix_pid!=target.info.unix_pid ||
        again.info.start_time!=target.info.start_time || again.name!=target.name || !fresh(w.observed_time)) return false;
    out.wine_pid=target.info.pid;out.native_pid=target.info.unix_pid;
    out.start_time=start;out.ready=true;return true;
}
void diagnostic_file(State& s) {
    const char* directory=getenv("DMN_WINE_SOCKET_DIR");
    if (!directory || !*directory) return;
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir<0) return;
    struct stat st{};
    if (fstat(dir,&st) || st.st_uid!=getuid() || (st.st_mode&077)) { close(dir);return; }
    char name[64];snprintf(name,sizeof(name),"active-scene-%d.state",getpid());
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if (fd>=0) {
        if (fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() &&
            st.st_nlink==1 && (st.st_mode&077)==0 && ftruncate(fd,0)==0) {
            char text[256];const int n=snprintf(text,sizeof(text),
                "version=1 wine_pid=%u native_pid=%d start_time=%llu generation=%llu ready=%u observed_time=%llu\n",
                s.selected.wine_pid,s.selected.native_pid,
                static_cast<unsigned long long>(s.selected.start_time),
                static_cast<unsigned long long>(s.selected.generation),unsigned(s.selected.ready),
                static_cast<unsigned long long>(s.wire.observed_time));
            if (n>0 && n<int(sizeof(text))) (void)write(fd,text,size_t(n));
        }
        close(fd);
    }
    close(dir);
}
} // namespace

bool dmn_scene_source_snapshot(DmnSceneSourceSnapshot* out) noexcept {
    if (!out) return false;
    *out={};
    try {
        State& s=state();std::lock_guard<std::mutex> lock(s.mutex);
        uint32_t self_pid=0;
        if (!thread_pid(s,self_pid)) return false;
        const auto now=Clock::now();
        if (now<s.next_poll) { *out=s.selected;return out->ready; }
        s.next_poll=now+kPoll;
        spawn(s,now);
        const bool changed=pump(s,self_pid,now);
        if (s.last_wire==Clock::time_point{} || now-s.last_wire>kStale || !fresh(s.wire.observed_time)) {
            idle(s);s.candidate_count=0;
        } else if (changed) {
            const bool possible=(s.wire.flags&3)==3 && !(s.wire.flags&DmnSceneWireSystem) &&
                s.wire.scene_pid && s.wire.scene_creation;
            if (!possible) { idle(s);s.candidate_count=0; }
            else {
                if (s.candidate_pid!=s.wire.scene_pid || s.candidate_creation!=s.wire.scene_creation) {
                    s.candidate_pid=s.wire.scene_pid;s.candidate_creation=s.wire.scene_creation;
                    s.candidate_count=1;idle(s);
                } else ++s.candidate_count;
                if (s.candidate_count>=2) {
                    DmnSceneSourceSnapshot next{};
                    if (!map(s,self_pid,next)) idle(s);
                    else {
                        if (!s.selected.ready || s.selected.wine_pid!=next.wine_pid ||
                            s.selected.native_pid!=next.native_pid || s.selected.start_time!=next.start_time) {
                            next.generation=s.selected.generation+1;
                            fprintf(stderr,"WINE-SCENE: selected wine_pid=%u native_pid=%d generation=%llu\n",
                                next.wine_pid,next.native_pid,static_cast<unsigned long long>(next.generation));
                        } else next.generation=s.selected.generation;
                        s.selected=next;
                    }
                }
            }
        }
        diagnostic_file(s);*out=s.selected;return out->ready;
    } catch (...) { return false; }
}
