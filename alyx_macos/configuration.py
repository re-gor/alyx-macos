"""Persistent profiles; offline edits require this wrapper to be stopped."""
import json
import plistlib
from pathlib import Path
import shlex
import shutil

from .core import Error, REPO, assert_stopped, run, sha256, write_json

EXPECTED_ALVR_SHA = '5b2dc0012254fa3c45268ed655c3f589b2d460a62907c620670cfb5d22a48fa8'
EXPECTED_WATCHER_SHA = 'c4daa99a3efa4873d7a6e79c78c5c706cbeffd0e497cec7278b847bdc3409102'

def wine_registry_values():
    desktop=r'HKCU\Control Panel\Desktop'
    return [
        (r'HKCU\Software\Wine\Mac Driver','RetinaMode','REG_SZ','Y'),
        (desktop,'LogPixels','REG_DWORD','192'),
        (desktop,'FontSmoothing','REG_SZ','2'),
        (desktop,'FontSmoothingType','REG_DWORD','2'),
        (desktop,'FontSmoothingGamma','REG_DWORD','1400'),
        (desktop,'FontSmoothingOrientation','REG_DWORD','1'),
        (r'HKCU\Software\Wine','Version','REG_SZ','win10'),
        (r'HKLM\Software\Microsoft\Windows NT\CurrentVersion','ProductName','REG_SZ','Microsoft Windows 10 Pro'),
        (r'HKCU\Software\Wine\AppDefaults\vrserver.exe\DllOverrides','msvcp140','REG_SZ','native,builtin'),
    ]

def apply_profile(session, prefs):
    """Leave unrelated schema fields and paired clients intact."""
    video = session['session_settings']['video']
    connection = session['session_settings']['connection']
    width, height = prefs['render_eye']
    if width % 32 or height % 32:
        raise Error('Use dimensions divisible by32; ALVR otherwise rounds them down.')
    for name in ('emulated_headset_view_resolution', 'transcoding_view_resolution'):
        video[name]['variant'] = 'Absolute'
        video[name]['Absolute'] = {'width':width, 'height':{'set':True, 'content':height}}
    video['preferred_fps'] = prefs['refresh_hz']
    video['enforce_server_frame_pacing'] = prefs['pacing']
    video['preferred_codec']['variant'] = 'H264'
    video['bitrate']['mode'].update(variant='ConstantMbps', ConstantMbps=prefs['bitrate_mbps'])
    video['bitrate']['adapt_to_framerate']['enabled'] = False
    software = video['encoder_config']['software']
    software.update(force_software_encoding=True, thread_count=prefs['encoder_threads'])
    ffr = video['foveated_encoding']
    ffr['enabled'] = prefs['foveated_encoding']
    ffr['content'].update(force_enable=False, center_size_x=.7, center_size_y=.7,
                          center_shift_x=.4, center_shift_y=.1, edge_ratio_x=2., edge_ratio_y=2.)
    connection.update(packet_size=prefs['packet_size'], max_queued_server_video_frames=prefs['video_queue'],
                      stream_protocol={'variant':'Tcp'}, web_server_port=8083,
                      wired_client_autolaunch=False)
    session['session_settings']['audio']['game_audio']['content']['buffering'].update(
        average_buffering_ms=50, batch_ms=10)
    session['session_settings']['audio']['game_audio']['content']['mute_when_streaming'] = False
    session['session_settings']['audio']['microphone']['enabled'] = False
    session['session_settings']['extra']['open_setup_wizard'] = False
    session['session_settings']['extra']['steamvr_launcher']['open_close_steamvr_with_dashboard'] = False
    ov = session['openvr_config']
    ov.update(refresh_rate=prefs['refresh_hz'], eye_resolution_width=width, eye_resolution_height=height,
        target_eye_resolution_width=width, target_eye_resolution_height=height,
        force_sw_encoding=True, sw_thread_count=prefs['encoder_threads'], codec=0,
        enable_foveated_encoding=prefs['foveated_encoding'], foveation_center_size_x=.7,
        foveation_center_size_y=.7, foveation_center_shift_x=.4, foveation_center_shift_y=.1,
        foveation_edge_ratio_x=2., foveation_edge_ratio_y=2.)
    return session

