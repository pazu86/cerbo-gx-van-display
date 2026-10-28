#include "sdkconfig.h"
#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "inet.h"
#include "power.h"
#include "splash.h"
#include "settings.h"
#include "esp_log.h"
#include "fwupdate.h"
#include "net.h"

static const char *TAG = "ui";

static const char *const s_page_names[] = {"Overview", "Solar", "Travel", LV_SYMBOL_SETTINGS};
#define PAGE_COUNT 4

static lv_obj_t *s_tiles;
static lv_obj_t *s_tabs[PAGE_COUNT];
static lv_obj_t *s_clock;
static lv_obj_t *s_status;
static lv_obj_t *s_fire;
static lv_obj_t *s_fire_lbl;
static lv_obj_t *s_fw, *s_fw_title, *s_fw_bar, *s_fw_text;
static int s_page;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, const char *icon, const char *title)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_clickable(c, false);

    char t[64];
    snprintf(t, sizeof(t), "%s  %s", icon ? icon : "", title);
    lv_obj_t *l = ui_label(c, &lv_font_montserrat_20, C_TEXT, t);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    return c;
}

void ui_text(lv_obj_t *label, const char *text)
{
    static char buf[1024];   /* LVGL task only */
    char hex[8];
    snprintf(hex, sizeof(hex), "%06X", (unsigned)C_DIM);
    strlcpy(buf, text, sizeof(buf));
    for (char *p = buf; (p = strstr(p, "#969591 ")) != NULL; p += 8) {
        memcpy(p + 1, hex, 6);
    }
    lv_label_set_text(label, buf);
}

void ui_text_fmt(lv_obj_t *label, const char *fmt, ...)
{
    static char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_text(label, buf);
}

const char *ui_num(char *buf, size_t n, const char *fmt, float v)
{
    if (isnan(v)) {
        return "--";
    }
    snprintf(buf, n, fmt, v);
    return buf;
}

void ui_duration(char *buf, size_t n, float s)
{
    if (isnan(s) || s <= 0) {
        snprintf(buf, n, "--");
    } else if (s >= 24 * 3600) {
        int h = (int)(s / 3600);
        snprintf(buf, n, "%dd %dh", h / 24, h % 24);
    } else {
        int m = (int)(s / 60);
        snprintf(buf, n, "%dh %02dm", m / 60, m % 60);
    }
}

/* Connectors: elbow line + a dot at each end, like the Victron overview */
typedef struct {
    lv_point_precise_t pts[4];
    lv_obj_t *line;
    lv_obj_t *dots[2];
} connector_t;

static connector_t s_conn[8];
static int s_conn_n;

static lv_obj_t *make_dot(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 10, 10);
    lv_obj_set_pos(d, x - 5, y - 5);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(C_LINE_OFF), 0);
    return d;
}

lv_obj_t *ui_connector(lv_obj_t *parent, int x1, int y1, int x2, int y2)
{
    if (s_conn_n >= (int)(sizeof(s_conn) / sizeof(s_conn[0]))) {
        return NULL;
    }
    connector_t *c = &s_conn[s_conn_n++];
    if (y1 == y2 || x1 == x2) {
        c->pts[0] = (lv_point_precise_t){x1, y1};
        c->pts[1] = (lv_point_precise_t){x2, y2};
        c->pts[2] = c->pts[3] = c->pts[1];
    } else {
        /* horizontal - vertical - horizontal */
        int mx = (x1 + x2) / 2;
        c->pts[0] = (lv_point_precise_t){x1, y1};
        c->pts[1] = (lv_point_precise_t){mx, y1};
        c->pts[2] = (lv_point_precise_t){mx, y2};
        c->pts[3] = (lv_point_precise_t){x2, y2};
    }
    c->line = lv_line_create(parent);
    lv_line_set_points(c->line, c->pts, 4);
    lv_obj_set_style_line_width(c->line, 3, 0);
    lv_obj_set_style_line_rounded(c->line, true, 0);
    lv_obj_set_style_line_color(c->line, lv_color_hex(C_LINE_OFF), 0);
    c->dots[0] = make_dot(parent, x1, y1);
    c->dots[1] = make_dot(parent, x2, y2);
    lv_obj_set_user_data(c->line, c);
    return c->line;
}

