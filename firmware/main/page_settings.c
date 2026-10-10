/*
 * Settings: brightness, idle timeout, overnight schedule, device info.
 * Changes apply immediately and are saved; the web interface edits the same
 * settings, so the widgets re-sync whenever settings_version() moves.
 */
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fwupdate.h"
#include "inet.h"
#include "net.h"
#include "power.h"
#include "settings.h"
#include "ui.h"

static const uint16_t s_timeouts[] = {0, 10, 30, 60, 120, 300, 600, 900, 1800, 3600};   /* seconds */
static const uint8_t s_wake_secs[] = {15, 30, 60, 120};

static lv_obj_t *s_bright, *s_bright_lbl, *s_timeout, *s_theme;
static lv_obj_t *s_night_sw, *s_night_off, *s_night_on, *s_wake_bright, *s_wake_bright_lbl, *s_wake_secs_dd;
static lv_obj_t *s_info;
static uint32_t s_synced_version;

static int index_of_u16(const uint16_t *v, int n, int x)
{
    for (int i = 0; i < n; i++) {
        if (v[i] == x) return i;
    }
    return 0;
}

static int index_of_u8(const uint8_t *v, int n, int x)
{
    for (int i = 0; i < n; i++) {
        if (v[i] == x) return i;
    }
    return 1;
}

/* Read every widget back into a settings struct and save it */
static void save_from_widgets(void)
{
    settings_t s;
    settings_get(&s);
    s.brightness = lv_slider_get_value(s_bright);
    s.idle_timeout_s = s_timeouts[lv_dropdown_get_selected(s_timeout)];
    s.night_enabled = lv_obj_has_state(s_night_sw, LV_STATE_CHECKED);
    s.night_off_min = lv_dropdown_get_selected(s_night_off) * 30;
    s.night_on_min = lv_dropdown_get_selected(s_night_on) * 30;
    s.night_wake_brightness = lv_slider_get_value(s_wake_bright);
    s.night_wake_secs = s_wake_secs[lv_dropdown_get_selected(s_wake_secs_dd)];
    s.theme = lv_dropdown_get_selected(s_theme);   /* option order = theme_t */
    settings_set(&s);
    s_synced_version = settings_version();
}

static void changed_cb(lv_event_t *e)
{
    save_from_widgets();
}

static void bright_drag_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bright);
    ui_text_fmt(s_bright_lbl, "%d %%", v);
    power_preview_brightness(v);
}

static void bright_release_cb(lv_event_t *e)
{
    power_preview_brightness(-1);
    save_from_widgets();
}

static void wake_bright_drag_cb(lv_event_t *e)
{
    ui_text_fmt(s_wake_bright_lbl, "%d %%", (int)lv_slider_get_value(s_wake_bright));
}

static lv_obj_t *s_wifi_dd;

static void wifi_changed_cb(lv_event_t *e)
{
    net_select_async((int)lv_dropdown_get_selected(s_wifi_dd) - 1);   /* option 0 = Auto */
}

static void restart_cb(lv_event_t *e)
{
    if (!fw_busy()) {   /* never interrupt a firmware update or its confirmation */
        esp_restart();
    }
}

