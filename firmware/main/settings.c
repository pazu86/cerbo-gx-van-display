#include "settings.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NVS_NS   "settings"
#define NVS_KEY  "v1"

static const settings_t s_defaults = {
    .brightness = 80,
    .idle_timeout_min = 0,
    .idle_timeout_s = 0,
    .night_enabled = false,
    .night_off_min = 18 * 60,
    .night_on_min = 7 * 60,
    .night_wake_brightness = 20,
    .night_wake_secs = 30,
};

static settings_t s_cur;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_version = 1;
static TaskHandle_t s_saver;

/* NVS writes happen here, never in the caller: the LVGL task's stack is in
 * PSRAM, and flash writes (which freeze the cache) from such a task abort. */
static void saver_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(500));   /* coalesce quick successive changes */
        ulTaskNotifyTake(pdTRUE, 0);
        settings_t v;
        settings_get(&v);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, NVS_KEY, &v, sizeof(v));
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

static void sanitise(settings_t *s)
{
    if (s->brightness < 5 || s->brightness > 100) s->brightness = s_defaults.brightness;
    if (s->idle_timeout_s > 4 * 3600) s->idle_timeout_s = 0;   /* longest option is 1 h */
    s->idle_timeout_min = s->idle_timeout_s / 60;
    if (s->night_off_min >= 24 * 60) s->night_off_min = s_defaults.night_off_min;
    if (s->night_on_min >= 24 * 60) s->night_on_min = s_defaults.night_on_min;
    if (s->night_wake_brightness < 5 || s->night_wake_brightness > 100) s->night_wake_brightness = s_defaults.night_wake_brightness;
    if (s->night_wake_secs < 5) s->night_wake_secs = s_defaults.night_wake_secs;
    if (s->theme > THEME_DAY) s->theme = THEME_AUTO;
}

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cur = s_defaults;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        /* A blob from older firmware is shorter: fields it lacks stay zero */
        settings_t stored = {0};
        size_t len = 0;
        if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len <= sizeof(stored) &&
            nvs_get_blob(h, NVS_KEY, &stored, &len) == ESP_OK) {
            if (len < offsetof(settings_t, idle_timeout_s) + sizeof(stored.idle_timeout_s)) {
                stored.idle_timeout_s = stored.idle_timeout_min * 60;   /* saved by v1.1 or older */
            }
            s_cur = stored;
            sanitise(&s_cur);
        }
        nvs_close(h);
    }
    xTaskCreatePinnedToCore(saver_task, "settings", 2560, NULL, 2, &s_saver, 0);   /* internal stack */
    ESP_LOGI(TAG, "brightness %u%%, timeout %u s, night %s %02u:%02u-%02u:%02u", s_cur.brightness,
             s_cur.idle_timeout_s, s_cur.night_enabled ? "on" : "off", s_cur.night_off_min / 60,
             s_cur.night_off_min % 60, s_cur.night_on_min / 60, s_cur.night_on_min % 60);
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

void settings_set(const settings_t *s)
{
    settings_t v = *s;
    sanitise(&v);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = memcmp(&v, &s_cur, sizeof(v)) != 0;
    s_cur = v;
    xSemaphoreGive(s_lock);
    if (!changed) {
        return;
    }
    s_version++;
    xTaskNotifyGive(s_saver);
}

uint32_t settings_version(void)
{
    return s_version;
}