def configure(layout):
    layout.managed()
    assert_stopped(layout)
    if not (REPO/'.build/native/libd3dmetal-native.dylib').is_file() or not (REPO/'.build/libwine-utm-bridge.dylib').is_file():
        raise Error('Build the adapters first; configuration has not been changed.')
    require_built_watcher()
    if not (layout.alvr/'ALVR Dashboard.exe').is_file():
        raise Error('Install ALVR before configuring the audio watcher.')
    prefs = layout.preferences()
    layout.backup([layout.session, layout.prefs, layout.app/'Contents/Info.plist',
                   layout.steam/'config/steamvr.vrsettings'], 'configure')
    session = (json.loads(layout.session.read_text()) if layout.session.exists()
               else json.loads((REPO/'config/alvr-session-template.json').read_text()))
    apply_profile(session, prefs)
    # Empty scene means quiet idle; the observer selects the live game later.
    session['session_settings']['audio']['game_audio']['enabled'] = True
    write_json(layout.session, session)
    write_json(layout.prefs, prefs)
    path = layout.steam/'config/steamvr.vrsettings'
    vr = json.loads(path.read_text()) if path.exists() else {}
    vr.setdefault('steamvr', {}).update(allowDisplayLockedMode=True,
        startDashboardFromAppLaunch=False, startMonitorFromAppLaunch=False,
        enableHomeApp=False, supersampleManualOverride=True, supersampleScale=1.0)
    vr.setdefault('dashboard', {})['enableDashboard'] = False
    vr.setdefault('steamvr', {})['motionSmoothing'] = False
    write_json(path, vr)
    prefix = layout.env(hooks=False)
    for key,name,kind,value in wine_registry_values():
        run([layout.wine,'reg','add',key,'/v',name,'/t',kind,'/d',value,'/f'],env=prefix)
    from .runtime import stop
    stop(layout)
    install_built_adapter(layout)
    print('Configured profile. SteamVR/ALVR remain stopped; launch when ready.')

def require_built_watcher():
    watcher=REPO/'.build/scene/AudioSceneWatcher.exe'
    if not watcher.is_file() or sha256(watcher)!=EXPECTED_WATCHER_SHA:
        raise Error('Pinned AudioSceneWatcher build missing or changed; build-adapter first.')
    return watcher

