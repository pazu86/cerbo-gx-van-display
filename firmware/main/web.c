#include "sdkconfig.h"
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cerbo.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fwupdate.h"
#include "inet.h"
#include "mbedtls/base64.h"
#include "mdns.h"
#include "net.h"
#include "pm335.h"
#include "power.h"
#include "secrets.h"
#include "settings.h"

static const char *TAG = "web";

#define HOSTNAME CONFIG_DISPLAY_HOSTNAME

static char s_auth[96];   /* expected "Basic <base64(user:pass)>" */

extern const char web_index_html_start[] asm("_binary_index_html_start");
extern const char web_index_html_end[] asm("_binary_index_html_end");

/* ------------------------------------------------------------------ */

static bool authorised(httpd_req_t *req)
{
    char got[sizeof(s_auth)];
    if (httpd_req_get_hdr_value_str(req, "Authorization", got, sizeof(got)) == ESP_OK && strcmp(got, s_auth) == 0) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Van Display\"");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

/* While an update is in flight or being confirmed, a restart (or another
 * upload) would roll it back or corrupt it */
static bool refuse_if_busy(httpd_req_t *req)
{
    if (!fw_busy()) {
        return false;
    }
    fw_status_t st;
    fw_get(&st);
    char msg[96];
    snprintf(msg, sizeof(msg), "A firmware update is %s - try again in a minute", fw_phase_name(st.phase));
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, msg);
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return err;
}

static void restart_later(void *arg)
{
    esp_restart();
}

static void schedule_restart(void)
{
    const esp_timer_create_args_t args = {.callback = restart_later, .name = "restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1500 * 1000);
    }
}

/* ------------------------------------------------------------------ */

static esp_err_t index_get(httpd_req_t *req)
{
    if (!authorised(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_index_html_start, web_index_html_end - web_index_html_start);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB reset";
    default: return "other";
    }
}

