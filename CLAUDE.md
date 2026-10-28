# Van Display - Claude handover

Waveshare **ESP32-S3-Touch-LCD-7B** (ESP32-S3-WROOM-1-N16R8, 1024x600 RGB panel, GT911 touch,
CH32V003 IO expander @0x24) as a Victron-style WiFi dashboard for a caravan/RV's **Cerbo GX**
(Venus OS 3.x). ESP-IDF **v5.5.x** + **LVGL 9.6** + `espressif/esp_lvgl_adapter`. Optional Bluetooth
bridge to a **Projecta Intelli-RV PM335** (`CONFIG_PM335_ENABLE`, off by default).

This is a public starter kit made from a working, in-use display (README.md is the user guide).
**The person using it is probably new to this project, and often new to programming.** Explain
in plain words, keep changes small, and let them check each one on the screen. At the start, find
out:
- their Victron devices (battery monitor, MPPTs, DC-DC, inverter/charger, sensors), and the
  custom names their temperature sensors have on the Cerbo;
- their WiFi setup;
- whether they have a Projecta unit;
- whether ESP-IDF is installed, which OS they're on, and the display's COM port;
- whether they use git (if not, offer to `git init` so changes can be undone).

Then adapt the code to their setup, one step at a time. They test on the real device and report
back, often with photos. If their PC can reach the Cerbo, offer to read its MQTT data directly
(a small read-only Python script with `paho-mqtt`: send `R/<portal>/keepalive`, subscribe
`N/#`, list services and CustomName/ProductName) rather than guessing topic names. Never publish
`W/...` (write) topics on their Cerbo without asking first.

When they learn something about their own van (device names, odd units, quirks), add it to this
file under a "This van" section so later sessions know it.

## Workflow

