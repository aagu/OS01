#!/usr/bin/env python3
"""Compatibility entry point delegating to the GUI QEMU suite (gui_test.py)."""

import sys
from pathlib import Path

# Ensure repo root is on sys.path
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests.gui_test import main

if __name__ == "__main__":
    sys.exit(main())
