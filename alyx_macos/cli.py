"""Command line entry point; installers and starts always print their commands."""
import argparse
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from .core import DEFAULT_ROOT, Error, Layout, PORT, REPO, assert_stopped, run, sha256
from .configuration import EXPECTED_ALVR_SHA, change_settings, configure, fix_vulkan
from . import runtime, setup

COMMANDS = {
    'install-wine':'Download verified Sikarugir template/engine and create a new private prefix.',
    'install-steam':'Download official Windows Steam; run its interactive GUI installer.',
    'install-vc-runtime':'Download official Microsoft x64 v14 runtime; finish its GUI/EULA yourself.',
    'install-steamvr':'Ask owned Windows Steam to install Steam app250820.',
    'install-alyx':'Ask owned Windows Steam to install purchased Steam app546560.',
    'install-alvr':'Download verified ALVR20.14.1 and create its portable session.',
    'install-headset':'Install matching ALVR APK on one explicitly selected authorized Android headset.',
    'pair':'Trust the ALVR Hostname shown in the headset for the localhost USB tunnel.',
    'build-adapter':'Compile the source; do not run or install the adapters.',
    'configure':'Back up and apply the tested settings, native CRT override and built adapters.',
    'register-driver':'Register C:\\ALVR with this prefix\'s SteamVR.',
    'add-alvr-shortcut':'Back up binary shortcuts.vdf and add ALVR as a non-Steam game in this Windows Steam.',
    'make-gui-launcher':'Create a local Alyx VR.app for the full bootstrap; do not launch it.',
    'steam':'Open owned Windows Steam.',
    'alvr':'Open ALVR Dashboard.exe directly, without the installer.',
    'alvr-settings':'Open ALVR Dashboard for its Settings tab; print the localhost web URL.',
    'connect-headset':'Create TCP adb forwards and restart only the selected headset ALVR client.',
    'launch':'Cold-start this prefix, SteamVR, Alyx, its audio source and the USB client.',
    'stop':'Stop only the managed Wine prefix; never kill another wrapper or adb server.',
    'settings':'Show or change supported fields; cold restart is explicit.',
    'fix-vulkan':'Restore the DX11 boot choice with backup while the prefix is stopped.',
    'audio-status':'Read metadata for the running owned Alyx source; no audio recording.',
    'stats':'Read up to60s of ALVR events; report fresh-video cadence/estimated latency.',
    'doctor':'Read local installation checks; never start Wine, adb, SteamVR or recording.',
}

def doctor(layout):
    result={'root':str(layout.root),'managed':layout.marker.is_file(),
            'platform':platform.system(),'architecture':platform.machine(),
            'python':sys.version.split()[0], 'clang':shutil.which('clang++'),
            'wine_installed':layout.wine.is_file(),
            'steam_installed':(layout.steam/'Steam.exe').is_file(),
            'steamvr_installed':(layout.steamvr/'bin/win64/vrserver.exe').is_file(),
            'alyx_installed':(layout.game/'bin/win64/hlvr.exe').is_file(),
            'adapter_installed':(layout.adapter/'libwine-utm-bridge.dylib').is_file(),
            'alvr_web_port':PORT,'tested_steamvr':'2.17.10 (newer builds not validated)',
            'new_machine_install_tested':False}
    driver=layout.alvr/'bin/win64/driver_alvr_server.dll'
    result['alvr_driver_matches_pinned_build']=driver.is_file() and sha256(driver)==EXPECTED_ALVR_SHA
    if layout.prefs.exists():result['settings']=layout.preferences()
    print(json.dumps(result,indent=2))

