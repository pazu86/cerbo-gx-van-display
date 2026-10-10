"""Minimal Python (bleak) client for a Projecta Intelli-RV PM-series unit (e.g. PM335).

    pip install bleak
    python pm335_client.py scan
    python pm335_client.py status  <BLE-ADDRESS>                 # tanks, pump, hot water, battery
    python pm335_client.py switch  <BLE-ADDRESS> pump on|off
    python pm335_client.py switch  <BLE-ADDRESS> hotwater on|off
    python pm335_client.py dump    <BLE-ADDRESS>                 # every status query, raw hex

The login password is the "pairing key" the IntelliJay app asks for (shown on the
Projecta LCD). Pass it with --password or the PM335_PASSWORD environment variable.
Close the phone app first: the unit accepts one BLE connection at a time.

Implements the protocol in references/protocol.md. The rule that matters most:
the unit handles ONE request at a time and silently drops anything sent before it
has replied, so every request here waits for its reply (matched by message id).
"""
from __future__ import annotations

import argparse
import asyncio
import os
import struct

from bleak import BleakClient, BleakScanner

import pm335 as p

# Known BLE module profiles (the app's DUa class): service, notify, write
PROFILES = {
    "tbb": ("00000000-00b7-4807-beee-e0b0879cf3dd", "00000002-00b7-4807-beee-e0b0879cf3dd",
            "00000001-00b7-4807-beee-e0b0879cf3dd"),
    "1000": ("00001000-0000-1000-8000-00805f9b34fb", "00001002-0000-1000-8000-00805f9b34fb",
             "00001001-0000-1000-8000-00805f9b34fb"),
    "a002": ("0000a002-0000-1000-8000-00805f9b34fb", "0000c305-0000-1000-8000-00805f9b34fb",
             "0000c304-0000-1000-8000-00805f9b34fb"),
}

SYSTEM = "ffff00"
DEFAULT_CTL = "000b00"   # the load controller ("Omega2000") on the unit this was built on - check the device tree

# The app's status polling (FFFF00 = system; CTL = load controller from the device tree)
POLLS = [("ctl", 0x0100, 0x0129), (SYSTEM, 0x0104, 0x0109), (SYSTEM, 0x0110, 0x0110), (SYSTEM, 0x0002, 0x0009),
         (SYSTEM, 0x010F, 0x010F), (SYSTEM, 0x010E, 0x010E), (SYSTEM, 0x0101, 0x0103)]

SWITCHES = {"pump": 0x011D, "hotwater": 0x011E}


class PM:
    def __init__(self, client: BleakClient):
        self.client = client
        self.codec = p.Codec(header_format=p.HDR_V18)
        self.dec = p.StreamDecoder()
        self.waiting: dict[bytes, asyncio.Future] = {}
        self.key = bytes(4)
        self.write_uuid = self.notify_uuid = None

    async def start(self) -> None:
        svcs = {s.uuid.lower() for s in self.client.services}
        for name, (svc, notify, write) in PROFILES.items():
            if svc in svcs:
                self.notify_uuid, self.write_uuid = notify, write
                print(f"module profile: {name}")
                break
        else:
            raise SystemExit(f"no known PM service; found {sorted(svcs)}")
        await self.client.start_notify(self.notify_uuid, self._on_notify)

    def _on_notify(self, _c, data: bytearray) -> None:
        for fr in self.dec.feed(bytes(data)):
            if not fr.crc_ok or len(fr.body) < 6:
                continue
            fut = self.waiting.pop(fr.body[3:6], None)   # replies echo the request's message id
            if fut and not fut.done():
                fut.set_result(fr)

    async def request(self, msg_type: int, payload: bytes, timeout: float = 1.5) -> p.Frame | None:
        """Send one frame and wait for its reply - never overlap requests."""
        body = self.codec.build_body(msg_type, payload)
        wire = b"\xFE" + p.escape(p.u16(len(p.escape(body)))) + p.escape(body) + b"\xFE"
        fut = asyncio.get_running_loop().create_future()
        self.waiting[body[3:6]] = fut
        step = max(20, getattr(self.client, "mtu_size", 23) - 3)
        for i in range(0, len(wire), step):
            await self.client.write_gatt_char(self.write_uuid, wire[i:i + step], response=False)
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self.waiting.pop(body[3:6], None)
            return None

    async def login(self, user: str, password: str) -> None:
        r = await self.request(p.MSG["connectSend"], p.connect_payload())
        if not r:
            raise SystemExit("no reply to CONNECT")
        if len(r.payload) >= 6:
            self.key = r.payload[2:6]   # all zero on units seen so far
        r = await self.request(p.MSG["controlSend"],
                               p.control_payload(SYSTEM, 0x1000, p.login_value(user, password), self.key))
        code = struct.unpack(">H", r.payload[:2])[0] if r and len(r.payload) >= 2 else None
        if code != 0:
            raise SystemExit(f"login rejected (reply {code}) - check the pairing key")
        print("logged in")

    async def query(self, addr: str, first: int, last: int) -> tuple[int, bytes] | None:
        r = await self.request(p.MSG["querySend"], p.query_payload(addr, first, last))
        if not r or len(r.payload) < 9:
            return None
        count = struct.unpack(">H", r.payload[7:9])[0]
        return count, r.payload[9:]   # result16 | addr3 | first16 | count16 | data

    async def control(self, addr: str, prop: int, value: bytes) -> bool:
        r = await self.request(p.MSG["controlSend"], p.control_payload(addr, prop, value, self.key))
        return bool(r and r.payload[:2] == b"\x00\x00")

    async def heartbeat(self) -> None:
        await self.request(p.MSG["heartbeatSend"], b"\x00\x01")