void ui_connector_set_active(lv_obj_t *line, bool active)
{
    if (!line) {
        return;
    }
    connector_t *c = lv_obj_get_user_data(line);
    lv_color_t col = lv_color_hex(active ? C_BLUE : C_LINE_OFF);
    lv_obj_set_style_line_color(line, col, 0);
    lv_obj_set_style_bg_color(c->dots[0], col, 0);
    lv_obj_set_style_bg_color(c->dots[1], col, 0);
}

/* ------------------------------------------------------------------ */
/* Header + navigation                                                */
/* ------------------------------------------------------------------ */

static void show_page(int idx)
{
    s_page = idx;
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_set_style_text_color(s_tabs[i], lv_color_hex(i == idx ? C_TEXT : C_DIM), 0);
        lv_obj_set_style_border_width(s_tabs[i], i == idx ? 3 : 0, 0);
    }
}

static void tab_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_tileview_set_tile_by_index(s_tiles, idx, 0, LV_ANIM_ON);
    show_page(idx);
}

static void tile_changed(lv_event_t *e)
{
    lv_obj_t *tile = lv_tileview_get_tile_active(s_tiles);
    show_page(lv_obj_get_x(tile) / PAGE_W);
}

/* ------------------------------------------------------------------ */
/* WiFi picker (tap the WiFi status in the header)                    */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_picker;

static void picker_close(void)
{
    if (s_picker) {
        lv_obj_delete_async(s_picker);   /* we're inside one of its events */
        s_picker = NULL;
    }
}

static void picker_backdrop_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {   /* tap outside the card */
        picker_close();
    }
}

static void picker_choose_cb(lv_event_t *e)
{
    net_select_async((int)(intptr_t)lv_event_get_user_data(e));
    picker_close();
}

static void picker_add(lv_obj_t *list, const char *text, int idx, bool chosen)
{
    lv_obj_t *b = lv_button_create(list);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_bg_color(b, lv_color_hex(chosen ? C_BLUE : C_LINE_OFF), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_ver(b, 14, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(chosen ? 0xFFFFFF : C_TEXT), 0);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, picker_choose_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
}

static void wifi_picker_open_cb(lv_event_t *e)
{
    if (s_picker) {
        return;
    }
    net_status_t ns;
    net_get_status(&ns);
    int chosen = net_get_selected();

    s_picker = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_picker);
    lv_obj_set_size(s_picker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_picker, lv_color_black(), 0);
    lv_obj_set_clickable(s_picker, true);
    lv_obj_add_event_cb(s_picker, picker_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = ui_card(s_picker, 0, 0, 420, LV_SIZE_CONTENT, LV_SYMBOL_WIFI, "WiFi network");
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -16, HEADER_H + 4);
    lv_obj_set_clickable(card, true);   /* taps on the card don't close it */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);   /* the card's title label is the first row */

    char text[64];
    picker_add(card, "Auto - follow the Cerbo", -1, chosen < 0);
    for (int i = 0; i < net_count(); i++) {
        bool connected = ns.state == NET_STATE_CONNECTED && strcmp(ns.ssid, net_ssid(i)) == 0;
        snprintf(text, sizeof(text), "%s%s", net_ssid(i), connected ? "   " LV_SYMBOL_OK " connected" : "");
        picker_add(card, text, i, chosen == i);
    }
}

