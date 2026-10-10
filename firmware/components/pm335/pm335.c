#include "sdkconfig.h"
#include "pm335.h"

#include <stdio.h>
#include <string.h>

#include "cerbo.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include "pm335_codec.h"

static const char *TAG = "pm335";

#define LOG_SIZE        128
#define SCAN_MS         10000
#define SCAN_PAUSE_MS   20000
#define HANDSHAKE_GAP_MS 2000
#define NVS_NS          "pm335"

/* The app knows three BLE module profiles (service / notify / write, class DUa) */
#define PM_UUID(n) BLE_UUID128_INIT(0xdd, 0xf3, 0x9c, 0x87, 0xb0, 0xe0, 0xee, 0xbe, \
                                    0x07, 0x48, 0xb7, 0x00, (n), 0x00, 0x00, 0x00)
static const ble_uuid128_t s_tbb_svc = PM_UUID(0x00), s_tbb_write = PM_UUID(0x01), s_tbb_notify = PM_UUID(0x02);
static const ble_uuid16_t s_a002 = BLE_UUID16_INIT(0xA002), s_c304 = BLE_UUID16_INIT(0xC304),
                          s_c305 = BLE_UUID16_INIT(0xC305);
static const ble_uuid16_t s_1000 = BLE_UUID16_INIT(0x1000), s_1001 = BLE_UUID16_INIT(0x1001),
                          s_1002 = BLE_UUID16_INIT(0x1002);

typedef struct {
    const char *name;
    const ble_uuid_t *svc, *notify, *write;
} profile_t;

static const profile_t s_profiles[] = {
    {"tbb", &s_tbb_svc.u, &s_tbb_notify.u, &s_tbb_write.u},
    {"a002", &s_a002.u, &s_c305.u, &s_c304.u},
    {"1000", &s_1000.u, &s_1002.u, &s_1001.u},
};
static const profile_t *s_profile;
static uint32_t s_passkey;             /* BLE pairing passkey (NVS), 0 = don't pair */

void ble_store_config_init(void);      /* NimBLE store (bonds kept in RAM: NVS persist is off) */

static SemaphoreHandle_t s_lock;       /* s_st, log */
static SemaphoreHandle_t s_send_lock;  /* one frame at a time */
static SemaphoreHandle_t s_write_done;
static volatile int s_write_status;

static pm335_status_t s_st;
static uint8_t s_target[6];            /* ble_addr_t.val order (little-endian) */
static uint8_t s_own_addr_type;
static volatile bool s_synced, s_scanning;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

/* GATT discovery results */
static uint16_t s_svc_end, s_write_handle, s_notify_handle, s_notify_end, s_cccd_handle;
static uint8_t s_write_props, s_notify_props;

EXT_RAM_BSS_ATTR static pm335_decoder_t s_dec;
static pm335_log_entry_t *s_log;       /* ring in PSRAM */
static uint32_t s_seq;

/* Handshake / polling, driven by the pm335 task */
static int64_t s_ready_us;
static int s_handshake_step;
static int64_t s_next_poll_us;
static pm335_state_t s_published_state = (pm335_state_t)-1;

/* Session (CONNECT -> login -> poll/heartbeat) */
static volatile bool s_connect_reply;
static volatile int s_login_result = -1;   /* -1 pending, 0 ok, else the PM's code */
static int64_t s_ctl_tx_us;                /* last switch sent (timing) */
/* The PM handles one request at a time and silently drops anything that
 * arrives before it has replied: every send waits for its reply (by id) */
#define REPLY_WAIT_MS 1200
static SemaphoreHandle_t s_reply_sem;
static volatile uint32_t s_wait_id;        /* 3-byte message id awaited, 0 = none */
static uint32_t s_replies_missed;
static uint8_t s_key[4];
static char s_user[21] = "user", s_pass[21];
static char s_ctl_addr[7] = "000b00";
static int s_poll_idx;
static int64_t s_next_heartbeat_us;

/* Latest reply per query (addr + first property) */
#define VALUES_MAX 12
EXT_RAM_BSS_ATTR static pm335_value_t s_values[VALUES_MAX];
static uint32_t s_values_gen;          /* bumped on every update */
static uint32_t s_values_published[VALUES_MAX];

