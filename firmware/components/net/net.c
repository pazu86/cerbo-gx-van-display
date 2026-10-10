#include "sdkconfig.h"
#include "net.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define HOSTNAME             CONFIG_DISPLAY_HOSTNAME
#define SCAN_MAX_APS         24
#define CONNECT_TIMEOUT_MS   20000
#define IDLE_RESCAN_MS       10000
#define UPGRADE_RESCAN_MS    60000   /* look for a better network this often */
#define RSSI_POLL_MS         5000
#define RETRY_BACKOFF_MAX_MS 30000

#define BIT_GOT_IP       BIT0
#define BIT_DISCONNECTED BIT1
#define BIT_AVOID        BIT2
#define MAX_NETWORKS     8

static const net_network_t *s_nets;
static size_t s_count;
static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static volatile bool s_link_ok;
static TickType_t s_avoid_until[MAX_NETWORKS];
/* Where the app last had a working link; tried first after a reboot */
static int s_last_good = -1;
/* Network the user picked (-1 = Auto): used whenever it's in range, never
 * avoided. Not persisted - every boot starts in Auto. */
static int s_manual = -1;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static net_status_t s_status = {.state = NET_STATE_IDLE, .network = -1};

static void set_state(net_state_t state, int network)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.network = network;
    if (network >= 0) {
        strlcpy(s_status.ssid, s_nets[network].ssid, sizeof(s_status.ssid));
        s_status.cerbo = s_nets[network].cerbo;
        s_status.home = s_nets[network].home;
    } else {
        s_status.ssid[0] = '\0';
        s_status.cerbo = NULL;
        s_status.home = false;
    }
    if (state != NET_STATE_CONNECTED) {
        s_status.ip[0] = '\0';
        s_status.rssi = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

void net_get_status(net_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

int net_count(void)
{
    return (int)s_count;
}

const char *net_ssid(int idx)
{
    return idx >= 0 && idx < (int)s_count ? s_nets[idx].ssid : "";
}

int net_get_selected(void)
{
    return s_manual;
}

/* Called from any task. */
void net_select(int idx)
{
    if (idx < -1 || idx >= (int)s_count) {
        return;
    }
    s_manual = idx;
    if (idx >= 0) {
        s_avoid_until[idx] = 0;
    }
    ESP_LOGI(TAG, "network choice: %s", idx >= 0 ? s_nets[idx].ssid : "auto");
    xEventGroupSetBits(s_events, BIT_AVOID);   /* re-evaluate now */
}

static void select_task(void *arg)
{
    net_select((int)(intptr_t)arg);
    vTaskDelete(NULL);
}

void net_select_async(int idx)
{
    /* Off the caller's task: net_select can block briefly on the event group */
    xTaskCreate(select_task, "net_sel", 3072, (void *)(intptr_t)idx, 4, NULL);
}

const char *net_state_name(net_state_t state)
{
    switch (state) {
    case NET_STATE_IDLE: return "no network";
    case NET_STATE_SCANNING: return "scanning";
    case NET_STATE_CONNECTING: return "connecting";
    case NET_STATE_CONNECTED: return "connected";
    }
    return "?";
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "disconnected from %s (reason %d)", (char *)e->ssid, e->reason);
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        portENTER_CRITICAL(&s_lock);
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&e->ip_info.ip));
        portEXIT_CRITICAL(&s_lock);
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static bool avoided(int idx)
{
    return s_avoid_until[idx] && (int32_t)(s_avoid_until[idx] - xTaskGetTickCount()) > 0;
}

void net_set_link_ok(bool ok)
{
    s_link_ok = ok;
    if (!ok) {
        return;
    }
    net_status_t st;
    net_get_status(&st);
    if (st.network >= 0 && st.network != s_last_good) {
        s_last_good = st.network;
        nvs_handle_t h;
        if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "last_ok", (uint8_t)st.network);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "remembering %s as the working network", st.ssid);
    }
}

void net_avoid_current(uint32_t avoid_ms)
{
    net_status_t st;
    net_get_status(&st);
    if (st.network < 0) {
        return;
    }
    if (st.network == s_manual) {
        ESP_LOGI(TAG, "%s was chosen by the user, staying", st.ssid);
        return;
    }
    ESP_LOGW(TAG, "avoiding %s for %lu s", st.ssid, (unsigned long)(avoid_ms / 1000));
    s_avoid_until[st.network] = xTaskGetTickCount() + pdMS_TO_TICKS(avoid_ms);
    xEventGroupSetBits(s_events, BIT_AVOID);
}

/* Scan and return the network to use, or -1: the last network where the
 * app's link worked if it's in range, else the highest-priority known one
 * (lowest list index). Networks being avoided only win if nothing else is
 * in range. Works while connected too. */