static void build_header(lv_obj_t *scr)
{
    lv_obj_t *h = lv_obj_create(scr);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, PAGE_W, HEADER_H);
    lv_obj_set_scrollable(h, false);

    lv_obj_t *title = ui_label(h, &lv_font_montserrat_24, C_TEXT, CONFIG_DISPLAY_TITLE);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);

    /* Tabs in a flex row so the gaps are equal whatever the label widths */
    lv_obj_t *tabs = lv_obj_create(h);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, LV_SIZE_CONTENT, HEADER_H);
    lv_obj_set_pos(tabs, 140, 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, 12, 0);
    lv_obj_set_scrollable(tabs, false);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *t = ui_label(tabs, &lv_font_montserrat_20, C_DIM, s_page_names[i]);
        lv_obj_set_style_pad_hor(t, 16, 0);
        lv_obj_set_style_pad_ver(t, 10, 0);
        lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(C_BLUE), 0);
        lv_obj_set_clickable(t, true);
        lv_obj_add_event_cb(t, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_tabs[i] = t;
    }

    /* Internet source + WiFi: tap to choose the WiFi network */
    s_status = ui_label(h, &lv_font_montserrat_20, C_DIM, LV_SYMBOL_WIFI);
    lv_obj_set_style_pad_all(s_status, 10, 0);
    lv_obj_align(s_status, LV_ALIGN_RIGHT_MID, -90, 0);
    lv_obj_set_clickable(s_status, true);
    lv_obj_add_event_cb(s_status, wifi_picker_open_cb, LV_EVENT_CLICKED, NULL);

    s_clock = ui_label(h, &lv_font_montserrat_24, C_TEXT, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_RIGHT_MID, -20, 0);
}

static void build_fire_overlay(void)
{
    s_fire = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_fire);
    lv_obj_set_size(s_fire, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_fire, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_fire, lv_color_hex(0xB71C1C), 0);
    s_fire_lbl = ui_label(s_fire, &lv_font_montserrat_48, C_TEXT, "");
    lv_obj_set_style_text_align(s_fire_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_fire_lbl);
    lv_obj_set_hidden(s_fire, true);
}

/* Firmware update overlay: progress during upload, a countdown before the
 * reboot, then "finishing" until the new firmware is confirmed. */