static lv_obj_t *row_label(lv_obj_t *card, const char *text, int x, int y)
{
    lv_obj_t *l = ui_label(card, &lv_font_montserrat_20, C_TEXT, text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

static lv_obj_t *dropdown(lv_obj_t *card, const char *opts, int x, int y, int w)
{
    lv_obj_t *d = lv_dropdown_create(card);
    lv_dropdown_set_options(d, opts);
    lv_obj_set_width(d, w);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_add_event_cb(d, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return d;
}

static lv_obj_t *slider(lv_obj_t *card, int min, int max, int x, int y, int w)
{
    lv_obj_t *s = lv_slider_create(card);
    lv_slider_set_range(s, min, max);
    lv_obj_set_width(s, w);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, x, y);
    return s;
}

/* Push the stored settings into the widgets */
static void sync_widgets(void)
{
    settings_t s;
    settings_get(&s);
    lv_slider_set_value(s_bright, s.brightness, LV_ANIM_OFF);
    ui_text_fmt(s_bright_lbl, "%d %%", s.brightness);
    lv_dropdown_set_selected(s_timeout, index_of_u16(s_timeouts, sizeof(s_timeouts) / sizeof(s_timeouts[0]),
                                                     s.idle_timeout_s));
    lv_obj_set_state(s_night_sw, LV_STATE_CHECKED, s.night_enabled);
    lv_dropdown_set_selected(s_night_off, s.night_off_min / 30);
    lv_dropdown_set_selected(s_night_on, s.night_on_min / 30);
    lv_slider_set_value(s_wake_bright, s.night_wake_brightness, LV_ANIM_OFF);
    ui_text_fmt(s_wake_bright_lbl, "%d %%", s.night_wake_brightness);
    lv_dropdown_set_selected(s_wake_secs_dd, index_of_u8(s_wake_secs, sizeof(s_wake_secs), s.night_wake_secs));
    lv_dropdown_set_selected(s_theme, s.theme);
    s_synced_version = settings_version();
}

void page_settings_create(lv_obj_t *p)
{
    /* Half-hour steps for the schedule */
    static char times[48 * 6 + 1];
    if (!times[0]) {
        char *o = times;
        for (int i = 0; i < 48; i++) {
            o += sprintf(o, "%s%02d:%02d", i ? "\n" : "", i / 2, (i % 2) * 30);
        }
    }

    lv_obj_t *disp = ui_card(p, 16, 8, 488, 130, LV_SYMBOL_EYE_OPEN, "Display");
    row_label(disp, "Brightness", 0, 56);
    s_bright = slider(disp, 5, 100, 140, 62, 230);
    lv_obj_add_event_cb(s_bright, bright_drag_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bright, bright_release_cb, LV_EVENT_RELEASED, NULL);
    s_bright_lbl = ui_label(disp, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_bright_lbl, LV_ALIGN_TOP_RIGHT, 0, 56);
    s_theme = dropdown(disp, "Auto\nDark\nDay", 0, -8, 140);   /* order = theme_t; Auto: dark overnight */
    lv_obj_align(s_theme, LV_ALIGN_TOP_RIGHT, 0, -8);

    lv_obj_t *sleep = ui_card(p, 520, 8, 488, 130, LV_SYMBOL_POWER, "Sleep");
    row_label(sleep, "Screen off after", 0, 56);
    s_timeout = dropdown(sleep, "Never\n10 s\n30 s\n1 min\n2 min\n5 min\n10 min\n15 min\n30 min\n60 min", 250, 46, 200);

    lv_obj_t *night = ui_card(p, 16, 150, 992, 170, LV_SYMBOL_BELL, "Overnight");
    s_night_sw = lv_switch_create(night);
    lv_obj_align(s_night_sw, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_add_event_cb(s_night_sw, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    row_label(night, "Screen off at", 0, 52);
    s_night_off = dropdown(night, times, 160, 42, 130);
    row_label(night, "back on at", 320, 52);
    s_night_on = dropdown(night, times, 440, 42, 130);
    row_label(night, "A touch overnight wakes it at", 0, 108);
    s_wake_bright = slider(night, 5, 60, 330, 114, 220);
    lv_obj_add_event_cb(s_wake_bright, wake_bright_drag_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_wake_bright, changed_cb, LV_EVENT_RELEASED, NULL);
    s_wake_bright_lbl = ui_label(night, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_wake_bright_lbl, LV_ALIGN_TOP_LEFT, 570, 108);
    row_label(night, "for", 660, 108);
    s_wake_secs_dd = dropdown(night, "15 s\n30 s\n60 s\n2 min", 710, 98, 130);

    lv_obj_t *dev = ui_card(p, 16, 332, 992, 212, LV_SYMBOL_SETTINGS, "Device");
    s_info = ui_label(dev, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_style_text_line_space(s_info, 8, 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_t *btn = lv_button_create(dev);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *bl = ui_label(btn, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_REFRESH "  Restart");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, restart_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *hint = ui_label(dev, &lv_font_montserrat_16, C_DIM, "hold to restart");
    lv_obj_align_to(hint, btn, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    /* WiFi network choice (also from the header's WiFi status) */
    char opts[160] = "Auto";
    for (int i = 0; i < net_count(); i++) {
        strlcat(opts, "\n", sizeof(opts));
        strlcat(opts, net_ssid(i), sizeof(opts));
    }
    lv_obj_t *wl = ui_label(dev, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_WIFI "  WiFi");
    lv_obj_align(wl, LV_ALIGN_TOP_RIGHT, -210, 2);
    s_wifi_dd = lv_dropdown_create(dev);
    lv_dropdown_set_options(s_wifi_dd, opts);
    lv_obj_set_width(s_wifi_dd, 190);
    lv_obj_align(s_wifi_dd, LV_ALIGN_TOP_RIGHT, 0, -8);
    lv_dropdown_set_selected(s_wifi_dd, net_get_selected() + 1);
    lv_obj_add_event_cb(s_wifi_dd, wifi_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    sync_widgets();
}

void page_settings_update(const cerbo_data_t *d)
{
    /* Changed from the web page? Re-sync, unless a slider is being dragged */
    if (settings_version() != s_synced_version && !lv_obj_has_state(s_bright, LV_STATE_PRESSED) &&
        !lv_obj_has_state(s_wake_bright, LV_STATE_PRESSED)) {
        sync_widgets();
    }
    /* WiFi choice may have been changed from the header picker */
    if (!lv_dropdown_is_open(s_wifi_dd) && (int)lv_dropdown_get_selected(s_wifi_dd) != net_get_selected() + 1) {
        lv_dropdown_set_selected(s_wifi_dd, net_get_selected() + 1);
    }

    net_status_t ns;
    net_get_status(&ns);
    inet_status_t is;
    inet_get(&is);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    ui_text_fmt(s_info,
                          "Web settings   " "#387DC5 http://" CONFIG_DISPLAY_HOSTNAME ".local#" "   (%s)\n"
                          "WiFi  %s  %d dBm      Internet  %s %s\n"
                          "Cerbo  %s, %lu messages\n"
                          "Firmware  %s      Up  %lud %02lu:%02lu      Memory  %u kB internal, %u kB PSRAM free",
                          ns.ip[0] ? ns.ip : "no IP", ns.ssid[0] ? ns.ssid : "--", ns.rssi,
                          inet_kind_name(is.kind), is.isp, cerbo_state_name(d->state), (unsigned long)d->rx_count,
                          esp_app_get_description()->version, up / 86400, (up / 3600) % 24, (up / 60) % 60,
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
