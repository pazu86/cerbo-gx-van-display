/*
 * Optional self-update from Home Assistant.
 *
 * The van_display_update custom component (homeassistant/ in this repo)
 * serves the build that `tools\build.ps1 ha` published: GET /api/van_display/manifest?running=<v>
 * gives version, size and SHA-256, GET /api/van_display/firmware the image.
 * Both need this display's key (header X-Display-Key, DISPLAY_UPDATE_KEY in
 * secrets.h): the image embeds secrets.h, so HA never serves it publicly.
 *
 * Checked FIRST_CHECK_US after joining a network (once the running image is
 * confirmed), then every PERIOD_US, never while the van is on the move. On a
 * network flagged .home HA is asked at DISPLAY_HA_LOCAL_URL first, then at
 * DISPLAY_HA_URL (e.g. Nabu Casa) elsewhere or if that fails. A newer
 * version is streamed into the other OTA slot with its size and SHA-256
 * checked, then fwupdate.c's countdown, restart and self-confirmation follow. A version the bootloader rolled back (it didn't
 * confirm) is never fetched again.
 *
 * This task's stack is in PSRAM (room for TLS), and flash operations from a
 * PSRAM stack abort, so the OTA writes run in a short-lived helper task with
 * an internal-RAM stack.
 */
#include "selfupdate.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fwupdate.h"
#include "mbedtls/sha256.h"
#include "motion.h"
#include "net.h"
#include "secrets.h"

static const char *TAG = "selfupdate";

#define FIRST_CHECK_US   (60 * 1000 * 1000LL)        /* after joining a network */
#define PERIOD_US        (30 * 60 * 1000 * 1000LL)
#define MOVED_RECENTLY_S 600                         /* motion.c detection this recent = on the move */
#define RESTART_S        5                           /* on-screen countdown before restarting */
#define CHUNK            4096
#define FLASH_STACK      4096                        /* internal RAM, only while downloading */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_note[64];
static volatile bool s_check_now;
static volatile bool s_renote;   /* a press replaced the note with "checking..." */

static void set_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_note(const char *fmt, ...)
{
    char note[sizeof(s_note)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(note, sizeof(note), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", note);
    portENTER_CRITICAL(&s_mux);
    memcpy(s_note, note, sizeof(note));
    portEXIT_CRITICAL(&s_mux);
}

void selfupdate_note(char *out, size_t size)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(out, s_note, size);
    portEXIT_CRITICAL(&s_mux);
}

void selfupdate_check_now(void)
{
#ifdef DISPLAY_UPDATE_KEY
    set_note("checking...");   /* shown at once; the task replaces it with the result or why it waits */
    s_renote = true;
#endif
    s_check_now = true;
}

#ifdef DISPLAY_UPDATE_KEY

/* ---- OTA writes in an internal-RAM-stack helper task ---- */

typedef enum { FL_BEGIN, FL_WRITE, FL_FINISH, FL_ABORT } fl_op_t;

static struct {
    fl_op_t op;
    const esp_partition_t *part;
    esp_ota_handle_t ota;
    const void *buf;
    size_t len;
    esp_err_t err;
} s_fl;
static SemaphoreHandle_t s_fl_go, s_fl_done;

static void flash_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_fl_go, portMAX_DELAY);
        switch (s_fl.op) {
        case FL_BEGIN:
            s_fl.err = esp_ota_begin(s_fl.part, OTA_WITH_SEQUENTIAL_WRITES, &s_fl.ota);
            break;
        case FL_WRITE:
            s_fl.err = esp_ota_write(s_fl.ota, s_fl.buf, s_fl.len);
            break;
        case FL_FINISH:
            s_fl.err = esp_ota_end(s_fl.ota);   /* verifies the image; frees the handle either way */
            if (s_fl.err == ESP_OK) {
                s_fl.err = esp_ota_set_boot_partition(s_fl.part);
            }
            break;
        case FL_ABORT:
            s_fl.err = esp_ota_abort(s_fl.ota);
            break;
        }
        xSemaphoreGive(s_fl_done);
    }
}

static esp_err_t flash_op(fl_op_t op, const void *buf, size_t len)
{
    s_fl.op = op;
    s_fl.buf = buf;
    s_fl.len = len;
    xSemaphoreGive(s_fl_go);
    xSemaphoreTake(s_fl_done, portMAX_DELAY);
    return s_fl.err;
}

