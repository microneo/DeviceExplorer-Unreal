#!/usr/bin/env python3
"""Fail when the explicit CMake list drifts from DeviceExplorerWire."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WIRE = ROOT / "Source" / "DeviceExplorerWire"
SOURCE_LIST = WIRE / "sources.cmake"
ENTRY = re.compile(r'\$\{CMAKE_CURRENT_LIST_DIR\}/((?:Private|Public)/[^"\r\n]+)')


def main() -> int:
    listed = set(ENTRY.findall(SOURCE_LIST.read_text(encoding="utf-8")))
    present = {
        path.relative_to(WIRE).as_posix()
        for directory, suffix in (("Private", "*.cpp"), ("Public", "*.h"))
        for path in (WIRE / directory).rglob(suffix)
    }
    missing = sorted(present - listed)
    stale = sorted(listed - present)
    if missing or stale:
        if missing:
            print("missing from sources.cmake:")
            for path in missing:
                print(f"  {path}")
        if stale:
            print("listed but absent:")
            for path in stale:
                print(f"  {path}")
        return 1
    print(f"DeviceExplorerWire source list is complete ({len(present)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
