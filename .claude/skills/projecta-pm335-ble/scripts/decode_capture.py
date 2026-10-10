"""Decode TBB "FE" frames from btsnoop_att.py output (APP->PM writes, PM->APP notifications).

    python btsnoop_att.py btsnoop_hci.log AA:BB:CC:DD:EE:FF > att.txt
    python decode_capture.py att.txt

Control payloads are shown decrypted with the key from the connect reply
(all-zero on the units seen so far). Note: login frames contain the user's
pairing key in clear once decrypted - don't share decoded captures.
"""
import os, re, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pm335 as p

MSG = {v: k for k, v in p.MSG.items()}
def unesc(b): return p.unescape(b)

def frames(lines):
    buf = {"APP->PM": bytearray(), "PM->APP": bytearray()}
    for ln in lines:
        m = re.match(r'\s*([\d.]+) (APP->PM|PM->APP) (WRITE_CMD|WRITE_REQ|NOTIFY)\s+h=0x(\w+) (.*)', ln)
        if not m:
            if re.search(r'CONNECT|DISCONNECT|ENCRYPTION', ln): yield ('evt', ln.strip())
            continue
        t, who, op, h, data = m.groups()
        if h == '0021': continue           # CCCD
        b = bytes.fromhex(data.replace(' ', ''))
        buf[who] += b
        while True:
            s = buf[who]
            try:
                i = s.index(0xFE); j = s.index(0xFE, i + 1)
            except ValueError:
                break
            inner = bytes(s[i + 1:j]); del s[:j + 1]
            if inner:
                yield ('frame', (t, who, unesc(inner)[2:]))

key = bytes(4)
for kind, x in frames(open(sys.argv[1])):
    if kind == 'evt': print(x); continue
    t, who, body = x
    crc_ok = p.crc_bytes(body[:-2]) == body[-2:]
    fmt = struct.unpack('>H', body[:2])[0]
    hl = 14 if fmt == 0x12 else 4
    hdr = body[2:2 + hl]; typ = struct.unpack('>H', body[2 + hl:4 + hl])[0]; pl = body[4 + hl:-2]
    name = MSG.get(typ, hex(typ))
    extra = ''
    if typ == 0x40 and who == 'APP->PM':
        dec = p.decipher(pl, key)
        extra = f"\n{'':22}decrypted: {dec.hex(' ')}"
    print(f"{t:>9} {who} fmt{fmt} id={hdr[1:4].hex()} {'' if crc_ok else 'CRC!'} {name:16} ({len(pl)}) {pl.hex(' ')}{extra}")
