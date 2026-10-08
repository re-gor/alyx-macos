#define DMN_SCENE_TESTING 1
#include "../bridge/wine_scene_source.cpp"
#include <cassert>

static std::vector<uint8_t> fake_reply;
static uint32_t fake_count;
static unsigned fake_calls;
static bool fake_bad_self;
static uint32_t fake_server(void* raw) {
    auto& r=*static_cast<Request*>(raw);
    assert(get<uint32_t>(r.fixed,0)==76);
    assert(get<uint32_t>(r.fixed,4)==0 && r.count==0);
    ++fake_calls;
    const uint32_t cap=get<uint32_t>(r.fixed,8);
    if (cap<fake_reply.size()) { put<uint32_t>(r.fixed,8,uint32_t(fake_reply.size()));return 0xc0000004; }
    memcpy(r.reply_data,fake_reply.data(),fake_reply.size());
    if (fake_bad_self) put<int32_t>(r.reply_data,36,getpid()+1);
    put<uint32_t>(r.fixed,0,0);put<uint32_t>(r.fixed,4,uint32_t(fake_reply.size()));
    put<uint32_t>(r.fixed,8,uint32_t(fake_reply.size()));put<uint32_t>(r.fixed,12,fake_count);
    return 0;
}
static void add(uint32_t pid,int32_t unix_pid,int64_t creation,const std::u16string& name,int threads=1) {
    fake_reply.resize((fake_reply.size()+7)&~size_t(7));
    ProcessInfo info{creation,uint32_t(name.size()*2),threads,8,pid,4,0,10,unix_pid};
    const auto start=fake_reply.size();fake_reply.resize(start+sizeof(info)+name.size()*2);
    memcpy(fake_reply.data()+start,&info,sizeof(info));
    memcpy(fake_reply.data()+start+sizeof(info),name.data(),name.size()*2);
    fake_reply.resize((fake_reply.size()+7)&~size_t(7));
    if (threads>0) fake_reply.resize(fake_reply.size()+size_t(threads)*40);
    ++fake_count;
}
static void reset() { fake_reply.clear();fake_count=0;fake_calls=0;fake_bad_self=false; }
int main() {
    reset();add(12,getpid(),123,u"C:\\SteamVR\\vrserver.exe");add(24,12345,456,u"C:\\Game\\ArbitraryGame.exe",2);
    Process p{};bool found=false;
    assert(parse_processes(fake_reply.data(),fake_reply.size(),fake_count,24,p,found));
    assert(found && p.name=="arbitrarygame.exe" && p.info.unix_pid==12345 && p.info.start_time==456);
    assert(parse_processes(fake_reply.data(),fake_reply.size(),fake_count,99,p,found) && !found);
    assert(!parse_processes(fake_reply.data(),fake_reply.size()-1,fake_count,24,p,found));
    auto bad=fake_reply;put<uint32_t>(bad.data(),8,3);assert(!parse_processes(bad.data(),bad.size(),fake_count,24,p,found));
    bad=fake_reply;put<int32_t>(bad.data(),12,-1);assert(!parse_processes(bad.data(),bad.size(),fake_count,24,p,found));
    State s;s.call=fake_server;s.self_logged=true;
    assert(list(s,24,p,12) && p.info.unix_pid==12345 && fake_calls==1);
    fake_bad_self=true;assert(!list(s,24,p,12));fake_bad_self=false;
    add(24,54321,999,u"C:\\Game\\ArbitraryGame.exe");
    assert(!parse_processes(fake_reply.data(),fake_reply.size(),fake_count,24,p,found));
    reset();add(12,getpid(),123,u"vrserver.exe");add(24,12345,456,u"G\U0001f600me.exe");
    assert(parse_processes(fake_reply.data(),fake_reply.size(),fake_count,24,p,found) && found && p.name=="g\xf0\x9f\x98\x80me.exe");
    reset();add(12,getpid(),123,u"vrserver.exe",2000);add(24,12345,456,u"game.exe");
    assert(list(s,24,p,12) && fake_calls==2); // bounded size retry.
    DmnSceneWire w{};w.magic=DmnSceneWireMagic;w.version=DmnSceneWireVersion;w.size=sizeof(w);
    w.publisher_pid=28;w.publisher_creation=789;w.server_pid=12;w.server_creation=123;w.sequence=1;
    w.flags=3;w.scene_pid=24;w.scene_creation=456;strcpy(w.executable,"game.exe");strcpy(w.app_key,"system.generated.game.exe");
    w.observed_time=utc_filetime();assert(fresh(w.observed_time));
    assert(!fresh(w.observed_time-30000000ULL));assert(!fresh(w.observed_time+20000000ULL));
    const auto now=Clock::now();assert(accept_wire(s,w,12,now));assert(!accept_wire(s,w,12,now));
    w.sequence=2;w.server_pid=13;assert(!accept_wire(s,w,12,now));w.server_pid=12;
    w.flags=8;assert(!accept_wire(s,w,12,now));w.flags=3;
    w.executable[12]='x';assert(!accept_wire(s,w,12,now));w.executable[12]=0;
    assert(accept_wire(s,w,12,now));
    assert(!system_name("arbitrarygame.exe") && system_name("steamtours.exe") && system_name("vrcompositor.exe"));
    s.selected={24,12345,1000,7,true};idle(s);assert(!s.selected.ready && !s.selected.native_pid && s.selected.generation==8);
    idle(s);assert(s.selected.generation==8); // stable idle generation.
    printf("PASS: exact protocol/self-map guard, bounded/truncated/duplicate records, UTF16, stale/foreign/hidden wire rejection, system exclusions, idle generation\n");
}
