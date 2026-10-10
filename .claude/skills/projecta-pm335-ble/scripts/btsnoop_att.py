"""Extract ATT traffic for one BLE peer from an Android btsnoop_hci.log.

    python btsnoop_att.py <btsnoop_hci.log> <peer-mac>      e.g. AA:BB:CC:DD:EE:FF

Prints connection events, SMP and ATT PDUs (writes, notifications, reads,
CCCD/MTU) with handles, so the app's protocol can be read off directly.
"""
import struct
import sys
from datetime import datetime, timedelta

if len(sys.argv) < 3:
    sys.exit(__doc__)
PEER = sys.argv[2].upper()
EPOCH = datetime(1, 1, 1) + timedelta(0)  # btsnoop ts: us since 0000-01-01 (AD)

ATT_OPS = {
    0x01: "ERR", 0x02: "MTU_REQ", 0x03: "MTU_RSP", 0x04: "FIND_INFO_REQ", 0x05: "FIND_INFO_RSP",
    0x08: "READ_TYPE_REQ", 0x09: "READ_TYPE_RSP", 0x0A: "READ_REQ", 0x0B: "READ_RSP",
    0x10: "READ_GROUP_REQ", 0x11: "READ_GROUP_RSP", 0x12: "WRITE_REQ", 0x13: "WRITE_RSP",
    0x1B: "NOTIFY", 0x1D: "INDICATE", 0x1E: "CONFIRM", 0x52: "WRITE_CMD",
}


def records(path):
    with open(path, "rb") as f:
        hdr = f.read(16)
        assert hdr[:8] == b"btsnoop\0", "not a btsnoop file"
        while True:
            h = f.read(24)
            if len(h) < 24:
                return
            orig, incl, flags, drops, ts = struct.unpack(">IIIIq", h)
            yield flags, ts, f.read(incl)


def mac(b):
    return ":".join(f"{x:02X}" for x in reversed(b))


def main():
    path = sys.argv[1]
    handles = {}          # acl handle -> peer mac
    reasm = {}            # (handle, dir) -> partial l2cap
    t0 = None
    for flags, ts, d in records(path):
        if not d:
            continue
        sent = (flags & 1) == 0          # 0 = host->controller
        t = ts / 1e6
        t0 = t0 or t
        rel = f"{t - t0:9.3f}"
        typ = d[0]
        if typ == 0x04 and len(d) > 3:   # HCI event
            ev, plen = d[1], d[2]
            p = d[3:]
            if ev == 0x3E:               # LE meta
                sub = p[0]
                if sub in (0x01, 0x0A):  # connection complete / enhanced
                    status, hnd, role, atype = p[1], struct.unpack("<H", p[2:4])[0] & 0xFFF, p[4], p[5]
                    peer = mac(p[6:12])
                    handles[hnd] = peer
                    if peer == PEER:
                        print(f"{rel} CONNECT handle=0x{hnd:03x} status={status} role={role}")
            elif ev == 0x05:             # disconnection complete
                status, hnd, reason = p[0], struct.unpack("<H", p[1:3])[0] & 0xFFF, p[3]
                if handles.get(hnd) == PEER:
                    print(f"{rel} DISCONNECT handle=0x{hnd:03x} reason=0x{reason:02x}")
            elif ev == 0x08:             # encryption change
                hnd = struct.unpack("<H", p[1:3])[0] & 0xFFF
                if handles.get(hnd) == PEER:
                    print(f"{rel} ENCRYPTION status={p[0]} on={p[3]}")
        elif typ == 0x02 and len(d) >= 5:  # ACL
            hf = struct.unpack("<H", d[1:3])[0]
            hnd, pb = hf & 0xFFF, (hf >> 12) & 3
            if handles.get(hnd) != PEER:
                continue
            payload = d[5:]
            key = (hnd, sent)
            if pb in (0, 2):             # start
                reasm[key] = bytearray(payload)
            else:
                reasm.setdefault(key, bytearray()).extend(payload)
            buf = reasm[key]
            if len(buf) < 4:
                continue
            l2len, cid = struct.unpack("<HH", buf[:4])
            if len(buf) < 4 + l2len:
                continue
            pdu = bytes(buf[4:4 + l2len])
            del reasm[key]
            who = "APP->PM" if sent else "PM->APP"
            if cid == 4 and pdu:
                op = pdu[0]
                name = ATT_OPS.get(op, f"op{op:02x}")
                if op in (0x12, 0x52, 0x1B, 0x1D, 0x0A) and len(pdu) >= 3:
                    h = struct.unpack("<H", pdu[1:3])[0]
                    print(f"{rel} {who} {name:9} h=0x{h:04x} {pdu[3:].hex(' ')}")
                elif op in (0x0B,):
                    print(f"{rel} {who} {name:9} {pdu[1:].hex(' ')}")
                else:
                    print(f"{rel} {who} {name:9} {pdu[1:].hex(' ')}")
            elif cid == 6:
                print(f"{rel} {who} SMP       {pdu.hex(' ')}")


if __name__ == "__main__":
    main()