static esp_err_t health_get(httpd_req_t *req)
{
    if (!authorised(req)) return ESP_OK;

    static cerbo_data_t d;   /* big; httpd handlers run one at a time */
    cerbo_get(&d);
    net_status_t ns;
    net_get_status(&ns);
    inet_status_t is;
    inet_get(&is);
    const esp_app_desc_t *app = esp_app_get_description();
    int64_t now = esp_timer_get_time();

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "firmware", app->version);
    cJSON_AddStringToObject(j, "name", CONFIG_DISPLAY_TITLE);
    cJSON_AddStringToObject(j, "built", app->date);
    cJSON_AddStringToObject(j, "idf", app->idf_ver);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(now / 1000000));
    cJSON_AddStringToObject(j, "reset_reason", reset_reason());
    cJSON_AddStringToObject(j, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddStringToObject(j, "screen", power_state_name());
    fw_status_t fw;
    fw_get(&fw);
    cJSON *u = cJSON_AddObjectToObject(j, "update");
    cJSON_AddStringToObject(u, "phase", fw_phase_name(fw.phase));
    cJSON_AddNumberToObject(u, "pct", fw.pct);
    cJSON_AddNumberToObject(u, "secs", fw.secs);
    cJSON_AddStringToObject(u, "message", fw.message);

    cJSON *m = cJSON_AddObjectToObject(j, "memory_kb");
    cJSON_AddNumberToObject(m, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(m, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(m, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(m, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    cJSON_AddNumberToObject(m, "psram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);

    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "state", net_state_name(ns.state));
    cJSON_AddStringToObject(w, "ssid", ns.ssid);
    cJSON_AddStringToObject(w, "ip", ns.ip);
    cJSON_AddNumberToObject(w, "rssi", ns.rssi);

    cJSON *i = cJSON_AddObjectToObject(j, "internet");
    cJSON_AddStringToObject(i, "service", inet_kind_name(is.kind));
    cJSON_AddStringToObject(i, "isp", is.isp);

    cJSON *c = cJSON_AddObjectToObject(j, "cerbo");
    cJSON_AddStringToObject(c, "state", cerbo_state_name(d.state));
    cJSON_AddStringToObject(c, "host", d.host);
    cJSON_AddStringToObject(c, "portal", d.portal);
    cJSON_AddNumberToObject(c, "messages", d.rx_count);
    cJSON_AddNumberToObject(c, "last_message_s", d.last_rx_us ? (double)((now - d.last_rx_us) / 1000000) : -1);
    cJSON_AddBoolToObject(c, "travel_feed", d.travel.valid);
    return send_json(req, j);
}

static cJSON *settings_json(void)
{
    settings_t s;
    settings_get(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "brightness", s.brightness);
    cJSON_AddNumberToObject(j, "idle_timeout_s", s.idle_timeout_s);
    cJSON_AddBoolToObject(j, "night_enabled", s.night_enabled);
    cJSON_AddNumberToObject(j, "night_off_min", s.night_off_min);
    cJSON_AddNumberToObject(j, "night_on_min", s.night_on_min);
    cJSON_AddNumberToObject(j, "night_wake_brightness", s.night_wake_brightness);
    cJSON_AddNumberToObject(j, "night_wake_secs", s.night_wake_secs);
    cJSON_AddNumberToObject(j, "theme", s.theme);
    return j;
}

static esp_err_t settings_get_h(httpd_req_t *req)
{
    if (!authorised(req)) return ESP_OK;
    return send_json(req, settings_json());
}

static esp_err_t settings_post(httpd_req_t *req)
{
    if (!authorised(req)) return ESP_OK;
    char body[512];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
        return ESP_OK;
    }
    settings_t s;
    settings_get(&s);
    const cJSON *v;
#define NUM(field) if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, #field))) s.field = v->valueint
    NUM(brightness);
    NUM(idle_timeout_s);
    NUM(night_off_min);
    NUM(night_on_min);
    NUM(night_wake_brightness);
    NUM(night_wake_secs);
    NUM(theme);
#undef NUM
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "night_enabled"))) s.night_enabled = cJSON_IsTrue(v);
    cJSON_Delete(j);
    settings_set(&s);
    return send_json(req, settings_json());
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authorised(req) || refuse_if_busy(req)) return ESP_OK;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len <= 0 || req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no firmware or too big");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA: %d bytes -> %s", req->content_len, part->label);

    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }
    fw_receive_begin();
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    int remaining = req->content_len;
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    while (err == ESP_OK && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, r);
        remaining -= r;
        fw_receive_progress(req->content_len - remaining, req->content_len);
    }
    free(buf);

    if (err == ESP_OK) {
        fw_installing();
        err = esp_ota_end(ota);   /* verifies the image */
    } else {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        fw_failed(esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA ok, restarting into %s", part->label);
    httpd_resp_sendstr(req, "OK - restarting into the new firmware");
    fw_restart_countdown(3);   /* shows a countdown on screen, then reboots */
    return ESP_OK;
}

/* ---- PM335 bridge: status + frame log, enable/disable, raw send ---- */

static void to_hex(char *out, const uint8_t *data, size_t len)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = d[data[i] >> 4];
        out[i * 2 + 1] = d[data[i] & 15];
    }
    out[len * 2] = '\0';
}

static esp_err_t pm335_get_h(httpd_req_t *req)
{
    if (!authorised(req)) return ESP_OK;
    uint32_t since = 0;
    char q[32], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = strtoul(v, NULL, 10);
    }
    pm335_status_t st;
    pm335_get(&st);
    int64_t now = esp_timer_get_time();

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", pm335_state_name(st.state));
#ifdef CONFIG_PM335_ENABLE
    cJSON_AddTrueToObject(j, "available");
#else
    cJSON_AddFalseToObject(j, "available");   /* not built in: the web page hides the card */