/* ---- HTTP ---- */

static esp_http_client_handle_t open_get(const char *base, const char *path, int64_t *length)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    esp_http_client_set_header(c, "X-Display-Key", DISPLAY_UPDATE_KEY);
    if (esp_http_client_open(c, 0) == ESP_OK) {
        *length = esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            return c;
        }
        ESP_LOGW(TAG, "%s: HTTP %d", url, esp_http_client_get_status_code(c));
    } else {
        ESP_LOGW(TAG, "%s: no connection", url);
    }
    esp_http_client_cleanup(c);
    return NULL;
}

/* "2.10.0" is newer than "2.9.3" */
static bool newer(const char *candidate, const char *running)
{
    int a[3] = {0}, b[3] = {0};
    sscanf(candidate, "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf(running, "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

/* The version the bootloader last rolled back from. Read once in
 * selfupdate_start(): it reads flash, which would abort from this task's
 * PSRAM stack (that crash-looped the display once a newer version was published). */
static char s_rolled_back[32];

static bool rolled_back(const char *version)
{
    return s_rolled_back[0] && strcmp(s_rolled_back, version) == 0;
}

/* Streams the image into the other slot and selects it for the next boot.
 * NULL when installed, else why not. */
static const char *download(const char *base, int size, const char *sha256_hex)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || size <= 0 || (uint32_t)size > part->size) {
        return "size doesn't fit the slot";
    }
    /* The helper's stack must not starve WiFi of internal RAM */
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < FLASH_STACK + 1024 ||
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < FLASH_STACK + 10240) {
        return "not enough memory";
    }
    TaskHandle_t writer = NULL;
    if (xTaskCreatePinnedToCore(flash_task, "fwwrite", FLASH_STACK, NULL, 3, &writer, 0) != pdPASS) {
        return "not enough memory";
    }
    const char *failure = NULL;
    int64_t length = 0;
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    esp_http_client_handle_t c = buf ? open_get(base, "/api/van_display/firmware", &length) : NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    s_fl.part = part;
    if (!buf) {
        failure = "not enough memory";
    } else if (!c) {
        failure = "Home Assistant didn't send it";
    } else if (length != size) {
        failure = "unexpected file size";
    } else if (flash_op(FL_BEGIN, NULL, 0) != ESP_OK) {
        failure = "couldn't start the flash write";
    } else {
        fw_receive_begin();
        mbedtls_sha256_starts(&sha, 0);
        int done = 0;
        while (done < size && !failure) {
            int r = esp_http_client_read(c, buf, size - done < CHUNK ? size - done : CHUNK);
            if (r <= 0) {
                failure = "download interrupted";
            } else if (flash_op(FL_WRITE, buf, r) != ESP_OK) {
                failure = "flash write failed";
            } else {
                mbedtls_sha256_update(&sha, (const unsigned char *)buf, r);
                done += r;
                fw_receive_progress(done, size);
            }
        }
        if (!failure) {
            unsigned char digest[32];
            char hex[65];
            mbedtls_sha256_finish(&sha, digest);
            for (int i = 0; i < 32; i++) {
                snprintf(hex + 2 * i, 3, "%02x", digest[i]);
            }
            if (strcasecmp(hex, sha256_hex) != 0) {
                failure = "checksum mismatch";
            }
        }
        if (!failure) {
            fw_installing();
            if (flash_op(FL_FINISH, NULL, 0) != ESP_OK) {
                failure = "the new image failed verification";
            }
        } else {
            flash_op(FL_ABORT, NULL, 0);
        }
    }
    mbedtls_sha256_free(&sha);
    if (c) {
        esp_http_client_cleanup(c);
    }
    free(buf);
    vTaskDelete(writer);   /* idle, waiting for the next op */
    return failure;
}

/* Fetches the manifest from the first base that answers. Returns that base,
 * or NULL. */
