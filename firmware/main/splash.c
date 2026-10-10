#include "splash.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "rom/tjpgd.h"

static const char *TAG = "splash";

#define SPLASH_W        1024
#define SPLASH_H        600
#define SPLASH_FADE_MS  800
#define SPLASH_HOLD_MS  5000

/* main/splash/splash.jpg, made by tools/make_splash.py. A JPEG because embedded
 * data lives in PSRAM for good (SPIRAM_XIP_FROM_PSRAM) and a raw image is 1.2 MB;
 * it's decoded into s_px for the few seconds the splash is up. */
extern const uint8_t splash_jpg_start[] asm("_binary_splash_jpg_start");
extern const uint8_t splash_jpg_end[] asm("_binary_splash_jpg_end");

static lv_image_dsc_t s_dsc;
static uint16_t *s_px;
static size_t s_jpg_pos;
static volatile bool s_active = true;   /* until the splash is over (or failed): see splash.h */
static lv_obj_t *s_root;    /* black backdrop on the top layer */
static lv_obj_t *s_image;
static lv_timer_t *s_hold;
static bool s_fading_out;

static void set_image_opa(void *img, int32_t v)
{
    lv_obj_set_style_image_opa(img, (lv_opa_t)v, 0);
}

/* Fading out: image and backdrop together, revealing the Overview */
static void set_all_opa(void *unused, int32_t v)
{
    if (!s_root) return;
    lv_obj_set_style_image_opa(s_image, (lv_opa_t)v, 0);
    lv_obj_set_style_bg_opa(s_root, (lv_opa_t)v, 0);
}

static void finished(lv_anim_t *a)
{
    if (s_root) lv_obj_delete(s_root);
    s_root = s_image = NULL;
    s_fading_out = false;
    lv_image_cache_drop(&s_dsc);
    heap_caps_free(s_px);
    s_px = NULL;
    s_active = false;   /* the map may now take its memory */
}

static void fade_out(void)
{
    if (!s_root || s_fading_out) return;
    s_fading_out = true;
    if (s_hold) {
        lv_timer_delete(s_hold);
        s_hold = NULL;
    }
    lv_anim_delete(s_image, set_image_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_values(&a, lv_obj_get_style_image_opa(s_image, LV_PART_MAIN), 0);
    lv_anim_set_duration(&a, SPLASH_FADE_MS);
    lv_anim_set_exec_cb(&a, set_all_opa);
    lv_anim_set_completed_cb(&a, finished);
    lv_anim_start(&a);
}

static void hold_elapsed(lv_timer_t *t)
{
    s_hold = NULL;   /* single-shot: deletes itself */
    fade_out();
}

static void tapped(lv_event_t *e)
{
    fade_out();
}

/* ---- JPEG decode (ROM TJpgDec, RGB888 out) ---- */
static UINT jpg_in(JDEC *jd, BYTE *buf, UINT len)
{
    size_t left = (size_t)(splash_jpg_end - splash_jpg_start) - s_jpg_pos;
    if (len > left) len = left;
    if (buf) memcpy(buf, splash_jpg_start + s_jpg_pos, len);
    s_jpg_pos += len;
    return len;
}

/* RGB888 -> RGB565 with a 4x4 Bayer dither, so gradients don't band */
static const uint8_t s_bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static UINT jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    const uint8_t *p = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        uint16_t *row = s_px + (size_t)y * SPLASH_W;
        for (int x = r->left; x <= r->right; x++, p += 3) {
            unsigned b = s_bayer[y & 3][x & 3];
            unsigned r5 = (p[0] + b / 2) >> 3, g6 = (p[1] + b / 4) >> 2, b5 = (p[2] + b / 2) >> 3;
            if (r5 > 31) r5 = 31;
            if (g6 > 63) g6 = 63;
            if (b5 > 31) b5 = 31;
            row[x] = (uint16_t)(r5 << 11 | g6 << 5 | b5);
        }
    }
    return 1;
}

static bool decode(void)
{
    s_px = heap_caps_malloc(SPLASH_W * SPLASH_H * 2, MALLOC_CAP_SPIRAM);
    void *pool = heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    JDEC jd;
    JRESULT res = JDR_MEM1;
    int64_t t0 = esp_timer_get_time();
    if (s_px && pool) {
        s_jpg_pos = 0;
        res = jd_prepare(&jd, jpg_in, pool, 4096, NULL);
        if (res == JDR_OK && (jd.width != SPLASH_W || jd.height != SPLASH_H)) res = JDR_FMT2;
        if (res == JDR_OK) res = jd_decomp(&jd, jpg_out, 0);
    }
    heap_caps_free(pool);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "no splash: decode failed (%d), psram free %u kB", res,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        heap_caps_free(s_px);
        s_px = NULL;
        return false;
    }
    ESP_LOGI(TAG, "decoded in %lld ms", (esp_timer_get_time() - t0) / 1000);
    return true;
}

bool splash_active(void)
{
    return s_active;
}

void splash_start(void)
{
    s_active = true;
    if (!decode()) {
        s_active = false;
        return;
    }
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = SPLASH_W;
    s_dsc.header.h = SPLASH_H;
    s_dsc.header.stride = SPLASH_W * 2;
    s_dsc.data_size = SPLASH_W * SPLASH_H * 2;
    s_dsc.data = (const uint8_t *)s_px;

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);   /* a tap skips it, without pressing anything below */
    lv_obj_add_event_cb(s_root, tapped, LV_EVENT_CLICKED, NULL);
    /* Under the other top-layer overlays (fire alarm, update progress, sleep) */
    lv_obj_move_to_index(s_root, 0);

    s_image = lv_image_create(s_root);
    lv_image_set_src(s_image, &s_dsc);
    lv_obj_center(s_image);
    lv_obj_set_style_image_opa(s_image, LV_OPA_TRANSP, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_image);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, SPLASH_FADE_MS);
    lv_anim_set_exec_cb(&a, set_image_opa);
    lv_anim_start(&a);

    s_hold = lv_timer_create(hold_elapsed, SPLASH_FADE_MS + SPLASH_HOLD_MS, NULL);
    lv_timer_set_repeat_count(s_hold, 1);
}
