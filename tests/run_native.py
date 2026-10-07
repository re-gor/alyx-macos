"""Fake audio tests only. No real HAL/capture, Wine start, or GPU test."""
import argparse
from pathlib import Path
import platform
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--wine-coreaudio',type=Path,help='Read-only pinned winecoreaudio.so fixture for guarded bridge tests')
args=p.parse_args()
if platform.system()!='Darwin':raise SystemExit('Native fake tests require macOS; Python tests remain portable.')
out=ROOT/'.build/offline-tests';out.mkdir(parents=True,exist_ok=True)
bridge=ROOT/'src/bridge'
def run(cmd):
    print('+ '+' '.join(map(str,cmd)),flush=True)
    subprocess.run(list(map(str,cmd)),check=True,timeout=120)
for arch in ('arm64','x86_64'):
    common=['clang++','-arch',arch,'-std=c++17','-O2','-mmacosx-version-min=14.0']
    fake=out/('audio-fake-'+arch)
    run(common+[bridge/'audio/process_tap.cpp',bridge/'audio/process_tap_tests.cpp','-o',fake])
    run([fake])
    if args.wine_coreaudio:
        fixture=args.wine_coreaudio.expanduser().resolve()
        if not fixture.is_file():raise SystemExit('Read-only Wine fixture does not exist')
        test=out/('wine-audio-fake-'+arch)
        run(common+[bridge/'wine_audio_bridge_test.cpp','-framework','CoreAudio','-framework','Foundation','-o',test])
        run([test,fixture])
if not args.wine_coreaudio:print('Wine exact-binary guard tests skipped: no fixture supplied.')
print('Offline fake tests passed; no headset, game, HAL, recording or GPU was started.')
