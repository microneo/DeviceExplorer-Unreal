#!/usr/bin/env python3
"""Exercise the legacy protocol-9 host and optionally save raw wire captures.

The script intentionally uses only the Python standard library. Start the UE
host with a known token, then run this file from any machine on the same LAN.
It verifies the dashboard HTTP API, the device WebSocket path, and the mDNS
announcement produced in response to a one-shot query.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import socket
import struct
import time
from pathlib import Path
from typing import Any
from urllib.parse import quote_plus


PROTOCOL_VERSION = 9
SERVICE_NAME = "_deviceexplorer._tcp.local"
WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WEBSOCKET_KEY = "dGhlIHNhbXBsZSBub25jZQ=="
MASK_KEY = bytes.fromhex("12345678")


def recv_until(sock: socket.socket, marker: bytes, limit: int = 64 * 1024) -> bytes:
    data = bytearray()
    while marker not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed before the response was complete")
        data.extend(chunk)
        if len(data) > limit:
            raise RuntimeError(f"response exceeded {limit} bytes")
    return bytes(data)


def parse_http_response(raw: bytes) -> tuple[int, dict[str, str], bytes]:
    header_end = raw.find(b"\r\n\r\n")
    if header_end < 0:
        raise RuntimeError("HTTP response has no header terminator")
    header_bytes = raw[:header_end]
    body = raw[header_end + 4 :]
    lines = header_bytes.decode("iso-8859-1").split("\r\n")
    status_parts = lines[0].split(" ", 2)
    if len(status_parts) < 2 or not status_parts[1].isdigit():
        raise RuntimeError(f"invalid HTTP status line: {lines[0]!r}")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" not in line:
            raise RuntimeError(f"invalid HTTP header: {line!r}")
        key, value = line.split(":", 1)
        headers[key.strip().lower()] = value.strip()
    return int(status_parts[1]), headers, body


def http_get(host: str, port: int, path: str, timeout: float) -> tuple[bytes, bytes, Any]:
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Connection: close\r\n"
        "Accept: application/json\r\n\r\n"
    ).encode("ascii")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request)
        response = bytearray()
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
    status, headers, body = parse_http_response(bytes(response))
    content_length = int(headers.get("content-length", "0"))
    if status != 200 or len(body) != content_length:
        raise RuntimeError(
            f"GET {path} returned HTTP {status}, body={len(body)}, expected={content_length}"
        )
    return request, bytes(response), json.loads(body.decode("utf-8"))


def encode_client_frame(opcode: int, payload: bytes) -> bytes:
    if opcode >= 0x8 and len(payload) > 125:
        raise RuntimeError("WebSocket control payload exceeds 125 bytes")
    if len(payload) < 126:
        header = bytes((0x80 | opcode, 0x80 | len(payload)))
    elif len(payload) <= 0xFFFF:
        header = bytes((0x80 | opcode, 0x80 | 126)) + struct.pack("!H", len(payload))
    else:
        header = bytes((0x80 | opcode, 0x80 | 127)) + struct.pack("!Q", len(payload))
    masked = bytes(value ^ MASK_KEY[index % 4] for index, value in enumerate(payload))
    return header + MASK_KEY + masked


def recv_server_frame(sock: socket.socket) -> tuple[bytes, int, bytes]:
    first = sock.recv(2)
    if len(first) != 2:
        raise RuntimeError("short WebSocket frame header")
    if first[1] & 0x80:
        raise RuntimeError("server sent a masked WebSocket frame")
    length = first[1] & 0x7F
    extended = b""
    if length == 126:
        extended = sock.recv(2)
        if len(extended) != 2:
            raise RuntimeError("short WebSocket 16-bit length")
        length = struct.unpack("!H", extended)[0]
    elif length == 127:
        extended = sock.recv(8)
        if len(extended) != 8:
            raise RuntimeError("short WebSocket 64-bit length")
        length = struct.unpack("!Q", extended)[0]
    payload = bytearray()
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("short WebSocket payload")
        payload.extend(chunk)
    return first + extended + bytes(payload), first[0] & 0x0F, bytes(payload)


def websocket_smoke(
    host: str,
    port: int,
    token: str,
    timeout: float,
    device_id: str,
    expect_close_reply: bool,
) -> dict[str, Any]:
    expected_accept = base64.b64encode(
        hashlib.sha1((WEBSOCKET_KEY + WEBSOCKET_GUID).encode("ascii")).digest()
    ).decode("ascii")
    target = f"/device/connect?token={quote_plus(token)}"
    request = (
        f"GET {target} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {WEBSOCKET_KEY}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode("ascii")

    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.settimeout(timeout)
        sock.sendall(request)
        response = recv_until(sock, b"\r\n\r\n")
        status, headers, body = parse_http_response(response)
        if status != 101 or body:
            raise RuntimeError(f"WebSocket upgrade returned HTTP {status}")
        if headers.get("sec-websocket-accept") != expected_accept:
            raise RuntimeError("WebSocket accept key does not match RFC 6455")

        hello = json.dumps(
            {
                "type": "hello",
                "device_id": device_id,
                "name": "Protocol9Smoke",
                "project_name": "DeviceExplorer",
                "engine_version": "black-box",
                "platform": "Python",
                "configuration": "Development",
                "build_version": "capture",
                "protocol_version": PROTOCOL_VERSION,
                "uptime_seconds": 0,
                "capabilities": [],
                "commands": [],
                "file_roots": [],
                "data_modules": [],
            },
            separators=(",", ":"),
        ).encode("utf-8")
        hello_frame = encode_client_frame(0x1, hello)
        sock.sendall(hello_frame)

        ping_payload = b"protocol9"
        ping_frame = encode_client_frame(0x9, ping_payload)
        sock.sendall(ping_frame)
        pong_frame, pong_opcode, pong_payload = recv_server_frame(sock)
        if pong_opcode != 0xA or pong_payload != ping_payload:
            raise RuntimeError("host did not echo the WebSocket ping payload")

        close_frame = encode_client_frame(0x8, struct.pack("!H", 1000))
        sock.sendall(close_frame)
        capture = {
            "upgrade_request_base64": base64.b64encode(request).decode("ascii"),
            "upgrade_response_base64": base64.b64encode(response).decode("ascii"),
            "client_hello_frame_base64": base64.b64encode(hello_frame).decode("ascii"),
            "client_ping_frame_base64": base64.b64encode(ping_frame).decode("ascii"),
            "server_pong_frame_base64": base64.b64encode(pong_frame).decode("ascii"),
            "client_close_frame_base64": base64.b64encode(close_frame).decode("ascii"),
            "expected_accept": expected_accept,
        }
        if expect_close_reply:
            server_close, close_opcode, close_payload = recv_server_frame(sock)
            if close_opcode != 0x8 or close_payload != struct.pack("!H", 1000):
                raise RuntimeError("host did not echo the WebSocket close payload")
            capture["server_close_frame_base64"] = base64.b64encode(server_close).decode(
                "ascii"
            )
        return capture
    finally:
        sock.close()


def add_dns_name(packet: bytearray, name: str) -> None:
    for label in name.split("."):
        encoded = label.encode("utf-8")
        if len(encoded) > 63:
            raise RuntimeError(f"DNS label is too long: {label!r}")
        packet.append(len(encoded))
        packet.extend(encoded)
    packet.append(0)


def build_mdns_query() -> bytes:
    packet = bytearray(struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0))
    add_dns_name(packet, SERVICE_NAME)
    packet.extend(struct.pack("!HH", 12, 1))
    return bytes(packet)


def read_dns_name(packet: bytes, offset: int) -> tuple[str, int]:
    labels: list[str] = []
    resume: int | None = None
    jumps = 0
    while True:
        if offset >= len(packet):
            raise RuntimeError("DNS name runs past the datagram")
        length = packet[offset]
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(packet):
                raise RuntimeError("short DNS compression pointer")
            if resume is None:
                resume = offset + 2
            offset = ((length & 0x3F) << 8) | packet[offset + 1]
            jumps += 1
            if jumps > 64:
                raise RuntimeError("too many DNS compression jumps")
            continue
        if length & 0xC0:
            raise RuntimeError("invalid DNS label length")
        offset += 1
        if length == 0:
            return ".".join(labels), resume if resume is not None else offset
        end = offset + length
        if end > len(packet):
            raise RuntimeError("short DNS label")
        labels.append(packet[offset:end].decode("utf-8"))
        offset = end


def parse_mdns_records(packet: bytes) -> list[dict[str, Any]]:
    if len(packet) < 12:
        raise RuntimeError("short DNS header")
    _, _, questions, answers, authorities, additionals = struct.unpack("!HHHHHH", packet[:12])
    offset = 12
    for _ in range(questions):
        _, offset = read_dns_name(packet, offset)
        offset += 4
    records: list[dict[str, Any]] = []
    for _ in range(answers + authorities + additionals):
        name, offset = read_dns_name(packet, offset)
        if offset + 10 > len(packet):
            raise RuntimeError("short DNS resource-record header")
        record_type, record_class, ttl, data_length = struct.unpack(
            "!HHIH", packet[offset : offset + 10]
        )
        offset += 10
        data_offset = offset
        offset += data_length
        if offset > len(packet):
            raise RuntimeError("short DNS resource-record payload")
        record: dict[str, Any] = {
            "name": name,
            "type": record_type,
            "class": record_class,
            "ttl": ttl,
        }
        if record_type in (1, 12, 16, 33):
            record["data_offset"] = data_offset
            record["data_length"] = data_length
        if record_type == 1 and data_length == 4:
            record["address"] = socket.inet_ntoa(packet[data_offset:offset])
        elif record_type == 12:
            record["target"], _ = read_dns_name(packet, data_offset)
        elif record_type == 16:
            entries: list[str] = []
            txt_offset = data_offset
            while txt_offset < offset:
                entry_length = packet[txt_offset]
                txt_offset += 1
                entry_end = txt_offset + entry_length
                if entry_end > offset:
                    raise RuntimeError("short DNS TXT entry")
                entries.append(packet[txt_offset:entry_end].decode("utf-8"))
                txt_offset = entry_end
            record["entries"] = entries
        elif record_type == 33 and data_length >= 6:
            priority, weight, port = struct.unpack("!HHH", packet[data_offset : data_offset + 6])
            target, _ = read_dns_name(packet, data_offset + 6)
            record.update(
                {"priority": priority, "weight": weight, "port": port, "target": target}
            )
        records.append(record)
    return records


def mdns_smoke(token: str, device_port: int, timeout: float) -> dict[str, Any]:
    query = build_mdns_query()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.settimeout(timeout)
        sock.bind(("0.0.0.0", 0))
        sock.sendto(query, ("224.0.0.251", 5353))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("no matching DeviceExplorer mDNS response")
            sock.settimeout(remaining)
            response, sender = sock.recvfrom(9000)
            records = parse_mdns_records(response)
            if SERVICE_NAME not in {record["name"].lower() for record in records}:
                continue
            token_bytes = ("token=" + token).encode("utf-8")
            if token_bytes not in response:
                continue
            txt_entries = {
                entry
                for record in records
                for entry in record.get("entries", [])
            }
            srv_ports = {record["port"] for record in records if record["type"] == 33}
            if f"version={PROTOCOL_VERSION}" not in txt_entries or device_port not in srv_ports:
                continue
            return {
                "query_base64": base64.b64encode(query).decode("ascii"),
                "response_base64": base64.b64encode(response).decode("ascii"),
                "sender": f"{sender[0]}:{sender[1]}",
                "records": records,
            }
    finally:
        sock.close()


def encode_capture(raw: bytes) -> str:
    return base64.b64encode(raw).decode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--device-port", type=int, default=18081)
    parser.add_argument("--dashboard-port", type=int, default=18080)
    parser.add_argument("--token", required=True)
    parser.add_argument("--device-id", default="protocol9-black-box-smoke")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--skip-mdns", action="store_true")
    parser.add_argument(
        "--expect-close-reply",
        action="store_true",
        help="require the RFC 6455 close reply added by DeviceExplorerWire",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    device_health_request, device_health_response, device_health = http_get(
        args.host, args.device_port, "/health", args.timeout
    )
    dashboard_health_request, dashboard_health_response, dashboard_health = http_get(
        args.host, args.dashboard_port, "/health", args.timeout
    )
    config_request, config_response, config = http_get(
        args.host, args.dashboard_port, "/api/config", args.timeout
    )
    if device_health.get("status") != "ok" or dashboard_health.get("status") != "ok":
        raise RuntimeError("one of the health endpoints is not healthy")
    if config.get("protocol_version") != PROTOCOL_VERSION:
        raise RuntimeError(
            f"host reports protocol {config.get('protocol_version')}, expected {PROTOCOL_VERSION}"
        )
    if config.get("device_port") != args.device_port:
        raise RuntimeError("dashboard reports a different device port")

    websocket = websocket_smoke(
        args.host,
        args.device_port,
        args.token,
        args.timeout,
        args.device_id,
        args.expect_close_reply,
    )

    deadline = time.monotonic() + args.timeout
    devices: Any = None
    devices_request = b""
    devices_response = b""
    while time.monotonic() < deadline:
        devices_request, devices_response, devices = http_get(
            args.host, args.dashboard_port, "/api/devices", args.timeout
        )
        entries = devices.get("devices", []) if isinstance(devices, dict) else []
        if any(entry.get("id") == args.device_id for entry in entries):
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("the smoke device did not appear in /api/devices")

    capture: dict[str, Any] = {
        "schema_version": 1,
        "protocol_version": PROTOCOL_VERSION,
        "captured_unix_seconds": int(time.time()),
        "endpoint": {
            "host": args.host,
            "device_port": args.device_port,
            "dashboard_port": args.dashboard_port,
        },
        "http": {
            "device_health_request_base64": encode_capture(device_health_request),
            "device_health_response_base64": encode_capture(device_health_response),
            "dashboard_health_request_base64": encode_capture(dashboard_health_request),
            "dashboard_health_response_base64": encode_capture(dashboard_health_response),
            "config_request_base64": encode_capture(config_request),
            "config_response_base64": encode_capture(config_response),
            "devices_request_base64": encode_capture(devices_request),
            "devices_response_base64": encode_capture(devices_response),
        },
        "websocket": websocket,
    }
    if not args.skip_mdns:
        capture["mdns"] = mdns_smoke(args.token, args.device_port, args.timeout)

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(capture, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"protocol-9 smoke passed; capture written to {args.output}")
    else:
        print("protocol-9 smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
