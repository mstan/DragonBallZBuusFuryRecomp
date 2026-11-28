#!/usr/bin/env python3
"""Stage dependency notices through the pinned engine."""
from pathlib import Path
import runpy
import sys

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path(sys.argv[1]).resolve()
sys.argv.extend(["--title-root", str(ROOT)])
runpy.run_path(str(ENGINE / "tools/collect_licenses.py"), run_name="__main__")
