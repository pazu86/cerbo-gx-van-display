/*
 * Solar detail: one card per MPPT, plus a 7-day yield chart.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define CARDS 2

static const uint32_t s_series_col[CARDS] = {C_BLUE, C_ORANGE};

static lv_obj_t *s_card[CARDS], *s_title[CARDS], *s_power[CARDS], *s_state[CARDS];
static lv_obj_t *s_left[CARDS], *s_right[CARDS];
static lv_obj_t *s_chart, *s_legend, *s_max_lbl;
static lv_chart_series_t *s_series[CARDS];

void page_solar_create(lv_obj_t *p)
{
    for (int i = 0; i < CARDS; i++) {
        s_card[i] = ui_card(p, 16 + i * 504, 8, 488, 300, NULL, "");
        lv_obj_clean(s_card[i]);
        s_title[i] = ui_label(s_card[i], &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_CHARGE "  --");
        lv_obj_align(s_title[i], LV_ALIGN_TOP_LEFT, 0, 0);
        s_state[i] = ui_label(s_card[i], &lv_font_montserrat_20, C_DIM, "");
        lv_obj_align(s_state[i], LV_ALIGN_TOP_RIGHT, 0, 0);
        s_power[i] = ui_label(s_card[i], &lv_font_montserrat_48, C_TEXT, "--");
        lv_obj_align(s_power[i], LV_ALIGN_TOP_LEFT, 0, 34);

        s_left[i] = ui_label(s_card[i], &lv_font_montserrat_20, C_TEXT, "");
        lv_obj_set_style_text_line_space(s_left[i], 10, 0);
        lv_obj_align(s_left[i], LV_ALIGN_TOP_LEFT, 0, 110);
        s_right[i] = ui_label(s_card[i], &lv_font_montserrat_20, C_TEXT, "");
        lv_obj_set_style_text_line_space(s_right[i], 10, 0);
        lv_obj_align(s_right[i], LV_ALIGN_TOP_LEFT, 236, 110);
    }

    lv_obj_t *cc = ui_card(p, 16, 322, 992, 222, LV_SYMBOL_LIST, "Yield, last 7 days");
    s_legend = ui_label(cc, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_align(s_legend, LV_ALIGN_TOP_RIGHT, 0, 2);

    s_chart = lv_chart_create(cc);
    lv_obj_set_size(s_chart, 900, 130);
    lv_obj_align(s_chart, LV_ALIGN_TOP_RIGHT, 0, 32);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_chart, CERBO_SOLAR_DAYS);
    lv_chart_set_div_line_count(s_chart, 3, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(C_LINE_OFF), 0);
    lv_obj_set_style_pad_column(s_chart, 4, 0);
    for (int i = 0; i < CARDS; i++) {
        s_series[i] = lv_chart_add_series(s_chart, lv_color_hex(s_series_col[i]), LV_CHART_AXIS_PRIMARY_Y);
    }

    s_max_lbl = ui_label(cc, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_align(s_max_lbl, LV_ALIGN_TOP_LEFT, 0, 32);

    /* Day labels under the bars, oldest on the left */
    static const char *const days[CERBO_SOLAR_DAYS] = {"-6", "-5", "-4", "-3", "-2", "Yesterday", "Today"};
    for (int k = 0; k < CERBO_SOLAR_DAYS; k++) {
        lv_obj_t *l = ui_label(cc, &lv_font_montserrat_16, C_DIM, days[k]);
        lv_obj_set_width(l, 900 / CERBO_SOLAR_DAYS);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 992 - 28 - 900 + k * (900 / CERBO_SOLAR_DAYS), 168);
    }
}

static const char *short_name(const cerbo_solar_t *s)
{
    if (!s->dev.name[0]) return "MPPT";
    return strncmp(s->dev.name, "Solar - ", 8) == 0 ? s->dev.name + 8 : s->dev.name;
}

void page_solar_update(const cerbo_data_t *d)
{
    char a[16], b[16], c[16], e[16];
    const cerbo_solar_t *list[CARDS] = {0};
    int n = 0;
    for (int i = 0; i < CERBO_MAX_SOLAR && n < CARDS; i++) {
        if (d->solar[i].dev.instance >= 0) list[n++] = &d->solar[i];
    }

    float max_kwh = 0.1f;
    char legend[96] = "";
    int ln = 0;
    for (int i = 0; i < CARDS; i++) {
        const cerbo_solar_t *s = list[i];
        lv_obj_set_hidden(s_card[i], s == NULL);
        if (!s) continue;

        ui_text_fmt(s_title[i], LV_SYMBOL_CHARGE "  %s", short_name(s));
        bool fault = s->error > 0;
        ui_text(s_state[i], fault ? "Error" : cerbo_charger_state_name(s->state));
        lv_obj_set_style_text_color(s_state[i], lv_color_hex(fault ? C_RED : s->state >= 3 ? C_GREEN : C_DIM), 0);
        ui_text_fmt(s_power[i], "%s" UNIT("W"), ui_num(a, sizeof(a), "%.0f", s->pv_w));

        float pv_a = (!isnan(s->pv_w) && !isnan(s->pv_v) && s->pv_v > 1) ? s->pv_w / s->pv_v : NAN;
        ui_text_fmt(s_left[i],
                              "PV  %s" UNIT("V") "\n"
                              "PV  %s" UNIT("A") "\n"
                              "Charge  %s" UNIT("A") "\n"
                              "Max today  %s" UNIT("W"),
                              ui_num(a, sizeof(a), "%.1f", s->pv_v), ui_num(b, sizeof(b), "%.1f", pv_a),
                              ui_num(c, sizeof(c), "%.1f", s->out_a), ui_num(e, sizeof(e), "%.0f", s->max_w_today));
        char err[16];
        snprintf(err, sizeof(err), s->error > 0 ? "#F35C58 #%d#" : "None", s->error);
        ui_text_fmt(s_right[i],
                              "Today  %s" UNIT("kWh") "\n"
                              "Yesterday  %s" UNIT("kWh") "\n"
                              "Total  %s" UNIT("kWh") "\n"
                              "Error  %s",
                              ui_num(a, sizeof(a), "%.2f", s->daily_kwh[0]), ui_num(b, sizeof(b), "%.2f", s->daily_kwh[1]),
                              ui_num(c, sizeof(c), "%.0f", s->yield_total_kwh), s->error < 0 ? "--" : err);

        /* Chart: index 0 = oldest */
        for (int k = 0; k < CERBO_SOLAR_DAYS; k++) {
            float v = s->daily_kwh[CERBO_SOLAR_DAYS - 1 - k];
            if (!isnan(v) && v > max_kwh) max_kwh = v;
            lv_chart_set_series_value_by_id(s_chart, s_series[i], k, isnan(v) ? LV_CHART_POINT_NONE : (int32_t)(v * 100));
        }
        ln += snprintf(legend + ln, sizeof(legend) - ln, "%s#%06lX " LV_SYMBOL_STOP "# %s", ln ? "     " : "",
                       (unsigned long)s_series_col[i], short_name(s));
    }
    ui_text(s_legend, legend);
    int32_t top = (int32_t)(ceilf(max_kwh * 2) / 2 * 100);  /* round up to 0.5 kWh */
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, top);
    ui_text_fmt(s_max_lbl, "%.1f\nkWh", top / 100.0);
    lv_chart_refresh(s_chart);
}
