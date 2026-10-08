// Generic, read-only OpenVR background observer. Never launches VR or games.
#include "scene_wire.h"
using U16 = char16_t;
using U32 = unsigned int;
using U64 = unsigned long long;
using I32 = int;
using Handle = void*;
#define API __declspec(dllimport)
struct FileTime { U32 low, high; };
struct ProcessEntry {
    U32 size, usage, pid; U64 heap; U32 module, threads, parent;
    I32 priority; U32 flags; U16 name[260];
};
static_assert(sizeof(ProcessEntry) == 568);
static_assert(__builtin_offsetof(ProcessEntry, name) == 44);
extern "C" {
API Handle GetStdHandle(U32);
API I32 WriteFile(Handle,const void*,U32,U32*,void*);
API U32 GetCurrentProcessId();
API Handle GetCurrentProcess();
API I32 GetProcessTimes(Handle,FileTime*,FileTime*,FileTime*,FileTime*);
API void GetSystemTimeAsFileTime(FileTime*);
API Handle CreateToolhelp32Snapshot(U32,U32);
API I32 Process32FirstW(Handle,ProcessEntry*);
API I32 Process32NextW(Handle,ProcessEntry*);
API Handle OpenProcess(U32,I32,U32);
API I32 CloseHandle(Handle);
API I32 QueryFullProcessImageNameW(Handle,U32,U16*,U32*);
API I32 WideCharToMultiByte(U32,U32,const U16*,I32,char*,I32,const char*,I32*);
API Handle LoadLibraryW(const U16*);
API void* GetProcAddress(Handle,const char*);
API void Sleep(U32);
API void ExitProcess(U32);
}
using Init = U32(*)(I32*,I32);
using Generic = void*(*)(const char*,I32*);
using Shutdown = void(*)();
using ScenePid = U32(*)();
using SceneState = I32(*)();
using AppKey = I32(*)(U32,char*,U32);
using AppBool = bool(*)(char*,I32,I32*);
static DmnSceneWire record;
static Handle output;
static ProcessEntry process_entry;
static U16 image_path[1024];
static U16 vr_dll[] = u"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll";

