"""ALVR20.14.1 localhost API/WebSocket subset; no logging changes."""
import base64
import hashlib
import json
import os
import select
import socket
import time
import urllib.request
FLAGS = ("log_tracking", "log_button_presses")
BASE_PATH = ("session_settings", "extra", "logging")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def post(command, port):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/api/dashboard-request",
        data=json.dumps(command).encode(),
        headers={"X-ALVR": "true", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=5) as response:
        if response.status != 200:
            raise RuntimeError(f"ALVR returned HTTP {response.status}")


def set_flags(flags, port):
    post({"SetValues": [{
        "path": [{"Name": name} for name in (*BASE_PATH, key)],
        "value": value,
    } for key, value in flags.items()]}, port)


class EventSocket:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buffer = bytearray()
        self.fragment = bytearray()
        self.fragment_opcode = None
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET /api/events HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            "X-ALVR: true\r\n\r\n"
        )
        try:
            self.sock.sendall(request.encode())
            while b"\r\n\r\n" not in self.buffer:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise ConnectionError("ALVR closed the WebSocket handshake")
                self.buffer.extend(chunk)
                if len(self.buffer) > 65536:
                    raise RuntimeError("Oversized WebSocket handshake")
            header, remainder = self.buffer.split(b"\r\n\r\n", 1)
            lines = header.decode("iso-8859-1").split("\r\n")
            if len(lines[0].split()) < 2 or lines[0].split()[1] != "101":
                raise RuntimeError(f"WebSocket handshake: {lines[0]}")
            expected = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            actual = next((line.split(":", 1)[1].strip() for line in lines[1:]
                           if line.split(":", 1)[0].lower() == "sec-websocket-accept"), "")
            if actual != expected:
                raise RuntimeError("Invalid WebSocket accept digest")
            self.buffer = bytearray(remainder)
            self.sock.setblocking(False)
        except BaseException:
            self.sock.close()
            raise

    def close(self):
        self.sock.close()

    def _pong(self, payload):
        if len(payload) > 125:
            raise RuntimeError("Invalid oversized WebSocket ping")
        mask = os.urandom(4)
        packet = bytes((0x8A, 0x80 | len(payload))) + mask
        packet += bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.setblocking(True)
        self.sock.settimeout(2)
        try:
            self.sock.sendall(packet)
        finally:
            self.sock.setblocking(False)

    def _frame(self):
        b = self.buffer
        if len(b) < 2:
            return None
        final, opcode, masked = bool(b[0] & 0x80), b[0] & 15, bool(b[1] & 0x80)
        length, offset = b[1] & 127, 2
        if length in (126, 127):
            extra = 2 if length == 126 else 8
            if len(b) < offset + extra:
                return None
            length = int.from_bytes(b[offset:offset + extra], "big")
            offset += extra
        if length > 8 * 1024 * 1024:
            raise RuntimeError("Oversized ALVR WebSocket frame")
        mask = bytes(b[offset:offset + 4]) if masked else None
        offset += 4 if masked else 0
        if len(b) < offset + length:
            return None
        payload = bytes(b[offset:offset + length])
        del b[:offset + length]
        if mask:
            payload = bytes(v ^ mask[i % 4] for i, v in enumerate(payload))
        return final, opcode, payload

    def event(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            frame = self._frame()
            if frame is not None:
                final, opcode, payload = frame
                if opcode == 8:
                    raise ConnectionError("ALVR closed the event stream")
                if opcode == 9:
                    self._pong(payload)
                    continue
                if opcode == 10:
                    continue
                if opcode in (1, 2):
                    self.fragment_opcode = opcode
                    self.fragment = bytearray(payload)
                elif opcode == 0 and self.fragment_opcode is not None:
                    self.fragment.extend(payload)
                else:
                    raise RuntimeError(f"Unexpected WebSocket opcode {opcode}")
                if len(self.fragment) > 8 * 1024 * 1024:
                    raise RuntimeError("Oversized ALVR event message")
                if final:
                    message = bytes(self.fragment)
                    is_text = self.fragment_opcode == 1
                    self.fragment_opcode = None
                    self.fragment.clear()
                    if is_text:
                        return json.loads(message)
                continue
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.sock], [], [], remaining)[0]:
                return None
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("ALVR disconnected its event stream")
            self.buffer.extend(data)


def session_flags(ws, port, expected=None):
    post("GetSession", port)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        event = ws.event(max(0.01, deadline - time.monotonic()))
        if event and event.get("event_type", {}).get("id") == "Session":
            ws.last_session = event["event_type"]["data"]
            logging = event["event_type"]["data"]["session_settings"]["extra"]["logging"]
            result = {key: logging[key] for key in FLAGS}
            if not all(type(value) is bool for value in result.values()):
                raise ValueError("Original ALVR logging flags are not boolean")
            if expected is None or result == expected:
                return result
    raise TimeoutError("No ALVR Session event matching the requested logging flags")


