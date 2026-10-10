"""Projecta Intelli-RV / TBB Power BLE protocol ("FE" frames) - frame codec.

Reverse-engineered from the IntelliJay PM200 & 300BTJ app v1.2.6 and confirmed
byte-for-byte against an Android btsnoop capture of the app talking to a PM335.
See references/protocol.md in this skill.

Wire format (one GATT write stream / notification stream):

    FE | esc(u16be(len(esc(body)))) | esc(body) | FE
    esc():  FE -> FD 01,  FD -> FD 00

    body (header format 0x12 = 18, what the app uses for everything):
        00 12 | 02 id id id | 04 ED | 00 00 00 00 | ts32 | u16be(msgType) | payload | crc16
        02 = source "app"; id = 3 random bytes, echoed in the PM's reply; ts32 = Unix seconds

    body (header format 9, also understood by the app code, not seen on the wire):
        00 09 | 02 id id id | u16be(msgType) | payload | crc16

    crc16 = CRC-16/MODBUS over body-without-crc, appended low byte first.
"""
from __future__ import annotations

import os
import struct
import time
from dataclasses import dataclass, field

# --- BLE ---------------------------------------------------------------------
SERVICE_UUID = "00000000-00b7-4807-beee-e0b0879cf3dd"
WRITE_UUID = "00000001-00b7-4807-beee-e0b0879cf3dd"
NOTIFY_UUID = "00000002-00b7-4807-beee-e0b0879cf3dd"

# --- message types (QUa enum -> u16) -----------------------------------------
MSG = {
    "connectSend": 0x10, "connectResponse": 0x11,
    "realDataSend": 0x20, "realDataResponse": 0x21,
    "configSend": 0x30, "configResponse": 0x31,
    "controlSend": 0x40, "controlResponse": 0x41,
    "querySend": 0x50, "queryResponse": 0x51,
    "renewalSend": 0x60, "renewalResponse": 0x61,
    "timeSend": 0x90, "timeResponse": 0x91,
    "queryPropertySend": 0xA0, "queryPropertyResponse": 0xA1,
    "queryPropertyTreeSend": 0xA2, "queryPropertyTreeResponse": 0xA3,
    "heartbeatSend": 0xB0, "heartbeatResponse": 0xB1,
    "setPasswordSend": 0xC0, "setPasswordResponse": 0xC1,
    "advancedPasswordSend": 0xC2, "advancedPasswordResponse": 0xC3,
}
MSG_NAME = {v: k for k, v in MSG.items()}

# --- PM300-series property IDs (from the app's load/relay table) -------------
PROP = {
    "system_status_first": 0x0104, "system_status_last": 0x0109,
    "power_switch": 0x0109,          # CONTROL_POWER_SWITCH
    "ceiling_light": 0x0118,
    "bedroom_light": 0x0119,
    "dining_light": 0x011A,
    "water_pump": 0x011B,            # CONTROL_WATER_PUMP
    "tv": 0x011C,
    "outside_light": 0x011D,         # (also used by CONTROL_WATER_PUMP on some models)
    "switch_a": 0x011E,              # CONTROL_HEAT (hot water) uses this
    "switch_b": 0x011F,
    "device_tree": 0x0200,           # QUERY_DEVICE_TREE (System address, 0200..0200)
    "night_mode": 0x0207,            # CONTROL_SILENT
    "login": 0x1000,                 # LOGIN / EMPTY_LOGIN (address FFFF00)
}

HDR_V9 = 9
HDR_V18 = 0x12
APP_SRC_ID = 0x02  # TUa.app
DEFAULT_PROTO_VERSION = 0x04ED  # what the app sends (capture)


# --- CRC-16/MODBUS -------------------------------------------------------------
def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def crc_bytes(data: bytes) -> bytes:
    return crc16_modbus(data).to_bytes(2, "little")


