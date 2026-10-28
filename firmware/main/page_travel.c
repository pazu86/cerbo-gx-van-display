/*
 * Travel: live map plus the Node-RED travel logger's state (VanDisplay/travel).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "map.h"
#include "ui.h"

#define RECENT_LINES 4

static lv_obj_t *s_status, *s_speed, *s_odo;
static lv_obj_t *s_trip_title, *s_trip_body;
static lv_obj_t *s_service_body;
static lv_obj_t *s_recent;

void page_travel_create(lv_obj_t *p)
{
    map_create(p, 16, 20);

    const int x = 544, w = 464;
    lv_obj_t *now = ui_card(p, x, 8, w, 132, LV_SYMBOL_GPS, "Now");
    s_status = ui_label(now, &lv_font_montserrat_20, C_TEXT, "--");
    lv_obj_set_style_bg_opa(s_status, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_status, lv_color_hex(C_LINE_OFF), 0);
    lv_obj_set_style_radius(s_status, 6, 0);
    lv_obj_set_style_pad_hor(s_status, 10, 0);
    lv_obj_set_style_pad_ver(s_status, 2, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, 0, -2);
    s_speed = ui_label(now, &lv_font_montserrat_48, C_TEXT, "--");
    lv_obj_align(s_speed, LV_ALIGN_BOTTOM_LEFT, 0, 4);
    s_odo = ui_label(now, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_set_style_text_align(s_odo, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_odo, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *trip = ui_card(p, x, 152, w, 136, NULL, "");
    lv_obj_clean(trip);
    s_trip_title = ui_label(trip, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_SHUFFLE "  Last trip");
    lv_obj_align(s_trip_title, LV_ALIGN_TOP_LEFT, 0, 0);
    s_trip_body = ui_label(trip, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_width(s_trip_body, w - 28);
    lv_label_set_long_mode(s_trip_body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_line_space(s_trip_body, 6, 0);
    lv_obj_align(s_trip_body, LV_ALIGN_TOP_LEFT, 0, 32);

    lv_obj_t *svc = ui_card(p, x, 300, w, 92, LV_SYMBOL_SETTINGS, "Service");
    s_service_body = ui_label(svc, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_align(s_service_body, LV_ALIGN_TOP_LEFT, 0, 32);

    lv_obj_t *rec = ui_card(p, x, 404, w, 140, LV_SYMBOL_LIST, "Recent trips");
    s_recent = ui_label(rec, &lv_font_montserrat_16, C_TEXT, "");
    lv_obj_set_width(s_recent, w - 28);
    lv_obj_set_style_text_line_space(s_recent, 3, 0);
    lv_obj_align(s_recent, LV_ALIGN_TOP_LEFT, 0, 30);
}

/* "Trip to Christmas Tree Farm Beerburrum" -> "Christmas Tree Farm Beerburrum" */
static const char *place(const char *name)
{
    return strncmp(name, "Trip to ", 8) == 0 ? name + 8 : name;
}

/* "2026-07-26" -> "26 Jul" */
static void short_date(char *buf, size_t n, const char *iso)
{
    static const char *const mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, m, d;
    if (sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12) {
        snprintf(buf, n, "%d %s %02d", d, mon[m - 1], y % 100);
    } else {
        snprintf(buf, n, "--");
    }
}

void page_travel_prefetch(const cerbo_data_t *d)
{
    const cerbo_travel_t *t = &d->travel;
    bool fix = d->gps_fix == 1 && !isnan(d->lat);
    map_prefetch(fix ? d->lat : NAN, fix ? d->lon : NAN, t->valid ? t->home_lat : NAN, t->valid ? t->home_lon : NAN);
}