static void on_reply(const pm335_frame_t *f)
{
    const uint8_t *pl = f->body + f->payload_off;
    size_t n = f->payload_len;
    switch (f->msg_type) {
    case 0x11:   /* connectResponse: 6 bytes, all zero on the van's PM -> key 00000000 */
        if (n >= 6) memcpy(s_key, pl + 2, 4);
        s_connect_reply = true;
        break;
    case 0x41:   /* controlResponse: u16 result (login: 0 ok, 5 rejected) */
        if (s_ctl_tx_us && s_st.switch_reply_ms < 0) s_st.switch_reply_ms = (esp_timer_get_time() - s_ctl_tx_us) / 1000;
        if (n >= 2 && s_login_result < 0 && s_handshake_step == 2) s_login_result = (pl[0] << 8) | pl[1];
        break;
    case 0x51: { /* queryResponse: u16 result | addr3 | first16 | count16 | data */
        if (n < 9) break;
        int slot = -1, empty = -1;
        for (int i = 0; i < VALUES_MAX; i++) {
            if (s_values[i].count && !memcmp(s_values[i].addr, pl + 2, 3) &&
                s_values[i].first == ((pl[5] << 8) | pl[6])) {
                slot = i;
            } else if (!s_values[i].count && empty < 0) {
                empty = i;
            }
        }
        if (slot < 0) slot = empty;
        if (slot < 0) break;
        pm335_value_t *v = &s_values[slot];
        v->result = (pl[0] << 8) | pl[1];
        memcpy(v->addr, pl + 2, 3);
        v->first = (pl[5] << 8) | pl[6];
        v->count = (pl[7] << 8) | pl[8];
        v->len = n - 9 < sizeof(v->data) ? n - 9 : sizeof(v->data);
        memcpy(v->data, pl + 9, v->len);
        v->t_us = esp_timer_get_time();
        v->gen = ++s_values_gen;
        break;
    }
    default:
        break;
    }
}

/* Controller block 0100-0129 (Omega2000): byte offsets matched to the Projecta
 * LCD and to the app's button presses in the capture */
#define VAN_TANK_OFF   36     /* 3 x u16 %, tanks 1-3 */
#define VAN_PUMP_OFF   61     /* 1 while the water pump is on (also byte 43) */
#define VAN_HWS_OFF    63     /* 1 while the hot water is on */
#define VAN_STALE_US   (30 * 1000000LL)

void pm335_get_van(pm335_van_t *out)
{
    *out = (pm335_van_t){.tank = {-1, -1, -1}, .pump = -1, .hot_water = -1};
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->online = s_st.logged_in;
    for (int i = 0; i < VALUES_MAX; i++) {
        const pm335_value_t *v = &s_values[i];
        char a[7];
        snprintf(a, sizeof(a), "%02x%02x%02x", v->addr[0], v->addr[1], v->addr[2]);
        if (!v->count || v->first != 0x0100 || strcmp(a, s_ctl_addr) != 0 || v->len <= VAN_HWS_OFF) continue;
        if (esp_timer_get_time() - v->t_us > VAN_STALE_US) break;
        for (int t = 0; t < 3; t++) {
            out->tank[t] = (v->data[VAN_TANK_OFF + t * 2] << 8) | v->data[VAN_TANK_OFF + t * 2 + 1];
        }
        out->pump = v->data[VAN_PUMP_OFF] != 0;
        out->hot_water = v->data[VAN_HWS_OFF] != 0;
        out->gen = v->gen;
        out->valid = true;
    }
    xSemaphoreGive(s_lock);
}

/* Switch requests from the UI: queued, sent by the pm335 task (never blocks LVGL) */
typedef struct {
    pm335_switch_t sw;
    bool on;
    int64_t queued_us;
} switch_req_t;
static QueueHandle_t s_switchq;

bool pm335_switch(pm335_switch_t sw, bool on)
{
    switch_req_t r = {sw, on, esp_timer_get_time()};
    return s_switchq && xQueueSend(s_switchq, &r, 0) == pdTRUE;
}

static void do_switch(const switch_req_t *r)
{
    static const uint8_t on2[2] = {0, 1}, off2[2] = {0, 0};
    uint16_t prop = r->sw == PM335_SW_PUMP ? 0x011D : 0x011E;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = pm335_control(s_ctl_addr, prop, r->on ? on2 : off2, 2);
    s_ctl_tx_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.switch_queue_ms = (t0 - r->queued_us) / 1000;
    s_st.switch_send_ms = (s_ctl_tx_us - t0) / 1000;
    s_st.switch_reply_ms = -1;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s %s: %s", r->sw == PM335_SW_PUMP ? "pump" : "hot water", r->on ? "on" : "off",
             esp_err_to_name(err));
}

int pm335_values(pm335_value_t *out, int max)
{
    int n = 0;
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < VALUES_MAX && n < max; i++) {
        if (s_values[i].count) out[n++] = s_values[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* ------------------------------------------------------------------ */

const char *pm335_state_name(pm335_state_t state)
{
    switch (state) {
    case PM335_DISABLED: return "disabled";
    case PM335_STARTING: return "starting";
    case PM335_SCANNING: return "scanning";
    case PM335_CONNECTING: return "connecting";
    case PM335_DISCOVERING: return "discovering";
    case PM335_READY: return "ready";
    case PM335_ONLINE: return "online";
    }
    return "?";
}

static void set_state(pm335_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_st.state != st) {
        ESP_LOGI(TAG, "%s -> %s", pm335_state_name(s_st.state), pm335_state_name(st));
        s_st.state = st;
    }
    xSemaphoreGive(s_lock);
}

static void set_error(const char *fmt, int rc)
{
    char msg[sizeof(s_st.last_error)];
    snprintf(msg, sizeof(msg), fmt, rc);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.last_error, msg, sizeof(s_st.last_error));
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "%s", msg);
}

