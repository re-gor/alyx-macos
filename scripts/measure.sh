#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$repo_dir/scripts/alyx_macos.py" stats "$@"