static const char *fetch_manifest(const char *running, char *version, int *size, char *sha)
{
    net_status_t ns;
    net_get_status(&ns);
    const char *bases[2];
    int n = 0;
#ifdef DISPLAY_HA_LOCAL_URL
    if (ns.home) {
        bases[n++] = DISPLAY_HA_LOCAL_URL;
    }
#endif
    bases[n++] = DISPLAY_HA_URL;

    char path[80];
    snprintf(path, sizeof(path), "/api/van_display/manifest?running=%s", running);
    for (int i = 0; i < n; i++) {
        int64_t length = 0;
        esp_http_client_handle_t c = open_get(bases[i], path, &length);
        if (!c) {
            continue;
        }
        char body[512];
        int len = esp_http_client_read_response(c, body, sizeof(body) - 1);
        esp_http_client_cleanup(c);
        if (len <= 0) {
            continue;
        }
        body[len] = '\0';
        cJSON *j = cJSON_Parse(body);
        if (!j) {
            continue;
        }
        const cJSON *v = cJSON_GetObjectItem(j, "version");
        const cJSON *sz = cJSON_GetObjectItem(j, "size");
        const cJSON *sh = cJSON_GetObjectItem(j, "sha256");
        snprintf(version, 32, "%s", cJSON_IsString(v) ? v->valuestring : "");
        snprintf(sha, 65, "%s", cJSON_IsString(sh) ? sh->valuestring : "");
        *size = cJSON_IsNumber(sz) ? sz->valueint : 0;
        cJSON_Delete(j);
        return bases[i];
    }
    return NULL;
}

static bool on_the_move(void)
{
    motion_status_t mo;
    motion_get(&mo);
    return mo.far_s > 0 || (mo.last_trip_s >= 0 && mo.last_trip_s < MOVED_RECENTLY_S);
}

static void check(void)
{
    const char *running = esp_app_get_description()->version;
    char version[32] = "", sha[65] = "";
    int size = 0;
    const char *base = fetch_manifest(running, version, &size, sha);
    if (!base) {
        set_note("couldn't reach Home Assistant");
    } else if (!version[0]) {
        set_note("%s, nothing published", running);
    } else if (!newer(version, running)) {
        set_note("%s is current", running);
    } else if (rolled_back(version)) {
        set_note("skipping %s (it was rolled back)", version);
    } else if (strlen(sha) != 64) {
        set_note("%s published without a checksum", version);
    } else {
        set_note("%s found, downloading", version);
        const char *why = download(base, size, sha);
        if (why) {
            set_note("%s failed: %s", version, why);
            fw_failed(why);
        } else {
            set_note("%s installed, restarting", version);
            fw_restart_countdown(RESTART_S);
        }
    }
}

static void selfupdate_task(void *arg)
{
    int64_t next_us = 0;   /* 0 = not scheduled on this network yet */
    bool was_up = false;
    char ssid[33] = "";
    bool waiting_noted = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        net_status_t ns;
        net_get_status(&ns);
        bool up = ns.state == NET_STATE_CONNECTED;
        if (up != was_up || (up && strcmp(ssid, ns.ssid) != 0)) {
            was_up = up;
            strlcpy(ssid, ns.ssid, sizeof(ssid));
            next_us = up ? now + FIRST_CHECK_US : 0;   /* new network: check once it has settled */
        }
        bool asked = s_check_now;
        if (!up || fw_busy() || on_the_move()) {
            if (asked && (!waiting_noted || s_renote)) {
                waiting_noted = true;
                s_renote = false;
                set_note("check asked for, waiting for %s",
                         !up ? "WiFi" : fw_busy() ? "the current update" : "the van to stop");
            }
            continue;
        }
        if (asked) {
            s_check_now = false;
            s_renote = false;
            waiting_noted = false;
            next_us = now;
        }
        if (!next_us || now < next_us) {
            continue;
        }
        next_us = now + PERIOD_US;
        check();
    }
}

void selfupdate_start(void)
{
    s_fl_go = xSemaphoreCreateBinary();
    s_fl_done = xSemaphoreCreateBinary();
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();   /* app_main: internal stack */
    esp_app_desc_t desc;
    if (bad && esp_ota_get_partition_description(bad, &desc) == ESP_OK) {
        strlcpy(s_rolled_back, desc.version, sizeof(s_rolled_back));
    }
    /* PSRAM stack: TLS needs room and internal RAM is scarce */
    xTaskCreatePinnedToCoreWithCaps(selfupdate_task, "selfupdate", 10240, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

#else  /* no DISPLAY_UPDATE_KEY in secrets.h */

void selfupdate_start(void)
{
    set_note("not set up (no DISPLAY_UPDATE_KEY)");
}

#endif