static void log_frame(bool tx, const pm335_frame_t *f)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pm335_log_entry_t *e = &s_log[s_seq % LOG_SIZE];
    e->seq = ++s_seq;
    e->t_us = esp_timer_get_time();
    e->tx = tx;
    e->crc_ok = f->crc_ok;
    e->fmt = f->fmt;
    e->msg_type = f->msg_type;
    e->len = f->body_len < PM335_LOG_DATA ? f->body_len : PM335_LOG_DATA;
    memcpy(e->body, f->body, e->len);
    if (tx) {
        s_st.tx_frames++;
    } else {
        s_st.rx_frames++;
        s_st.last_rx_us = s_st.last_seen_us = e->t_us;
        if (!f->crc_ok) {
            s_st.crc_errors++;
        } else {
            if (f->fmt >= 0) s_st.session_fmt = f->fmt;
            on_reply(f);
            uint32_t id = ((uint32_t)f->body[3] << 16) | (f->body[4] << 8) | f->body[5];
            if (s_wait_id && id == s_wait_id) {
                s_wait_id = 0;
                xSemaphoreGive(s_reply_sem);
            }
        }
        if (s_st.state == PM335_READY) {
            s_st.state = PM335_ONLINE;
        }
    }
    xSemaphoreGive(s_lock);
}

