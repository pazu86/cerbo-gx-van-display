#pragma once
/*
 * Copy to secrets.h (git-ignored) and fill in.
 *
 * Known networks in priority order: the first one that's visible wins, and
 * while connected to a lower-priority network the display keeps looking for
 * a better one. Each network carries the Cerbo GX's address on that network.
 */
#define DISPLAY_NETWORKS                                       \
    {                                                         \
        {.ssid = "VanWifi", .pass = "secret", .cerbo = "192.168.1.102"}, \
        {.ssid = "HomeWifi", .pass = "secret", .cerbo = "192.168.0.20", .home = true},   \
    }

/* Web settings / health / OTA at http://van-display.local (HTTP basic auth) */
#define DISPLAY_WEB_USER "admin"
#define DISPLAY_WEB_PASS "change-me"

/* Optional self-update from Home Assistant (selfupdate.c, `tools\build.ps1 ha`, HA side and
 * setup in homeassistant/README.md). `python tools/setup_update_key.py` adds these lines for you.
 * The key must match `van_display_key` in HA's secrets.yaml. The local URL is tried first on
 * networks flagged .home. Without the key (the default) self-update stays off. */
// #define DISPLAY_HA_URL "https://example.ui.nabu.casa"
// #define DISPLAY_HA_LOCAL_URL "http://192.168.0.10:8123"
// #define DISPLAY_UPDATE_KEY "64-hex-character-random-key"

/* Optional self-update from Home Assistant (selfupdate.c, `tools\build.ps1 ha`, HA side in
 * homeassistant/). `python tools/setup_update_key.py` adds these three lines for you. The key must
 * match `van_display_key` in HA's secrets.yaml. The local URL is tried first on networks flagged
 * .home. Leave the key out (the default) and self-update stays off. */
// #define DISPLAY_HA_URL "https://example.ui.nabu.casa"
// #define DISPLAY_HA_LOCAL_URL "http://192.168.0.10:8123"
// #define DISPLAY_UPDATE_KEY "64-hex-character-random-key"