def install_built_adapter(layout):
    artifacts = REPO/'.build'
    native, bridge = artifacts/'native/libd3dmetal-native.dylib', artifacts/'libwine-utm-bridge.dylib'
    if not native.is_file() or not bridge.is_file():
        raise Error('Build outputs missing; run ./scripts/build_adapter.sh before configure.')
    watcher=require_built_watcher()
    if not (layout.alvr/'ALVR Dashboard.exe').is_file():
        raise Error('Install ALVR before configuring the audio watcher.')
    layout.adapter.mkdir(parents=True, exist_ok=True)
    layout.backup([layout.adapter/native.name,layout.adapter/bridge.name,
                   layout.alvr/'AudioSceneWatcher.exe'], 'adapter')
    for source in (native,bridge):
        shutil.copy2(source, layout.adapter/source.name)
    # Build links use @loader_path/native; the installed layout keeps both libs together.
    installed = layout.adapter/bridge.name
    run(['install_name_tool','-rpath','@loader_path/native','@loader_path',installed])
    run(['codesign','--force','--sign','-',installed])
    shutil.copy2(watcher,layout.alvr/'AudioSceneWatcher.exe')
    info = layout.app/'Contents/Info.plist'
    value = plistlib.loads(info.read_bytes())
    block = '; '.join([
        'export DMN_WINE_SHARING=1', 'export DMN_WINE_SOCKET_DIR='+shlex.quote(str(layout.broker)),
        'mkdir -p "$DMN_WINE_SOCKET_DIR"', 'chmod 700 "$DMN_WINE_SOCKET_DIR"',
        'export CX_FWD_COMPAT_GL_CTX=1', 'export DMN_ALVR_ACTIVATION_WAIT5=0',
        'export DMN_ALVR_FINGER_GRIP_ONLY=1', 'export DMN_AUDIO_TAP=1',
        'export DMN_AUDIO_SOURCE_MODE=scene',
        'export WINESERVER='+shlex.quote(str(layout.engine/'bin/wineserver')),
        'export DMN_AUDIO_CAPTURE_BUFFER_MS=100', 'export DMN_AUDIO_DIAGNOSTICS=0',
        'export WINEESYNC=1', 'export WINEMSYNC=1', 'export DMN_LOG=info',
        'unset DMN_AUDIO_SOURCE_PID',
        'export ANDROID_ADB_SERVER_PORT='+str(layout.preferences()['adb_port']),
        'export DYLD_INSERT_LIBRARIES='+shlex.quote(str(installed)),
    ])
    value['CLI Custom Commands'] = block
    value['NSAudioCaptureUsageDescription'] = 'Stream the active SteamVR game audio to your own ALVR headset.'
    value['Program Name and Path'] = '/Program Files (x86)/Steam/Steam.exe'
    value['Program Flags'] = ''
    value['D3DMETAL'] = 1
    value['DXVK'] = 0
    value['WINEESYNC'] = 1
    value['WINEMSYNC'] = 1
    value['Skip Gecko'] = 1
    value['Skip Mono'] = 1
    info.write_bytes(plistlib.dumps(value, sort_keys=False))
    print('Installed graphics/audio adapters with retained library load path and ad-hoc signature.')

def game_flags(prefs, save=None):
    flags = ['-vr','-steam','-noasserts','-nopassiveasserts','-condebug','-dx11',
             '-nomultiview','-novid','+vr_fidelity_level_auto','0','+vr_fidelity_level','3',
             '+vr_msaa',str(prefs['msaa'])]
    return flags + (['+load',save] if save else ['+map','startup'])

def fix_vulkan(layout):
    assert_stopped(layout)
    file = layout.game/'hlvr/cfg/boot.vcfg'
    layout.backup([file], 'dx11')
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text('"boot"\n{\n\t"DefaultRenderSystemOption"\t\t"-dx11"\n'
                    '\t"RenderSystemOptionFlags"\t\t"0x0000000000000001"\n}\n')

def change_settings(layout, changes):
    layout.managed()
    assert_stopped(layout)
    prefs = layout.preferences()
    layout.backup([layout.prefs, layout.session], 'settings')
    prefs.update({key:value for key,value in changes.items() if value is not None})
    if not 1 <= prefs['encoder_threads'] <= 32: raise Error('Threads must be1..32')
    if not 1 <= prefs['bitrate_mbps'] <= 500: raise Error('Bitrate must be1..500Mbps')
    if prefs['packet_size'] not in (1400,16384): raise Error('Use1400 or16384 for this TCP-only tool')
    if not all(256 <= d <= 4096 and d % 32 == 0 for d in prefs['render_eye']):
        raise Error('Eye dimensions must be256..4096 and divisible by32')
    if layout.session.exists():
        session = json.loads(layout.session.read_text())
        apply_profile(session,prefs)
        session['session_settings']['audio']['game_audio']['enabled'] = True
        write_json(layout.session,session)
    write_json(layout.prefs,prefs)
    print(json.dumps(prefs,indent=2))
    print('Saved. MSAA/fidelity flags apply on the next game launch; refresh applies after VR/client restart.')
