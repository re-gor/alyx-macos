// Read-only source guard: no tap, stream, recording or permission operation.
#include "../bridge/wine_audio_bridge.cpp"
#include <CoreAudio/CoreAudio.h>

extern "C" bool wine_bridge_own_pe_name(char*,size_t) { return false; }
bool dmn_scene_source_snapshot(DmnSceneSourceSnapshot* out) noexcept {
    *out={};return false;
}
extern "C" dmn_audio_status dmn_audio_tap_create_idle(dmn_audio_tap**,dmn_audio_error*) {
    return DMN_AUDIO_DISABLED;
}
extern "C" dmn_audio_status dmn_audio_tap_retarget_pid(dmn_audio_tap*,int32_t,dmn_audio_error*) {
    return DMN_AUDIO_DISABLED;
}

extern "C" dmn_audio_status dmn_audio_tap_create(int32_t, dmn_audio_tap**, dmn_audio_error*) {
    return DMN_AUDIO_DISABLED;
}
extern "C" dmn_audio_status dmn_audio_tap_destroy(dmn_audio_tap**, dmn_audio_error*) {
    return DMN_AUDIO_DISABLED;
}
extern "C" dmn_audio_status dmn_audio_tap_get_format(const dmn_audio_tap*, dmn_audio_format*) {
    return DMN_AUDIO_DISABLED;
}
extern "C" dmn_audio_status dmn_audio_tap_get_uid(const dmn_audio_tap*, char*, size_t, size_t*) {
    return DMN_AUDIO_DISABLED;
}
extern "C" const char* dmn_audio_status_name(dmn_audio_status) { return "read-only-probe-disabled"; }

int main(int argc, char** argv) {
    if (argc != 3) { puts("usage: source_ready_probe NATIVE_PID EXPECTED_PREFIX"); return 2; }
    const int32_t pid=producer_pid(argv[1]);
    if (!pid || !*argv[2]) { puts("valid_arguments=false"); return 2; }
    proc_bsdinfo info{};
    const int metadata=proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof(info));
    const bool exists=metadata==sizeof(info);
    const bool owner=exists && info.pbi_uid==getuid();
    printf("pid=%d exists=%d same_uid=%d\n",pid,exists,owner);
    if (!owner) return 1;
    std::array<char,32768> args{};
    int mib[]={CTL_KERN,KERN_PROCARGS2,pid};
    size_t size=args.size();
    const int status=sysctl(mib,3,args.data(),&size,nullptr,0);
    const int argument_errno=status ? errno : 0;
    const bool match=status==0 && producer_arguments(args.data(),size,argv[2]);
    printf("args_read=%d errno=%d exact_game_and_prefix=%d\n",status==0,argument_errno,match);
    // Never print argv or environment. Reuse the exact current bridge parser.
    args.fill(0);
    if (!match) return 1;
    const pid_t source=pid;
    AudioObjectID object=kAudioObjectUnknown;
    UInt32 bytes=sizeof(object);
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslatePIDToProcessObject,
        kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    const OSStatus os=AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,
        sizeof(source),&source,&bytes,&object);
    const bool audio=os==noErr && bytes==sizeof(object) && object!=kAudioObjectUnknown;
    printf("audio_property_status=%d hex=%08x size_ok=%d producer_audio_object=%d\n",
        int(os),unsigned(os),bytes==sizeof(object),audio);
    if (audio) {
        UInt32 output_running=0;
        bytes=sizeof(output_running);
        const AudioObjectPropertyAddress running_address{kAudioProcessPropertyIsRunningOutput,
            kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        const OSStatus running_status=AudioObjectGetPropertyData(object,&running_address,
            0,nullptr,&bytes,&output_running);
        printf("source_output_status=%d size_ok=%d running=%d\n",int(running_status),
            bytes==sizeof(output_running),output_running!=0);
        const AudioObjectPropertyAddress devices_address{kAudioProcessPropertyDevices,
            kAudioObjectPropertyScopeOutput,kAudioObjectPropertyElementMain};
        UInt32 device_bytes=0;
        OSStatus device_status=AudioObjectGetPropertyDataSize(object,&devices_address,
            0,nullptr,&device_bytes);
        std::array<AudioObjectID,8> devices{};
        if (device_status==noErr && device_bytes && device_bytes<=sizeof(devices) &&
            device_bytes%sizeof(AudioObjectID)==0) {
            device_status=AudioObjectGetPropertyData(object,&devices_address,0,nullptr,
                &device_bytes,devices.data());
            const unsigned count=device_bytes/sizeof(AudioObjectID);
            printf("source_output_devices_status=%d count=%u\n",int(device_status),count);
            if (device_status==noErr && count<=devices.size()) {
                for (unsigned i=0;i<count;++i) {
                    Float64 rate=0;
                    UInt32 size=sizeof(rate);
                    const AudioObjectPropertyAddress rate_address{kAudioDevicePropertyNominalSampleRate,
                        kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
                    const OSStatus rate_status=AudioObjectGetPropertyData(devices[i],&rate_address,
                        0,nullptr,&size,&rate);
                    UInt32 frames=0;
                    size=sizeof(frames);
                    const AudioObjectPropertyAddress frames_address{kAudioDevicePropertyBufferFrameSize,
                        kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
                    const OSStatus frames_status=AudioObjectGetPropertyData(devices[i],&frames_address,
                        0,nullptr,&size,&frames);
                    // Never enumerate unrelated devices or print names/UIDs/sample data.
                    printf("source_output_device_index=%u rate_status=%d rate=%.0f frame_status=%d buffer_frames=%u\n",
                        i,int(rate_status),rate,int(frames_status),frames);
                }
            }
        } else printf("source_output_devices_status=%d bounded_size_ok=0\n",int(device_status));
    }
    return audio ? 0 : 3;
}
