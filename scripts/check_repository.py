#!/usr/bin/env python3
"""Read-only bounded audit of project documentation, source and entrypoints."""
import ast
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parent.parent
def main():
    issues=[]
    docs=list(ROOT.glob('README*.md'))+list((ROOT/'docs').rglob('*.md'))+[ROOT/'THIRD_PARTY_NOTICES.md']
    for p in docs:
        text=p.read_text()
        for dest in re.findall(r'\]\(([^)]+)\)',text):
            if '://' in dest or dest.startswith('#'):continue
            if not (p.parent/dest.split('#')[0]).resolve().exists():issues.append(f'{p.relative_to(ROOT)}: broken {dest}')
        if re.search(r'(?m)^#{1,6}[^#\s]',text):issues.append(f'{p.relative_to(ROOT)}: malformed heading')
    for lang in ('en','ru'):
        for topic in ('install','usage','settings','faq','history','architecture','development'):
            if not (ROOT/'docs'/lang/(topic+'.md')).is_file():issues.append(f'Missing {lang}/{topic}')
    for p in (ROOT/'scripts').glob('*.sh'):
        subprocess.run(['sh','-n',str(p)],check=True)
    for folder in ('alyx_macos','scripts','tests'):
        for p in (ROOT/folder).glob('*.py'):ast.parse(p.read_text(),filename=str(p))
    for folder in ('src','config','docs','alyx_macos','scripts'):
        for p in (ROOT/folder).rglob('*'):
            if not p.is_file() or '__pycache__' in p.parts:continue
            if p.suffix not in ('.h','.c','.cpp','.mm','.py','.json','.md','.sh','.txt','.build'):continue
            if re.search(r'/Users/[A-Za-z0-9_.-]+/',p.read_text(errors='replace')):
                issues.append(f'{p.relative_to(ROOT)}: hard-coded personal home path')
    if issues:raise SystemExit('\n'.join(issues))
    print(f'Documentation links/language pairs, {len(list((ROOT/"scripts").glob("*.sh")))} shell scripts and source-path audit passed.')
if __name__=='__main__':main()
