# Implementation notes (Bluetooth link)

Lessons from building a working BLE bridge to a PM335 on an ESP32-S3 (ESP-IDF 5.5, NimBLE) and from
a Python/bleak client. Read the section you need.

## Request handling

- Serialise every request: hold a send lock, write the frame, then wait (≤ ~1.2 s) for a reply whose
  3-byte message id (body bytes 3-5) matches. Count misses; don't retry blindly.
- If replies are handled on another task (NimBLE host task), set any state the reply handler checks
  **before** sending - otherwise the reply can arrive while the request call is still in flight and
  get ignored (e.g. a "waiting for login reply" step set only after the send returned).
- Poll the load controller block first after a control so the UI confirms quickly.
- A full poll of the app's 7 queries takes ~1 s when serialised; repeat every ~3 s plus a heartbeat
  every ~12 s.

## Switch UI state

- On tap: show the requested state immediately (optimistic), queue the command (never block the UI
  thread on BLE), mark it pending.
- Ignore status readings for ~12 s after the tap (they may describe the previous state), then confirm
  on the first fresh reading that matches; give up and show the reported state after ~30 s.
- For changes made elsewhere (the Projecta buttons, the phone app), require two agreeing fresh
  readings before changing the displayed state.
- Interlock: hot water ON → pump ON first; pump OFF → hot water OFF too. Queue them in order; the
  one-request-at-a-time rule then spaces them correctly.

## ESP32 / NimBLE

- Central + observer roles only, 1 connection. Scan for the target MAC (match `ble_addr_t.val`,
  little-endian), connect with the address type from the advert.
- Discover **all** services and pick the profile (TI `0x1000`, TBB 128-bit, or `0xA002`); find the
  CCCD with descriptor discovery and write `01 00`. Exchange MTU.
- Security/SMP is not needed (the unit doesn't support it) - leave `BT_NIMBLE_SECURITY_ENABLE` off.
- Use `ble_gattc_write_no_rsp_flat` when the write characteristic allows it (the app does); waiting
  for write responses adds a round trip per frame.
- Keep MQTT/HTTP publishing **off** the BLE task - a busy MQTT client blocked `esp_mqtt_client_enqueue`
  long enough to stall polling (stale/blank data) and delay switch commands. Give it its own task.
- Memory on a RAM-tight S3 (RGB LCD, WiFi): `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` (moves ~17 kB of
  controller code out of IRAM; fine when code runs from PSRAM via `SPIRAM_XIP_FROM_PSRAM`),
  `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`, task stacks with `xTaskCreatePinnedToCoreWithCaps(...,
  MALLOC_CAP_SPIRAM)` (NimBLE host task too, via your own task that calls `nimble_port_run()`),
  big buffers `EXT_RAM_BSS_ATTR` with `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`,
  `CONFIG_BT_CTRL_BLE_MAX_ACT=2`, `CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=n`.
- WiFi + BLE coexistence worked; scan in bursts (10 s every 30 s) while not connected.
- Store the login password in NVS (set via an authenticated API), not in source or firmware images.
- If enabling an existing project's BT in `sdkconfig`, note that `sdkconfig.defaults` won't override
  options already written to `sdkconfig` (e.g. `# CONFIG_BT_ENABLED is not set`) - edit those lines.

## Python / bleak

- `scripts/pm335_client.py` implements the session above; Windows, Linux and macOS work with bleak.
- Writes: `write_gatt_char(write_uuid, chunk, response=False)`; notifications via `start_notify`.
- Close the phone app (or turn the phone's Bluetooth off - iOS keeps the link after closing the app).
