"""Create a Finder launcher without starting it or interacting with the desktop."""
import plistlib
import shlex
import sys
from .core import Error, REPO, run

def create(layout):
    layout.managed()
    app=layout.root/'Alyx VR.app'
    if app.exists():raise Error('Launcher already exists; move it aside before regenerating.')
    folder=app/'Contents/MacOS';folder.mkdir(parents=True)
    layout.logs.mkdir(exist_ok=True)
    executable=folder/'launch'
    command=shlex.join([sys.executable,str(REPO/'scripts/alyx_macos.py'),'launch','--root',str(layout.root)])
    executable.write_text('#!/bin/sh\nexec '+command+' >> '+shlex.quote(str(layout.logs/'gui-launch.log'))+' 2>&1\n')
    executable.chmod(0o755)
    info={'CFBundleName':'Alyx VR','CFBundleDisplayName':'Alyx VR',
          'CFBundleIdentifier':'io.github.re-gor.alyx-macos.launcher',
          'CFBundleExecutable':'launch','CFBundlePackageType':'APPL',
          'CFBundleShortVersionString':'0.1.0',
          'NSAudioCaptureUsageDescription':'Stream your Alyx game audio to your own ALVR headset.'}
    (app/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    run(['codesign','--force','--sign','-',app],timeout=30)
    print('Created Finder launcher:',app)
    print('It starts the full cold bootstrap; GUI/game/audio acceptance is still unvalidated. Keep the repo/Python paths in place.')