# --- byte stuffing ---------------------------------------------------------------
def escape(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b == 0xFE:
            out += b"\xFD\x01"
        elif b == 0xFD:
            out += b"\xFD\x00"
        else:
            out.append(b)
    return bytes(out)


def unescape(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0xFD and i + 1 < len(data):
            out.append(0xFE if data[i + 1] == 0x01 else 0xFD)
            i += 2
        else:
            out.append(b)
            i += 1
    return bytes(out)


# --- payload cipher (control payloads, key = 4 bytes from CONNECT response) ------
def cipher(data: bytes, key: bytes) -> bytes:
    """App's sub_80abac: out = ~((((b ^ k0) + k1) - k2) ^ k3) & 0xFF."""
    if len(key) < 4:
        return b""
    k0, k1, k2, k3 = key[:4]
    return bytes((~((((b ^ k0) + k1 - k2) & 0xFF) ^ k3)) & 0xFF for b in data)


def decipher(data: bytes, key: bytes) -> bytes:
    k0, k1, k2, k3 = key[:4]
    out = bytearray()
    for c in data:
        t = (~c & 0xFF) ^ k3
        out.append(((t + k2 - k1) & 0xFF) ^ k0)
    return bytes(out)


# --- payload builders --------------------------------------------------------------
def addr_bytes(addr: str | bytes) -> bytes:
    return bytes.fromhex(addr) if isinstance(addr, str) else bytes(addr)


def u16(v: int) -> bytes:
    return struct.pack(">H", v & 0xFFFF)


def connect_payload() -> bytes:
    return bytes(64)  # confirmed by capture


def query_payload(addr: str | bytes, first_prop: int, last_prop: int) -> bytes:
    """pPa.toBytes(): 05 | addr | firstProp | addr | lastProp (not encrypted)."""
    a = addr_bytes(addr)
    return b"\x05" + a + u16(first_prop) + a + u16(last_prop)


def control_payload(addr: str | bytes, prop: int, value: bytes, key: bytes | None) -> bytes:
    """jSa.toBytes(): 05 | addr | prop | len(value) | value, then cipher() if key given."""
    raw = b"\x05" + addr_bytes(addr) + u16(prop) + bytes([len(value)]) + value
    return cipher(raw, key) if key else raw


def login_value(user: str = "user", password: str = "") -> bytes:
    """Login control value (capture): user[20] '&' password[20] -> 41 bytes.
    Sent as control to FFFF00 / 0x1000; reply 00 00 = OK, 00 05 = rejected."""
    u = user.encode()[:20].ljust(20, b"\0")
    p = password.encode()[:20].ljust(20, b"\0")
    return u + b"&" + p


# --- frame encode -----------------------------------------------------------------
@dataclass
class Codec:
    header_format: int = HDR_V18
    proto_version: int = DEFAULT_PROTO_VERSION

    def build_body(self, msg_type: int, payload: bytes, header_format: int | None = None,
                   msg_id: bytes | None = None, ts: int | None = None) -> bytes:
        """Header (confirmed from a btsnoop capture of the app, 2026-10-05):
        fmt 18: 00 12 | 02 id id id | 04 ED | 00 00 00 00 | ts32 | type16 | payload | crc
        The PM echoes the 3-byte message id in its reply."""
        fmt = self.header_format if header_format is None else header_format
        ts = int(time.time()) & 0xFFFFFFFF if ts is None else ts
        msg_id = bytes(x % 255 for x in os.urandom(3)) if msg_id is None else msg_id
        body = bytearray(u16(fmt))
        body += bytes([APP_SRC_ID]) + msg_id
        if fmt == HDR_V18:
            body += u16(self.proto_version) + bytes(4) + struct.pack(">I", ts)
        body += u16(msg_type)
        body += payload
        body += crc_bytes(bytes(body))
        return bytes(body)

    def encode(self, msg_type: int, payload: bytes, header_format: int | None = None) -> bytes:
        esc_body = escape(self.build_body(msg_type, payload, header_format))
        return b"\xFE" + escape(u16(len(esc_body))) + esc_body + b"\xFE"


# --- frame decode -------------------------------------------------------------------
@dataclass
class Frame:
    raw: bytes
    body: bytes
    crc_ok: bool
    fmt: int | None = None
    header: bytes = b""
    msg_type: int | None = None
    payload: bytes = b""

    @property
    def msg_name(self) -> str:
        return MSG_NAME.get(self.msg_type, f"0x{self.msg_type:04X}" if self.msg_type is not None else "?")

    def __str__(self) -> str:
        return (f"[{'CRC ok' if self.crc_ok else 'CRC BAD'}] fmt={self.fmt} hdr={self.header.hex(' ')} "
                f"type={self.msg_name} payload({len(self.payload)})={self.payload.hex(' ')}")


def parse_body(raw: bytes, body: bytes) -> Frame:
    crc_ok = len(body) >= 4 and crc_bytes(body[:-2]) == body[-2:]
    f = Frame(raw=raw, body=body, crc_ok=crc_ok)
    if len(body) < 6:
        return f
    f.fmt = struct.unpack(">H", body[:2])[0]
    hdr_len = {HDR_V9: 4, HDR_V18: 14}.get(f.fmt)
    if hdr_len is None or len(body) < 2 + hdr_len + 2 + 2:
        return f
    f.header = body[2:2 + hdr_len]
    f.msg_type = struct.unpack(">H", body[2 + hdr_len:4 + hdr_len])[0]
    f.payload = body[4 + hdr_len:-2]
    return f


@dataclass
class StreamDecoder:
    """Feed notification chunks; yields complete frames (FE-delimited)."""
    buf: bytearray = field(default_factory=bytearray)

    def feed(self, chunk: bytes) -> list[Frame]:
        self.buf += chunk
        frames = []
        while True:
            try:
                start = self.buf.index(0xFE)
            except ValueError:
                self.buf.clear()
                break
            try:
                end = self.buf.index(0xFE, start + 1)
            except ValueError:
                del self.buf[:start]
                break
            inner = bytes(self.buf[start + 1:end])
            if not inner:  # back-to-back FE: treat second FE as a new start
                del self.buf[:start + 1]
                continue
            del self.buf[:end + 1]
            un = unescape(inner)
            if len(un) < 2:
                continue
            frames.append(parse_body(b"\xFE" + inner + b"\xFE", un[2:]))
        return frames


def _selftest() -> None:
    assert crc16_modbus(b"123456789") == 0x4B37
    assert unescape(escape(bytes(range(256)))) == bytes(range(256))
    key = bytes([0x12, 0x34, 0x56, 0x78])
    assert decipher(cipher(bytes(range(256)), key), key) == bytes(range(256))
    c = Codec()
    for fmt in (HDR_V9, HDR_V18):
        payload = bytes([0xFE, 0xFD, 0x00, 0x01, 0x05])
        wire = c.encode(MSG["controlSend"], payload, fmt)
        d = StreamDecoder()
        got = d.feed(wire[:7]) + d.feed(wire[7:])
        assert len(got) == 1 and got[0].crc_ok and got[0].fmt == fmt, got
        assert got[0].msg_type == 0x40 and got[0].payload == payload, got[0]
        assert wire.count(0xFE) == 2
    # A CONNECT frame exactly as the app sent it (captured): id 398b48, ts 0x6ac374c3
    body = c.build_body(MSG["connectSend"], connect_payload(), HDR_V18, bytes.fromhex("398b48"), 0x6AC374C3)
    wire = b"\xFE" + escape(u16(len(escape(body)))) + escape(body) + b"\xFE"
    assert wire == bytes.fromhex("fe0054001202398b4804ed000000006ac374c30010" + "00" * 64 + "f755fe")
    # Pump-on control (addr 000B00, prop 011D, value 0001) with the all-zero key, as captured
    assert cipher(control_payload("000b00", 0x011D, u16(1), None), bytes(4)).hex() == "fafff4fffee2fdfffe"
    print("selftest ok (matches captured frames)")


if __name__ == "__main__":
    _selftest()