int pm335_log_since(uint32_t since, pm335_log_entry_t *out, int max)
{
    int n = 0;
    if (!s_lock || !s_log) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t first = s_seq > LOG_SIZE ? s_seq - LOG_SIZE + 1 : 1;
    if (since + 1 > first) first = since + 1;
    for (uint32_t q = first; q <= s_seq && n < max; q++) {
        out[n++] = s_log[(q - 1) % LOG_SIZE];
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool pm335_connecting(void)
{
    pm335_status_t st;
    pm335_get(&st);
    return st.enabled && !st.logged_in && st.state != PM335_DISABLED;
}

void pm335_get(pm335_status_t *out)
{
    if (!s_lock) {   /* not started (e.g. WiFi failed before pm335_start) */
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

static int write_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    s_write_status = err->status;
    xSemaphoreGive(s_write_done);
    return 0;
}

static esp_err_t write_chunk(const uint8_t *data, size_t len)
{
    /* Like the app: write-without-response when the characteristic allows it
     * (no round trip per write), else write-with-response */
    if (s_write_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) {
        return ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, data, len) ? ESP_FAIL : ESP_OK;
    }
    if (s_write_props & BLE_GATT_CHR_PROP_WRITE) {
        xSemaphoreTake(s_write_done, 0);
        int rc = ble_gattc_write_flat(s_conn, s_write_handle, data, len, write_cb, NULL);
        if (rc) return ESP_FAIL;
        if (xSemaphoreTake(s_write_done, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        return s_write_status == 0 ? ESP_OK : ESP_FAIL;
    }
    return ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, data, len) ? ESP_FAIL : ESP_OK;
}

esp_err_t pm335_send(int fmt, uint16_t msg_type, const uint8_t *payload, size_t len)
{
    pm335_status_t st;
    pm335_get(&st);
    if (st.state != PM335_READY && st.state != PM335_ONLINE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fmt < 0) fmt = st.session_fmt;

    EXT_RAM_BSS_ATTR static uint8_t wire[PM335_MAX_BODY * 2 + 8];
    EXT_RAM_BSS_ATTR static pm335_frame_t f;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    f.body_len = pm335_build_body(fmt, msg_type, payload, len, f.body);
    size_t n = f.body_len ? pm335_wrap(f.body, f.body_len, wire, sizeof(wire)) : 0;
    esp_err_t err = n ? ESP_OK : ESP_ERR_INVALID_SIZE;

    if (err == ESP_OK) {
        pm335_parse_body(&f);   /* fmt/type for the log */

        size_t step = (st.mtu > 23 ? st.mtu : 23) - 3;
        for (size_t i = 0; i < n && err == ESP_OK; i += step) {
            err = write_chunk(wire + i, n - i < step ? n - i : step);
        }
        if (err == ESP_OK) {
            log_frame(true, &f);
            /* Hold the send lock until the PM answers (or REPLY_WAIT_MS) */
            xSemaphoreTake(s_reply_sem, 0);
            s_wait_id = ((uint32_t)f.body[3] << 16) | (f.body[4] << 8) | f.body[5];
            if (xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(REPLY_WAIT_MS)) != pdTRUE) {
                s_wait_id = 0;
                s_replies_missed++;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_st.replies_missed = s_replies_missed;
                xSemaphoreGive(s_lock);
            }
        } else {
            set_error("write failed (%d)", err);
        }
    }
    xSemaphoreGive(s_send_lock);
    return err;
}

esp_err_t pm335_send_raw(const uint8_t *data, size_t len)
{
    pm335_status_t st;
    pm335_get(&st);
    if (st.state != PM335_READY && st.state != PM335_ONLINE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!len || len > PM335_LOG_DATA) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    size_t step = (st.mtu > 23 ? st.mtu : 23) - 3;
    for (size_t i = 0; i < len && err == ESP_OK; i += step) {
        err = write_chunk(data + i, len - i < step ? len - i : step);
    }
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        pm335_log_entry_t *e = &s_log[s_seq % LOG_SIZE];
        e->seq = ++s_seq;
        e->t_us = esp_timer_get_time();
        e->tx = true;
        e->crc_ok = false;
        e->fmt = -2;
        e->msg_type = -1;
        e->len = len;
        memcpy(e->body, data, len);
        s_st.tx_frames++;
        xSemaphoreGive(s_lock);
    } else {
        set_error("raw write failed (%d)", err);
    }
    xSemaphoreGive(s_send_lock);
    return err;
}

/* ------------------------------------------------------------------ */
/* GATT discovery: service -> characteristics -> CCCD -> subscribe    */
/* ------------------------------------------------------------------ */

static int gap_cb(struct ble_gap_event *ev, void *arg);

static void fail_connection(const char *what, int rc)
{
    set_error(what, rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int cccd_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        fail_connection("subscribe failed (%d)", err->status);
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.state = PM335_READY;
    s_st.connects++;
    s_st.last_error[0] = '\0';
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "subscribed, mtu %u", ble_att_mtu(conn));
    s_ready_us = esp_timer_get_time();
    s_handshake_step = 0;
    s_login_result = -1;
    s_connect_reply = false;
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    if (err->status == 0) {
        if (!s_cccd_handle && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s_cccd_handle) {
        fail_connection("no CCCD (%d)", err->status);
        return 0;
    }
    /* Notifications, or indications if that's all the characteristic offers */
    static uint8_t on[2];
    on[0] = (s_notify_props & BLE_GATT_CHR_PROP_NOTIFY) ? 0x01 : 0x02;
    on[1] = 0;
    int rc = ble_gattc_write_flat(conn, s_cccd_handle, on, sizeof(on), cccd_cb, NULL);
    if (rc) fail_connection("CCCD write (%d)", rc);
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        if (s_notify_handle && !s_notify_end && chr->def_handle > s_notify_handle) {
            s_notify_end = chr->def_handle - 1;   /* next characteristic bounds the notify one's descriptors */
        }
        char u[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "  chr %s val 0x%04x props 0x%02x", ble_uuid_to_str(&chr->uuid.u, u), chr->val_handle,
                 chr->properties);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t l = strlen(s_st.chars);
        snprintf(s_st.chars + l, sizeof(s_st.chars) - l, "%s%s@%04x/%02x", l ? " " : "", u, chr->val_handle,
                 chr->properties);
        xSemaphoreGive(s_lock);
        if (ble_uuid_cmp(&chr->uuid.u, s_profile->write) == 0) {
            s_write_handle = chr->val_handle;
            s_write_props = chr->properties;
        }
        if (ble_uuid_cmp(&chr->uuid.u, s_profile->notify) == 0) {   /* may be the same characteristic */
            s_notify_handle = chr->val_handle;
            s_notify_props = chr->properties;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s_write_handle || !s_notify_handle) {
        fail_connection("characteristics missing (%d)", err->status);
        return 0;
    }
    ESP_LOGI(TAG, "write 0x%04x (props 0x%02x), notify 0x%04x", s_write_handle, s_write_props, s_notify_handle);
    int rc = ble_gattc_disc_all_dscs(conn, s_notify_handle, s_notify_end ? s_notify_end : s_svc_end, dsc_cb, NULL);
    if (rc) fail_connection("descriptor discovery (%d)", rc);
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    static uint16_t start;
    static int best;   /* index into s_profiles, lower = preferred */
    if (err->status == 0) {
        char u[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&svc->uuid.u, u);
        ESP_LOGI(TAG, "service %s 0x%04x-0x%04x", u, svc->start_handle, svc->end_handle);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t l = strlen(s_st.services);
        if (l + strlen(u) + 2 < sizeof(s_st.services)) {
            snprintf(s_st.services + l, sizeof(s_st.services) - l, "%s%s", l ? " " : "", u);
        }
        xSemaphoreGive(s_lock);
        for (int i = 0; i < (int)(sizeof(s_profiles) / sizeof(s_profiles[0])); i++) {
            if (ble_uuid_cmp(&svc->uuid.u, s_profiles[i].svc) == 0 && (!s_profile || i < best)) {
                s_profile = &s_profiles[i];
                best = i;
                start = svc->start_handle;
                s_svc_end = svc->end_handle;
            }
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s_profile) {
        fail_connection("no known PM service (%d)", err->status);
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.profile, s_profile->name, sizeof(s_st.profile));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "using profile %s", s_profile->name);
    int rc = ble_gattc_disc_all_chrs(conn, start, s_svc_end, chr_cb, NULL);
    if (rc) fail_connection("characteristic discovery (%d)", rc);
    return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.mtu = err->status == 0 ? mtu : 23;
    xSemaphoreGive(s_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GAP                                                                */
/* ------------------------------------------------------------------ */

static void on_frame(const pm335_frame_t *f, void *arg)
{
    log_frame(false, f);
}


/* Short trace of connection/security events for /api/pm335 (reset on each connect) */
static void trace(const char *fmt, int a, int b)
{
    char t[24];
    snprintf(t, sizeof(t), fmt, a, b);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t l = strlen(s_st.trace);
    if (l + strlen(t) + 2 < sizeof(s_st.trace)) {
        snprintf(s_st.trace + l, sizeof(s_st.trace) - l, "%s%s", l ? " " : "", t);
    }
    xSemaphoreGive(s_lock);
}

static void start_discovery(void)
{
    if (ble_gattc_disc_all_svcs(s_conn, svc_cb, NULL) != 0) {
        fail_connection("service discovery (%d)", 0);
    }
}

static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (memcmp(ev->disc.addr.val, s_target, 6) == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.rssi = ev->disc.rssi;
            s_st.last_seen_us = esp_timer_get_time();
            bool want = s_st.enabled && s_st.state == PM335_SCANNING;
            if (want) s_st.state = PM335_CONNECTING;
            xSemaphoreGive(s_lock);
            if (want) {
                ESP_LOGI(TAG, "found PM335 (rssi %d), connecting", ev->disc.rssi);
                ble_gap_disc_cancel();
                s_scanning = false;
                int rc = ble_gap_connect(s_own_addr_type, &ev->disc.addr, 10000, NULL, gap_cb, NULL);
                if (rc) {
                    set_error("connect failed to start (%d)", rc);
                    set_state(PM335_SCANNING);
                }
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            set_error("connect failed (%d)", ev->connect.status);
            set_state(PM335_SCANNING);
            return 0;
        }
        s_conn = ev->connect.conn_handle;
        s_svc_end = s_write_handle = s_notify_handle = s_notify_end = s_cccd_handle = 0;
        s_write_props = 0;
        s_profile = NULL;
        s_dec.len = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.services[0] = s_st.profile[0] = s_st.chars[0] = '\0';
        s_st.encrypted = false;
        s_st.trace[0] = 0;
        xSemaphoreGive(s_lock);
        trace("conn", 0, 0);
        set_state(PM335_DISCOVERING);
        ble_gattc_exchange_mtu(s_conn, mtu_cb, NULL);
        if (s_passkey) {
            /* The PM's module drops unpaired links after ~20 s: pair first
             * (passkey entry), discover once encrypted (ENC_CHANGE) */
            int rc = ble_gap_security_initiate(s_conn);
            trace("sec:%d", rc, 0);
            if (rc) fail_connection("pairing failed to start (%d)", rc);
        } else {
            start_discovery();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        trace("enc:%d", ev->enc_change.status, 0);
        if (ev->enc_change.status != 0) {
            fail_connection("pairing failed (%d) - check the passkey", ev->enc_change.status);
            return 0;
        }
        ESP_LOGI(TAG, "link encrypted");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.encrypted = true;
        xSemaphoreGive(s_lock);
        start_discovery();
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {.action = ev->passkey.params.action};
        if (io.action == BLE_SM_IOACT_INPUT) {
            io.passkey = s_passkey;
        } else if (io.action == BLE_SM_IOACT_NUMCMP) {
            io.numcmp_accept = 1;
        } else {
            ESP_LOGW(TAG, "unsupported pairing action %d", io.action);
            return 0;
        }
        trace("pk%d:%d", io.action, ble_sm_inject_io(ev->passkey.conn_handle, &io));
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        trace("repeat", 0, 0);
        /* Stale bond (e.g. the PM was re-paired with the phone): forget it and pair again */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        trace("disc:%x", ev->disconnect.reason, 0);
        ESP_LOGW(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_st.state != PM335_DISABLED) s_st.state = PM335_SCANNING;
        s_st.mtu = 0;
        s_st.disconnect_reason = ev->disconnect.reason;
        s_st.logged_in = false;
        xSemaphoreGive(s_lock);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.attr_handle == s_notify_handle) {
            uint8_t buf[256];
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &len) == 0) {
                pm335_decode(&s_dec, buf, len, on_frame, NULL);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.mtu = ev->mtu.value;
        xSemaphoreGive(s_lock);
        return 0;

    default:
        trace("ev%d", ev->type, 0);
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params dp = {
        .passive = 0,             /* active: the PM's name is in the scan response */
        .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(s_own_addr_type, SCAN_MS, &dp, gap_cb, NULL);
    if (rc == 0) {
        s_scanning = true;
    } else if (rc != BLE_HS_EALREADY) {
        set_error("scan failed (%d)", rc);
    }
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    ESP_LOGI(TAG, "BLE ready");
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "BLE host reset (%d)", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();   /* returns only after nimble_port_stop() */
    vTaskDeleteWithCaps(NULL);
}

/* ------------------------------------------------------------------ */
/* Session task: scanning cadence, handshake, polling, MQTT bridge    */
/* ------------------------------------------------------------------ */

static void query(const char *addr_hex, uint16_t first, uint16_t last)
{
    uint8_t p[11];
    uint8_t a[3] = {0};
    for (int i = 0; i < 3; i++) {
        unsigned v = 0;
        sscanf(addr_hex + i * 2, "%2x", &v);
        a[i] = v;
    }
    p[0] = 0x05;
    memcpy(p + 1, a, 3);
    p[4] = first >> 8;
    p[5] = first & 0xFF;
    memcpy(p + 6, a, 3);
    p[9] = last >> 8;
    p[10] = last & 0xFF;
    pm335_send(PM335_FMT_18, PM335_MSG_QUERY, p, sizeof(p));
}

/* What the app polls (btsnoop capture, 2026-10-05). "ctl" = the controller
 * address from the device tree (the van's Omega2000 is 000B00). */
static const struct {
    bool ctl;
    uint16_t first, last;
} s_polls[] = {
    /* Controller block first: it carries the tanks and the pump / hot water state,
     * and a control restarts the cycle so the change is confirmed quickly */
    {true, 0x0100, 0x0129}, {false, 0x0104, 0x0109}, {false, 0x0110, 0x0110}, {false, 0x0002, 0x0009},
    {false, 0x010F, 0x010F}, {false, 0x010E, 0x010E}, {false, 0x0101, 0x0103},
};
#define HEARTBEAT_US   (12 * 1000000LL)
#define REPLY_TIMEOUT_US (4 * 1000000LL)

static void encrypt(uint8_t *data, size_t len)
{
    /* App cipher (libapp 0x80abac); the PM hands out the key in its connect reply */
    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)~((((data[i] ^ s_key[0]) + s_key[1] - s_key[2]) & 0xFF) ^ s_key[3]);
    }
}

esp_err_t pm335_control(const char *addr_hex, uint16_t prop, const uint8_t *value, size_t len)
{
    pm335_status_t st;
    pm335_get(&st);
    if (!st.logged_in && prop != 0x1000) return ESP_ERR_INVALID_STATE;
    if (len > 64) return ESP_ERR_INVALID_SIZE;
    uint8_t p[7 + 64];
    p[0] = 0x05;
    for (int i = 0; i < 3; i++) {
        unsigned v = 0;
        sscanf(addr_hex + i * 2, "%2x", &v);
        p[1 + i] = v;
    }
    p[4] = prop >> 8;
    p[5] = prop & 0xFF;
    p[6] = len;
    memcpy(p + 7, value, len);
    encrypt(p, 7 + len);
    esp_err_t err = pm335_send(PM335_FMT_18, PM335_MSG_CONTROL, p, 7 + len);
    if (err == ESP_OK && st.logged_in) {
        s_poll_idx = 0;           /* refresh the status straight away */
        s_next_poll_us = 0;
    }
    return err;
}

static void session_step(const pm335_status_t *st)
{
    int64_t now = esp_timer_get_time();
    if (st->state != PM335_READY && st->state != PM335_ONLINE) return;

    switch (s_handshake_step) {
    case 0: {   /* CONNECT: 64 zero bytes; the reply carries the cipher key */
        static const uint8_t zero[64] = {0};
        s_connect_reply = false;
        pm335_send(PM335_FMT_18, PM335_MSG_CONNECT, zero, sizeof(zero));
        s_handshake_step = 1;
        s_ready_us = now;
        break;
    }
    case 1:
        if (s_connect_reply) {
            /* Login: control FFFF00 / 1000, value user[20] '&' password[20] */
            uint8_t v[41] = {0};
            memcpy(v, s_user, strnlen(s_user, 20));
            v[20] = '&';
            memcpy(v + 21, s_pass, strnlen(s_pass, 20));
            /* Step first: pm335_send waits for the reply, which lands in on_reply
             * before pm335_control returns */
            s_login_result = -1;
            s_handshake_step = 2;
            s_ready_us = now;
            pm335_control("ffff00", 0x1000, v, sizeof(v));
        } else if (now - s_ready_us > REPLY_TIMEOUT_US) {
            fail_connection("no connect reply (%d)", 0);
            s_handshake_step = 9;
        }
        break;
    case 2:
        if (s_login_result == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_st.logged_in = true;
            s_st.last_error[0] = 0;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "logged in");
            s_handshake_step = 3;
            s_poll_idx = 0;
            s_next_poll_us = now;
            s_next_heartbeat_us = now + HEARTBEAT_US;
        } else if (s_login_result > 0) {
            fail_connection("login rejected (%d) - check user/password", s_login_result);
            s_handshake_step = 9;
        } else if (now - s_ready_us > REPLY_TIMEOUT_US) {
            fail_connection("no login reply (%d)", 0);
            s_handshake_step = 9;
        }
        break;
    case 3:
        if (now >= s_next_heartbeat_us) {
            static const uint8_t hb[2] = {0x00, 0x01};
            pm335_send(PM335_FMT_18, PM335_MSG_HEARTBEAT, hb, sizeof(hb));
            s_next_heartbeat_us = now + HEARTBEAT_US;
        } else if (now >= s_next_poll_us) {
            /* One query per tick, the whole list every CONFIG_PM335_POLL_S */
            query(s_polls[s_poll_idx].ctl ? s_ctl_addr : "ffff00", s_polls[s_poll_idx].first,
                  s_polls[s_poll_idx].last);
            if (++s_poll_idx >= (int)(sizeof(s_polls) / sizeof(s_polls[0]))) {
                s_poll_idx = 0;
                s_next_poll_us = now + CONFIG_PM335_POLL_S * 1000000LL;
            }
        }
        break;
    default:
        break;   /* failed: waiting for the disconnect */
    }
}

static void hex(char *out, const uint8_t *data, size_t len)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = d[data[i] >> 4];
        out[i * 2 + 1] = d[data[i] & 15];
    }
    out[len * 2] = '\0';
}

