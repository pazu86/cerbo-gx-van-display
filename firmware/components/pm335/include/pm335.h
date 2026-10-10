#pragma once
/*
 * Projecta PM335 (Intelli-RV) over BLE.
 *
 * A NimBLE central keeps a connection to the van's PM335, runs the app's
 * CONNECT handshake and polls system status. Frames in both directions are
 * kept in a small log (web API) and published to the Cerbo's broker under
 * PM335/... so the protocol can be finished from live data.
 *
 * The PM335 accepts one BLE connection: while the display holds it, the phone
 * app can't connect. pm335_set_enabled(false) releases it (persisted in NVS).
 *
 * Nothing here switches loads on its own: control frames are only sent when
 * asked via pm335_send().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PM335_DISABLED,
    PM335_STARTING,      /* BLE stack not synced yet */
    PM335_SCANNING,
    PM335_CONNECTING,
    PM335_DISCOVERING,   /* services, characteristics, notifications, MTU */
    PM335_READY,         /* subscribed; handshake sent */
    PM335_ONLINE,        /* frames arriving */
} pm335_state_t;

typedef struct {
    pm335_state_t state;
    bool enabled;
    char addr[18];
    int rssi;                 /* last advert, 0 = never seen */
    uint16_t mtu;
    uint32_t connects, tx_frames, rx_frames, crc_errors;
    int64_t last_seen_us;     /* last advert or frame, 0 = never */
    int64_t last_rx_us;
    int session_fmt;          /* header format used after CONNECT */
    char profile[8];          /* BLE module profile in use: tbb / a002 / 1000 */
    char services[160];       /* services found on the last connection */
    char chars[200];          /* characteristics of the profile's service: uuid@handle/props */
    int disconnect_reason;
    bool passkey_set;         /* a pairing passkey is configured */
    bool encrypted;           /* current link is paired/encrypted */
    char trace[160];          /* connection/security events of the last connection */
    bool logged_in;           /* CONNECT + login accepted: polling and control available */
    int switch_queue_ms, switch_send_ms, switch_reply_ms;   /* last switch: tap -> sent -> PM reply */
    uint32_t replies_missed;  /* requests the PM never answered */
    char last_error[48];
} pm335_status_t;

#define PM335_LOG_DATA 200

typedef struct {
    uint32_t seq;
    int64_t t_us;
    bool tx;
    bool crc_ok;
    int fmt, msg_type;
    uint16_t len;              /* body length (unescaped, no length field) */
    uint8_t body[PM335_LOG_DATA];
} pm335_log_entry_t;

void pm335_start(void);
void pm335_get(pm335_status_t *out);
const char *pm335_state_name(pm335_state_t state);

void pm335_set_enabled(bool enabled);   /* not from the LVGL task: writes NVS */
void pm335_set_passkey(uint32_t passkey);   /* BLE pairing passkey, 0 = none; NVS, not from LVGL */

/* Encode and send one frame. fmt < 0 uses the session format. */
esp_err_t pm335_send(int fmt, uint16_t msg_type, const uint8_t *payload, size_t len);

/* Latest reply to a status query (one per address + first property) */
typedef struct {
    uint8_t addr[3];
    uint16_t first, count;
    int result;
    uint16_t len;
    uint8_t data[120];
    int64_t t_us;
    uint32_t gen;
} pm335_value_t;

int pm335_values(pm335_value_t *out, int max);

/* Login the PM expects after CONNECT (the app's "pairing key" is the password).
 * Stored in NVS; reconnects to apply. Not from the LVGL task. */
void pm335_set_login(const char *user, const char *password);

/* Controller address for loads/settings (from the device tree; van: 000b00) */
void pm335_set_ctl_addr(const char *addr_hex);
const char *pm335_ctl_addr(void);

/* Encrypted control write: 05 | addr | prop | len | value. Needs logged_in.
 * e.g. water pump 011D, hot water 011E: value 00 01 on / 00 00 off; silent 0207: 01/00 */
esp_err_t pm335_control(const char *addr_hex, uint16_t prop, const uint8_t *value, size_t len);

/* What the display shows: tanks and the two switches (from the controller block) */
typedef struct {
    bool online;              /* logged in to the PM */
    bool valid;               /* fresh controller data (< 30 s) */
    int tank[3];              /* % (-1 unknown) */
    int pump, hot_water;      /* 1 on, 0 off, -1 unknown */
    uint32_t gen;             /* changes with every fresh controller reply */
} pm335_van_t;

void pm335_get_van(pm335_van_t *out);

/* Link enabled and on its way (scanning / connecting / logging in) */
bool pm335_connecting(void);

typedef enum { PM335_SW_PUMP, PM335_SW_HOT_WATER } pm335_switch_t;

/* Queue a pump / hot water switch (safe from the LVGL task). false if not queued. */
bool pm335_switch(pm335_switch_t sw, bool on);

/* Write bytes to the module as-is (chunked to the MTU); logged with fmt -2. */
esp_err_t pm335_send_raw(const uint8_t *data, size_t len);

/* Copies log entries with seq > since (oldest first); returns the count. */
int pm335_log_since(uint32_t since, pm335_log_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
