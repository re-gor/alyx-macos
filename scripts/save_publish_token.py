#!/usr/bin/env python3
"""Human-only secret entry for one publication; value is never printed."""
import getpass
import os
from pathlib import Path

root=Path(__file__).resolve().parent.parent
folder=root/'.local'
folder.mkdir(exist_ok=True,mode=0o700)
folder.chmod(0o700)
token=getpass.getpass('Fine-grained GitHub token (alyx-macos only; hidden): ').strip()
if not token or any(c.isspace() for c in token):raise SystemExit('Invalid token; no secret saved.')
path=folder/'github-token'
fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
with os.fdopen(fd,'w') as stream:stream.write(token+'\n')
path.chmod(0o600)
token=''
print('Saved private .local/github-token (ignored by Git). Token value was not printed.')
