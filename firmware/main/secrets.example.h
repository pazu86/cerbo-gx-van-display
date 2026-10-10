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
