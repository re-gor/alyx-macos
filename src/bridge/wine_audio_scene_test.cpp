// All tap and Wine callbacks are fake: no HAL, recording or SteamVR operation.
#define DMN_AUDIO_DIAGNOSTICS_TESTING 1
#include "wine_audio_bridge.cpp"
#include <cassert>
#include <vector>

struct dmn_audio_tap { bool valid=true; int32_t pid=0; };
static DmnSceneSourceSnapshot fake_scene{};
static bool poison=false,stop_fail=false,reset_fail=false;
static bool silent_packet=false;
static dmn_audio_status retarget_status=DMN_AUDIO_OK;
static unsigned retarbuffer_get_calls=0,starts=0,stops=0,resets=0,buffer_get_calls=0;
static unsigned packet_size_calls=0;
static uint32_t fake_packet_frames=2;
static std::array<float,4> samples={.1f,.2f,.3f,.4f};
static std::vector<int> order;
extern "C" bool wine_bridge_own_pe_name(char* name,size_t n) {
    if (n<13) return false;
    strcpy(name,"vrserver.exe");return true;
}
bool dmn_scene_source_snapshot(DmnSceneSourceSnapshot* out) noexcept {
    *out=fake_scene;return out->ready;
}
extern "C" dmn_audio_status dmn_audio_tap_create_idle(dmn_audio_tap** out,dmn_audio_error*) {
    *out=new dmn_audio_tap;return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_create(int32_t pid,dmn_audio_tap** out,dmn_audio_error* e) {
    auto s=dmn_audio_tap_create_idle(out,e);(*out)->pid=pid;return s;
}
extern "C" dmn_audio_status dmn_audio_tap_retarget_pid(dmn_audio_tap* tap,int32_t pid,dmn_audio_error*) {
    ++retarbuffer_get_calls;order.push_back(14);tap->pid=pid;
    if (poison) tap->valid=false;
    return retarget_status;
}
extern "C" dmn_audio_status dmn_audio_tap_destroy(dmn_audio_tap** p,dmn_audio_error*) {
    delete *p;*p=nullptr;return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_get_uid(const dmn_audio_tap* tap,char* p,size_t n,size_t* size) {
    if (!tap || !tap->valid) return DMN_AUDIO_NOT_READY;
    constexpr char uid[]="private-scene-test";*size=sizeof(uid);
    if (n<sizeof(uid)) return DMN_AUDIO_BUFFER_TOO_SMALL;
    memcpy(p,uid,sizeof(uid));return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_get_format(const dmn_audio_tap* tap,dmn_audio_format* f) {
    if (!tap || !tap->valid) return DMN_AUDIO_NOT_READY;
    *f={};f->struct_size=sizeof(*f);f->version=1;f->sample_rate=44100;
    f->channels=2;f->bits_per_channel=32;f->bytes_per_frame=8;return DMN_AUDIO_OK;
}
extern "C" const char* dmn_audio_status_name(dmn_audio_status) {return "fake";}
static int32_t create_stream_fake(void* raw) {
    auto& p=*static_cast<CreateStreamParams*>(raw);p.result=0;*p.stream=42;return 0;
}
static int32_t operation_fake(void* raw,int index) {
    auto& p=*static_cast<StreamOperationParams*>(raw);order.push_back(index);
    if (index==6) ++starts;
    if (index==7) ++stops;
    if (index==8) ++resets;
    p.result=(index==7 && stop_fail)||(index==8 && reset_fail) ? kFailure : 0;
    return 0;
}
static int32_t start_fake(void* p){return operation_fake(p,6);}
static int32_t stop_fake(void* p){return operation_fake(p,7);}
static int32_t reset_fake(void* p){return operation_fake(p,8);}
static int32_t get_fake(void* raw) {
    ++buffer_get_calls;order.push_back(12);auto& p=*static_cast<GetCaptureParams*>(raw);
    p.result=0;*p.frames=2;*p.data=silent_packet ? nullptr : reinterpret_cast<uint8_t*>(samples.data());
    *p.flags=silent_packet ? 2 : 0;return 0;
}
static int32_t release_capture_fake(void* raw) {
    static_cast<ReleaseCaptureParams*>(raw)->result=0;return 0;
}
static int32_t release_stream_fake(void* raw) {
    static_cast<ReleaseStreamParams*>(raw)->result=0;return 0;
}
static int32_t packet_size_fake(void* raw) {
    ++packet_size_calls;
    auto& p=*static_cast<GetNextPacketSizeParams*>(raw);
    p.result=0;*p.frames=fake_packet_frames;return 0;
}
static void reset_case() {
    State* v=state();
    if (v->tap) dmn_audio_tap_destroy(&v->tap,nullptr);
    v->ready=false;v->captures={};v->selected={};v->scene_poll_ns=0;
    v->scene_mode=false;v->scene_muted=true;v->scene_unsafe=false;v->scene_uid={};v->scene_format={};
    v->diagnostics_installed=false;poison=stop_fail=reset_fail=silent_packet=false;
    retarget_status=DMN_AUDIO_OK;fake_scene={};retarbuffer_get_calls=starts=stops=resets=buffer_get_calls=0;order.clear();
    packet_size_calls=0;fake_packet_frames=2;
    static std::array<UnixCall,36> original{};
    original[4]=create_stream_fake;original[5]=release_stream_fake;
    original[6]=start_fake;original[7]=stop_fake;original[8]=reset_fake;
    original[12]=get_fake;original[13]=release_capture_fake;
    original[21]=packet_size_fake;
    prepare_table(*v,original.data(),false);
    LoopbackParams l{};std::array<char,256> uid{};l.ret_device=uid.data();l.ret_device_len=uid.size();
    assert(get_loopback(&l)==0 && l.result==0 && v->tap && v->scene_mode);
    std::array<uint8_t,40> wave{};uint16_t tag=3,ch=2,bits=32,block=8;uint32_t rate=44100;
    memcpy(wave.data(),&tag,2);memcpy(wave.data()+2,&ch,2);memcpy(wave.data()+4,&rate,4);
    memcpy(wave.data()+12,&block,2);memcpy(wave.data()+14,&bits,2);
    uint64_t stream=0;uint32_t channels=0;
    CreateStreamParams c{};c.device=uid.data();c.flow=1;c.share=0;c.period=100000;c.duration=300000;
    c.format=wave.data();c.stream=&stream;c.channel_count=&channels;
    assert(create_capture(&c)==0 && c.result==0 && stream==42);
    StreamOperationParams start{42,kFailure};assert(start_managed(&start)==0 && start.result==0);
    starts=0;order.clear();
}
static GetCaptureParams get_args(uint8_t*& data,uint32_t& frames,uint32_t& flags) {
    GetCaptureParams g{};g.stream=42;g.data=&data;g.frames=&frames;g.flags=&flags;return g;
}
int main() {
    setenv("DMN_AUDIO_TAP","1",1);setenv("DMN_AUDIO_SOURCE_MODE","scene",1);
    uint8_t* data=nullptr;uint32_t frames=0,flags=0;
    // Emulate CPAL: packet-size is queried before GetBuffer. Idle must not
    // depend on a nonempty native packet to notice a game, or expose stale
    // idle packets which would keep CPAL's inner draining loop running.
    reset_case();frames=99;fake_packet_frames=0;
    GetNextPacketSizeParams next{42,kFailure,&frames};
    assert(state()->table[21]==get_next_packet_size_managed);
    assert(state()->table[14]!=get_next_packet_size_managed);
    assert(state()->table[21](&next)==0 && next.result==0 && frames==0);
    assert(packet_size_calls==0 && buffer_get_calls==0);
    fake_packet_frames=99;
    assert(state()->table[21](&next)==0 && frames==0 && packet_size_calls==0);
    fake_scene={456,123,100,1,true};state()->scene_poll_ns=0;fake_packet_frames=0;
    assert(state()->table[21](&next)==0 && next.result==0 && frames==0);
    assert(retarbuffer_get_calls==1 && buffer_get_calls==0);
    fake_packet_frames=2;
    assert(state()->table[21](&next)==0 && next.result==0 && frames==2);
    assert(packet_size_calls==2 && retarbuffer_get_calls==1 && buffer_get_calls==0);
    assert((order==std::vector<int>{7,8,14,6}));
    fake_scene={};state()->scene_poll_ns=0;fake_packet_frames=99;
    assert(state()->table[21](&next)==0 && next.result==0 && frames==0);
    assert(packet_size_calls==2 && state()->tap->pid==0);
    // Unrelated capture streams retain their original packet-size semantics.
    next.stream=1000;
    assert(state()->table[21](&next)==0 && frames==99 && packet_size_calls==3);
    reset_case();auto g=get_args(data,frames,flags);
    assert(get_capture_managed(&g)==0 && g.result==kBufferEmpty && frames==0 && buffer_get_calls==0);
    fake_scene={456,123,100,1,true};state()->scene_poll_ns=0;
    assert(get_capture_managed(&g)==0 && g.result==0 && frames==2);
    assert((order==std::vector<int>{7,8,14,6,12}));assert(retarbuffer_get_calls==1 && state()->captures[0].borrowed);
    fake_scene={457,124,101,2,true};state()->scene_poll_ns=0;
    state()->captures[1]={43,true,false,false};
    auto other=g;other.stream=43;
    assert(get_capture_managed(&other)==0 && other.result==kBufferEmpty && retarbuffer_get_calls==1);
    next.stream=43;state()->scene_poll_ns=0;frames=99;
    assert(get_next_packet_size_managed(&next)==0 && next.result==0 && frames==0);
    assert(retarbuffer_get_calls==1 && packet_size_calls==0);
    ReleaseCaptureParams r{42,2,kFailure};assert(release_capture_managed(&r)==0 && r.result==0);
    state()->captures[1]={};state()->scene_poll_ns=0;
    assert(get_capture_managed(&g)==0 && g.result==0 && retarbuffer_get_calls==2 && state()->selected.native_pid==124);
    assert(release_capture_managed(&r)==0);
    fake_scene={};state()->scene_poll_ns=0;
    assert(get_capture_managed(&g)==0 && g.result==kBufferEmpty && state()->tap->pid==0);
    for (int poison_case=0;poison_case<2;++poison_case) {
        reset_case();g=get_args(data,frames,flags);fake_scene={456,123,100,1,true};
        poison=true;retarget_status=poison_case ? DMN_AUDIO_BAD_UID : DMN_AUDIO_HAL_FAILURE;
        assert(get_capture_managed(&g)==0 && g.result==kBufferEmpty);
        assert(state()->scene_unsafe && starts==0 && buffer_get_calls==0);
    }
    for (int failure=0;failure<2;++failure) {
        reset_case();g=get_args(data,frames,flags);fake_scene={456,123,100,1,true};
        stop_fail=!failure;reset_fail=failure;
        assert(get_capture_managed(&g)==0 && g.result==kBufferEmpty);
        assert(state()->scene_unsafe && retarbuffer_get_calls==0 && starts==0 && buffer_get_calls==0);
    }
    reset_case();g=get_args(data,frames,flags);fake_scene={456,123,100,1,true};silent_packet=true;
    assert(get_capture_managed(&g)==0 && g.result==0 && frames==2 && data==nullptr);
    assert(state()->captures[0].borrowed);
    ReleaseCaptureParams silent_release{42,2,kFailure};
    assert(release_capture_managed(&silent_release)==0 && !state()->captures[0].borrowed);
    puts("PASS scene capture: packet-size idle liveness, ordered switch, borrowed deferral, poison and stop/reset fail-closed");
}