void page_travel_update(const cerbo_data_t *d)
{
    const cerbo_travel_t *t = &d->travel;
    char a[16], b[16], c[16], e[16];

    bool fix = d->gps_fix == 1 && !isnan(d->lat);
    map_update(fix ? d->lat : NAN, fix ? d->lon : NAN, d->speed_kmh > 3 ? d->course_deg : NAN,
               t->valid ? t->home_lat : NAN, t->valid ? t->home_lon : NAN);

    /* Status chip */
    const char *status;
    uint32_t col;
    if (fix && d->speed_kmh > 5) {
        status = "Driving", col = C_GREEN;
    } else if (t->valid && t->trip_active) {
        status = "On trip", col = C_BLUE;
    } else if (t->valid && t->at_home) {
        status = "At home", col = C_LINE_OFF;
    } else if (t->valid) {
        status = "Away", col = C_ORANGE;
    } else {
        status = "No travel data", col = C_LINE_OFF;
    }
    if (!fix) {
        status = d->gps_fix == 0 ? "No GPS fix" : status;
    }
    ui_text(s_status, status);
    lv_obj_set_style_bg_color(s_status, lv_color_hex(col), 0);

    ui_text_fmt(s_speed, "%s" UNIT("km/h"), ui_num(a, sizeof(a), "%.0f", fix ? d->speed_kmh : NAN));
    ui_text_fmt(s_odo, "Odometer\n%s km", ui_num(a, sizeof(a), "%.0f", t->valid ? t->odo_km : NAN));

    if (!t->valid) {
        ui_text(s_trip_body, "Waiting for the Node-RED travel feed");
        ui_text(s_service_body, "--");
        ui_text(s_recent, "");
        return;
    }

    /* Current trip, or the last one when parked */
    if (t->trip_active) {
        ui_text(s_trip_title, LV_SYMBOL_SHUFFLE "  Current trip");
        ui_text_fmt(s_trip_body, "%s" UNIT("km") "     %s" UNIT("L fuel") "\nMax %s" UNIT("km/h") "     %s" UNIT("km from home"),
                              ui_num(a, sizeof(a), "%.1f", t->trip_km), ui_num(b, sizeof(b), "%.1f", t->trip_fuel_l),
                              ui_num(c, sizeof(c), "%.0f", t->trip_max_kmh),
                              ui_num(e, sizeof(e), "%.0f", t->trip_max_from_home_km));
    } else if (t->last.name[0]) {
        char date[16];
        short_date(date, sizeof(date), t->last.end);
        ui_text(s_trip_title, LV_SYMBOL_SHUFFLE "  Last trip");
        ui_text_fmt(s_trip_body, "%s\n%s" UNIT("km") "   %d" UNIT("nights") "   %s" UNIT("L") "   %s",
                              place(t->last.name), ui_num(a, sizeof(a), "%.0f", t->last.km), t->last.nights,
                              ui_num(b, sizeof(b), "%.0f", t->last.fuel_l), date);
    } else {
        ui_text(s_trip_body, "No trips logged yet");
    }

    /* Service: distance and date, whichever comes first */
    char when[48];
    if (t->service_days_left == INT32_MIN) {
        snprintf(when, sizeof(when), "date unknown");
    } else if (t->service_days_left < 0) {
        snprintf(when, sizeof(when), "#F35C58 overdue %d days#", -t->service_days_left);
    } else {
        snprintf(when, sizeof(when), "in %d days", t->service_days_left);
    }
    ui_text_fmt(s_service_body, "%s" UNIT("km left") "   %s",
                          ui_num(a, sizeof(a), "%.0f", t->service_km_left), when);
    lv_obj_set_style_text_color(s_service_body, lv_color_hex(t->service_km_left <= 500 ? C_ORANGE : C_TEXT), 0);

    /* Recent trips */
    char list[512] = "";
    int n = 0;
    for (int i = 0; i < t->recent_count && i < RECENT_LINES; i++) {
        const cerbo_trip_t *r = &t->recent[i];
        char date[16];
        short_date(date, sizeof(date), r->end);
        n += snprintf(list + n, sizeof(list) - n, "%-.30s #969591 %.0f km, %d n, %s#\n", place(r->name), r->km, r->nights, date);
    }
    ui_text(s_recent, list);
}