static void publish(const pm335_status_t *st)
{
    if (st->state != s_published_state) {
        char js[160];
        int n = snprintf(js, sizeof(js), "{\"state\":\"%s\",\"rssi\":%d,\"mtu\":%u,\"error\":\"%s\"}",
                         pm335_state_name(st->state), st->rssi, st->mtu, st->last_error);
        if (cerbo_publish("PM335/state", js, n, true)) s_published_state = st->state;
    }
    /* Latest query replies: PM335/q/<addr>/<first> (retained) */
    {
        EXT_RAM_BSS_ATTR static pm335_value_t v[VALUES_MAX];
        EXT_RAM_BSS_ATTR static char vj[sizeof(v[0].data) * 2 + 96];
        EXT_RAM_BSS_ATTR static char vh[sizeof(v[0].data) * 2 + 1];
        int nv = pm335_values(v, VALUES_MAX);
        for (int i = 0; i < nv; i++) {
            int slot = i;
            if (v[i].gen == s_values_published[slot]) continue;
            char topic[40];
            snprintf(topic, sizeof(topic), "PM335/q/%02x%02x%02x/%04x", v[i].addr[0], v[i].addr[1], v[i].addr[2],
                     v[i].first);
            hex(vh, v[i].data, v[i].len);
            int n = snprintf(vj, sizeof(vj), "{\"result\":%d,\"count\":%d,\"data\":\"%s\"}", v[i].result, v[i].count, vh);
            if (cerbo_publish(topic, vj, n, true)) s_values_published[slot] = v[i].gen;
        }
    }
    /* Per-frame publishing (PM335/frame) removed: the frame log is on /api/pm335 */
}

