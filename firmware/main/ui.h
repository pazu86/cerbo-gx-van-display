#pragma once
/*
 * UI shell: Victron-style dark or day theme, header with page tabs,
 * swipeable pages, fire-alarm overlay. Pages live in page_*.c and are
 * refreshed once a second from the LVGL task with a fresh cerbo_data_t
 * snapshot. Changing theme rebuilds every page with the new palette.
 */

#include "cerbo.h"
#include "lvgl.h"

/* Theme-dependent colours (Victron GX dark / day mode) */
typedef struct {
    bool dark;
    uint32_t bg, card, card_hi, text, dim, line_off;
} ui_palette_t;

extern const ui_palette_t *ui_pal;

#define C_BG        (ui_pal->bg)
#define C_CARD      (ui_pal->card)
#define C_CARD_HI   (ui_pal->card_hi)    /* battery card */
#define C_TEXT      (ui_pal->text)
#define C_DIM       (ui_pal->dim)
#define C_LINE_OFF  (ui_pal->line_off)   /* idle connectors, chips */

/* The same in both themes */
#define C_BORDER    0x387DC5
#define C_BLUE      0x387DC5
#define C_GREEN     0x72B84C
#define C_ORANGE    0xF0962E
#define C_RED       0xF35C58

/* Recolour tag for units, used as "%s" UNIT("W") */
#define UNIT(u) " #969591 " u "#"

#define HEADER_H    48
#define PAGE_W      1024
#define PAGE_H      (600 - HEADER_H)

void ui_init(void);

/* Card with Victron-style title row (icon + title). Returns the card. */
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, const char *icon, const char *title);

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);

/* "12.3" or "--" for NAN. Each value in one printf needs its own buffer:
 * the returned pointer is only read when the printf runs. */
const char *ui_num(char *buf, size_t n, const char *fmt, float v);

/* lv_label_set_text(_fmt) that also re-tints UNIT() tags (written as
 * #969591) to the current theme's dim colour, so units stay readable on
 * light backgrounds. Use for any label text containing UNIT(). */
void ui_text(lv_obj_t *label, const char *text);
void ui_text_fmt(lv_obj_t *label, const char *fmt, ...) LV_FORMAT_ATTRIBUTE(2, 3);

/* "6d 22h" / "3h 05m" / "--" */
void ui_duration(char *buf, size_t n, float seconds);

/* Connector between two cards; set_active colours it like Victron's flow lines */
lv_obj_t *ui_connector(lv_obj_t *parent, int x1, int y1, int x2, int y2);
void ui_connector_set_active(lv_obj_t *line, bool active);

/* Pages */
void page_overview_create(lv_obj_t *parent);
void page_overview_update(const cerbo_data_t *d);
void page_solar_create(lv_obj_t *parent);
void page_solar_update(const cerbo_data_t *d);
void page_travel_create(lv_obj_t *parent);
void page_travel_update(const cerbo_data_t *d);
void page_settings_create(lv_obj_t *parent);
void page_settings_update(const cerbo_data_t *d);