def decode_status(ctl: bytes | None, system: bytes | None) -> dict:
    """Field offsets mapped on a PM335 against its LCD and the app's buttons."""
    out = {}
    if ctl and len(ctl) >= 64:
        out["tanks_pct"] = [struct.unpack(">H", ctl[36 + i * 2:38 + i * 2])[0] for i in range(3)]
        out["pump_on"] = bool(ctl[61])
        out["hot_water_on"] = bool(ctl[63])
    if system and len(system) >= 12:
        out["battery_v"] = struct.unpack(">H", system[0:2])[0] / 100
        out["battery_soc_pct"] = system[6]
        out["power_on"] = bool(system[-1])
    return out


async def run(args) -> None:
    if args.cmd == "scan":
        found = await BleakScanner.discover(timeout=10, return_adv=True)
        for d, adv in sorted(found.values(), key=lambda x: -(x[1].rssi or -999)):
            print(f"{d.address}  rssi={adv.rssi:4}  name={adv.local_name or d.name!r}")
        print("PM units usually advertise a serial-style name; the app lists them with their MAC.")
        return

    password = args.password or os.environ.get("PM335_PASSWORD", "")
    async with BleakClient(args.device, timeout=20) as client:
        pm = PM(client)
        await pm.start()
        await pm.login(args.user, password)
        ctl_addr = args.ctl

        if args.cmd == "switch":
            on = args.state == "on"
            prop = SWITCHES[args.what]
            # The hot water system needs the pump: pump on first; pump off takes hot water with it
            if args.what == "hotwater" and on:
                print("pump on:", await pm.control(ctl_addr, SWITCHES["pump"], p.u16(1)))
            if args.what == "pump" and not on:
                print("hot water off:", await pm.control(ctl_addr, SWITCHES["hotwater"], p.u16(0)))
            print(f"{args.what} {args.state}:", await pm.control(ctl_addr, prop, p.u16(1 if on else 0)))
            print("(the unit's reported status lags its relays by several seconds)")
            return

        results = {}
        for who, first, last in POLLS:
            addr = ctl_addr if who == "ctl" else who
            results[(addr, first)] = await pm.query(addr, first, last)
            if args.cmd == "dump":
                r = results[(addr, first)]
                print(f"{addr} {first:04X}-{last:04X}: " + (f"n={r[0]} {r[1].hex(' ')}" if r else "no reply"))
        tree = await pm.query(SYSTEM, 0x0200, 0x0200)
        if args.cmd == "dump" and tree:
            print("device tree:", tree[1].hex(" "))
        ctl = results.get((ctl_addr, 0x0100))
        system = results.get((SYSTEM, 0x0104))
        print(decode_status(ctl[1] if ctl else None, system[1] if system else None))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--user", default="user", help="login user (the app uses 'user')")
    ap.add_argument("--password", help="the app's pairing key (or env PM335_PASSWORD)")
    ap.add_argument("--ctl", default=DEFAULT_CTL, help="load controller address (hex, from the device tree)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan")
    for name in ("status", "dump", "switch"):
        sp = sub.add_parser(name)
        sp.add_argument("device")
        if name == "switch":
            sp.add_argument("what", choices=sorted(SWITCHES))
            sp.add_argument("state", choices=["on", "off"])
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