#endif
    cJSON_AddBoolToObject(j, "enabled", st.enabled);
    cJSON_AddStringToObject(j, "addr", st.addr);
    cJSON_AddNumberToObject(j, "rssi", st.rssi);
    cJSON_AddNumberToObject(j, "mtu", st.mtu);
    cJSON_AddNumberToObject(j, "connects", st.connects);
    cJSON_AddNumberToObject(j, "tx_frames", st.tx_frames);
    cJSON_AddNumberToObject(j, "rx_frames", st.rx_frames);
    cJSON_AddNumberToObject(j, "crc_errors", st.crc_errors);
    cJSON_AddNumberToObject(j, "session_fmt", st.session_fmt);
    cJSON_AddStringToObject(j, "profile", st.profile);
    cJSON_AddStringToObject(j, "services", st.services);
    cJSON_AddStringToObject(j, "chars", st.chars);
    cJSON_AddNumberToObject(j, "disconnect_reason", st.disconnect_reason);
    cJSON_AddBoolToObject(j, "passkey_set", st.passkey_set);
    cJSON_AddBoolToObject(j, "encrypted", st.encrypted);
    cJSON_AddStringToObject(j, "trace", st.trace);
    cJSON_AddBoolToObject(j, "logged_in", st.logged_in);
    cJSON *sw = cJSON_AddObjectToObject(j, "last_switch_ms");
    cJSON_AddNumberToObject(sw, "queued", st.switch_queue_ms);
    cJSON_AddNumberToObject(sw, "send", st.switch_send_ms);
    cJSON_AddNumberToObject(sw, "reply", st.switch_reply_ms);
    cJSON_AddNumberToObject(j, "replies_missed", st.replies_missed);
    cJSON_AddStringToObject(j, "ctl_addr", pm335_ctl_addr());
    {
        EXT_RAM_BSS_ATTR static pm335_value_t vals[12];
        EXT_RAM_BSS_ATTR static char vh[sizeof(vals[0].data) * 2 + 1];
        int nv = pm335_values(vals, 12);
        cJSON *va = cJSON_AddArrayToObject(j, "values");
        for (int i = 0; i < nv; i++) {
            cJSON *o = cJSON_CreateObject();
            char a[7], f[5];
            snprintf(a, sizeof(a), "%02x%02x%02x", vals[i].addr[0], vals[i].addr[1], vals[i].addr[2]);
            snprintf(f, sizeof(f), "%04x", vals[i].first);
            cJSON_AddStringToObject(o, "addr", a);
            cJSON_AddStringToObject(o, "first", f);
            cJSON_AddNumberToObject(o, "count", vals[i].count);
            cJSON_AddNumberToObject(o, "result", vals[i].result);
            cJSON_AddNumberToObject(o, "age_ms", (double)((now - vals[i].t_us) / 1000));
            to_hex(vh, vals[i].data, vals[i].len);
            cJSON_AddStringToObject(o, "data", vh);
            cJSON_AddItemToArray(va, o);
        }
    }
    cJSON_AddNumberToObject(j, "last_seen_s", st.last_seen_us ? (double)((now - st.last_seen_us) / 1000000) : -1);
    cJSON_AddStringToObject(j, "error", st.last_error);

    EXT_RAM_BSS_ATTR static pm335_log_entry_t e[16];   /* httpd handlers run one at a time */
    EXT_RAM_BSS_ATTR static char h[PM335_LOG_DATA * 2 + 1];
    int n = pm335_log_since(since, e, 16);
    cJSON *arr = cJSON_AddArrayToObject(j, "frames");
    for (int i = 0; i < n; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "seq", e[i].seq);
        cJSON_AddNumberToObject(f, "age_ms", (double)((now - e[i].t_us) / 1000));
        cJSON_AddStringToObject(f, "dir", e[i].tx ? "tx" : "rx");
        cJSON_AddNumberToObject(f, "fmt", e[i].fmt);
        cJSON_AddNumberToObject(f, "type", e[i].msg_type);
        cJSON_AddBoolToObject(f, "crc", e[i].crc_ok);
        to_hex(h, e[i].body, e[i].len);
        cJSON_AddStringToObject(f, "body", h);
        cJSON_AddItemToArray(arr, f);
    }
    return send_json(req, j);
}

