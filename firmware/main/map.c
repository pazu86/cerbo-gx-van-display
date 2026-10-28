#include "map.h"

#include <math.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "net.h"
#include "png565.h"
#include "splash.h"
#include "ui.h"

static const char *TAG = "map";

#define TILE        256
#define SLOTS       9             /* exactly what a 512x512 view can show; 1.15 MB PSRAM */
#define ZOOM_MIN    6
#define ZOOM_MAX    17
#define ZOOM_DEFAULT 13
#define PNG_MAX     (96 * 1024)
#define RETRY_US    (30LL * 1000 * 1000)
#define TILE_URL    "https://tile.openstreetmap.org/%d/%d/%d.png"
/* OSM tile policy: identify the app; we fetch a handful of tiles and cache them */
#define USER_AGENT  "Van-Display/1.0 (ESP32 caravan dashboard)"

typedef enum { SLOT_EMPTY, SLOT_WANTED, SLOT_LOADING, SLOT_READY, SLOT_FAILED } slot_state_t;

typedef struct {
    int z, x, y;
    slot_state_t state;
    uint32_t gen;                 /* bumps every time new pixels land */
    uint32_t last_used;
    int64_t retry_at_us;
    lv_image_dsc_t dsc;           /* points at px */
    uint16_t *px;
} slot_t;

static slot_t s_slots[SLOTS];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_dirty = true;
static uint32_t s_tick;
/* Centre of the view in tile units, so tiles load from the middle outwards */
static int s_view_z;
static double s_view_tx, s_view_ty;

/* LVGL side */
static lv_obj_t *s_box, *s_imgs[9], *s_van, *s_heading, *s_home, *s_msg, *s_zoom_lbl;
static uint32_t s_img_gen[9];
static int s_img_slot[9];
static lv_point_precise_t s_heading_pts[2];
static int s_zoom = ZOOM_DEFAULT;
static double s_last_lat = NAN, s_last_lon = NAN;
static int s_last_zoom;

/* ------------------------------------------------------------------ */
/* Web mercator                                                       */
/* ------------------------------------------------------------------ */

static double lon_to_px(double lon, int z) { return (lon + 180.0) / 360.0 * (TILE << z); }

static double lat_to_px(double lat, int z)
{
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (TILE << z);
}

/* ------------------------------------------------------------------ */
/* Slot pool (s_lock held)                                            */
/* ------------------------------------------------------------------ */

static slot_t *find_slot(int z, int x, int y)
{
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &s_slots[i];
        if (s->state != SLOT_EMPTY && s->z == z && s->x == x && s->y == y) {
            return s;
        }
    }
    return NULL;
}

/* Claim a slot for a tile, evicting the least recently used one that isn't
 * on screen or loading. */
static slot_t *request_slot(int z, int x, int y)
{
    slot_t *victim = NULL;
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &s_slots[i];
        if (s->state == SLOT_LOADING || s->last_used == s_tick) {
            continue;
        }
        if (!victim || s->state == SLOT_EMPTY || (victim->state != SLOT_EMPTY && s->last_used < victim->last_used)) {
            victim = s;
        }
    }
    if (victim) {
        victim->z = z;
        victim->x = x;
        victim->y = y;
        victim->state = SLOT_WANTED;
        victim->retry_at_us = 0;
        victim->last_used = s_tick;
    }
    return victim;
}

/* ------------------------------------------------------------------ */
/* Fetch task                                                         */
/* ------------------------------------------------------------------ */

/* One HTTPS connection reused for every tile (keep-alive): after the first
 * tile, each fetch skips the TLS handshake, which is most of the time. */
typedef struct {
    uint8_t *buf;
    int max, len;
    bool overflow;
} fetch_ctx_t;

static esp_http_client_handle_t s_http;
static fetch_ctx_t s_fetch;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        fetch_ctx_t *f = e->user_data;
        if (f->len + e->data_len <= f->max) {
            memcpy(f->buf + f->len, e->data, e->data_len);
            f->len += e->data_len;
        } else {
            f->overflow = true;
        }
    }
    return ESP_OK;
}