/* MQTT publishing runs on its own: the MQTT client can block for seconds when
 * it's busy, and that must never stall polling or a switch request */
static void pub_task(void *arg)
{
    for (;;) {
        pm335_status_t st;
        pm335_get(&st);
        publish(&st);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void pm335_task(void *arg)
{
    int64_t next_scan_us = 0;
    for (;;) {
        pm335_status_t st;
        pm335_get(&st);
        int64_t now = esp_timer_get_time();

        if (!st.enabled) {
            if (s_scanning) {
                ble_gap_disc_cancel();
                s_scanning = false;
            }
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            set_state(PM335_DISABLED);
        } else if (!s_synced) {
            set_state(PM335_STARTING);
        } else if (st.state == PM335_DISABLED || st.state == PM335_STARTING) {
            set_state(PM335_SCANNING);
            next_scan_us = 0;
        } else if (st.state == PM335_SCANNING && !s_scanning && now >= next_scan_us) {
            start_scan();
            next_scan_us = now + (SCAN_MS + SCAN_PAUSE_MS) * 1000LL;   /* leave the radio to WiFi in between */
        } else {
            session_step(&st);
        }

        /* Switch requests are handled the moment they arrive; otherwise this
         * is the 500 ms tick */
        switch_req_t r;
        if (xQueueReceive(s_switchq, &r, pdMS_TO_TICKS(500)) == pdTRUE) {
            do_switch(&r);
            while (xQueueReceive(s_switchq, &r, 0) == pdTRUE) do_switch(&r);   /* e.g. pump + hot water */
        }
    }
}

/* ------------------------------------------------------------------ */

void pm335_set_passkey(uint32_t passkey)
{
    if (!s_lock) return;
    s_passkey = passkey;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.passkey_set = passkey != 0;
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "passkey", passkey);
        nvs_commit(h);
        nvs_close(h);
    }
    /* Takes effect on the next connection */
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void nvs_put_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

void pm335_set_login(const char *user, const char *password)
{
    if (!s_lock) return;
    strlcpy(s_user, user, sizeof(s_user));
    strlcpy(s_pass, password, sizeof(s_pass));
    nvs_put_str("user", s_user);
    nvs_put_str("pass", s_pass);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

void pm335_set_ctl_addr(const char *addr_hex)
{
    if (!s_lock || strlen(addr_hex) != 6) return;
    strlcpy(s_ctl_addr, addr_hex, sizeof(s_ctl_addr));
    nvs_put_str("ctl", s_ctl_addr);
}

const char *pm335_ctl_addr(void)
{
    return s_ctl_addr;
}

void pm335_set_enabled(bool enabled)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.enabled = enabled;
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "enabled", enabled);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Crash guard: OTA rollback only covers a firmware that never confirms itself.
 * If the display keeps crash-restarting with BLE on, skip BLE until it has
 * stayed up for a while, so the dashboard still works. */
#define CRASH_LIMIT     2
#define STABLE_US       (5 * 60 * 1000000LL)

static void nvs_put_crashes(uint8_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "crashes", n);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void stable_cb(void *arg)
{
    nvs_put_crashes(0);   /* esp_timer task: internal stack, flash ops are fine */
}

static bool crash_guard_tripped(void)
{
    uint8_t n = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "crashes", &n);
        nvs_close(h);
    }
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        n = n < 255 ? n + 1 : n;
        break;
    default:
        n = 0;
        break;
    }
    nvs_put_crashes(n);
    const esp_timer_create_args_t args = {.callback = stable_cb, .name = "pm335_ok"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, STABLE_US);
    }
    return n >= CRASH_LIMIT;
}

