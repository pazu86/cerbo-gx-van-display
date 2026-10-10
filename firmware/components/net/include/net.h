#pragma once
/*
 * WiFi station manager with a priority-ordered list of known networks.
 *
 * Joins the highest-priority network that's visible. While connected to a
 * lower-priority one it rescans periodically and moves up when it can -
 * unless the app reports the link is doing its job (net_set_link_ok), or
 * has asked to avoid a network where its service isn't reachable. The last
 * network with a working link is remembered in NVS and tried first after a
 * reboot. Together that makes the display follow whichever network the
 * Cerbo is on.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ssid;
    const char *pass;
    const char *cerbo;  /* Cerbo GX host/IP on this network */
    bool home;          /* a fixed home network (shown as "Home" rather than probed) */
} net_network_t;

typedef enum {
    NET_STATE_IDLE,        /* no known network in range, waiting to rescan */
    NET_STATE_SCANNING,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,   /* associated and has an IP */
} net_state_t;

typedef struct {
    net_state_t state;
    int network;           /* index into the list, -1 if none */
    char ssid[33];
    char ip[16];
    int8_t rssi;
    const char *cerbo;     /* Cerbo host for the current network, NULL if none */
    bool home;             /* current network is flagged home */
} net_status_t;

/* The list must stay valid for the life of the program. */
esp_err_t net_start(const net_network_t *networks, size_t count);

void net_get_status(net_status_t *out);

/* The app's service (the Cerbo) is working on this network: stop looking
 * for a higher-priority one. Cleared automatically on every reconnect. */
void net_set_link_ok(bool ok);

/* The service isn't reachable here: drop this network and prefer others for
 * avoid_ms. If nothing else is in range it is still used. */
void net_avoid_current(uint32_t avoid_ms);

const char *net_state_name(net_state_t state);

/* Saved networks, and the user's choice: -1 = Auto (follow the Cerbo), else
 * an index that's used whenever in range and never avoided. Not persisted:
 * every boot starts in Auto. */
int net_count(void);
const char *net_ssid(int idx);
int net_get_selected(void);
void net_select(int idx);
void net_select_async(int idx);   /* safe from the LVGL task */

#ifdef __cplusplus
}
#endif