/* One GET on the shared connection (opened if needed); on failure the connection is dropped */
static esp_err_t perform(const char *url, uint8_t *buf, int max)
{
    s_fetch = (fetch_ctx_t){.buf = buf, .max = max};
    if (!s_http) {
        esp_http_client_config_t cfg = {
            .url = url,
            .user_agent = USER_AGENT,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .buffer_size = 2048,
            .keep_alive_enable = true,
            .event_handler = http_event,
            .user_data = &s_fetch,
        };
        s_http = esp_http_client_init(&cfg);
        if (!s_http) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        esp_http_client_set_url(s_http, url);
    }

    esp_err_t err = esp_http_client_perform(s_http);
    if (err != ESP_OK) {
        /* Drop the connection; the next tile starts a fresh one */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    return err;
}

static int fetch_png(int z, int x, int y, uint8_t *buf, int max)
{
    char url[96];
    snprintf(url, sizeof(url), TILE_URL, z, x, y);
    bool reused = s_http != NULL;
    esp_err_t err = perform(url, buf, max);
    /* A kept-alive connection the server closed while idle fails at once:
     * retry on a fresh one now rather than in RETRY_US */
    if (err != ESP_OK && reused) {
        err = perform(url, buf, max);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tile %d/%d/%d: %s", z, x, y, esp_err_to_name(err));
        return -1;
    }
    int status = esp_http_client_get_status_code(s_http);
    if (status != 200 || s_fetch.overflow) {
        ESP_LOGW(TAG, "tile %d/%d/%d: HTTP %d%s", z, x, y, status, s_fetch.overflow ? " (too big)" : "");
        return -1;
    }
    return s_fetch.len;
}

static void map_task(void *arg)
{
    /* The boot splash borrows this PSRAM for its image: wait until it's done */
    while (splash_active()) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SLOTS; i++) {
        s_slots[i].px = heap_caps_malloc(TILE * TILE * 2, MALLOC_CAP_SPIRAM);
        s_slots[i].dsc.data = (const uint8_t *)s_slots[i].px;
    }
    xSemaphoreGive(s_lock);

    /* All decode memory is allocated once, here: no per-tile heap churn */
    uint8_t *png = heap_caps_malloc(PNG_MAX, MALLOC_CAP_SPIRAM);
    png565_t *dec = png565_create(TILE, TILE);
    if (!png || !dec) {
        ESP_LOGE(TAG, "no memory for tile decoding, map disabled");
        vTaskDeleteWithCaps(NULL);
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        net_status_t ns;
        net_get_status(&ns);
        if (ns.state != NET_STATE_CONNECTED) {
            continue;
        }

        /* Next tile to load: wanted, or failed and due a retry */
        slot_t *job = NULL;
        int z = 0, x = 0, y = 0;
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        double best = 1e18;
        for (int i = 0; i < SLOTS; i++) {
            slot_t *s = &s_slots[i];
            if (s->state == SLOT_WANTED || (s->state == SLOT_FAILED && now >= s->retry_at_us)) {
                double dx = s->x + 0.5 - s_view_tx, dy = s->y + 0.5 - s_view_ty;
                double dist = (s->z == s_view_z) ? dx * dx + dy * dy : 1e9;  /* old zoom last */
                if (dist < best) {
                    best = dist;
                    job = s;
                }
            }
        }
        if (job) {
            job->state = SLOT_LOADING;
            z = job->z, x = job->x, y = job->y;
        }
        xSemaphoreGive(s_lock);
        if (!job) {
            continue;
        }

        int len = fetch_png(z, x, y, png, PNG_MAX);
        bool ok = len > 0 && png565_decode(dec, png, len, job->px, TILE, TILE);
        ESP_LOGI(TAG, "tile %d/%d/%d %s (%d bytes), %lld ms", z, x, y, ok ? "ok" : "FAILED", len,
                 (esp_timer_get_time() - now) / 1000);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ok) {
            job->state = SLOT_READY;
            job->gen++;
            s_dirty = true;
        } else {
            job->state = SLOT_FAILED;
            job->retry_at_us = esp_timer_get_time() + RETRY_US;
        }
        xSemaphoreGive(s_lock);
        xTaskNotifyGive(s_task);  /* go straight on to the next one */
    }
}

/* ------------------------------------------------------------------ */
/* LVGL side                                                          */
/* ------------------------------------------------------------------ */

static void zoom_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int z = s_zoom + delta;
    if (z >= ZOOM_MIN && z <= ZOOM_MAX) {
        s_zoom = z;
        s_dirty = true;
        lv_label_set_text_fmt(s_zoom_lbl, "%d", s_zoom);
    }
}

static lv_obj_t *zoom_btn(const char *txt, int y, int delta)
{
    lv_obj_t *b = lv_button_create(s_box);
    lv_obj_set_size(b, 52, 52);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -10, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_24, C_TEXT, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, zoom_cb, LV_EVENT_CLICKED, (void *)(intptr_t)delta);
    return b;
}