def parser():
    p=argparse.ArgumentParser(description='Vibe-coded experimental Alyx on Apple Silicon. MIT; no warranty.')
    subs=p.add_subparsers(dest='command',required=True)
    for name,help in COMMANDS.items():
        sub=subs.add_parser(name,help=help,description=help)
        sub.add_argument('--root',type=Path,default=DEFAULT_ROOT,
                         help='Private installation folder; never use an existing unrelated wrapper')
        sub.add_argument('--dry-run',action='store_true',help='Print a plan; no downloads, writes or processes')
        if name in ('install-headset','connect-headset','launch'):
            sub.add_argument('--serial',help='Exact adb device serial if more than one is connected')
        if name=='install-headset':
            sub.add_argument('--adb',type=Path)
            sub.add_argument('--download-adb',action='store_true')
            sub.add_argument('--accept-platform-tools-license',action='store_true')
        if name=='pair':sub.add_argument('--hostname',required=True)
        if name=='add-alvr-shortcut':sub.add_argument('--steam-userid',help='Numeric Steam userdata folder when several accounts exist')
        if name=='build-adapter':
            sub.add_argument('--framework',type=Path,help='Existing Apple D3DMetal binary; read only')
            sub.add_argument('--tools-dir',type=Path,help='Existing Meson/Ninja bin folder instead of a new venv')
            sub.add_argument('--jobs',type=int,choices=range(1,17),default=2)
        if name=='launch':
            sub.add_argument('--save',help='Existing save, e.g. s0/quick; otherwise start the menu')
            sub.add_argument('--no-audio',action='store_true')
        if name=='stats':sub.add_argument('--seconds',type=int,choices=range(1,61),default=30)
        if name=='settings':
            sub.add_argument('--profile',choices=('current','performance90'))
            sub.add_argument('--msaa',type=int,choices=(2,4))
            sub.add_argument('--hz',type=int,choices=(72,90))
            sub.add_argument('--bitrate',type=int)
            sub.add_argument('--threads',type=int)
            sub.add_argument('--packet-size',type=int,choices=(1400,16384))
            sub.add_argument('--pacing',choices=('on','off'))
            sub.add_argument('--ffr',choices=('on','off'))
            sub.add_argument('--eye',type=int,nargs=2,metavar=('WIDTH','HEIGHT'))
            sub.add_argument('--restart',action='store_true',help='Stop and relaunch only this prefix to apply fields')
    return p

def main(argv=None):
    args=parser().parse_args(argv)
    layout=Layout(args.root)
    if args.dry_run:
        print(json.dumps({'command':args.command,'installation_root':str(layout.root),
              'app':str(layout.app),'plan':COMMANDS[args.command],
              'arguments':{k:str(v) if isinstance(v,Path) else v for k,v in vars(args).items()},
              'action_performed':False},indent=2))
        return 0
    try:
        name=args.command
        if name=='doctor':doctor(layout)
        elif name=='install-wine':setup.install_wine(layout)
        elif name=='install-steam':setup.installer(layout,'steam')
        elif name=='install-vc-runtime':setup.installer(layout,'vc-runtime')
        elif name=='install-steamvr':runtime.steam_install(layout,250820)
        elif name=='install-alyx':runtime.steam_install(layout,546560)
        elif name=='install-alvr':setup.install_alvr(layout)
        elif name=='install-headset':setup.install_headset(layout,adb=args.adb,serial=args.serial,
                download_adb=args.download_adb,accepted_license=args.accept_platform_tools_license)
        elif name=='pair':setup.pair(layout,args.hostname)
        elif name=='build-adapter':setup.build_adapter(layout,framework=args.framework,
                tools_dir=args.tools_dir,jobs=args.jobs)
        elif name=='configure':configure(layout)
        elif name=='register-driver':runtime.register_driver(layout)
        elif name=='add-alvr-shortcut':
            from .steam_shortcut import install
            install(layout,args.steam_userid)
        elif name=='make-gui-launcher':
            from .gui import create
            create(layout)
        elif name=='steam':runtime.start_steam(layout)
        elif name=='alvr':runtime.dashboard(layout)
        elif name=='alvr-settings':runtime.open_settings(layout)
        elif name=='connect-headset':runtime.connect_headset(layout,args.serial)
        elif name=='launch':runtime.launch(layout,save=args.save,with_audio=not args.no_audio,serial=args.serial)
        elif name=='stop':runtime.stop(layout)
        elif name=='fix-vulkan':fix_vulkan(layout.managed())
        elif name=='audio-status':runtime.source_status(layout.managed())
        elif name=='stats':runtime.stats(layout.managed(),args.seconds)
        elif name=='settings':
            mapping={'msaa':args.msaa,'refresh_hz':args.hz,'bitrate_mbps':args.bitrate,
                'encoder_threads':args.threads,'packet_size':args.packet_size,'render_eye':args.eye,
                'pacing':None if args.pacing is None else args.pacing=='on',
                'foveated_encoding':None if args.ffr is None else args.ffr=='on'}
            if args.profile:
                mapping={**json.loads((REPO/'config/profiles.json').read_text())[args.profile],
                         **{k:v for k,v in mapping.items() if v is not None}}
            if not any(v is not None for v in mapping.values()):
                print(json.dumps(layout.preferences(),indent=2));return 0
            if args.restart:runtime.stop(layout)
            change_settings(layout,mapping)
            if args.restart:runtime.launch(layout)
        return 0
    except (Error,subprocess.SubprocessError,OSError,TimeoutError,ValueError) as error:
        print('ERROR: '+str(error),file=sys.stderr)
        return 1

if __name__=='__main__':raise SystemExit(main())