static int scan_best(void)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return -1;
    }
    uint16_t n = SCAN_MAX_APS;
    wifi_ap_record_t *aps = calloc(n, sizeof(*aps));
    if (!aps) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, aps);

    int best = -1, best_avoided = -1;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < (int)s_count; k++) {
            if (strcmp((char *)aps[i].ssid, s_nets[k].ssid) != 0) {
                continue;
            }
            int *slot = avoided(k) ? &best_avoided : &best;
            if (*slot < 0 || k < *slot) {
                *slot = k;
            }
        }
    }
    bool last_good_seen = false, manual_seen = false;
    for (int i = 0; i < n; i++) {
        if (s_last_good >= 0) last_good_seen |= strcmp((char *)aps[i].ssid, s_nets[s_last_good].ssid) == 0;
        if (s_manual >= 0) manual_seen |= strcmp((char *)aps[i].ssid, s_nets[s_manual].ssid) == 0;
    }
    free(aps);
    if (manual_seen) {
        return s_manual;
    }
    if (last_good_seen && !avoided(s_last_good)) {
        return s_last_good;
    }
    return best >= 0 ? best : best_avoided;
}

static bool connect_to(int idx)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_nets[idx].ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_nets[idx].pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    /* Same SSID can have several APs (mesh/extenders): pick the strongest */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    set_state(NET_STATE_CONNECTING, idx);
    ESP_LOGI(TAG, "connecting to %s", s_nets[idx].ssid);
    s_link_ok = false;
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED | BIT_AVOID);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (bits & BIT_GOT_IP) {
        set_state(NET_STATE_CONNECTED, idx);
        net_status_t st;
        net_get_status(&st);
        ESP_LOGI(TAG, "connected to %s, ip %s, cerbo %s", st.ssid, st.ip, st.cerbo);
        return true;
    }
    ESP_LOGW(TAG, "failed to connect to %s", s_nets[idx].ssid);
    esp_wifi_disconnect();
    return false;
}

static void update_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.rssi = ap.rssi;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void drop_link(void)
{
    esp_wifi_disconnect();
    xEventGroupWaitBits(s_events, BIT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
}

/* Stay connected until the link drops, the app asks us to avoid this
 * network, or a better network shows up (only while the app's link isn't ok). */
static void stay_connected(int idx)
{
    TickType_t last_upgrade_scan = xTaskGetTickCount();
    for (;;) {
        update_rssi();
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DISCONNECTED | BIT_AVOID, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(RSSI_POLL_MS));
        if (bits & BIT_DISCONNECTED) {
            xEventGroupClearBits(s_events, BIT_DISCONNECTED);
            return;
        }
        if (bits & BIT_AVOID) {
            xEventGroupClearBits(s_events, BIT_AVOID);
            if (scan_best() != idx) {
                drop_link();
                return;
            }
            ESP_LOGI(TAG, "no alternative to %s in range, staying", s_nets[idx].ssid);
        }
        bool off_choice = s_manual >= 0 && idx != s_manual;   /* user's pick not in range earlier */
        if ((!s_link_ok || off_choice) && xTaskGetTickCount() - last_upgrade_scan >= pdMS_TO_TICKS(UPGRADE_RESCAN_MS)) {
            last_upgrade_scan = xTaskGetTickCount();
            int best = scan_best();
            if (best >= 0 && best != idx && (best < idx || avoided(idx) || best == s_manual)) {
                ESP_LOGI(TAG, "%s is in range, switching from %s", s_nets[best].ssid, s_nets[idx].ssid);
                drop_link();
                return;
            }
        }
    }
}

static void net_task(void *arg)
{
    uint32_t backoff_ms = 1000;
    for (;;) {
        set_state(NET_STATE_SCANNING, -1);
        int idx = scan_best();
        if (idx < 0) {
            set_state(NET_STATE_IDLE, -1);
            vTaskDelay(pdMS_TO_TICKS(IDLE_RESCAN_MS));
            continue;
        }
        if (!connect_to(idx)) {
            set_state(NET_STATE_IDLE, -1);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 > RETRY_BACKOFF_MAX_MS ? RETRY_BACKOFF_MAX_MS : backoff_ms * 2;
            continue;
        }
        backoff_ms = 1000;
        stay_connected(idx);
    }
}

esp_err_t net_start(const net_network_t *networks, size_t count)
{
    ESP_RETURN_ON_FALSE(networks && count > 0 && count <= MAX_NETWORKS, ESP_ERR_INVALID_ARG, TAG, "network list");
    s_nets = networks;
    s_count = count;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    nvs_handle_t h;
    uint8_t last;
    if (nvs_open("net", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "last_ok", &last) == ESP_OK && last < count) {
            s_last_good = last;
        }

        nvs_close(h);
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* The display is mains-powered from the van; power save adds MQTT latency */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "events");
    ESP_RETURN_ON_FALSE(xTaskCreate(net_task, "net", 4096, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