static bool equal(const U16* s,const char* t) {
    for (U32 i=0;i<260;++i) {
        U32 a=s[i],b=(unsigned char)t[i];
        if (a>='A'&&a<='Z') a+=32;
        if (b>='A'&&b<='Z') b+=32;
        if (a!=b) return false;
        if (!a) return true;
    }
    return false;
}
static bool equal_ascii(const char* a,const char* b) {
    for (U32 i=0;i<256;++i) {
        U32 x=(unsigned char)a[i],y=(unsigned char)b[i];
        if (x>='A'&&x<='Z') x+=32;
        if (y>='A'&&y<='Z') y+=32;
        if (x!=y) return false;
        if (!x) return true;
    }
    return false;
}
static bool system_executable(const char* name) {
    // No generated-app-key prefix exclusion: unmanifested games may use it.
    return equal_ascii(name,"steam.exe") || equal_ascii(name,"steamwebhelper.exe") ||
        equal_ascii(name,"steamservice.exe") || equal_ascii(name,"vrserver.exe") ||
        equal_ascii(name,"vrcompositor.exe") || equal_ascii(name,"vrmonitor.exe") ||
        equal_ascii(name,"vrwebhelper.exe") || equal_ascii(name,"vrdashboard.exe") ||
        equal_ascii(name,"vrstartup.exe") || equal_ascii(name,"steamtours.exe") ||
        equal_ascii(name,"AudioSceneWatcher.exe") || equal_ascii(name,"explorer.exe");
}
static U64 creation(Handle process) {
    FileTime a{},b{},c{},d{};
    if (!GetProcessTimes(process,&a,&b,&c,&d)) return 0;
    return U64(a.low)|(U64(a.high)<<32);
}
static bool server(U32& pid,U64& born) {
    pid=0; born=0;
    const Handle snap=CreateToolhelp32Snapshot(2,0);
    if (snap==(Handle)-1) return false;
    process_entry.size=sizeof(process_entry);
    I32 more=Process32FirstW(snap,&process_entry);
    U32 matches=0,visits=0;
    while (more && ++visits<=1024) {
        if (equal(process_entry.name,"vrserver.exe")) { pid=process_entry.pid; ++matches; }
        more=Process32NextW(snap,&process_entry);
    }
    CloseHandle(snap);
    if (more || matches!=1) { pid=0; return false; }
    const Handle p=OpenProcess(0x1000,0,pid);
    if (!p) { pid=0; return false; }
    born=creation(p); CloseHandle(p);
    return born!=0;
}
static bool target(U32 pid) {
    const Handle p=OpenProcess(0x1000,0,pid);
    if (!p) return false;
    const U64 a=creation(p);
    U32 count=1024;
    bool ok=a && QueryFullProcessImageNameW(p,0,image_path,&count) && count && count<1024;
    if (ok) {
        image_path[count]=0;
        U32 start=0;
        for (U32 i=0;i<count;++i) if (image_path[i]=='\\'||image_path[i]=='/') start=i+1;
        const I32 converted=WideCharToMultiByte(65001,0x80,image_path+start,
            I32(count-start),record.executable,255,nullptr,nullptr);
        ok=converted>0 && converted<256 && creation(p)==a;
        if (ok) { record.executable[converted]=0; record.scene_creation=a; }
    }
    CloseHandle(p);
    return ok;
}
static bool publish() {
    U32 n=0;
    return WriteFile(output,&record,sizeof(record),&n,nullptr) && n==sizeof(record);
}
extern "C" void entry() {
    output=GetStdHandle(U32(-11));
    record.magic=DmnSceneWireMagic; record.version=DmnSceneWireVersion;
    record.size=sizeof(record); record.publisher_pid=GetCurrentProcessId();
    record.publisher_creation=creation(GetCurrentProcess());
    if (!record.publisher_creation) ExitProcess(1);
    Init init=nullptr; Generic generic=nullptr; Shutdown shutdown=nullptr;
    void** apps=nullptr; bool initialized=false;
    U32 owner_pid=0; U64 owner_creation=0;
    for (;;) {
        U32 spid=0; U64 sborn=0;
        const bool alive=server(spid,sborn);
        if (initialized && (!alive || spid!=owner_pid || sborn!=owner_creation)) {
            shutdown(); initialized=false; apps=nullptr;
        }
        record.server_pid=alive?spid:0; record.server_creation=alive?sborn:0;
        record.scene_pid=0; record.scene_creation=0; record.flags=0; record.scene_state=0;
        for (U32 i=0;i<256;++i) record.executable[i]=0;
        for (U32 i=0;i<128;++i) record.app_key[i]=0;
        // No existing server => no VR_Init call. This avoids implicit VR startup.
        if (alive && !initialized) {
            if (!init) {
                Handle dll=LoadLibraryW(vr_dll);
                if (dll) {
                    init=(Init)GetProcAddress(dll,"VR_InitInternal");
                    generic=(Generic)GetProcAddress(dll,"VR_GetGenericInterface");
                    shutdown=(Shutdown)GetProcAddress(dll,"VR_ShutdownInternal");
                }
            }
            if (init && generic && shutdown) {
                // Recheck owner immediately before init; never Scene/Overlay.
                U32 again=0; U64 again_born=0;
                if (server(again,again_born) && again==spid && again_born==sborn) {
                    I32 error=0; init(&error,3); // VRApplication_Background.
                    if (!error) {
                        initialized=true; owner_pid=spid; owner_creation=sborn;
                        apps=(void**)generic("FnTable:IVRApplications_008",&error);
                        if (error || !apps) { shutdown(); initialized=false; apps=nullptr; }
                    }
                }
            }
        }
        if (initialized && apps) {
            record.flags=DmnSceneWireVRReady;
            record.scene_state=((SceneState)apps[25])();
            const U32 scene=((ScenePid)apps[30])();
            if (scene && scene!=record.publisher_pid && target(scene)) {
                I32 error=((AppKey)apps[5])(scene,record.app_key,sizeof(record.app_key));
                if (!error && ((ScenePid)apps[30])()==scene) {
                    record.scene_pid=scene; record.flags|=DmnSceneWireSceneValid;
                    if (system_executable(record.executable)) record.flags|=DmnSceneWireSystem;
                    // Property60 is explicitly DashboardOverlay; missing property
                    // is tolerated for generated, legitimately unmanifested games.
                    error=0;
                    const bool overlay=((AppBool)apps[15])(record.app_key,60,&error);
                    if (!error && overlay) record.flags|=DmnSceneWireSystem;
                }
            }
        }
        FileTime observed{};GetSystemTimeAsFileTime(&observed);
        record.observed_time=U64(observed.low)|(U64(observed.high)<<32);
        ++record.sequence;
        if (!publish()) break;
        Sleep(200);
    }
    if (initialized) shutdown();
    ExitProcess(0);
}
