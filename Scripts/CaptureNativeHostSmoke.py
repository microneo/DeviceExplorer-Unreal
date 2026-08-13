#!/usr/bin/env python3
"""Exercise the native host's production dashboard and transfer paths."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import socket
import threading
import time
from typing import Any

from CaptureProtocolSmoke import (
    CLIENT_NONCE,
    DEVICE_PROOF_LABEL,
    HOST_PROOF_LABEL,
    PROTOCOL_VERSION,
    WEBSOCKET_GUID,
    WEBSOCKET_KEY,
    compute_proof,
    encode_json_frame,
    parse_http_response,
    recv_json_frame,
    recv_until,
)


def http_request(
    host: str,
    port: int,
    method: str,
    path: str,
    body: bytes = b"",
    content_type: str = "application/json",
    trusted: bool = False,
    timeout: float = 5.0,
) -> tuple[int, dict[str, str], bytes]:
    headers = [
        f"{method} {path} HTTP/1.1",
        f"Host: {host}:{port}",
        "Connection: close",
        f"Content-Length: {len(body)}",
    ]
    if body or method != "GET":
        headers.append(f"Content-Type: {content_type}")
    if trusted:
        headers.append("X-DeviceExplorer-Request: 1")
    request = ("\r\n".join(headers) + "\r\n\r\n").encode("ascii") + body
    with socket.create_connection((host, port), timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall(request)
        response = bytearray()
        while True:
            chunk = connection.recv(64 * 1024)
            if not chunk:
                break
            response.extend(chunk)
    status, response_headers, response_body = parse_http_response(bytes(response))
    expected = int(response_headers.get("content-length", "0"))
    if len(response_body) != expected:
        raise RuntimeError(f"short HTTP body: got {len(response_body)}, expected {expected}")
    return status, response_headers, response_body


def connect_device(host: str, port: int, token: str, timeout: float) -> socket.socket:
    expected_accept = base64.b64encode(
        hashlib.sha1((WEBSOCKET_KEY + WEBSOCKET_GUID).encode("ascii")).digest()
    ).decode("ascii")
    connection = socket.create_connection((host, port), timeout=timeout)
    connection.settimeout(timeout)
    request = (
        "GET /device/connect HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {WEBSOCKET_KEY}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode("ascii")
    connection.sendall(request)
    status, headers, body = parse_http_response(recv_until(connection, b"\r\n\r\n"))
    if status != 101 or body or headers.get("sec-websocket-accept") != expected_accept:
        raise RuntimeError("WebSocket upgrade failed")
    connection.sendall(encode_json_frame({
        "type": "auth_request",
        "protocol_version": PROTOCOL_VERSION,
        "client_nonce": CLIENT_NONCE,
    }))
    _, challenge = recv_json_frame(connection, "auth_challenge")
    host_nonce = challenge["host_nonce"]
    expected_host_proof = compute_proof(token, HOST_PROOF_LABEL, CLIENT_NONCE, host_nonce)
    if challenge.get("host_proof") != expected_host_proof:
        raise RuntimeError("host proof mismatch")
    connection.sendall(encode_json_frame({
        "type": "auth_response",
        "client_proof": compute_proof(token, DEVICE_PROOF_LABEL, CLIENT_NONCE, host_nonce),
    }))
    recv_json_frame(connection, "auth_ok")
    return connection


def json_body(body: bytes) -> dict[str, Any]:
    value = json.loads(body.decode("utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError("expected a JSON object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--device-port", type=int, default=18081)
    parser.add_argument("--dashboard-port", type=int, default=18080)
    parser.add_argument("--trace-port", type=int, default=1981)
    parser.add_argument("--token", required=True)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()

    device_id = "native-host-smoke"
    connection = connect_device(args.host, args.device_port, args.token, args.timeout)
    second_connection: socket.socket | None = None
    try:
        connection.sendall(encode_json_frame({
            "type": "hello",
            "device_id": device_id,
            "device_session": "1",
            "connection_id": "native-host-smoke-connection-1",
            "name": "NativeHostSmoke",
            "project_name": "DeviceExplorer",
            "engine_version": "black-box",
            "platform": "Python",
            "configuration": "Development",
            "build_version": "capture",
            "protocol_version": PROTOCOL_VERSION,
            "uptime_seconds": 1,
            "capabilities": ["logs", "console", "files", "trace"],
            "commands": [],
            "file_roots": [{"name": "Saved", "path": "Saved"}],
            "data_modules": [],
        }))
        _, attach_ack = recv_json_frame(connection, "attach_ack")
        if attach_ack.get("accepted") is not True:
            raise RuntimeError(f"host rejected native smoke attach: {attach_ack!r}")
        connection.sendall(encode_json_frame({
            "type": "log_batch",
            "entries": [{
                "timestamp": "2026-01-01T00:00:00Z",
                "category": "LogNativeSmoke",
                "verbosity": "Display",
                "message": "streamed through native host",
            }],
            "dropped": 0,
        }))
        time.sleep(0.05)

        status, _, body = http_request(
            args.host, args.dashboard_port, "GET", f"/api/devices/{device_id}/logs", timeout=args.timeout
        )
        if status != 200 or json_body(body)["entries"][0]["message"] != "streamed through native host":
            raise RuntimeError("log stream did not reach the dashboard")

        second_connection = connect_device(args.host, args.device_port, args.token, args.timeout)
        second_connection.sendall(encode_json_frame({
            "type": "hello",
            "device_id": "native-host-smoke-peer",
            "device_session": "1",
            "connection_id": "native-host-smoke-peer-connection-1",
            "name": "NativeHostSmokePeer",
            "protocol_version": PROTOCOL_VERSION,
            "capabilities": [],
            "commands": [],
            "file_roots": [],
            "data_modules": [],
        }))
        _, peer_attach_ack = recv_json_frame(second_connection, "attach_ack")
        if peer_attach_ack.get("accepted") is not True:
            raise RuntimeError(f"host rejected peer smoke attach: {peer_attach_ack!r}")

        proxy_result: dict[str, Any] = {}
        def post_command() -> None:
            proxy_result["response"] = http_request(
                args.host,
                args.dashboard_port,
                "POST",
                f"/api/devices/{device_id}/command",
                json.dumps({"command": "stat fps"}).encode("utf-8"),
                trusted=True,
                timeout=args.timeout,
            )

        command_thread = threading.Thread(target=post_command)
        command_thread.start()
        _, command = recv_json_frame(connection, "execute_command")
        second_connection.sendall(encode_json_frame({
            "type": "command_result",
            "request_id": command["request_id"],
            "success": True,
            "output": "wrong channel",
        }))
        time.sleep(0.05)
        if not command_thread.is_alive():
            raise RuntimeError("a different authenticated channel completed a pending request")
        connection.sendall(encode_json_frame({
            "type": "command_result",
            "request_id": command["request_id"],
            "success": True,
            "output": "ok",
        }))
        command_thread.join(args.timeout)
        if command_thread.is_alive() or proxy_result["response"][0] != 200:
            raise RuntimeError("dashboard command proxy failed")

        transfer_payload = b"TRC2" + bytes(range(256)) * 1024
        status, _, body = http_request(
            args.host,
            args.dashboard_port,
            "POST",
            f"/api/devices/{device_id}/transfers",
            json.dumps({"root": "Saved", "path": "Profiling/test.utrace", "archive": False}).encode("utf-8"),
            trusted=True,
            timeout=args.timeout,
        )
        if status != 202:
            raise RuntimeError(f"transfer creation returned HTTP {status}: {body!r}")
        transfer = json_body(body)
        _, upload = recv_json_frame(connection, "upload_file")
        upload_target = upload["upload_url"].split(f":{args.device_port}", 1)[1]
        status, _, _ = http_request(
            args.host,
            args.device_port,
            "PUT",
            upload_target,
            transfer_payload,
            content_type="application/octet-stream",
            timeout=args.timeout,
        )
        if status != 200:
            raise RuntimeError(f"upload returned HTTP {status}")
        status, _, downloaded = http_request(
            args.host,
            args.dashboard_port,
            "GET",
            f"/api/transfers/{transfer['id']}/download",
            timeout=args.timeout,
        )
        if status != 200 or downloaded != transfer_payload:
            raise RuntimeError("streamed transfer round-trip mismatch")

        trace_bytes = bytearray()
        trace_ready = threading.Event()
        def receive_trace() -> None:
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind((args.host, args.trace_port))
                listener.listen(1)
                trace_ready.set()
                peer, _ = listener.accept()
                with peer:
                    while True:
                        chunk = peer.recv(64 * 1024)
                        if not chunk:
                            break
                        trace_bytes.extend(chunk)

        trace_thread = threading.Thread(target=receive_trace)
        trace_thread.start()
        if not trace_ready.wait(args.timeout):
            raise RuntimeError("trace receiver did not start")
        status, _, body = http_request(
            args.host,
            args.dashboard_port,
            "POST",
            f"/api/transfers/{transfer['id']}/trace",
            b"{}",
            trusted=True,
            timeout=args.timeout,
        )
        trace_thread.join(args.timeout)
        if status != 200 or json_body(body).get("status") != "sent" or bytes(trace_bytes) != transfer_payload:
            raise RuntimeError("trace forward mismatch")

        status, headers, index = http_request(args.host, args.dashboard_port, "GET", "/", timeout=args.timeout)
        if status != 200 or "content-security-policy" not in headers or b"DeviceExplorer" not in index:
            raise RuntimeError("dashboard static files were not served securely")
    finally:
        if second_connection is not None:
            second_connection.close()
        connection.close()

    print("native host production smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
