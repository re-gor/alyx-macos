"""Scoped starts/stops, USB setup and automatic active-scene audio."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

from .core import Error, PORT, REPO, owned_pids, process_prefix, run, wait_pid, write_json
from .setup import adb_base
from .configuration import EXPECTED_ALVR_SHA, EXPECTED_WATCHER_SHA, game_flags
from .alvr_api import EventSocket, post, session_flags

def stop(layout):
    layout.managed()
    if not layout.wine.exists(): return
    env = layout.env(hooks=False)
    result = run([layout.engine/'bin/wineserver','-k'],env=env,check=False,timeout=20)
    if result.returncode not in (0,1): raise Error('Scoped wineserver stop failed')
    deadline = time.monotonic()+5
    while owned_pids(layout) and time.monotonic()<deadline: time.sleep(.2)
    remaining = owned_pids(layout)
    if remaining:
        # Only verified same-prefix survivors; no global process-name killing.
        for pid in remaining:
            if process_prefix(pid) != layout.prefix.resolve():
                raise Error('Ownership changed; refusing orphan termination')
            os.kill(pid,signal.SIGTERM)
        time.sleep(.5)
    if owned_pids(layout): raise Error('Owned processes remain; inspect them before changing adapters')
    if layout.app.exists(): (layout.broker/'audio-source.pid').unlink(missing_ok=True)
    print('Stopped only this managed Wine prefix. adb and other wrappers remain untouched.')

def require_no_other_vr(layout):
    result = subprocess.run(['ps','-axo','pid=,comm='],capture_output=True,text=True,check=True)
    for row in result.stdout.splitlines():
        parts=row.strip().split(None,1)
        if len(parts)!=2: continue
        if not any(parts[1].lower().endswith(n) for n in ('vrserver.exe','vrcompositor.exe')):continue
        prefix = process_prefix(int(parts[0]))
        if prefix is not None and prefix != layout.prefix.resolve():
            raise Error('Another VR Wine prefix is running. Close it yourself before using the same headset; it will not be stopped.')

def start_steam(layout):
    layout.managed()
    if not (layout.steam/'Steam.exe').is_file():raise Error('Windows Steam missing; run install-steam.')
    found=owned_pids(layout,'Steam.exe')
    if len(found)==1:return found[0]
    if len(found)>1:raise Error('Multiple Steam processes in this prefix')
    run(['open','-n',layout.app],env=layout.env(hooks=bool((layout.adapter/'libwine-utm-bridge.dylib').is_file())))
    return wait_pid(layout,'Steam.exe',60)

def ensure_steam_stable(layout, stable_seconds=15, timeout=120):
    """Do not start Alyx after a transient Steam process has already exited."""
    deadline=time.monotonic()+timeout
    previous=None;since=None;attempts=0
    while time.monotonic()<deadline:
        pids=owned_pids(layout,'Steam.exe')
        if len(pids)>1:raise Error('Multiple owned Steam instances; resolve before game start')
        if not pids:
            previous=None;since=None;attempts+=1
            if attempts>3:raise Error('Steam repeatedly exits; game/audio bootstrap not attempted')
            start_steam(layout)
        else:
            pid=pids[0]
            if pid!=previous:previous=pid;since=time.monotonic()
            elif time.monotonic()-since>=stable_seconds:return pid
        time.sleep(.5)
    raise Error('Steam never stayed ready; inspect its window/logs before retrying')

def steam_install(layout,appid):
    start_steam(layout)
    run([layout.wine,layout.steam/'Steam.exe',f'steam://install/{appid}'],
        env=layout.env(hooks=False),timeout=30)
    print('Finish Steam account/license/download dialogs yourself. Downloads are not bundled.')

def spawn_wine(layout,exe,args=(),label='program', *, hooks=True):
    layout.logs.mkdir(exist_ok=True)
    env=layout.env(hooks=hooks)
    if exe.name.lower()=='hlvr.exe':env.update(SteamAppId='546560',SteamGameId='546560')
    with (layout.logs/(label+'.log')).open('wb') as out:
        child=subprocess.Popen([str(layout.wine),str(exe),*[str(x) for x in args]],
            cwd=exe.parent,env=env,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
    return child

def assert_api_owner(layout):
    result=subprocess.run(['lsof','-nP','-t',f'-iTCP:{PORT}','-sTCP:LISTEN'],
                          capture_output=True,text=True,timeout=5)
    pids={int(x) for x in result.stdout.split() if x.isdigit()}
    if not pids or any(process_prefix(pid)!=layout.prefix.resolve() for pid in pids):
        raise Error('ALVR API is absent or belongs to another prefix; refusing request.')

def api_session(layout):
    assert_api_owner(layout)
    ws=EventSocket(PORT)
    try:
        session_flags(ws,PORT)
        return ws.last_session
    finally:ws.close()

def set_audio(layout,enabled):
    assert_api_owner(layout)
    ws=EventSocket(PORT)
    try:
        post({'SetValues':[{'path':[{'Name':n} for n in
             ('session_settings','audio','game_audio','enabled')],'value':enabled}]},PORT)
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            session_flags(ws,PORT)
            if ws.last_session['session_settings']['audio']['game_audio']['enabled'] is enabled:return
        raise Error('Could not verify GameAudio setting')
    finally:ws.close()

def dashboard(layout):
    layout.managed()
    exe=layout.alvr/'ALVR Dashboard.exe'
    if not exe.is_file():raise Error('ALVR missing; run install-alvr.')
    if not owned_pids(layout,'ALVR Dashboard.exe'):
        spawn_wine(layout,exe,label='alvr-dashboard')
    print('ALVR Dashboard opened directly. Use Settings; do not run the installer again.')

def open_settings(layout):
    dashboard(layout)
    print(f'Alternatively, when the owned server is running: http://127.0.0.1:{PORT}/')

def register_driver(layout):
    layout.managed()
    exe=layout.steamvr/'bin/win64/vrpathreg.exe'
    if not exe.is_file():raise Error('SteamVR missing. Install Steam app250820 first.')
    driver=layout.alvr/'bin/win64/driver_alvr_server.dll'
    from .core import sha256
    if not driver.is_file() or sha256(driver)!=EXPECTED_ALVR_SHA:
        raise Error('Expected pinned ALVR20.14.1 driver; refusing unknown driver')
    env=layout.env(hooks=False)
    run([layout.wine,exe,'setruntime',r'C:\Program Files (x86)\Steam\steamapps\common\SteamVR'],env=env)
    run([layout.wine,exe,'adddriver',r'C:\ALVR'],env=env)
    run([layout.wine,exe,'show'],env=env)
    print('Registered ALVR in this Wine prefix. This is not a Steam-store ALVR installation.')

def connect_headset(layout,serial=None):
    prefs=layout.preferences()
    if not prefs.get('hostname'):raise Error('Pair the ALVR Hostname first: pair --hostname1234.client')
    base=adb_base(layout,prefs.get('adb_path'),serial)
    for port in (9943,9944):run(base+['forward',f'tcp:{port}',f'tcp:{port}'],timeout=10)
    run(base+['shell','am','force-stop','alvr.client.stable'],timeout=10)
    run(base+['shell','am','start','-n','alvr.client.stable/android.app.NativeActivity'],timeout=10)
    print('USB tunnel connected. Wear the headset and keep ALVR in the foreground.')

def launch(layout, *, save=None, with_audio=True, serial=None):
    layout.managed()
    require_no_other_vr(layout)
    game=layout.game/'bin/win64/hlvr.exe'
    for file in (game,layout.steamvr/'bin/win64/vrserver.exe',layout.steamvr/'bin/win64/vrcompositor.exe'):
        if not file.is_file():raise Error('Required Steam game/runtime file missing: '+str(file))
    if save and (not re.fullmatch(r's[0-9]+/[A-Za-z0-9_-]+',save)
                 or not (layout.game/'hlvr/save'/(save+'.sav')).is_file()):
        raise Error('Use an existing save, e.g. s0/quick; arbitrary console commands are rejected.')
    crt=layout.prefix/'drive_c/windows/system32/msvcp140.dll'
    if not crt.is_file():
        raise Error('x64 MSVC runtime missing; run install-vc-runtime and finish its GUI installer.')
    header=crt.read_bytes()[:512]
    if b'Wine builtin DLL' in header or b'Wine built-in DLL' in header:
        raise Error('MSVCP140 is still Wine builtin; install the official Microsoft x64 v14 runtime first.')
    from .core import sha256
    watcher=layout.alvr/'AudioSceneWatcher.exe'
    if with_audio and (not watcher.is_file() or sha256(watcher)!=EXPECTED_WATCHER_SHA):
        raise Error('Automatic audio watcher missing or changed. Rebuild/configure while stopped.')
    stop(layout)
    session=json.loads(layout.session.read_text())
    session['session_settings']['audio']['game_audio']['enabled']=with_audio
    write_json(layout.session,session)
    start_steam(layout)
    print('Allowing Steam startup30s before the VR driver to avoid the observed startup race.',flush=True)
    time.sleep(30)
    spawn_wine(layout,layout.alvr/'ALVR Dashboard.exe',label='alvr-dashboard')
    spawn_wine(layout,layout.steamvr/'bin/win64/vrserver.exe',['-keepalive'],'vrserver')
    wait_pid(layout,'vrserver.exe',45)
    spawn_wine(layout,layout.steamvr/'bin/win64/vrcompositor.exe',label='vrcompositor')
    wait_pid(layout,'vrcompositor.exe',45)
    assert_api_owner(layout)
    ensure_steam_stable(layout)
    spawn_wine(layout,game,game_flags(layout.preferences(),save),'alyx')
    pid=wait_pid(layout,'hlvr.exe',60)
    print('Owned Alyx process:',pid,flush=True)
    # The Wine callbacks launch the observer and retarget the private tap.
    # Do not select/cache a game PID or toggle audio on every scene change.
    connect_headset(layout,serial)
    print('Started. Approve any macOS audio-capture request yourself. Success requires image, hands and audible sound inside the headset.')

def source_status(layout):
    servers=owned_pids(layout,'vrserver.exe')
    if len(servers)!=1:raise Error('Exactly one running owned vrserver is needed')
    file=layout.broker/f'active-scene-{servers[0]}.state'
    if not file.is_file():
        print('Scene observer has not published yet. Check Game audio, active Quest and adapter guards.')
        return
    if file.is_symlink() or file.stat().st_uid!=os.getuid() or file.stat().st_size>512:
        raise Error('Unexpected audio-state file; refusing read')
    values=dict(re.findall(r'\b(version|wine_pid|native_pid|start_time|generation|ready|observed_time)=(\d+)\b',file.read_text()))
    if set(values)!={'version','wine_pid','native_pid','start_time','generation','ready','observed_time'}:
        raise Error('Invalid scene observer state')
    state={k:int(v) for k,v in values.items()}
    state['observation_age_s']=(time.time()+11644473600)-state['observed_time']/10000000
    print(json.dumps(state,indent=2))
    print('ready proves process mapping only, not audible output. Old observations can be stale.')

def stats(layout,seconds=30):
    assert_api_owner(layout)
    if not 0 < seconds <=60:raise Error('Statistics sample must be1..60 seconds')
    ws=EventSocket(PORT);graphs=[];logs=[];summaries=[]
    try:
        session_flags(ws,PORT)
        session=ws.last_session
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            e=ws.event(max(.01,deadline-time.monotonic()))
            if not e:continue
            t=e.get('event_type',{})
            if t.get('id')=='GraphStatistics':graphs.append(t['data'])
            elif t.get('id')=='Log':logs.append(t['data'])
            elif t.get('id')=='StatisticsSummary':summaries.append(t['data'])
    finally:ws.close()
    values=sorted(g['total_pipeline_latency_s']*1000 for g in graphs)
    counters=[s['video_packets_total'] for s in summaries]
    reset=any(b<a for a,b in zip(counters,counters[1:]))
    result={'seconds':seconds,'new_video_reports_per_second':len(graphs)/seconds,
            'counter_reset':reset,'median_estimated_video_latency_ms':values[len(values)//2] if values else None,
            'p95_estimated_video_latency_ms':values[round((len(values)-1)*.95)] if values else None,
            'logs':logs,'note':'Fresh decoder reports, not game GPU FPS. Latency is ALVR estimated, not physical motion-to-photon.'}
    print(json.dumps(result,indent=2))
    write_json(layout.logs/'last-statistics.json',result)
    return result
