// Compile/run only with fake tap functions below: no HAL/recording/TCC calls.
#define DMN_AUDIO_DIAGNOSTICS_TESTING 1 // No worker, HAL or real capture in tests.
#include "wine_audio_bridge.cpp"
#include <cassert>
#include <string>
#include <vector>
#include <atomic>
#include <condition_variable>

struct dmn_audio_tap { bool ready; };
extern "C" bool wine_bridge_own_pe_name(char* name,size_t size) {
    if (size<13) return false;
    strcpy(name,"vrserver.exe");return true;
}
bool dmn_scene_source_snapshot(DmnSceneSourceSnapshot* out) noexcept {
    *out={};return false;
}
namespace {
unsigned creates=0, destroys=0, original_calls=0;
bool fail_create=false, fail_cleanup=false, invalid_format=false;
int32_t original(void*) { ++original_calls; return 0; }
CreateStreamParams observed{};
void* observed_pointer=nullptr;
int32_t capture_result=0;
unsigned stream_calls=0,get_calls=0,release_calls=0;
uint64_t fake_stream_handle=0x12345678;
int32_t get_result=0,get_status=0;
uint32_t get_frames=2,get_flags=0;
std::array<float,4> get_samples={0,.5f,-.25f,1};
uint8_t* get_data=reinterpret_cast<uint8_t*>(get_samples.data());
bool release_saw_retired=false;
bool replace_generation_in_get=false;
std::mutex sample_gate_mutex;
std::condition_variable sample_gate;
bool sample_gate_entered=false,release_gate_entered=false,allow_sample_read=false;
std::atomic<bool> concurrent_case{false},release_lock_blocked{false},release_saw_commit{false};
std::atomic<unsigned> concurrent_original_releases{0};
void hold_after_generation_check() {
    std::unique_lock<std::mutex> lock(sample_gate_mutex);
    sample_gate_entered=true;sample_gate.notify_all();
    assert(sample_gate.wait_for(lock,std::chrono::seconds(2),[]{return allow_sample_read;}));
}
void mark_release_mutex_attempt() {
    // Different thread: a successful try_lock would expose the original gap.
    const bool acquired=state()->diagnostics.mutex.try_lock();
    release_lock_blocked.store(!acquired);
    if (acquired) state()->diagnostics.mutex.unlock();
    {std::lock_guard<std::mutex> lock(sample_gate_mutex);release_gate_entered=true;}
    sample_gate.notify_all();
}
int32_t fake_stream(void* raw) {
    ++stream_calls;
    auto& params=*static_cast<CreateStreamParams*>(raw);
    observed=params; observed_pointer=raw;
    params.result=capture_result;
    if (params.channel_count) *params.channel_count=2;
    if (params.stream) *params.stream=fake_stream_handle;
    return 0;
}
int32_t fake_get(void* raw) {
    ++get_calls;
    if (!raw) return get_status;
    auto& p=*static_cast<GetCaptureParams*>(raw);p.result=get_result;
    if (replace_generation_in_get) {
        replace_generation_in_get=false;
        signal_register(state(),p.stream,{44100,4411},123);
    }
    if (get_result==0) {
        if (p.frames) *p.frames=get_frames;
        if (p.flags) *p.flags=get_flags;
        if (p.data) *p.data=get_data;
    }
    return get_status;
}
int32_t fake_release(void* raw) {
    ++release_calls;
    if (concurrent_case.load()) ++concurrent_original_releases;
    if (!raw) return 0;
    auto& p=*static_cast<ReleaseStreamParams*>(raw);
    {std::lock_guard<std::mutex> lock(state()->diagnostics.mutex);
        release_saw_retired=true;
        for (const auto& e:state()->diagnostics.entries)
            if (e.stream==p.stream) release_saw_retired=false;
        if (concurrent_case.load()) {
            const auto& c=state()->diagnostics.window;
            release_saw_commit.store(c.calls==1 && c.frames==2 && c.sampled_frames==2 &&
                c.finite==4 && c.nonzero==3);
        }
    }
    p.result=0;return 0;
}
void clear_diagnostics(State* value) {
    std::lock_guard<std::mutex> lock(value->diagnostics.mutex);
    auto& d=value->diagnostics;d.entries={};d.window={};d.next_generation=1;
    d.registered=d.retired=d.full=d.deadline_ns=0;d.source_pid=0;d.started=d.active=false;
}
}
extern "C" dmn_audio_status dmn_audio_tap_create(int32_t pid, dmn_audio_tap** out,
                                                dmn_audio_error*) {
    assert(pid==123 || pid==124);
    ++creates;
    *out=new dmn_audio_tap{!fail_create};
    return fail_create ? DMN_AUDIO_HAL_FAILURE : DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_destroy(dmn_audio_tap** inout,
                                                 dmn_audio_error*) {
    ++destroys;
    if (fail_cleanup) return DMN_AUDIO_CLEANUP_FAILED;
    delete *inout;
    *inout=nullptr;
    return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_create_idle(dmn_audio_tap** out,dmn_audio_error* error) {
    return dmn_audio_tap_create(123,out,error);
}
extern "C" dmn_audio_status dmn_audio_tap_retarget_pid(dmn_audio_tap*,int32_t,dmn_audio_error*) {
    return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_get_format(const dmn_audio_tap* tap,
                                                     dmn_audio_format* out) {
    assert(tap && tap->ready);
    *out={};
    out->struct_size=sizeof(*out);
    out->version=DMN_AUDIO_FORMAT_VERSION;
    out->sample_rate=48000;
    out->channels=invalid_format ? 1 : 2;
    return DMN_AUDIO_OK;
}
extern "C" dmn_audio_status dmn_audio_tap_get_uid(const dmn_audio_tap* tap,
                                                  char* out, size_t size, size_t* required) {
    assert(tap && tap->ready);
    constexpr char uid[]="dmn-audio-test-private-uid";
    *required=sizeof(uid);
    if (size<sizeof(uid)) return DMN_AUDIO_BUFFER_TOO_SMALL;
    memcpy(out,uid,sizeof(uid));
    return DMN_AUDIO_OK;
}
extern "C" const char* dmn_audio_status_name(dmn_audio_status) { return "fake"; }

int main(int argc, char** argv) {
    unsetenv("DMN_AUDIO_SOURCE_MODE");
    assert(argc==2);
    assert(file_matches(argv[1]));
    assert(!file_matches("/nonexistent/winecoreaudio.so"));
    std::array<uint8_t,kFileBytes> corrupt{};
    assert(!digest_matches(corrupt.data(),corrupt.size()));
    std::array<UnixCall,36> mapped{};
    constexpr uintptr_t base=0x10000000;
    for (size_t i=0; i<mapped.size(); ++i)
        mapped[i]=reinterpret_cast<UnixCall>(base+kOffsets[i]);
    assert(table_matches(base,mapped.data()));
    mapped[35]=reinterpret_cast<UnixCall>(base+0x951);
    assert(!table_matches(base,mapped.data()));
    std::array<uint8_t,0x1b00> loaded{};
    memcpy(loaded.data()+kOffsets[5],kReleasePrefix.data(),kReleasePrefix.size());
    memcpy(loaded.data()+kOffsets[12],kGetCapturePrefix.data(),kGetCapturePrefix.size());
    assert(diagnostics_code_matches(reinterpret_cast<uintptr_t>(loaded.data())));
    loaded[kOffsets[12]+1]^=1;
    assert(!diagnostics_code_matches(reinterpret_cast<uintptr_t>(loaded.data())));
    assert(server_argument("C:\\SteamVR\\bin\\win64\\VRServer.EXE"));
    assert(!server_argument("notvrserver.exe"));
    assert(!server_argument("vrserver.exe.bak"));
    assert(!server_argument(""));
    assert(producer_pid("123")==123);
    for (const char* bad : {"", "0", "-1", "+123", " 123", "123x", "2147483648"})
        assert(producer_pid(bad)==0);
    assert(producer_pid(nullptr)==0);
    // Native argument parsing verifies both exact game basename and prefix;
    // an unrelated/stale PID file cannot select an arbitrary desktop process.
    std::string args(sizeof(int),'\0');
    const int fake_argc=2;
    memcpy(args.data(),&fake_argc,sizeof(fake_argc));
    auto append=[&](const char* value) { args+=value; args+='\0'; };
    append("/wine/bin/wine64-preloader"); args+='\0';
    append("wine64"); append("C:\\Steam\\hlvr.exe");
    append("WINEPREFIX=/prefix/test"); append("OTHER=value");
    assert(producer_arguments(args.data(),args.size(),"/prefix/test"));
    assert(!producer_arguments(args.data(),args.size(),"/prefix/other"));
    assert(!producer_arguments(args.data(),args.size()-1,"/prefix/test"));

    State* value=state();
    assert(value);
    value->original_loopback=&original;
    LoopbackParams params{};
    params.result=kNotImplemented;
    unsetenv("DMN_AUDIO_TAP");
    setenv("DMN_AUDIO_SOURCE_PID","123",1);
    assert(get_loopback(&params)==0 && creates==0 && original_calls==1);
    for (const char* gate : {"0", "true", "01"}) {
        setenv("DMN_AUDIO_TAP",gate,1);
        assert(get_loopback(&params)==0 && creates==0);
    }
    setenv("DMN_AUDIO_TAP","1",1);
    setenv("DMN_AUDIO_SOURCE_PID","-1",1);
    assert(get_loopback(&params)==0 && creates==0);
    assert(wine_audio_interpose_table(mapped.data(),"__wine_unix_call_wow64_funcs")
        ==mapped.data());
    setenv("DMN_AUDIO_SOURCE_PID","123",1);

    fail_create=true;
    assert(get_loopback(&params)==0 && params.result==kFailure && creates==1);
    assert(value->tap && !value->ready);
    fail_cleanup=true;
    assert(get_loopback(&params)==0 && creates==1 && destroys==1);
    assert(value->tap && !value->ready);
    fail_cleanup=false;
    fail_create=false;
    invalid_format=true;
    assert(get_loopback(&params)==0 && creates==2 && destroys==2 && !value->ready);
    invalid_format=false;
    assert(get_loopback(&params)==0 && creates==3 && destroys==3 && value->ready);
    assert(params.result==kBufferTooSmall && params.ret_device_len>1);

    const auto needed=params.ret_device_len;
    assert(get_loopback(&params)==0 && params.result==kOutOfMemory && creates==3);
    std::array<char,DMN_AUDIO_UID_MAX_BYTES+2> output{};
    output.fill('Z');
    params.ret_device=output.data()+1;
    params.ret_device_len=needed-1;
    assert(get_loopback(&params)==0 && params.result==kBufferTooSmall && creates==3);
    for (char c:output) assert(c=='Z');
    params.ret_device_len=needed;
    assert(get_loopback(&params)==0 && params.result==0 && creates==3);
    assert(std::string(params.ret_device)=="dmn-audio-test-private-uid");
    assert(output[0]=='Z' && output[needed+1]=='Z');
    params.ret_device_len=needed;
    assert(get_loopback(&params)==0 && params.result==0 && creates==3);
    setenv("DMN_AUDIO_SOURCE_PID","124",1);
    assert(get_loopback(&params)==0 && params.result==kFailure && creates==3);
    assert(value->pid==123 && destroys==3); // Never destroy an advertised live UID.
    unsetenv("DMN_AUDIO_TAP");
    const unsigned previous=original_calls;
    assert(get_loopback(&params)==0 && original_calls==previous+1 && creates==3);
    assert(get_loopback(nullptr)==0);
    // Capacity override changes only an owned private shared-capture call.
    std::array<UnixCall,36> originals{};
    originals[4]=&fake_stream;
    value->original_table=originals.data();
    std::array<uint8_t,40> wave{};
    const uint16_t tag=3,channels=2,bits=32,block=8;
    const uint32_t rate=44100;
    memcpy(wave.data(),&tag,2);memcpy(wave.data()+2,&channels,2);
    memcpy(wave.data()+4,&rate,4);memcpy(wave.data()+12,&block,2);
    memcpy(wave.data()+14,&bits,2);
    uint32_t output_channels=0; uint64_t output_stream=0;
    CreateStreamParams cp{};
    cp.device="dmn-audio-test-private-uid";
    cp.flow=1;cp.share=0;cp.duration=300000;cp.period=100000;
    cp.format=wave.data();cp.channel_count=&output_channels;cp.stream=&output_stream;
    setenv("DMN_AUDIO_CAPTURE_BUFFER_MS","100",1);
    assert(create_capture(&cp)==0 && observed_pointer==&cp && observed.duration==300000);
    setenv("DMN_AUDIO_TAP","1",1);
    assert(create_capture(&cp)==0 && observed.duration==1000000 && observed.period==100000);
    assert(observed_pointer!=&cp && cp.duration==300000 && cp.period==100000);
    assert(observed.format==cp.format && observed.channel_count==cp.channel_count);
    assert(output_channels==2 && output_stream==0x12345678 && cp.result==0);
    cp.device="unrelated-input";
    assert(create_capture(&cp)==0 && observed_pointer==&cp && observed.duration==300000);
    cp.device="dmn-audio-test-private-uid";
    cp.flow=0;
    assert(create_capture(&cp)==0 && observed_pointer==&cp);
    cp.flow=1;cp.share=1;
    assert(create_capture(&cp)==0 && observed_pointer==&cp);
    cp.share=0;
    for (const char* invalid : {"0","20","201","garbage"}) {
        setenv("DMN_AUDIO_CAPTURE_BUFFER_MS",invalid,1);
        assert(create_capture(&cp)==0 && observed.duration==300000);
    }
    setenv("DMN_AUDIO_CAPTURE_BUFFER_MS","100",1);
    cp.duration=1500000;
    assert(create_capture(&cp)==0 && observed.duration==1500000);
    cp.duration=300000;
    capture_result=kFailure;
    assert(create_capture(&cp)==0 && cp.result==kFailure && cp.duration==300000);
    assert(create_capture(nullptr)==0);

    // Optional metadata callbacks: originals exactly once; no HAL/sample files.
    originals[5]=&fake_release;originals[12]=&fake_get;
    State cloned_off{},cloned_on{};
    prepare_table(cloned_off,originals.data(),false);
    prepare_table(cloned_on,originals.data(),true);
    assert(cloned_off.table[5]==&fake_release && cloned_off.table[12]==&fake_get);
    assert(cloned_on.table[5]==&release_stream_observed && cloned_on.table[12]==&get_capture_observed);
    assert(cloned_off.table[4]==&create_capture && cloned_off.table[15]==&get_loopback);
    assert(!cloned_on.diagnostics.started && !cloned_on.diagnostics.active); // clone/init does not arm.
    value->diagnostics_installed=true;clear_diagnostics(value);
    capture_result=0;cp.device="dmn-audio-test-private-uid";cp.flow=1;cp.share=0;
    uint32_t returned_frames=0,returned_flags=0;
    uint8_t* returned_data=nullptr;
    GetCaptureParams gp{fake_stream_handle,0,&returned_data,&returned_frames,&returned_flags,nullptr,nullptr};
    unsetenv("DMN_AUDIO_DIAGNOSTICS");
    assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    unsigned calls=get_calls;
    assert(get_capture_observed(&gp)==0 && get_calls==calls+1 && value->diagnostics.window.calls==0);
    for (const char* off : {"0","true","01"}) {
        setenv("DMN_AUDIO_DIAGNOSTICS",off,1);
        assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    }
    setenv("DMN_AUDIO_DIAGNOSTICS","1",1);
    cp.device="unrelated-input";
    assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    cp.device="microphone";assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    cp.device="dmn-audio-test-private-uid";cp.flow=0;
    assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    cp.flow=1;capture_result=kFailure;
    assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    capture_result=0;fake_stream_handle=0;
    assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    fake_stream_handle=0x12345678;
    // Exact extensible Float32 GUID is required, not Data1 alone.
    std::array<uint8_t,40> extended=wave;
    const uint16_t ext_tag=0xfffe,ext_size=22;
    constexpr uint8_t float_guid[]={3,0,0,0,0,0,16,0,128,0,0,170,0,56,155,113};
    memcpy(extended.data(),&ext_tag,2);memcpy(extended.data()+16,&ext_size,2);
    memcpy(extended.data()+24,float_guid,16);
    SignalFormat checked{};cp.format=extended.data();
    assert(signal_format(cp,checked));extended[39]^=1;
    assert(!signal_format(cp,checked));
    assert(create_capture(&cp)==0 && value->diagnostics.registered==0);
    cp.format=wave.data();
    assert(create_capture(&cp)==0 && value->diagnostics.registered==1);
    assert(signal_active_streams(value->diagnostics)==1 && value->diagnostics.source_pid==123);
    calls=get_calls;
    assert(get_capture_observed(&gp)==0 && get_calls==calls+1 && gp.result==0);
    assert(returned_frames==2 && returned_flags==0 && returned_data==get_data);
    assert(get_samples[1]==.5f && get_samples[2]==-.25f && get_samples[3]==1); // Never write samples.
    auto c=value->diagnostics.window;
    assert(c.calls==1 && c.frames==2 && c.sampled_frames==2 && c.finite==4 && c.nonzero==3);
    assert(c.nonfinite==0 && c.peak==1 && std::fabs(c.squares-1.3125)<1e-9);
    get_samples.fill(0);assert(get_capture_observed(&gp)==0);
    c=value->diagnostics.window;
    assert(c.frames==4 && c.finite==8 && c.nonzero==3 && c.silent_frames==0); // flags0 silence.
    get_result=int32_t(0x08890001u);gp.data=nullptr;gp.frames=nullptr;gp.flags=nullptr;
    assert(get_capture_observed(&gp)==0 && gp.result==get_result && value->diagnostics.window.empty==1);
    get_result=kFailure;
    assert(get_capture_observed(&gp)==0 && gp.result==kFailure && value->diagnostics.window.errors==1);
    get_result=0;
    get_status=int32_t(0xc0000001u);calls=get_calls;
    assert(get_capture_observed(&gp)==get_status && get_calls==calls+1 && value->diagnostics.window.errors==2);
    get_status=0;
    assert(get_capture_observed(&gp)==0 && value->diagnostics.window.invalid==1);
    gp.data=&returned_data;gp.frames=&returned_frames;gp.flags=&returned_flags;
    get_data=nullptr;
    assert(get_capture_observed(&gp)==0 && value->diagnostics.window.invalid==2);
    get_flags=2;
    assert(get_capture_observed(&gp)==0 && value->diagnostics.window.silent_frames==2);
    get_flags=0;get_data=reinterpret_cast<uint8_t*>(get_samples.data());
    get_samples={std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-.5f,0};
    assert(get_capture_observed(&gp)==0 && value->diagnostics.window.nonfinite==2);
    std::vector<float> subset(4300*2,.25f);
    get_data=reinterpret_cast<uint8_t*>(subset.data());get_frames=4300;
    const auto sampled_before=value->diagnostics.window.sampled_frames;
    const auto frames_before=value->diagnostics.window.frames;
    assert(get_capture_observed(&gp)==0 && value->diagnostics.window.frames==frames_before+4300 &&
        value->diagnostics.window.sampled_frames==sampled_before+4096);
    get_frames=2;get_data=reinterpret_cast<uint8_t*>(get_samples.data());
    const auto known_calls=value->diagnostics.window.calls;
    gp.stream=0xabcdef;
    assert(get_capture_observed(&gp)==0 && value->diagnostics.window.calls==known_calls);
    gp.stream=fake_stream_handle;
    ReleaseStreamParams rp{fake_stream_handle,nullptr,0};calls=release_calls;
    assert(release_stream_observed(&rp)==0 && release_calls==calls+1 && release_saw_retired);
    assert(signal_active_streams(value->diagnostics)==0 && value->diagnostics.retired==1);
    // Reused handle for unrelated input stays unobserved after retirement.
    cp.device="microphone";assert(create_capture(&cp)==0);
    assert(get_capture_observed(&gp)==0 && value->diagnostics.window.calls==known_calls);
    cp.device="dmn-audio-test-private-uid";assert(create_capture(&cp)==0);
    assert(value->diagnostics.registered==2 && signal_active_streams(value->diagnostics)==1);
    replace_generation_in_get=true;
    const auto prior_generation_calls=value->diagnostics.window.calls;
    calls=get_calls;
    assert(get_capture_observed(&gp)==0 && get_calls==calls+1 &&
        value->diagnostics.window.calls==prior_generation_calls); // Reuse during native call is ignored.
    const auto before=value->diagnostics.window.calls;
    {std::lock_guard<std::mutex> lock(value->diagnostics.mutex);
        value->diagnostics.deadline_ns=signal_now_ns()-1;}
    calls=get_calls;
    assert(get_capture_observed(&gp)==0 && get_calls==calls+1 &&
        !value->diagnostics.active && value->diagnostics.window.calls==before);
    unsetenv("DMN_AUDIO_DIAGNOSTICS");calls=get_calls;
    assert(get_capture_observed(&gp)==0 && get_calls==calls+1 && value->diagnostics.window.calls==before);
    assert(release_stream_observed(&rp)==0 && release_saw_retired && signal_active_streams(value->diagnostics)==0);
    calls=get_calls;assert(get_capture_observed(nullptr)==0 && get_calls==calls+1);
    // One deterministic concurrent-retire case: postcheck reader lock pins the
    // borrowed data until metadata is committed; native free cannot pass it.
    clear_diagnostics(value);setenv("DMN_AUDIO_DIAGNOSTICS","1",1);
    get_result=get_status=0;get_flags=0;get_frames=2;get_samples={0,.5f,-.25f,1};
    get_data=reinterpret_cast<uint8_t*>(get_samples.data());
    assert(create_capture(&cp)==0 && signal_active_streams(value->diagnostics)==1);
    diagnostics_test_after_current=&hold_after_generation_check;
    diagnostics_test_before_retire_lock=&mark_release_mutex_attempt;
    concurrent_case=true;concurrent_original_releases=0;release_lock_blocked=false;release_saw_commit=false;
    const unsigned get_before=get_calls,release_before=release_calls;
    std::thread reader([&]{assert(get_capture_observed(&gp)==0);});
    {
        std::unique_lock<std::mutex> lock(sample_gate_mutex);
        assert(sample_gate.wait_for(lock,std::chrono::seconds(2),[]{return sample_gate_entered;}));
    }
    std::thread releaser([&]{assert(release_stream_observed(&rp)==0);});
    {
        std::unique_lock<std::mutex> lock(sample_gate_mutex);
        assert(sample_gate.wait_for(lock,std::chrono::seconds(2),[]{return release_gate_entered;}));
        assert(release_lock_blocked.load() && concurrent_original_releases.load()==0);
        allow_sample_read=true;
    }
    sample_gate.notify_all();reader.join();releaser.join();
    assert(get_calls==get_before+1 && release_calls==release_before+1 &&
        concurrent_original_releases==1 && release_saw_commit && release_saw_retired);
    assert(signal_active_streams(value->diagnostics)==0);
    concurrent_case=false;diagnostics_test_after_current=nullptr;diagnostics_test_before_retire_lock=nullptr;
    puts("PASS: concurrent retire waits for bounded sample read and counter commit");
    // Fake-only teardown; production intentionally caches private HAL objects.
    assert(dmn_audio_tap_destroy(&value->tap,nullptr)==DMN_AUDIO_OK);
    puts("PASS: Wine audio guards, stable cache, capacity and scoped metadata diagnostics");
}
