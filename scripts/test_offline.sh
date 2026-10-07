#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_dir"
python3 -m unittest discover -s tests -v
exec python3 tests/run_native.py "$@"
