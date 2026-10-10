# Projecta Intelli-RV (TBB Power) BLE protocol

Confirmed byte-for-byte from an Android btsnoop capture of the **IntelliJay PM200 & 300BTJ** app
(v1.2.6) controlling a **PM335**, plus static analysis of the same app. Items marked *(app code)*
come from the app's code but weren't seen on the wire.

## Contents
1. BLE transport
2. Frame format
3. Message types
4. Session: connect, login, heartbeat
5. Queries and replies
6. Controls
7. Known property map (PM335)
8. Behaviour that matters
9. The other (legacy) protocol in the app

## 1. BLE transport

The app knows three BLE module "profiles" (service / notify / write). The PM335 seen used the TI one:

| Profile | Service | Notify | Write |
|---|---|---|---|
| TI module (seen on PM335) | `0x1000` | `0x1002` (read, notify) | `0x1001` (read, write, write-without-response) |
| TBB module *(app code)* | `00000000-00B7-4807-BEEE-E0B0879CF3DD` | `…0002…` | `…0001…` |
| A002 module *(app code)* | `0xA002` | `0xC305` | `0xC304` |

The TI module also has `0x1003` (write), `0x1004` (read), `0x1005` (read/write) and TI's OAD service
`f000ffd0-0451-4000-b000-000000000000`; the app doesn't use them.

- **No BLE pairing / SMP.** The unit ignores SMP pairing requests. The "pairing key" the app asks for
  (shown on the Projecta LCD) is an application-level **login password** (section 4).
- Enable notifications on the notify characteristic (CCCD = `01 00`), raise the MTU (the app gets 251),
  write frames with **write-without-response** like the app. Chunk writes to MTU-3 if needed.
- The unit accepts **one connection at a time**; a connected phone app stops it advertising.
- Without a valid login the unit **drops the link ~20 s** after connecting.
- Advertised name is serial-like (e.g. `CG` + digits); the app's scan list shows the MAC.

## 2. Frame format

```
FE | esc(u16be(len(esc(body)))) | esc(body) | FE
esc():  FE -> FD 01,   FD -> FD 00
```

`body` (header **format 18 = 0x12**, used by the app for everything):

```
00 12 | 02 | id id id | 04 ED | 00 00 00 00 | ts32 | type16 | payload | crc16
```

- `02` = source "app" (server 0, microcontroller 1, app 2).
- `id` = 3 random bytes; **the reply echoes them** - use them to match replies to requests.
- `04 ED` = protocol version the app sends. `ts32` = Unix seconds (big-endian).
- Replies carry their own values in the 4 bytes after the version (e.g. `00 00 00 67`); ignore them.
- `crc16` = **CRC-16/MODBUS** (init 0xFFFF, poly 0xA001 reflected) over the body before it,
  appended **low byte first**.
- Format 9 *(app code)*: `00 09 | 02 id id id | type16 | payload | crc16` (no version/timestamp).

A frame can span several notifications and several frames can share one; split on `FE`, unescape,
drop the 2-byte length, check the CRC.

## 3. Message types (u16)

| Request | Reply | Meaning |
|---|---|---|
| `0x0010` | `0x0011` | connect |
| `0x0040` | `0x0041` | control (also used for login) |
| `0x0050` | `0x0051` | query (read properties) |
| `0x00B0` | `0x00B1` | heartbeat |

*(app code)* also: realData 0x20, config 0x30, renewal 0x60, time 0x90, queryProperty 0xA0,
queryPropertyTree 0xA2, setPassword 0xC0, advancedPassword 0xC2, records 0xD0-0xDF, file transfer 0xE0-0xE3.

## 4. Session

1. **Connect** (`0x10`): payload **64 zero bytes**. Reply (`0x11`): 6 bytes, all zero on units seen.
   *(app code)* the app takes the cipher key from the reply data (bytes 2-5 here) - all zero, so the
   cipher reduces to a bitwise NOT.
2. **Login**: a control (`0x40`) to address `FF FF 00`, property `10 00`, value (41 bytes):
   `user[20] '&' password[20]` - UTF-8, zero-padded. The app uses user **`user`** and the pairing
   key as the password. Reply `0x41` payload `00 00` = accepted, `00 05` = rejected (e.g. blank login).
3. **Heartbeat** (`0xB0`) payload `00 01` every ~12 s; reply `00 00`.
4. Poll status (section 5) every few seconds; send controls when needed.

## 5. Queries

Request payload (not encrypted):

```
05 | addr3 | first16 | addr3 | last16        e.g. 05 FF FF 00 01 04 FF FF 00 01 09
```

Reply payload:

```
result16 | addr3 | first16 | count16 | data          result 00 00 = ok
```

Property values are packed back to back; most are u16 big-endian, some are 1 or 4 bytes or blocks
(widths are hard-coded in the app). Lengths seen for the app's polls:

| Address | Range | Count | Data bytes |
|---|---|---|---|
| FFFF00 | 0110 | 1 | 4 |
| FFFF00 | 0002-0009 | 8 | 20 (starts with a 4-byte Unix time) |
| FFFF00 | 0104-0109 | 6 | 12 |
| *controller* | 0100-0129 | 42 | 88 |
| FFFF00 | 010F | 1 | 1 |
| FFFF00 | 010E | 1 | 100 |
| FFFF00 | 0101-0103 | 3 | 8 |
| FFFF00 | 0200 | 1 | ~256 - device tree |

**Device tree** (`FFFF00` prop `0200`): fixed-size entries with an ASCII serial/name, a 3-byte
address and a firmware string (e.g. `Omega4060 …`, `Omega2000 …`, `Kappa4000 …`). The load
controller (the `Omega2000` entry on a PM335) is the address controls go to - `00 0B 00` on the
unit this was built on; read the tree rather than assuming.

## 6. Controls

```
payload = cipher( 05 | addr3 | prop16 | len8 | value )
cipher(b) = ~((((b ^ k0) + k1) - k2) ^ k3) & 0xFF        key k0..k3 from the connect reply (0 -> NOT)
```

Reply `0x41`: `00 00` = accepted (sent within ~0.1-0.4 s). The relay switches immediately; the
unit's *reported* status (and its own LCD) catches up several seconds later.

## 7. Known property map (PM335)

Controls (all to the **load controller** address, except power):

| Function | Prop | On / Off |
|---|---|---|
| Water pump | `011D` | `00 01` / `00 00` |
| Hot water (HWS) | `011E` | `00 01` / `00 00` |
| Silent / night mode | `0207` | `01` / `00` |
| System power (loads) | `0109` @ **FFFF00** | `01` / `00` - powers the loads off, including anything on them |

*(app code)* PM300-series load table: 0118 ceiling light, 0119 bedroom light, 011A dining light,
011B water pump (other models), 011C TV, 011D outside light / pump, 011E switch A (hot water),
011F switch B, 0207 night mode; 0209 PV clear protection; 8002 reset battery; 8004 calibrate battery.

Status (byte offsets into the reply **data**):

| Source | Offset | Meaning |
|---|---|---|
| controller 0100-0129 | 36, 38, 40 | Tank 1, 2, 3 level % (u16) |
| controller 0100-0129 | 61 | water pump on (1/0) - byte 43 follows it too |
| controller 0100-0129 | 63 | hot water on (1/0) |
| controller 0100-0129 | 12/16 | voltages ×0.01 (u16); 14/18/34 look like currents ×0.01 (s16) |
| controller 0100-0129 | 46 | temperature ×0.1 °C (likely) |
| FFFF00 0104-0109 | 0 | battery voltage ×0.01 V (u16) |
| FFFF00 0104-0109 | 2 | battery current (u16/s16) |
| FFFF00 0104-0109 | 6 | state of charge % (u8) |
| FFFF00 0104-0109 | last byte | system power on (1/0) |

Tank and switch offsets were matched against the Projecta LCD and the app's buttons; the rest are
best guesses - verify against the LCD before relying on them.

## 8. Behaviour that matters

- **One request at a time.** The unit silently drops a request that arrives before it has replied
  to the previous one (seen: a hot-water command sent right after a pump command was ignored; status
  queries sent straight after a control never got answered). Send, wait for the reply with the same
  id (~70-350 ms, allow ~1.2 s), then send the next.
- **Reported status lags the relays** by several seconds (up to ~10 s). If you show switch state,
  show the requested state immediately and only accept a *fresh* reading that matches it after a
  hold-off; otherwise a late reading of the old state flips the UI back.
- **Hot water needs the pump** - switch the pump on first; when turning the pump off, turn hot water
  off too.
- The link survives idle periods with heartbeats; it drops ~20 s after connect without a login.

## 9. The other (legacy) protocol in the app

The app also contains a Modbus-like `7E FF` protocol for RS-485 devices and older modules:
`7E FF addr func reg len payload crc16` (func 03 read / 06 write, `len` = whole frame incl. CRC,
CRC-16/MODBUS). Addresses: panel 0x00, SP 0x40, ZCM6 0xE0; a "check code" login is
`7E FF 40 06 E1 len 00 <password ascii> crc`. **The PM335 over BLE did not answer any of it** - it
uses the FE protocol above. Listed only so nobody repeats that detour.
