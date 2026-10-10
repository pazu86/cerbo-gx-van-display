/*
 * Van Display - Victron-style head unit for a caravan's Cerbo GX.
 */
#include "sdkconfig.h"
#include <stdlib.h>
#include <time.h>

#include "board.h"
#include "cerbo.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "fwupdate.h"
#include "esp_timer.h"
#include "inet.h"
#include "net.h"
#include "pm335.h"
#include "secrets.h"
#include "settings.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "display";

static const net_network_t s_networks[] = DISPLAY_NETWORKS;

static void heartbeat_cb(lv_timer_t *t)
{
    static cerbo_data_t d;
    cerbo_get(&d);
    net_status_t ns;
    net_get_status(&ns);
    pm335_status_t pm;
    pm335_get(&pm);
    ESP_LOGI(TAG, "up %lus | int %u kB (min %u) psram %u kB | %s %s | cerbo %s, %lu msgs | travel %s | pm335 %s",
             (unsigned long)(esp_timer_get_time() / 1000000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             net_state_name(ns.state), ns.ssid, cerbo_state_name(d.state), (unsigned long)d.rx_count,
             d.travel.valid ? "ok" : "none", pm335_state_name(pm.state));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Van Display starting");
    settings_init();   /* also initialises NVS */
    fw_init();         /* confirms a freshly updated firmware once it's healthy */
    ESP_ERROR_CHECK(board_init());

    /* The van's local time (menuconfig: Van Display > Time zone) */
    setenv("TZ", CONFIG_DISPLAY_TZ, 1);
    tzset();

    /* Before the UI: its first refresh reads cerbo data. The task just waits
     * for WiFi until net_start() below. */
    cerbo_start();

    if (board_lvgl_lock(-1)) {
        ui_init();
        lv_timer_create(heartbeat_cb, 30000, NULL);
        board_lvgl_unlock();
    }

    /* Never fatal: without WiFi the display still works and shows it's offline */
    esp_err_t err = net_start(s_networks, sizeof(s_networks) / sizeof(s_networks[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return;
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    inet_start();
    web_start();

    /* After WiFi has its buffers: BLE is optional, the display works without it */
    pm335_start();
}
