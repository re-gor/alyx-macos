"""Add one non-Steam ALVR entry, retaining typed binary VDF records verbatim."""
from dataclasses import dataclass
import os
from pathlib import Path
import struct
import tempfile
import zlib

from .core import Error, assert_stopped

@dataclass
class Entry:
    kind: int
    name: str
    payload: object

def parse(data):
    if len(data)>16*1024*1024:raise Error('Unexpectedly large shortcuts file; use Steam GUI instead.')
    pos=0
    def take(n):
        nonlocal pos
        if pos+n>len(data):raise Error('Truncated binary VDF; original file left unchanged.')
        out=data[pos:pos+n];pos+=n;return out
    def cstring():
        nonlocal pos
        end=data.find(b'\0',pos)
        if end<0:raise Error('Unterminated binary VDF string')
        out=data[pos:end+1];pos=end+1;return out
    def block(depth=0):
        if depth>32:raise Error('VDF nesting too deep')
        entries=[]
        while True:
            kind=take(1)[0]
            if kind==8:return entries
            name=cstring()[:-1].decode('utf-8','surrogateescape')
            if kind==0:value=block(depth+1)
            elif kind==1:value=cstring()
            elif kind in (2,3,4,6):value=take(4)
            elif kind in (7,10):value=take(8)
            elif kind==5:
                chunks=[]
                while True:
                    item=take(2);chunks.append(item)
                    if item==b'\0\0':break
                value=b''.join(chunks)
            else:raise Error(f'Unknown VDF type{kind}; refuse overwrite, use Steam GUI.')
            entries.append(Entry(kind,name,value))
    if not data:return [Entry(0,'shortcuts',[])]
    result=block()
    if pos!=len(data):raise Error('Trailing VDF data; refuse overwrite')
    return result

def serialize(entries):
    out=bytearray()
    for entry in entries:
        out.append(entry.kind)
        out.extend(entry.name.encode('utf-8','surrogateescape')+b'\0')
        out.extend(serialize(entry.payload) if entry.kind==0 else entry.payload)
    out.append(8)
    return bytes(out)

def string(name,value):return Entry(1,name,value.encode('utf-8')+b'\0')
def number(name,value):return Entry(2,name,struct.pack('<I',value))

def add_entry(entries):
    roots=[entry for entry in entries if entry.kind==0 and entry.name=='shortcuts']
    if len(roots)!=1:raise Error('Expected one shortcuts root; refuse overwrite')
    shortcuts=roots[0].payload
    label='ALVR Dashboard (alyx-macos)'
    exe='"C:\\ALVR\\ALVR Dashboard.exe"'
    for entry in shortcuts:
        if entry.kind!=0:continue
        fields={x.name:x for x in entry.payload}
        name=fields.get('AppName')
        if name and name.kind==1 and name.payload[:-1].decode('utf-8','replace')==label:
            return False
    index=max([int(e.name) for e in shortcuts if e.name.isdigit()]+[-1])+1
    appid=zlib.crc32((exe+label).encode('utf-8'))|0x80000000
    fields=[number('appid',appid),string('AppName',label),string('Exe',exe),
        string('StartDir','"C:\\ALVR"'),string('icon',''),string('ShortcutPath',''),
        string('LaunchOptions',''),number('IsHidden',0),number('AllowDesktopConfig',1),
        number('AllowOverlay',0),number('OpenVR',0),number('Devkit',0),
        string('DevkitGameID',''),number('LastPlayTime',0),Entry(0,'tags',[])]
    shortcuts.append(Entry(0,str(index),fields))
    return True

def install(layout,steam_userid=None):
    layout.managed();assert_stopped(layout)
    folder=layout.steam/'userdata'
    accounts=[p for p in folder.iterdir() if p.is_dir() and p.name.isdigit()] if folder.is_dir() else []
    if steam_userid:
        accounts=[p for p in accounts if p.name==steam_userid]
    if len(accounts)!=1:raise Error('Log into this Windows Steam once, exit it, then select --steam-userid if several accounts exist.')
    file=accounts[0]/'config/shortcuts.vdf'
    if not file.resolve().is_relative_to(layout.root):raise Error('Shortcut file escapes owned installation')
    entries=parse(file.read_bytes() if file.exists() else b'')
    if not add_entry(entries):print('ALVR shortcut already present; no duplicate added.');return
    layout.backup([file],'steam-shortcut')
    file.parent.mkdir(parents=True,exist_ok=True)
    fd,temp=tempfile.mkstemp(prefix='.shortcuts-',dir=file.parent)
    try:
        with os.fdopen(fd,'wb') as out:
            out.write(serialize(entries));out.flush();os.fsync(out.fileno())
        os.replace(temp,file)
    finally:
        if os.path.exists(temp):os.unlink(temp)
    print('Added ALVR Dashboard (alyx-macos) as a non-Steam game. Open this Windows Steam library to launch it without Terminal.')