static void build_update_overlay(void)
{
    s_fw = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_fw);
    lv_obj_set_size(s_fw, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_fw, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_fw, lv_color_black(), 0);
    lv_obj_set_clickable(s_fw, true);   /* swallow touches while updating */

    lv_obj_t *box = lv_obj_create(s_fw);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 560, 240);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_pad_all(box, 28, 0);

    s_fw_title = lv_label_create(box);
    lv_obj_set_style_text_font(s_fw_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_fw_title, lv_color_hex(0xFAF9F5), 0);
    lv_obj_align(s_fw_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_fw_bar = lv_bar_create(box);
    lv_obj_set_size(s_fw_bar, 500, 20);
    lv_obj_align(s_fw_bar, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_bar_set_range(s_fw_bar, 0, 100);
    lv_obj_set_style_bg_color(s_fw_bar, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_fw_bar, lv_color_hex(C_BLUE), LV_PART_INDICATOR);

    s_fw_text = lv_label_create(box);
    lv_obj_set_width(s_fw_text, 500);
    lv_obj_set_style_text_font(s_fw_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_fw_text, lv_color_hex(0x969591), 0);
    lv_obj_align(s_fw_text, LV_ALIGN_TOP_LEFT, 0, 110);

    lv_obj_set_hidden(s_fw, true);
}

static void update_overlay_cb(lv_timer_t *t)
{
    fw_status_t st;
    fw_get(&st);
    bool show = st.phase != FW_IDLE;
    lv_obj_set_hidden(s_fw, !show);
    if (!show) {
        return;
    }
    const char *title = "", *text = "";
    char buf[96];
    int pct = st.pct;
    switch (st.phase) {
    case FW_RECEIVING:
        title = LV_SYMBOL_DOWNLOAD "  Updating firmware";
        snprintf(buf, sizeof(buf), "Receiving... %d %%\nDon't power off the display.", st.pct);
        text = buf;
        break;
    case FW_INSTALLING:
        title = LV_SYMBOL_DOWNLOAD "  Updating firmware";
        text = "Checking the new firmware...";
        pct = 100;
        break;
    case FW_RESTARTING:
        title = LV_SYMBOL_REFRESH "  Restarting";
        snprintf(buf, sizeof(buf), "Restarting into the new firmware in %d...", st.secs > 0 ? st.secs : 0);
        text = buf;
        pct = 100;
        break;
    case FW_VERIFYING:
        title = LV_SYMBOL_OK "  Finishing update";
        snprintf(buf, sizeof(buf), "Confirming version %s.\nDon't power off or restart the display.", st.version);
        text = buf;
        break;
    case FW_FAILED:
        title = LV_SYMBOL_WARNING "  Update failed";
        snprintf(buf, sizeof(buf), "%s\nThe current firmware is still running.", st.message);
        text = buf;
        pct = 0;
        break;
    default:
        break;
    }
    lv_label_set_text(s_fw_title, title);
    lv_label_set_text(s_fw_text, text);
    lv_bar_set_value(s_fw_bar, pct, LV_ANIM_OFF);
}

static void update_header(const cerbo_data_t *d)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 120) {
        lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }

    /* Internet source, then WiFi (green = Cerbo live, orange = no Cerbo, red = no WiFi) */
    net_status_t ns;
    net_get_status(&ns);
    inet_status_t is;
    inet_get(&is);
    uint32_t col = C_RED;
    if (ns.state == NET_STATE_CONNECTED) {
        col = d->state == CERBO_LIVE ? C_GREEN : C_ORANGE;
    }
    const char *icon = is.kind == INET_STARLINK ? LV_SYMBOL_UPLOAD : is.kind == INET_CELLULAR ? LV_SYMBOL_CALL
                     : is.kind == INET_HOME ? LV_SYMBOL_HOME : is.kind == INET_OFFLINE ? LV_SYMBOL_WARNING
                     : LV_SYMBOL_GPS;
    char inet[48] = "";
    if (is.kind == INET_CELLULAR && is.isp[0]) {
        char carrier[16];
        sscanf(is.isp, "%15s", carrier);   /* "Telstra Corporation" -> "Telstra" */
        snprintf(inet, sizeof(inet), "#%06X %s %s %s#   ", (unsigned)C_TEXT, icon, inet_kind_name(is.kind), carrier);
    } else if (is.kind != INET_UNKNOWN) {
        snprintf(inet, sizeof(inet), "#%06X %s %s#   ", (unsigned)(is.kind == INET_OFFLINE ? C_ORANGE : C_TEXT),
                 icon, inet_kind_name(is.kind));
    }
    lv_label_set_text_fmt(s_status, "%s" LV_SYMBOL_WIFI " %s", inet, ns.state == NET_STATE_CONNECTED ? ns.ssid : "");
    lv_obj_set_style_text_color(s_status, lv_color_hex(col), 0);
}

static void update_fire(const cerbo_data_t *d)
{
    const cerbo_input_t *alarm = NULL;
    for (int i = 0; i < CERBO_MAX_INPUTS; i++) {
        if (d->inputs[i].dev.instance >= 0 && d->inputs[i].alarm == 2) {
            alarm = &d->inputs[i];
            break;
        }
    }
    if (alarm) {
        lv_label_set_text_fmt(s_fire_lbl, LV_SYMBOL_WARNING "\n%s\nALARM",
                              alarm->dev.name[0] ? alarm->dev.name : "Alarm");
        if (lv_obj_is_hidden(s_fire)) {
            ESP_LOGW(TAG, "alarm: %s", alarm->dev.name);
        }
    }
    lv_obj_set_hidden(s_fire, alarm == NULL);
    /* Fire alarm, or a firmware update in progress, keeps the screen on */
    power_set_alarm(alarm != NULL || fw_busy());
}

/* ------------------------------------------------------------------ */
/* Themes                                                             */
/* ------------------------------------------------------------------ */