static esp_err_t pm335_post(httpd_req_t *req)
{
    if (!authorised(req)) return ESP_OK;
    char body[600];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
        return ESP_OK;
    }
    const cJSON *v;
    esp_err_t err = ESP_OK;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "enabled"))) {
        pm335_set_enabled(cJSON_IsTrue(v));
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "passkey"))) {   /* never echoed back */
        pm335_set_passkey((uint32_t)v->valuedouble);
    }
    /* {"login": {"user": "user", "password": "...."}} - the app's pairing key; never echoed */
    const cJSON *lg = cJSON_GetObjectItem(j, "login");
    if (cJSON_IsObject(lg)) {
        const cJSON *u = cJSON_GetObjectItem(lg, "user");
        const cJSON *pw = cJSON_GetObjectItem(lg, "password");
        if (cJSON_IsString(u) && cJSON_IsString(pw)) pm335_set_login(u->valuestring, pw->valuestring);
    }
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "ctl_addr"))) {
        pm335_set_ctl_addr(v->valuestring);
    }
    /* {"control": {"prop": "011d", "value": "0001", "addr": "000b00"}} - addr defaults to ctl_addr */
    const cJSON *ct = cJSON_GetObjectItem(j, "control");
    if (cJSON_IsObject(ct)) {
        const cJSON *pr = cJSON_GetObjectItem(ct, "prop");
        const cJSON *va = cJSON_GetObjectItem(ct, "value");
        const cJSON *ad = cJSON_GetObjectItem(ct, "addr");
        uint8_t val[64];
        size_t vn = 0;
        const char *hx = cJSON_IsString(va) ? va->valuestring : "";
        while (hx[0] && hx[1] && vn < sizeof(val)) {
            unsigned b;
            if (sscanf(hx, "%2x", &b) != 1) break;
            val[vn++] = b;
            hx += 2;
        }
        if (cJSON_IsString(pr) && vn) {
            err = pm335_control(cJSON_IsString(ad) ? ad->valuestring : pm335_ctl_addr(),
                                (uint16_t)strtoul(pr->valuestring, NULL, 16), val, vn);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    /* {"send": {"type": 80, "fmt": 9, "payload": "0500000001..."}} - fmt optional */
    const cJSON *s = cJSON_GetObjectItem(j, "send");
    if (cJSON_IsObject(s)) {
        const cJSON *t = cJSON_GetObjectItem(s, "type");
        const cJSON *p = cJSON_GetObjectItem(s, "payload");
        const cJSON *fm = cJSON_GetObjectItem(s, "fmt");
        EXT_RAM_BSS_ATTR static uint8_t pl[200];
        size_t pn = 0;
        const char *hx = cJSON_IsString(p) ? p->valuestring : "";
        while (hx[0] && hx[1] && pn < sizeof(pl)) {
            unsigned b;
            if (sscanf(hx, "%2x", &b) != 1) break;
            pl[pn++] = b;
            hx += 2;
        }
        err = cJSON_IsNumber(t) ? pm335_send(cJSON_IsNumber(fm) ? fm->valueint : -1, t->valueint, pl, pn)
                                : ESP_ERR_INVALID_ARG;
    }
    /* {"raw": "7eff0003a008...."} - bytes written verbatim */
    const cJSON *raw = cJSON_GetObjectItem(j, "raw");
    if (err == ESP_OK && cJSON_IsString(raw)) {
        EXT_RAM_BSS_ATTR static uint8_t rb[200];
        size_t rn = 0;
        const char *hx = raw->valuestring;
        while (hx[0] && hx[1] && rn < sizeof(rb)) {
            unsigned b;
            if (sscanf(hx, "%2x", &b) != 1) break;
            rb[rn++] = b;
            hx += 2;
        }
        err = pm335_send_raw(rb, rn);
    }
    cJSON_Delete(j);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, esp_err_to_name(err));
        return ESP_OK;
    }
    return pm335_get_h(req);
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (!authorised(req) || refuse_if_busy(req)) return ESP_OK;
    httpd_resp_sendstr(req, "restarting");
    schedule_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

void web_start(void)
{
    /* Expected Authorization header */
    char userpass[64];
    snprintf(userpass, sizeof(userpass), "%s:%s", DISPLAY_WEB_USER, DISPLAY_WEB_PASS);
    size_t olen = 0;
    strcpy(s_auth, "Basic ");
    mbedtls_base64_encode((unsigned char *)s_auth + 6, sizeof(s_auth) - 7, &olen, (const unsigned char *)userpass,
                          strlen(userpass));
    s_auth[6 + olen] = '\0';

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 30;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/health", .method = HTTP_GET, .handler = health_get},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_h},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post},
        {.uri = "/api/pm335", .method = HTTP_GET, .handler = pm335_get_h},
        {.uri = "/api/pm335", .method = HTTP_POST, .handler = pm335_post},
    };
    for (size_t k = 0; k < sizeof(uris) / sizeof(uris[0]); k++) {
        httpd_register_uri_handler(srv, &uris[k]);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_instance_name_set(CONFIG_DISPLAY_TITLE);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    ESP_LOGI(TAG, "web interface on http://%s.local", HOSTNAME);
}
