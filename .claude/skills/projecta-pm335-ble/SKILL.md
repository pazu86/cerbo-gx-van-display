---
name: projecta-pm335-ble
description: Connect to and control a Projecta Intelli-RV / TBB Power RV power manager (PM335, PM300/PM400 series, IntelliJay app) over Bluetooth LE - login with the app's pairing key, read water tank levels, battery and switch status, and switch the water pump, hot water (HWS), silent mode or system power. Includes the reverse-engineered frame protocol (FE framing, CRC-16/MODBUS, login, queries, controls), a tested Python codec, a bleak client, ESP32/NimBLE notes and tools to decode an Android btsnoop capture of the app. Use this whenever someone wants to integrate a Projecta or TBB Power caravan/RV/camper power management system with Home Assistant, Node-RED, Victron Cerbo, an ESP32/ESPHome device or a script, mentions the IntelliJay app, or asks why their BLE connection to a Projecta unit drops, ignores commands or won't pair - even if they don't name the protocol.
---

# Projecta Intelli-RV (PM335) over Bluetooth

This skill gives you a working, verified protocol for talking to Projecta Intelli-RV power managers
(made by TBB Power) without the IntelliJay phone app. It was built against a PM335 and confirmed
byte-for-byte against a capture of the app; other PM200/300/400 models share the app and very
likely the protocol, but their property map may differ.

## What's here

| File | Use it for |
|---|---|
| `references/protocol.md` | The protocol: BLE services, frame format, session, queries, controls, property map, quirks. **Read this first** for any implementation work. |
| `references/implementation-notes.md` | Getting it right in code: request serialisation, switch-state UI, ESP32/NimBLE config and memory, bleak tips. |
| `references/capture-and-reverse.md` | When something isn't covered (another model or property): capturing the app with Android HCI snoop + adb, decoding, and how the app was reverse-engineered. |
| `scripts/pm335.py` | Codec: escape/unescape, CRC, frame build/parse, cipher, payload builders. `python pm335.py` self-tests against captured frames. |
| `scripts/pm335_client.py` | Ready-to-run bleak client: `scan`, `status`, `dump`, `switch pump|hotwater on|off`. |
| `scripts/btsnoop_att.py`, `scripts/decode_capture.py` | Turn an Android `btsnoop_hci.log` into readable, decrypted protocol frames. |

## The essentials (so you don't relearn them)

1. **It's not BLE pairing.** The "pairing key" the app asks for (shown on the Projecta LCD) is a
   login password sent inside the protocol (user `user`). Standard SMP pairing is ignored, and the
   unit drops any link that hasn't logged in within ~20 s.
2. **Frames:** `FE | escaped length | escaped body | FE`, body = header (format `00 12`, 3-byte message
   id that the reply echoes, version `04 ED`, timestamp) + type + payload + CRC-16/MODBUS (low byte
   first). Session: CONNECT (64 zero bytes) → login control → queries/controls, heartbeat every ~12 s.
3. **One request at a time.** The unit silently drops anything sent before it replied. Wait for each
   reply (match the message id) before the next request. This one rule caused ignored commands,
   blank status and "laggy" switches until it was found.
4. **Status lags the relays** by several seconds (the unit's own LCD lags too). Relays switch
   immediately; show the requested state optimistically and confirm later.
5. **Controls** go to the load-controller address from the device tree (`00 0B 00` on the PM335 seen):
   water pump `011D`, hot water `011E` (`0001`/`0000`), silent `0207` (`01`/`00`). System power is
   `FFFF00` `0109` - it switches the 12 V loads off, possibly including whatever is running your
   integration.
6. **Tank levels**: load-controller block `0100-0129`, data bytes 36/38/40 (u16 %). Pump state byte 61,
   hot water byte 63.

## Typical tasks

- **"Read my tanks / switch the pump from a script"** - run `scripts/pm335_client.py` (needs `pip
  install bleak`). Ask the user for the unit's BLE address (`scan`) and the pairing key; pass it via
  `--password` or the `PM335_PASSWORD` environment variable rather than writing it into files.
- **ESP32 / ESPHome / Arduino bridge** - port `pm335.py` (it's small) and follow
  `implementation-notes.md`. Publish values over MQTT/HTTP from a separate task.
- **Home Assistant / Node-RED** - a bridge (ESP32 or a Pi running the client) that exposes tank % and
  switch entities; serialise requests and poll every few seconds.
- **A different model or an unmapped value** - capture the app (`capture-and-reverse.md`), diff the
  query replies around a known change, and extend the property map.

## Working safely with real equipment

The pump, hot water system and system power are real loads in someone's van. Treat switching them
as the user's call: build the controls, but let the user trigger them (or get an explicit go-ahead
for each test) - a hot water system without water flow, or cutting power to the device running the
integration, are easy mistakes. Read-only queries are harmless.

Keep the pairing key and any captures private: login frames contain the key, and the unit's MAC and
serial identify the owner's van. Don't commit them to source control or bake them into firmware.
