# Optional: self-update from Home Assistant

If you run Home Assistant, the display can update itself: you build on your PC, publish the build
to HA with one command, and the display installs it the next time it checks (a minute after it
joins a network, then every 30 minutes; never while the van is moving). No USB cable, and it works
when the van is away from home as long as the display can reach HA (for example through Nabu Casa).
A build that doesn't start properly rolls back by itself.

You don't need any of this: without it, update over USB or `.\tools\build.ps1 ota` as before.

## Setup (once)

1. Copy `custom_components/van_display_update` into your HA `/config/custom_components/`
   (Samba share, File editor or SSH add-on).
2. Generate the display's key. From the project folder:

   ```
   python tools/setup_update_key.py --url https://xxxx.ui.nabu.casa --local-url http://192.168.0.10:8123
   ```

   `--url` is how the display reaches HA from anywhere; `--local-url` (optional) is tried first on
   the WiFi marked `.home = true` in `secrets.h`. The script adds the key and addresses to
   `firmware/main/secrets.h`. Add `--ssh root@homeassistant.local` to put the key into HA's
   `secrets.yaml` too; otherwise it writes `tools/van_display_key.yaml` for you to paste into
   `secrets.yaml` (then delete that file).
3. Add to HA's `configuration.yaml`, then restart HA:

   ```yaml
   van_display_update:
     key: !secret van_display_key
   ```

   HA now has an entity `update.van_display_firmware` showing what the display runs and what is
   published.
4. Flash the display once more over USB or `ota`, so it has the key.

## Publishing a build

Bump `PROJECT_VER` in `firmware/CMakeLists.txt`, then:

```
.\tools\build.ps1 ha
```

This copies the build to `/config/van_display/firmware.bin` over SSH (set `$env:DISPLAY_HA_SSH`
if your SSH target isn't called `ha`) and checks the copy. Without SSH, copy
`van_display.bin` from the build folder to `/config/van_display/firmware.bin` yourself.

Never put the file in `/config/www`: the build contains your WiFi passwords, which is why HA only
gives it to requests carrying the display's key.

The Settings page shows the last result on its "Updates" line, and **Check for update** asks
straight away (also on the web page).
