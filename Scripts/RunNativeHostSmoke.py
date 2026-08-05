#!/usr/bin/env python3
"""Start dexp-host on isolated ports and run the production black-box smoke."""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()

    ports: set[int] = set()
    while len(ports) != 3:
        ports.add(free_port())
    dashboard_port, device_port, trace_port = ports
    token = "native-host-cmake-smoke-token"
    with tempfile.TemporaryDirectory(prefix="deviceexplorer-native-smoke-") as temporary:
        temporary_path = Path(temporary)
        command = [
            str(args.executable),
            "--dashboard-port", str(dashboard_port),
            "--device-port", str(device_port),
            "--token", token,
            "--web-root", str(args.source_root / "Resources" / "Web"),
            "--transfer-dir", str(temporary_path / "transfers"),
            "--state-dir", str(temporary_path / "state"),
            "--trace-port", str(trace_port),
        ]
        host = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                try:
                    with socket.create_connection(("127.0.0.1", dashboard_port), timeout=0.1):
                        break
                except OSError:
                    if host.poll() is not None:
                        raise RuntimeError("native host exited during startup")
                    time.sleep(0.05)
            else:
                raise RuntimeError("native host did not become ready")

            duplicate = subprocess.run(
                command[:1] + ["--dashboard-port", "0", "--device-port", "0", "--token", token,
                              "--state-dir", str(temporary_path / "state")],
                text=True,
                capture_output=True,
                timeout=5,
            )
            if duplicate.returncode == 0 or "already running" not in duplicate.stderr:
                raise RuntimeError("per-user singleton lock did not reject a second host")

            smoke = subprocess.run(
                [
                    sys.executable,
                    str(args.source_root / "Scripts" / "CaptureNativeHostSmoke.py"),
                    "--dashboard-port", str(dashboard_port),
                    "--device-port", str(device_port),
                    "--trace-port", str(trace_port),
                    "--token", token,
                ],
                cwd=args.source_root,
                text=True,
                capture_output=True,
                timeout=30,
            )
            sys.stdout.write(smoke.stdout)
            sys.stderr.write(smoke.stderr)
            return smoke.returncode
        finally:
            host.terminate()
            try:
                output, _ = host.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                host.kill()
                output, _ = host.communicate()
            sys.stdout.write(output)


if __name__ == "__main__":
    raise SystemExit(main())