static lv_obj_t *dot(int size, uint32_t fill, uint32_t border)
{
    lv_obj_t *d = lv_obj_create(s_box);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(fill), 0);
    lv_obj_set_style_border_width(d, 3, 0);
    lv_obj_set_style_border_color(d, lv_color_hex(border), 0);
    lv_obj_set_clickable(d, false);
    return d;
}

void map_create(lv_obj_t *parent, int x, int y)
{
    /* Called again on every theme rebuild: the tile pool and fetch task are
     * created once and kept; only the LVGL objects are recreated. */
    static bool started;
    if (started) {
        goto build_objects;
    }
    started = true;
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &s_slots[i];
        /* px is allocated by map_task, after the boot splash */
        s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s->dsc.header.w = TILE;
        s->dsc.header.h = TILE;
        s->dsc.header.stride = TILE * 2;
        s->dsc.data_size = TILE * TILE * 2;
    }
    /* Stack in PSRAM: TLS + PNG decode want ~10 kB and internal RAM is scarce */
    xTaskCreatePinnedToCoreWithCaps(map_task, "map", 12 * 1024, NULL, 3, &s_task, 1, MALLOC_CAP_SPIRAM);

build_objects:
    s_last_lat = s_last_lon = NAN;
    s_dirty = true;

    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_pos(s_box, x, y);
    lv_obj_set_size(s_box, MAP_W, MAP_H);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x202224), 0);
    lv_obj_set_style_radius(s_box, 10, 0);
    lv_obj_set_style_clip_corner(s_box, true, 0);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_border_color(s_box, lv_color_hex(C_BORDER), 0);
    lv_obj_set_scrollable(s_box, false);

    for (int i = 0; i < 9; i++) {
        s_imgs[i] = lv_image_create(s_box);
        lv_obj_set_hidden(s_imgs[i], true);
        s_img_slot[i] = -1;
    }

    s_msg = ui_label(s_box, &lv_font_montserrat_20, C_DIM, "Waiting for GPS");
    lv_obj_center(s_msg);

    s_home = dot(18, C_GREEN, C_TEXT);
    lv_obj_set_hidden(s_home, true);

    s_heading = lv_line_create(s_box);
    lv_obj_set_style_line_width(s_heading, 5, 0);
    lv_obj_set_style_line_color(s_heading, lv_color_hex(C_BLUE), 0);
    lv_obj_set_style_line_rounded(s_heading, true, 0);
    lv_obj_set_hidden(s_heading, true);
    s_van = dot(24, C_BLUE, C_TEXT);
    lv_obj_set_pos(s_van, MAP_W / 2 - 12, MAP_H / 2 - 12);
    lv_obj_set_hidden(s_van, true);

    zoom_btn(LV_SYMBOL_PLUS, 10, +1);
    s_zoom_lbl = ui_label(s_box, &lv_font_montserrat_16, C_TEXT, "");
    lv_label_set_text_fmt(s_zoom_lbl, "%d", s_zoom);
    lv_obj_set_style_bg_opa(s_zoom_lbl, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_zoom_lbl, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_pad_hor(s_zoom_lbl, 6, 0);
    lv_obj_align(s_zoom_lbl, LV_ALIGN_TOP_RIGHT, -26, 70);
    zoom_btn(LV_SYMBOL_MINUS, 96, -1);

    lv_obj_t *attr = ui_label(s_box, &lv_font_montserrat_12, 0x333333, "(c) OpenStreetMap contributors");
    lv_obj_set_style_bg_opa(attr, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(attr, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(attr, 4, 0);
    lv_obj_align(attr, LV_ALIGN_BOTTOM_RIGHT, -4, -4);

}

void map_update(double lat, double lon, float course, double home_lat, double home_lon)
{
    bool have_pos = !isnan(lat) && !isnan(lon);
    if (!have_pos && !isnan(home_lat)) {
        lat = home_lat;   /* no fix yet: show home */
        lon = home_lon;
    }
    if (isnan(lat) || isnan(lon)) {
        lv_obj_set_hidden(s_msg, false);
        return;
    }
    lv_obj_set_hidden(s_msg, true);

    bool moved = fabs(lat - s_last_lat) > 1e-6 || fabs(lon - s_last_lon) > 1e-6 || s_zoom != s_last_zoom;
    if (!moved && !s_dirty) {
        return;
    }
    s_dirty = false;
    s_last_lat = lat;
    s_last_lon = lon;
    s_last_zoom = s_zoom;

    const int z = s_zoom, n = 1 << z;
    double cx = lon_to_px(lon, z), cy = lat_to_px(lat, z);
    double left = cx - MAP_W / 2.0, top = cy - MAP_H / 2.0;
    int tx0 = (int)floor(left / TILE), ty0 = (int)floor(top / TILE);
    int tx1 = (int)floor((left + MAP_W - 1) / TILE), ty1 = (int)floor((top + MAP_H - 1) / TILE);

    bool want_fetch = false;
    int img = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tick++;
    s_view_z = z;
    s_view_tx = cx / TILE;
    s_view_ty = cy / TILE;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1 && img < 9; tx++, img++) {
            lv_obj_t *o = s_imgs[img];
            int wx = ((tx % n) + n) % n;
            if (ty < 0 || ty >= n) {
                lv_obj_set_hidden(o, true);
                continue;
            }
            slot_t *s = find_slot(z, wx, ty);
            if (!s) {
                s = request_slot(z, wx, ty);
                want_fetch = true;
            }
            if (!s) {
                lv_obj_set_hidden(o, true);
                continue;
            }
            s->last_used = s_tick;
            if (s->state != SLOT_READY) {
                lv_obj_set_hidden(o, true);
                continue;
            }
            int idx = s - s_slots;
            if (s_img_slot[img] != idx || s_img_gen[img] != s->gen) {
                lv_image_cache_drop(&s->dsc);   /* same buffer, new pixels */
                lv_image_set_src(o, &s->dsc);
                s_img_slot[img] = idx;
                s_img_gen[img] = s->gen;
            }
            lv_obj_set_pos(o, (int)lround(tx * TILE - left), (int)lround(ty * TILE - top));
            lv_obj_set_hidden(o, false);
        }
    }
    xSemaphoreGive(s_lock);
    for (; img < 9; img++) {
        lv_obj_set_hidden(s_imgs[img], true);
    }
    if (want_fetch) {
        xTaskNotifyGive(s_task);
    }

    /* Van marker + heading */
    lv_obj_set_hidden(s_van, !have_pos);
    bool heading = have_pos && !isnan(course);
    if (heading) {
        double r = course * M_PI / 180.0;
        s_heading_pts[0] = (lv_point_precise_t){MAP_W / 2, MAP_H / 2};
        s_heading_pts[1] = (lv_point_precise_t){MAP_W / 2 + 30 * sin(r), MAP_H / 2 - 30 * cos(r)};
        lv_line_set_points(s_heading, s_heading_pts, 2);
    }
    lv_obj_set_hidden(s_heading, !heading);

    /* Home marker */
    bool home_vis = false;
    if (!isnan(home_lat) && !isnan(home_lon)) {
        double hx = lon_to_px(home_lon, z) - left, hy = lat_to_px(home_lat, z) - top;
        home_vis = hx >= 0 && hx < MAP_W && hy >= 0 && hy < MAP_H;
        if (home_vis) {
            lv_obj_set_pos(s_home, (int)hx - 9, (int)hy - 9);
        }
    }
    lv_obj_set_hidden(s_home, !home_vis);
}

