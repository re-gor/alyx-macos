"""Paths, downloads and process ownership for this project's private wrapper."""
from __future__ import annotations
import ctypes
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import shlex
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parent.parent
DEFAULT_ROOT = Path.home() / 'wine/alyx-macos'
PORT = 8083

class Error(RuntimeError):
    pass

def run(args, *, env=None, cwd=None, timeout=120, capture=False, check=True):
    args = [str(x) for x in args]
    print('+ ' + shlex.join(args), flush=True)
    return subprocess.run(args, env=env, cwd=cwd, timeout=timeout, check=check,
                          text=True, capture_output=capture)

def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

def check_platform():
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise Error('This experimental installer targets Apple Silicon macOS only.')
    version = subprocess.check_output(['sw_vers', '-productVersion'], text=True).strip()
    if int(version.split('.')[0]) < 15:
        raise Error('Use macOS 15+. The end-to-end reference machine ran macOS15.8.')

def safe_member(name):
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or '\\' in name:
        raise Error('Unsafe archive path: ' + name)

def extract(archive, destination):
    """Reject escapes and special files; keep trusted internal framework symlinks."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                safe_member(member.filename)
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise Error('Unexpected symlink in ZIP: ' + member.filename)
            z.extractall(destination)
            for member in z.infolist():
                path = destination / member.filename
                mode = (member.external_attr >> 16) & 0o777
                if mode and path.is_file():
                    path.chmod(mode)
    else:
        with tarfile.open(archive) as t:
            for member in t.getmembers():
                safe_member(member.name)
                if member.isdev() or member.isfifo():
                    raise Error('Special file in archive: ' + member.name)
                if member.issym() or member.islnk():
                    link = PurePosixPath(member.linkname)
                    if link.is_absolute():
                        raise Error('External archive symlink: ' + member.name)
                    parent = (destination / member.name).parent if member.issym() else destination
                    target = (parent / member.linkname).resolve()
                    if not target.is_relative_to(destination.resolve()):
                        raise Error('Archive link escapes destination: ' + member.name)
            # Symlinks are checked before extraction. Python3.9 has no filter API.
            if hasattr(tarfile, 'data_filter'):
                t.extractall(destination, filter='data')
            else:
                t.extractall(destination)

@dataclass
class Layout:
    root: Path
    def __post_init__(self):
        self.root = self.root.expanduser().resolve()
    @property
    def marker(self): return self.root / '.alyx-macos.json'
    @property
    def app(self): return self.root / 'SteamVR.app'
    @property
    def prefix(self): return self.app / 'Contents/SharedSupport/prefix'
    @property
    def engine(self): return self.app / 'Contents/SharedSupport/wine'
    @property
    def wine(self): return self.engine / 'bin/wine'
    @property
    def drive(self): return self.app / 'Contents/drive_c'
    @property
    def steam(self): return self.drive / 'Program Files (x86)/Steam'
    @property
    def steamvr(self): return self.steam / 'steamapps/common/SteamVR'
    @property
    def game(self): return self.steam / 'steamapps/common/Half-Life Alyx/game'
    @property
    def alvr(self): return self.drive / 'ALVR'
    @property
    def session(self): return self.alvr / 'session.json'
    @property
    def adapter(self): return self.app / 'Contents/Frameworks/UTMWineAdapter'
    @property
    def prefs(self): return self.root / 'settings.json'
    @property
    def logs(self): return self.root / 'logs'
    @property
    def broker(self): return Path('/private/tmp') / f'utm-wine-{os.getuid()}-{self.app.stat().st_ino}'

    def managed(self):
        if not self.marker.is_file():
            raise Error(f'Not a managed alyx-macos installation: {self.root}. Run install-wine first.')
        if json.loads(self.marker.read_text()).get('project') != 'alyx-macos':
            raise Error('Invalid installation marker; refusing operation.')
        if self.marker.stat().st_uid != os.getuid():
            raise Error('Installation belongs to another user; refusing operation.')
        for path in (self.app,self.prefix,self.engine):
            if path.exists() and not path.resolve().is_relative_to(self.root):
                raise Error('Managed path points outside the installation; refusing operation.')
        return self

    def create(self):
        if self.root.exists() and any(self.root.iterdir()) and not self.marker.exists():
            raise Error('Destination is not empty and is not managed; choose a new --root.')
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        if not self.marker.exists():
            write_json(self.marker, {'project':'alyx-macos', 'schema':1,
                       'created_at':datetime.now(timezone.utc).isoformat()})
        self.logs.mkdir(exist_ok=True)

    def backup(self, paths, label):
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        folder = self.root / 'backups' / (label + '-' + stamp)
        folder.mkdir(parents=True, exist_ok=False)
        index = {}
        for path in paths:
            path = Path(path)
            if path.is_file():
                key = f'{len(index)}-{path.name}'
                shutil.copy2(path, folder / key)
                index[key] = str(path.relative_to(self.root))
        write_json(folder / 'index.json', index)
        return folder

    def env(self, *, hooks=True):
        self.managed()
        base = self.app / 'Contents'
        metal = base / 'Frameworks/renderer/d3dmetal'
        env = {k:v for k,v in os.environ.items() if not k.startswith('DMN_')}
        env.update(WINEPREFIX=str(self.prefix), WINEARCH='win64',
            WINESERVER=str(self.engine / 'bin/wineserver'), WINELOADER=str(self.wine),
            WINEESYNC='1', WINEMSYNC='1', WINEDEBUG='-all',
            WINEDLLPATH_PREPEND=str(metal / 'wine'),
            WINEDLLPATH=str(metal / 'wine') + ':' + str(self.engine / 'lib/wine'),
            WINEDLLOVERRIDES='mscoree,mshtml=d;d3d11,dxgi=b',
            CX_D3DMETALPATH=str(metal), CX_FWD_COMPAT_GL_CTX='1',
            DYLD_FALLBACK_LIBRARY_PATH=':'.join(map(str, [base/'Frameworks', metal/'external',
                self.engine/'lib/wine/x86_64-unix',
                base/'Frameworks/GStreamer.framework/Versions/Current/lib'])) + ':/usr/lib',
            DYLD_FRAMEWORK_PATH=str(metal/'external') + ':' + str(base/'Frameworks'),
            ANDROID_ADB_SERVER_PORT=str(self.preferences()['adb_port']))
        env.pop('DYLD_INSERT_LIBRARIES', None)
        if hooks:
            bridge = self.adapter / 'libwine-utm-bridge.dylib'
            if not bridge.is_file():
                raise Error('Adapter not installed. Run build-adapter and configure first.')
            self.broker.mkdir(mode=0o700, exist_ok=True)
            self.broker.chmod(0o700)
            env.update(DMN_WINE_SHARING='1', DMN_WINE_SOCKET_DIR=str(self.broker),
                DYLD_INSERT_LIBRARIES=str(bridge), DMN_LOG='info',
                DMN_ALVR_ACTIVATION_WAIT5='0', DMN_ALVR_FINGER_GRIP_ONLY='1',
                DMN_AUDIO_TAP='1', DMN_AUDIO_SOURCE_MODE='scene',
                DMN_AUDIO_CAPTURE_BUFFER_MS='100', DMN_AUDIO_DIAGNOSTICS='0')
            env.pop('DMN_AUDIO_SOURCE_PID', None)
        return env

    def preferences(self):
        profiles = json.loads((REPO/'config/profiles.json').read_text())
        value = dict(profiles['current'], adb_port=5038, hostname=None, serial=None)
        if self.prefs.exists(): value.update(json.loads(self.prefs.read_text()))
        return value

def download(layout, name):
    artifact = json.loads((REPO / 'config/downloads.json').read_text())[name]
    url, digest = artifact['url'], artifact['sha256']
    cache = layout.root / 'downloads'
    cache.mkdir(exist_ok=True)
    target = cache / url.rsplit('/', 1)[-1]
    if target.is_file() and (digest is None or sha256(target) == digest):
        return target
    temporary = target.with_suffix(target.suffix + '.part')
    request = urllib.request.Request(url, headers={'User-Agent':'alyx-macos/0.1'})
    print('Download official vendor artifact:', url, flush=True)
    try:
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open('wb') as out:
            if not response.url.startswith('https://'):
                raise Error('Refusing a non-HTTPS download redirect')
            shutil.copyfileobj(response, out, 1024*1024)
        actual = sha256(temporary)
        if digest and actual != digest:
            raise Error(f'Checksum mismatch for {name}: expected {digest}, got {actual}')
        temporary.replace(target)
        write_json(cache/(target.name+'.receipt.json'),
                   {'url':url,'sha256':actual,'pinned':digest is not None})
        return target
    finally:
        temporary.unlink(missing_ok=True)

def process_prefix(pid):
    """Read only one candidate's argv/env; never print other process environments."""
    if platform.system() != 'Darwin': return None
    library = ctypes.CDLL(None, use_errno=True)
    mib = (ctypes.c_int * 3)(1, 49, int(pid)) # CTL_KERN/KERN_PROCARGS2
    buf = ctypes.create_string_buffer(65536)
    size = ctypes.c_size_t(len(buf))
    if library.sysctl(mib, 3, buf, ctypes.byref(size), None, 0): return None
    data = buf.raw[:size.value]
    if len(data) < 4: return None
    argc = int.from_bytes(data[:4], sys_byteorder())
    if not 0 < argc < 4096: return None
    pos = data.find(b'\0', 4)
    if pos < 0: return None
    while pos < len(data) and data[pos] == 0: pos += 1
    for _ in range(argc):
        pos = data.find(b'\0', pos)
        if pos < 0: return None
        pos += 1
    for item in data[pos:].split(b'\0'):
        if item.startswith(b'WINEPREFIX='):
            return Path(os.fsdecode(item[len(b'WINEPREFIX='):])).resolve()
    return None

def sys_byteorder():
    import sys
    return sys.byteorder

def owned_pids(layout, executable=None):
    result = subprocess.run(['ps','-axo','pid=,comm='], capture_output=True, text=True, check=True)
    found = []
    for row in result.stdout.splitlines():
        parts = row.strip().split(None, 1)
        if len(parts) != 2: continue
        pid, command = int(parts[0]), parts[1]
        if executable and not command.lower().endswith(executable.lower()): continue
        if not executable and not ('wine' in command.lower() or '.exe' in command.lower()): continue
        if process_prefix(pid) == layout.prefix.resolve(): found.append(pid)
    return found

def wait_pid(layout, name, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pids = owned_pids(layout, name)
        if len(pids) == 1: return pids[0]
        if len(pids) > 1: raise Error('Multiple owned instances of ' + name)
        time.sleep(.5)
    raise Error(f'{name} did not start in this prefix; inspect {layout.logs}')

def assert_stopped(layout):
    if owned_pids(layout):
        raise Error('This wrapper is running. Stop it first; other prefixes are never stopped.')