- First setup: `firmware/main/secrets.h` from `secrets.example.h`: WiFi list (priority order, each
  with that network's Cerbo IP; `.home = true` marks the home network) and the web login. Never put
  real credentials anywhere else, and never commit `secrets.h`.
- Build/flash: `.\tools\build.ps1` (build), `.\tools\build.ps1 flash monitor` (USB),
  `.\tools\build.ps1 ota` (over WiFi: uploads, then follows the update until it's confirmed),
  `.\tools\build.ps1 ha` (publishes to Home Assistant for the optional self-update, see below).
  Env vars: `DISPLAY_PORT` (COM port, default COM10), `DISPLAY_HOST` (IP if mDNS doesn't resolve),
  `DISPLAY_WEB_PASS`, `DISPLAY_HA_SSH` (ssh target for `ha`, default `ha`). The script dot-sources the Espressif installer's
  `C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`; adjust it to the version they
  installed. Run it from PowerShell. On macOS/Linux, use `idf.py` in `firmware/` directly.
- `idf.py menuconfig`: **Van Display** (header title, hostname, time zone as a POSIX TZ string,
  default `AEST-10`) and **Projecta PM335**.
  `sdkconfig.defaults` holds the board's required settings. Once `firmware/sdkconfig` exists, new
  defaults don't override lines already in it: edit `sdkconfig`, or delete it to regenerate.
- **Versioning:** `PROJECT_VER` in `firmware/CMakeLists.txt` (patch = fix, minor = feature). If
  they use git, commit and tag each release. The running version shows on the Settings page and
  in `/api/health`.
- Web UI / API at `http://<hostname>.local/` with HTTP basic auth from `secrets.h`:
  - `GET /api/health`
  - `GET/POST /api/settings`
  - `POST /api/ota`
  - `POST /api/restart`
  - `POST /api/update/check` (self-update: ask HA now)
  - `GET/POST /api/pm335`
- Serial logs: the native USB-Serial-JTAG port. Never toggle DTR/RTS: it resets into download mode.

## Layout

```
firmware/components/board   I2C, expander, RGB panel, GT911, backlight PWM, LVGL adapter
firmware/components/net     WiFi: priority list, follows the Cerbo (avoids networks without it),
                            remembers the last working network in NVS
firmware/components/cerbo   MQTT client to the Cerbo's broker (1883): data model, keepalive,
                            devices by name, VanDisplay/... custom topics (travel JSON from Node-RED)
firmware/components/pm335   optional Projecta PM335 BLE bridge (NimBLE central)
firmware/main               ui.c (header, themes, overlays), page_overview/solar/travel/settings,
                            map.c + png565.c (OSM tiles), power.c (sleep), settings.c (NVS),
                            fwupdate.c (OTA lifecycle), web.c + web/index.html, inet.c (ISP),
                            splash.c (boot splash, main/splash/splash.jpg),
                            motion.c (travel safety), selfupdate.c (optional update from HA)
homeassistant/              van_display_update custom component (serves builds to selfupdate.c)
tools/setup_update_key.py   self-update key -> secrets.h (+ HA secrets.yaml with --ssh)
tools/make_splash.py        picture -> 1024x600 baseline JPEG splash (+ preview PNG)
nodered/                    optional "Van Display feed" tab (travel summary -> MQTT VanDisplay/travel)
enclosure/                  CadQuery rear shell -> STEP/STL
.claude/skills/projecta-pm335-ble   PM335 protocol skill (read it before touching components/pm335)
```

## Hard-won rules (don't relearn these)

- **Flash + PSRAM must run at 120 MHz** (in `sdkconfig.defaults`). At 80 MHz the RGB scan-out
  starves: flickering, horizontally shifted lines.
- **Internal RAM is the scarce resource.** RGB bounce buffers are 20 lines (10 tears). The LVGL
  adapter draw buffer is `buffer_height=20`; it's always internal in triple-partial mode, and 50
  starved WiFi/MQTT. Small mallocs go to PSRAM (`SPIRAM_MALLOC_ALWAYSINTERNAL=0`). WiFi uses dynamic
  TX buffers and no IRAM optimisation; mbedTLS allocates in PSRAM. Put big static buffers in PSRAM
  with `EXT_RAM_BSS_ATTR`, and new task stacks with `xTaskCreatePinnedToCoreWithCaps(...,
  MALLOC_CAP_SPIRAM)`, unless the task does flash ops. Watch `int`/`min` free in the heartbeat log
  and `/api/health`.
- **Never do flash ops (NVS, OTA state) from the LVGL task** or any task with a PSRAM stack: flash
  ops disable the cache and abort. Use the settings saver task / an esp_timer.
- **CH32V003 expander keeps its state across ESP32 resets**; only a power cycle resets it. If the
  backlight or brightness misbehaves, power-cycle before blaming the firmware.
  - EXIO1 touch reset, EXIO2 backlight enable, EXIO3 LCD reset, EXIO4 SD CS.
  - **EXIO5 USB(0)/CAN(1): must stay 0**, or the USB port vanishes until a power cycle.
  - **EXIO6 = LCD_VDD_EN (panel power)**: must be high, or the panel stays black even with the
    backlight on.
  - Reg 0x05 = backlight PWM, inverted (0 = brightest, capped at 97 %); reg 0x06 = battery ADC.
- **Boot-strap quirk:** GPIO0 (BOOT strap) is also the panel's G3 data line, and is weakly pulled
  low after a chip reset. So RESET, USB/RTS reset or a quick replug often lands in download mode
  with a black screen. Software reboots are fine. `build.ps1` reboots via the RTC watchdog after
  flashing. If stuck black: **unplug ~30 s**. If a firmware crash-loops: hold BOOT, press RESET,
  then flash.
- Two Type-C ports: **"USB"** = native USB-Serial-JTAG (VID 303A, use this); **"UART1"** = CH343
  USB-UART.
- Don't enable `CONFIG_LCD_RGB_ISR_IRAM_SAFE`: the LVGL adapter's callback context is in PSRAM,
  so you get a black screen and a hang. Leave the LVGL perf monitor / sysmon off: its overlay pins
  the CPU at 100 %.
- LVGL 9.6 API: `lv_obj_set_hidden/is_hidden`; recolour with `#RRGGBB text#`. Each `ui_num()` in
  one printf needs its own buffer. Touch the UI only under `board_lvgl_lock()` (or from LVGL
  timers/events).
- **Boot splash** (`splash.c`): a baseline JPEG decoded at boot by the ROM TJpgDec into a temporary
  1.2 MB PSRAM RGB565 buffer (Bayer-dithered), on `lv_layer_top()` at index 0 so the alarm / update
  / sleep overlays stay above it. Fade in 0.8 s, hold 5 s, fade out; a tap skips it. Not a raw
  image: with `SPIRAM_XIP_FROM_PSRAM` everything embedded is copied into PSRAM for good. The buffer
  is borrowed from the map: `map_task` waits for `splash_active()` to go false before allocating
  its tile pool. Decode failure just skips the splash. Must stay 1024x600, baseline (not progressive).
- Map tiles are decoded by `png565.c` with fixed buffers: lodepng's growing reallocs fragmented
  PSRAM. One keep-alive HTTPS connection, centre tile first. OSM's tile policy needs a real
  User-Agent (`map.c`).
- After an OTA the new firmware confirms itself (15 s with WiFi up, else 60 s). Restarts and
  uploads are refused (409) until then; an unconfirmed image rolls back on the next reset.
- Map tiles are prefetched while another page shows (`page_travel_prefetch` from `ui.c`'s
  refresh). A kept-alive tile connection the server closed while idle is retried at once on a
  fresh one.
- Inline Python patch scripts via a bash heredoc mangle `\n` in C strings; use a file edit tool.

## Cerbo notes

- The Cerbo's MQTT (Settings > Services > MQTT, plaintext 1883) needs a keepalive
  (`R/<portal>/keepalive`), which `cerbo.c` sends. The portal ID is discovered from
  `N/+/system/0/Serial`.
- Devices are matched by service type with wildcards and identified by **CustomName/ProductName**,
  not instance numbers, which can change. Temperature tile order: `s_temp_order` in
  `page_overview.c`.
- WiFi follows the Cerbo: if no broker answers within 25 s the network is avoided for 10 min. The
  last network where the Cerbo worked is tried first after a reboot.
- The original van's 240 V charger isn't visible to the Cerbo; it shows up as negative "DC system"
  power, so the Overview shows an *inferred* charger and marks DC loads unknown. A van with a
  Multiplus/Quattro or a VE.Direct/VE.Can charger should read it directly instead: adapt
  `cerbo.c` and the Overview.
- Already read but not displayed: `tank/+` (Victron tank sensors, e.g. Mopeka), temperature
  humidity. Digital inputs in alarm (`Alarm == 2`) show a full-screen overlay and wake the screen.
- Anything published under `VanDisplay/...` on the Cerbo's broker is available as a custom value.
  The Travel page uses `VanDisplay/travel` (JSON) from the optional Node-RED tab
  (`nodered/build_feed_flow.py`; set its DIR and broker node id first).

## Projecta PM335 (optional, `CONFIG_PM335_ENABLE`)

Read `.claude/skills/projecta-pm335-ble/` first: protocol, quirks and tools. In short:

- NimBLE central to the unit's MAC (`CONFIG_PM335_MAC`). Session: CONNECT (64 zero bytes), then
  login (user `user` + the app's pairing key, set via `POST /api/pm335 {"login":...}` and kept in
  NVS, never in files), then poll every `CONFIG_PM335_POLL_S` plus a heartbeat every 12 s.
- **The PM handles ONE request at a time** and silently drops anything sent before it replied:
  `pm335_send` waits for the reply by message id.
- Its reported status lags the relays by up to ~10 s, so the Overview shows a tapped state at
  once, ignores readings for 12 s and confirms on a fresh match (30 s cap). Changes made
  elsewhere need two agreeing readings.
- Hot water ON turns the pump on first; pump OFF turns hot water off too.
- Van power (FFFF00 0109) is web-only and Off-only: it can power the display down.
- **Don't switch loads yourself.** The user presses the buttons; read-only queries are fine.
- **Travel safety** (`motion.c`, on by default, Settings switch / `travel_safety` setting): an
  anchor follows wherever the van sits still (50 m for 20 s); once every good GPS fix for 30 s is
  more than 350 m from it, pump and hot water are switched off (hot water first) for 90 s, then the
  anchor moves on. GPS jumps that come back reset the dwell. The switch only stops the switching;
  detection always runs (`/api/health` `motion`) and also holds back self-updates on the move.
  Without `CONFIG_PM335_ENABLE` the Settings switch is greyed out and nothing is switched.
- Memory: BLE controller in flash (`BT_CTRL_RUN_IN_FLASH_ONLY`), NimBLE allocations and task
  stacks in PSRAM. A crash guard skips BLE after 2 crash boots. With the option off, BLE never
  starts and the linker drops most of it. Leave `CONFIG_BT_ENABLED=y`: the component needs the
  headers.

## Self-update from Home Assistant (optional)

`selfupdate.c` + `homeassistant/custom_components/van_display_update` (user guide:
`homeassistant/README.md`). Off unless `secrets.h` defines `DISPLAY_UPDATE_KEY` (with
`DISPLAY_HA_URL`, optional `DISPLAY_HA_LOCAL_URL` tried first on `.home` networks).
- A minute after joining a network, then every 30 min, not while moving (motion.c) or during an
  update: `GET /api/van_display/manifest?running=<v>` (header `X-Display-Key`), and if newer,
  `GET /api/van_display/firmware` streamed into the other OTA slot with size + SHA-256 checked;
  then fwupdate.c's countdown, restart and self-confirmation. A version the bootloader rolled back
  is never fetched again.
- HA serves `/config/van_display/firmware.bin` only with the key (the image embeds secrets.h: never
  `/config/www`) and checks the image's project name is `van_display`. Entity
  `update.van_display_firmware`. Keep `project(van_display)` in `firmware/CMakeLists.txt` in step
  with `PROJECT` in the component.
- The self-update task has a PSRAM stack (TLS), so OTA writes run in a short-lived internal-stack
  helper task, and the rolled-back version is read once in `selfupdate_start()` from app_main:
  reading it from the PSRAM-stack task crash-looped the original display.

## Status of this kit

- Builds cleanly with ESP-IDF 5.5.5, with and without `CONFIG_PM335_ENABLE` and self-update.
- 1.1.0 matches the original display's v2.9.3, in production: travel safety, self-update from
  HA, Check for update, map prefetch. The kit's renames (title, hostname, MQTT prefix
  `VanDisplay/`, `DISPLAY_` secrets, the `van_display_update` HA component, cut down from a
  multi-display one), the time zone option and the PM335 build switch were build-tested only, not
  run on hardware: check the first flash closely.
- 1.0.0 matched the original's v2.4.7 plus the v2.5.0 boot splash.
- Public repository: keep personal data (WiFi names, IPs, portal IDs, MACs, serials, home
  location, names in pictures) out of anything committed. `secrets.h` is git-ignored.