void map_prefetch(double lat, double lon, double home_lat, double home_lon)
{
    if (!s_task) {
        return;
    }
    if (isnan(lat) || isnan(lon)) {
        lat = home_lat;   /* no fix: home, as map_update shows */
        lon = home_lon;
    }
    if (isnan(lat) || isnan(lon)) {
        return;
    }

    const int z = s_zoom, n = 1 << z;
    double cx = lon_to_px(lon, z), cy = lat_to_px(lat, z);
    double left = cx - MAP_W / 2.0, top = cy - MAP_H / 2.0;
    int tx0 = (int)floor(left / TILE), ty0 = (int)floor(top / TILE);
    int tx1 = (int)floor((left + MAP_W - 1) / TILE), ty1 = (int)floor((top + MAP_H - 1) / TILE);

    bool want_fetch = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tick++;
    s_view_z = z;
    s_view_tx = cx / TILE;
    s_view_ty = cy / TILE;
    for (int ty = ty0; ty <= ty1; ty++) {
        if (ty < 0 || ty >= n) {
            continue;
        }
        for (int tx = tx0; tx <= tx1; tx++) {
            int wx = ((tx % n) + n) % n;
            slot_t *s = find_slot(z, wx, ty);
            if (!s) {
                s = request_slot(z, wx, ty);
                want_fetch = true;
            }
            if (s) {
                s->last_used = s_tick;
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (want_fetch) {
        xTaskNotifyGive(s_task);
    }
}
