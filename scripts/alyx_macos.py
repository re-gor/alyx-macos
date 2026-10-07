#!/usr/bin/env python3
"""Run from any working directory; all mutable state goes into an owned root."""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent))
from alyx_macos.cli import main
raise SystemExit(main())
