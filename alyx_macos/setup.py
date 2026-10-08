"""Install into a new owned wrapper; downloads are not distributed by this repo."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

from .core import Error, REPO, assert_stopped, check_platform, download, extract, run, sha256, write_json
from .configuration import EXPECTED_ALVR_SHA

def install_wine(layout):
    check_platform()
    layout.create()
    if layout.wine.exists():
        print('Wine already installed in this managed wrapper. No overwrite.')
        return
    if layout.app.exists():
        raise Error('Partial wrapper exists. Keep it for diagnosis and use a new --root; no automatic deletion.')
    if subprocess.run(['arch','-x86_64','/usr/bin/true'],capture_output=True).returncode:
        raise Error('Rosetta missing. Install from Apple (softwareupdate --install-rosetta) and accept its license yourself.')
    template, engine = download(layout,'template'), download(layout,'wine')
    with tempfile.TemporaryDirectory(prefix='assemble-',dir=layout.root) as tmp:
        folder = Path(tmp)
        extract(template,folder/'template')
        extract(engine,folder/'engine')
        app = folder/'template/Template-1.0.11.app'
        shutil.move(str(folder/'engine/wswine.bundle'),app/'Contents/SharedSupport/wine')
        info = app/'Contents/Info.plist'
        value = plistlib.loads(info.read_bytes())
        value.update(D3DMETAL=1,DXVK=0)
        value['Program Name and Path'] = '/Program Files (x86)/Steam/Steam.exe'
        value['Program Flags'] = ''
        value['Skip Gecko'] = 1
        value['Skip Mono'] = 1
        value['CFBundleIdentifier'] = 'io.github.re-gor.alyx-macos'
        info.write_bytes(plistlib.dumps(value,sort_keys=False))
        shutil.move(str(app),layout.app)
    run([layout.app/'Contents/MacOS/launcher','WSS-wineprefixcreate'],env=layout.env(hooks=False),timeout=180)
    if not (layout.drive/'Program Files (x86)').is_dir():
        raise Error('Prefix not initialized. Inspect wrapper logs before proceeding.')
    write_json(layout.prefs,layout.preferences())
    print('Installed pinned Wine10.0_6/template1.0.11. No other wrapper was changed.')

def installer(layout,name):
    layout.managed()
    file = download(layout,name)
    print('Complete the official GUI installer yourself, including any license confirmation.')
    print('For Steam: use the default path and uncheck Run Steam on the final screen.')
    run([layout.app/'Contents/MacOS/launcher','WSS-installer',file],timeout=600)

def install_alvr(layout):
    layout.managed()
    assert_stopped(layout)
    archive = download(layout,'alvr')
    if layout.alvr.exists():
        driver = layout.alvr/'bin/win64/driver_alvr_server.dll'
        if driver.is_file() and sha256(driver)==EXPECTED_ALVR_SHA:
            print('Pinned ALVR already present; paired clients/settings preserved.')
            return
        raise Error('Existing ALVR differs from the pinned build; refuse overwrite.')
    with tempfile.TemporaryDirectory(prefix='alvr-',dir=layout.root) as tmp:
        extract(archive,tmp)
        stage = Path(tmp)
        if not (stage/'ALVR Dashboard.exe').is_file():
            raise Error('Unexpected ALVR archive layout')
        if sha256(stage/'bin/win64/driver_alvr_server.dll') != EXPECTED_ALVR_SHA:
            raise Error('Unexpected ALVR driver; memory/ABI hooks must not run on unknown binaries')
        shutil.move(str(stage),layout.alvr)
    template = json.loads((REPO/'config/alvr-session-template.json').read_text())
    write_json(layout.session,template)
    print('Installed standalone ALVR streamer. SteamVR registration is a separate command.')

def adb_path(layout, explicit=None, *, download_tools=False, accepted_license=False):
    if explicit:
        value = Path(explicit).expanduser().resolve()
        if not value.is_file(): raise Error('Specified adb does not exist')
        return value
    candidates = [layout.root/'tools/platform-tools/adb',
                  Path.home()/'Library/Android/sdk/platform-tools/adb']
    found = shutil.which('adb')
    if found: candidates.append(Path(found))
    for candidate in candidates:
        if candidate.is_file(): return candidate
    if not download_tools:
        raise Error('adb missing. Pass --adb PATH, or --download-adb --accept-platform-tools-license after reading Google SDK terms.')
    if not accepted_license:
        raise Error('Read https://developer.android.com/studio/terms and explicitly pass --accept-platform-tools-license.')
    tools = layout.root/'tools'
    extract(download(layout,'adb'),tools)
    candidate = tools/'platform-tools/adb'
    if not candidate.is_file(): raise Error('Unexpected Google platform-tools archive')
    return candidate

def adb_base(layout, explicit=None, serial=None, **options):
    adb = adb_path(layout,explicit,**options)
    port = layout.preferences()['adb_port']
    # No kill-server: a custom port does not justify stopping somebody else's adb.
    prefix = [str(adb),'-P',str(port)]
    result = run(prefix+['devices'],capture=True,timeout=10)
    devices = []
    for row in result.stdout.splitlines()[1:]:
        fields = row.split()
        if len(fields)>=2: devices.append((fields[0],fields[1]))
    serial = serial or layout.preferences().get('serial')
    if serial:
        state = dict(devices).get(serial)
        if state != 'device': raise Error(f'Headset is not ready ({state}); accept USB debugging inside the headset.')
    else:
        ready = [s for s,state in devices if state=='device']
        if len(ready)!=1: raise Error('Need exactly one authorized device, or pass --serial. Check USB debugging and cable.')
        serial = ready[0]
    return prefix+['-s',serial]

def install_headset(layout, *, adb=None, serial=None, download_adb=False, accepted_license=False):
    layout.managed()
    base = adb_base(layout,adb,serial,download_tools=download_adb,accepted_license=accepted_license)
    apk = download(layout,'headset')
    run(base+['install','-r',apk],timeout=180)
    run(base+['shell','am','start','-n','alvr.client.stable/android.app.NativeActivity'],timeout=10)
    prefs = layout.preferences()
    prefs['serial'] = base[-1]
    prefs['adb_path'] = base[0]
    write_json(layout.prefs,prefs)
    print('ALVR20.14.1 installed. Read its Hostname inside the headset and run pair --hostname VALUE.')

def pair(layout,hostname):
    layout.managed()
    assert_stopped(layout)
    if not hostname or len(hostname)>200 or any(c.isspace() for c in hostname):
        raise Error('Use the Hostname shown by ALVR, for example1234.client')
    if hostname=='wired.client':
        raise Error('Use the actual headset hostname; native Windows adb wired mode is intentionally bypassed.')
    session = json.loads(layout.session.read_text())
    layout.backup([layout.session,layout.prefs],'pair')
    session['client_connections'][hostname] = {'display_name':'Quest USB','current_ip':None,
        'manual_ips':['127.0.0.1'],'trusted':True,'connection_state':'Disconnected'}
    write_json(layout.session,session)
    prefs = layout.preferences();prefs['hostname']=hostname;write_json(layout.prefs,prefs)
    print('Paired only this loopback client for the USB tunnel.')

def build_adapter(layout, *, framework=None, tools_dir=None, jobs=2):
    if framework is None:
        framework = layout.app/'Contents/Frameworks/renderer/d3dmetal/external/D3DMetal.framework/Versions/A/D3DMetal'
    framework = Path(framework).expanduser().resolve()
    if not framework.is_file(): raise Error('D3DMetal framework missing; install-wine first or pass --framework PATH.')
    out = REPO/'.build';out.mkdir(exist_ok=True)
    if tools_dir:
        tools = Path(tools_dir).resolve()
    else:
        venv = REPO/'.venv'
        if not (venv/'bin/python3').is_file():run([sys.executable,'-m','venv',venv])
        run([venv/'bin/python3','-m','pip','install','-r',REPO/'requirements-build.txt'],timeout=300)
        tools = venv/'bin'
    env = os.environ.copy();env['PATH']=str(tools)+os.pathsep+env.get('PATH','')
    src = REPO/'src/d3dmetal-native';native = out/'native'
    command = [tools/'meson','setup']
    if (native/'meson-private/coredata.dat').exists():command.append('--reconfigure')
    command += [native,src,'--cross-file',src/'build-macos-x86_64.txt',
                '--buildtype=debugoptimized','-Dtests=disabled','-Ddev_framework_path='+str(framework)]
    run(command,env=env,timeout=120)
    run([tools/'meson','compile','-C',native,'-j',str(jobs)],env=env,timeout=600)
    bridge = REPO/'src/bridge'
    obj = out/'process_tap_native.o'
    run(['clang++','-arch','x86_64','-std=c++17','-mmacosx-version-min=14.0','-fobjc-arc',
         '-c',bridge/'audio/process_tap_native.mm','-o',obj],timeout=120)
    sources = ['wine_bridge.cpp','alvr_error_bridge.cpp','alvr_frame_trace.cpp',
        'alvr_activation_bridge.cpp','alvr_finger_bridge.cpp','compositor_frame_trace.cpp',
        'client_frame_trace.cpp','wine_audio_bridge.cpp','wine_scene_source.cpp','audio/process_tap.cpp']
    run(['clang++','-arch','x86_64','-std=c++17','-mmacosx-version-min=14.0','-dynamiclib',
         *[bridge/s for s in sources],obj,'-framework','CoreAudio','-framework','Foundation',
         '-L'+str(native),'-ld3dmetal-native','-Wl,-rpath,@loader_path/native',
         '-install_name','@rpath/libwine-utm-bridge.dylib','-o',out/'bridge-x86_64.dylib'],timeout=180)
    run(['clang','-arch','arm64','-mmacosx-version-min=14.0','-dynamiclib',bridge/'native_helper_noop.c',
         '-Wl,-rpath,@loader_path/native','-install_name','@rpath/libwine-utm-bridge.dylib',
         '-o',out/'bridge-arm64.dylib'])
    run(['lipo','-create',out/'bridge-x86_64.dylib',out/'bridge-arm64.dylib',
         '-output',out/'libwine-utm-bridge.dylib'])
    run(['codesign','--force','--sign','-',out/'libwine-utm-bridge.dylib'])
    run([sys.executable,REPO/'src/scene/build_watcher.py'],timeout=120)
    run(['clang++','-arch','x86_64','-std=c++17','-mmacosx-version-min=14.0',
         REPO/'src/probes/source_ready_probe.cpp','-framework','CoreAudio','-framework','Foundation',
         '-o',out/'source_ready_probe'],timeout=120)
    print('Built adapters only; no Wine, SteamVR, capture, headset or GPU tests were started.')
