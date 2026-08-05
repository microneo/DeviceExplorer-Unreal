#!/usr/bin/env python3
"""Check the explicit source list and the sans-I/O wire boundary."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WIRE = ROOT / "Source" / "DeviceExplorerWire"
SOURCE_LIST = WIRE / "sources.cmake"
ENTRY = re.compile(r'\$\{CMAKE_CURRENT_LIST_DIR\}/((?:Private|Public)/[^"\r\n]+)')
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
FORBIDDEN_INCLUDE_PREFIXES = (
    "CoreMinimal.h",
    "GenericPlatform/",
    "HAL/",
    "IPAddress.h",
    "Misc/",
    "SocketSubsystem.h",
    "Sockets.h",
    "asio/",
    "condition_variable",
    "filesystem",
    "fstream",
    "future",
    "mutex",
    "thread",
)
FORBIDDEN_CODE = {
    "dynamic_cast<": "RTTI",
    "std::filesystem": "file-system I/O",
    "std::ifstream": "file I/O",
    "std::ofstream": "file I/O",
    "std::thread": "threading",
    "typeid(": "RTTI",
}


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

    violations: list[str] = []
    for relative in sorted(present):
        text = (WIRE / relative).read_text(encoding="utf-8")
        for include in INCLUDE.findall(text):
            if include.startswith(FORBIDDEN_INCLUDE_PREFIXES):
                violations.append(f"{relative}: forbidden dependency include {include}")
        for token, boundary in FORBIDDEN_CODE.items():
            if token in text:
                violations.append(f"{relative}: forbidden {boundary} token {token}")
        if re.search(r"\bthrow\b", text):
            violations.append(f"{relative}: forbidden exception token throw")

    if violations:
        print("DeviceExplorerWire boundary violations:")
        for violation in violations:
            print(f"  {violation}")
        return 1
    print(f"DeviceExplorerWire source list is complete ({len(present)} files)")
    print("DeviceExplorerWire sans-I/O dependency boundary is clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