static const ui_palette_t s_dark = {
    .dark = true, .bg = 0x0B0B0C, .card = 0x1C1C1E, .card_hi = 0x26303B,
    .text = 0xFAF9F5, .dim = 0x969591, .line_off = 0x3A3A3C,
};

/* Day mode, matching the Cerbo GX GUI's light theme */
static const ui_palette_t s_day = {
    .dark = false, .bg = 0xE6E5E1, .card = 0xFFFFFF, .card_hi = 0xA9C9EC,
    .text = 0x272622, .dim = 0x6B6A66, .line_off = 0xC4C3BF,
};

const ui_palette_t *ui_pal = &s_dark;

static bool want_dark(void)
{
    settings_t s;
    settings_get(&s);
    if (s.theme == THEME_DARK) return true;
    if (s.theme == THEME_DAY) return false;

    /* Auto: dark through the overnight window, or 18:00-07:00 without one */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return true;   /* clock not set yet */
    int m = tm.tm_hour * 60 + tm.tm_min;
    int off = s.night_enabled ? s.night_off_min : 18 * 60;
    int on = s.night_enabled ? s.night_on_min : 7 * 60;
    if (off == on) return true;
    return off < on ? (m >= off && m < on) : (m >= off || m < on);
}

/* (Re)create the header and every page with the current palette */
static void build_screen(void)
{
    lv_display_t *disp = lv_display_get_default();
    /* Stock widgets (sliders, dropdowns, switches, charts) follow the theme too */
    lv_display_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(C_BLUE), lv_color_hex(C_RED), ui_pal->dark,
                                                     &lv_font_montserrat_20));

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    s_conn_n = 0;
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);
    lv_obj_set_scrollable(scr, false);

    build_header(scr);

    s_tiles = lv_tileview_create(scr);
    lv_obj_set_pos(s_tiles, 0, HEADER_H);
    lv_obj_set_size(s_tiles, PAGE_W, PAGE_H);
    lv_obj_set_style_bg_opa(s_tiles, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tiles, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tiles, tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    page_overview_create(lv_tileview_add_tile(s_tiles, 0, 0, LV_DIR_RIGHT));
    page_solar_create(lv_tileview_add_tile(s_tiles, 1, 0, LV_DIR_HOR));
    page_travel_create(lv_tileview_add_tile(s_tiles, 2, 0, LV_DIR_HOR));
    page_settings_create(lv_tileview_add_tile(s_tiles, 3, 0, LV_DIR_LEFT));

    lv_tileview_set_tile_by_index(s_tiles, s_page, 0, LV_ANIM_OFF);
    show_page(s_page);
}

static void refresh_cb(lv_timer_t *t)
{
    static cerbo_data_t d;  /* ~3 kB: keep off the LVGL task stack */

    /* Theme change (setting or Auto crossing day/night): rebuild. Done here,
     * never from a widget's own event, since it deletes every widget. */
    bool dark = want_dark();
    if (dark != ui_pal->dark) {
        ui_pal = dark ? &s_dark : &s_day;
        ESP_LOGI(TAG, "theme -> %s", dark ? "dark" : "day");
        build_screen();
    }

    cerbo_get(&d);
    update_header(&d);
    update_fire(&d);
    switch (s_page) {
    case 0: page_overview_update(&d); break;
    case 1: page_solar_update(&d); break;
    case 2: page_travel_update(&d); break;
    case 3: page_settings_update(&d); break;
    }
    if (s_page != 2) {
        page_travel_prefetch(&d);   /* map tiles load in the background */
    }
}

void ui_init(void)
{
    ui_pal = want_dark() ? &s_dark : &s_day;
    build_screen();

    /* On the top layer, so they survive theme rebuilds */
    build_fire_overlay();
    build_update_overlay();
    power_init();   /* after the overlays so its sleep layer sits on top */
    lv_timer_create(update_overlay_cb, 250, NULL);

    lv_timer_create(refresh_cb, 1000, NULL);
    refresh_cb(NULL);

    splash_start();   /* boot splash over the Overview, under the overlays */
}
