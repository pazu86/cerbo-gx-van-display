# Capturing and decoding the app's Bluetooth traffic

Use this when something isn't covered by protocol.md (another model, another property, a setting).
A capture of the real app beats static analysis: on this project several details read from the
decompiled app (header layout, connect payload, protocol version, login format) were wrong until a
capture showed the real frames.

## 1. Capture with an Android device

1. Install the same app on an Android phone/tablet and make sure it connects to the unit.
2. Settings → About → tap **Build number** 7× → **Developer options** → **Enable Bluetooth HCI snoop
   log** (choose *Enabled*/full). Toggle Bluetooth off/on so logging starts.
3. Disconnect other clients (turn Bluetooth off on other phones - iOS keeps the link after the app is
   closed). The unit accepts one connection.
4. In the app: connect, enter the pairing key, wait ~10 s, then press each control once with a few
   seconds between, noting the order. Note what the unit's LCD shows (tank %, volts) for mapping.
5. Get the log - on Android 10+ it's only in a bug report:
   - Developer options → **USB debugging** on, plug into a PC, accept the prompt, then
     `adb bugreport bugreport.zip` (Android platform-tools), or *Take bug report* on the device.
   - Inside the zip: `FS/data/misc/bluetooth/logs/btsnoop_hci.log` (and `.last` from the previous
     Bluetooth session).
6. Treat the capture as private: login frames contain the pairing key.

## 2. Decode

```
python scripts/btsnoop_att.py btsnoop_hci.log AA:BB:CC:DD:EE:FF > att.txt   # ATT writes/notifications for that MAC
python scripts/decode_capture.py att.txt                                     # FE frames, types, decrypted controls
```

To map a control to a status byte, diff consecutive query replies around each button press - bytes
that flip 0→1→0 in step with the presses are the state flags (that's how pump = byte 61 and hot
water = byte 63 were found).

## 3. Static analysis of the app (if needed)

The IntelliJay app is Flutter; the protocol lives in Dart code in `lib/arm64-v8a/libapp.so`
(the Java side is only a generic BLE plugin, `com.tbbpower.plugin.method/ble_lib`).

- Get an arm64 build (APK/XAPK split `config.arm64_v8a.apk`).
- Decompile with **Blutter** (github.com/worawit/blutter); on Windows it needs Visual Studio Build
  Tools (C++ + CMake), runs from the x64 Native Tools environment, and builds a matching Dart VM.
- v1.2.6 is **obfuscated**: Blutter emits few function bodies, but its object pool (`pp.txt`), objects
  (`objs.txt`) and IDA name script give enough to disassemble `libapp.so` yourself with capstone,
  annotating `[x27, #off]` loads from the pool - strings, enum values and const maps then show up
  in context. Enum indices and `Map<enum,int>` literals give message-type and address tables;
  CRC tables, `0x7E`/`0xFE` constants and Uint8List builders lead to the frame encoders.
- Expect two protocols (FE frames and legacy `7E FF`); check which one the unit actually answers.