void pm335_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
    s_write_done = xSemaphoreCreateBinary();
    s_switchq = xQueueCreate(4, sizeof(switch_req_t));
    s_reply_sem = xSemaphoreCreateBinary();
    s_log = heap_caps_calloc(LOG_SIZE, sizeof(pm335_log_entry_t), MALLOC_CAP_SPIRAM);

#ifndef CONFIG_PM335_ENABLE
    /* Not configured (menuconfig > Projecta PM335): the API answers "disabled"
     * and Bluetooth is never started */
    s_st.enabled = false;
    s_st.state = PM335_DISABLED;
    snprintf(s_st.last_error, sizeof(s_st.last_error), "not built in");
    return;
#endif

    unsigned b[6] = {0};
    sscanf(CONFIG_PM335_MAC, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    for (int i = 0; i < 6; i++) s_target[i] = b[5 - i];
    strlcpy(s_st.addr, CONFIG_PM335_MAC, sizeof(s_st.addr));
    s_st.session_fmt = PM335_FMT_18;
    s_st.enabled = true;
    nvs_handle_t h;
    uint8_t en;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "enabled", &en) == ESP_OK) s_st.enabled = en;
        nvs_get_u32(h, "passkey", &s_passkey);
        size_t l = sizeof(s_user);
        nvs_get_str(h, "user", s_user, &l);
        l = sizeof(s_pass);
        nvs_get_str(h, "pass", s_pass, &l);
        l = sizeof(s_ctl_addr);
        nvs_get_str(h, "ctl", s_ctl_addr, &l);
        nvs_close(h);
    }
    s_st.passkey_set = s_passkey != 0;
    s_st.state = s_st.enabled ? PM335_STARTING : PM335_DISABLED;

    if (crash_guard_tripped()) {
        ESP_LOGE(TAG, "repeated crashes: BLE skipped this boot");
        s_st.state = PM335_DISABLED;
        snprintf(s_st.last_error, sizeof(s_st.last_error), "skipped after repeated crashes");
        return;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK || !s_log) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        snprintf(s_st.last_error, sizeof(s_st.last_error), "BLE init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* Pairing: we can enter the PM's passkey (KEYBOARD_ONLY), bond in RAM */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    /* Both stacks in PSRAM: internal RAM is what this board runs short of */
    xTaskCreatePinnedToCoreWithCaps(host_task, "nimble", 4096, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(pm335_task, "pm335", 4096, NULL, 4, NULL, 1, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(pub_task, "pm335_pub", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "started, target %s (%s)", CONFIG_PM335_MAC, s_st.enabled ? "enabled" : "disabled");
}
