/*
 * Overview: Victron-style power flow (Solar, DC-DC and the 240 V charger
 * into the Battery, Battery out to DC loads) with temperatures below, and
 * (CONFIG_PM335_ENABLE) Projecta PM335 water tanks and pump / hot water switches.
 *
 * The 240 V charger isn't connected to the Cerbo. The Cerbo estimates DC
 * loads as solar + DC-DC - battery charge, so when that charger is running
 * the estimate goes negative: -DC loads is then a lower bound for the
 * charger's output, and the real loads can't be known.
 */
#include "sdkconfig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_timer.h"
#include "pm335.h"
#include "ui.h"

#define TEMP_TILES      4
#define SHORE_MIN_W     15    /* below this a negative load estimate is just noise */

static lv_obj_t *s_solar_val, *s_solar_rows, *s_solar_today;
static lv_obj_t *s_dcdc_val, *s_dcdc_sub;
static lv_obj_t *s_shore_val, *s_shore_sub;
static lv_obj_t *s_batt_val, *s_batt_state, *s_batt_row;
static lv_obj_t *s_load_val, *s_load_sub;
static lv_obj_t *s_flow_val, *s_flow_sub, *s_net_val, *s_net_sub;
static lv_obj_t *s_c_solar, *s_c_dcdc, *s_c_shore, *s_c_load;
static lv_obj_t *s_temp_name[TEMP_TILES], *s_temp_val[TEMP_TILES];

#ifdef CONFIG_PM335_ENABLE
/* Projecta PM335 row */
#define SW_HOT_WATER    0
#define SW_PUMP         1
/* The PM's reported status lags its relays by up to ~10 s (its own LCD lags
 * too), so after a tap the button shows the requested state, ignores readings
 * for SW_MIN_HOLD_US (they may still describe the previous state), then waits
 * for a fresh reading that matches - or gives up after SW_PENDING_US */
#define SW_MIN_HOLD_US  (12 * 1000000LL)
#define SW_PENDING_US   (30 * 1000000LL)
static lv_obj_t *s_tank_val[3], *s_tank_bar[3];
static lv_obj_t *s_sw_btn[2], *s_sw_name[2], *s_sw_state[2];
static bool s_sw_want[2];
static int64_t s_sw_pending_until[2], s_sw_req_us[2];
/* Debounce: a reported state is only shown once two fresh replies agree */
static int s_sw_shown[2] = {-1, -1}, s_sw_last[2] = {-1, -1};
static uint32_t s_van_gen;

static void switch_cb(lv_event_t *e);
static void update_pm335(void);
#endif

void page_overview_create(lv_obj_t *p)
{
    /* ---- top row: flow ---- */
    lv_obj_t *solar = ui_card(p, 16, 8, 300, 330, LV_SYMBOL_CHARGE, "Solar");
    s_solar_val = ui_label(solar, &lv_font_montserrat_48, C_TEXT, "--");
    lv_obj_align(s_solar_val, LV_ALIGN_TOP_LEFT, 0, 36);
    s_solar_rows = ui_label(solar, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_style_text_line_space(s_solar_rows, 8, 0);
    lv_obj_align(s_solar_rows, LV_ALIGN_TOP_LEFT, 0, 120);
    s_solar_today = ui_label(solar, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_solar_today, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *dcdc = ui_card(p, 362, 8, 144, 120, LV_SYMBOL_SHUFFLE, "DC-DC");
    s_dcdc_val = ui_label(dcdc, &lv_font_montserrat_32, C_TEXT, "--");
    lv_obj_align(s_dcdc_val, LV_ALIGN_TOP_LEFT, 0, 30);
    s_dcdc_sub = ui_label(dcdc, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_align(s_dcdc_sub, LV_ALIGN_BOTTOM_LEFT, 0, 4);

    lv_obj_t *shore = ui_card(p, 518, 8, 144, 120, LV_SYMBOL_POWER, "240V");
    s_shore_val = ui_label(shore, &lv_font_montserrat_32, C_TEXT, "Off");
    lv_obj_align(s_shore_val, LV_ALIGN_TOP_LEFT, 0, 30);
    s_shore_sub = ui_label(shore, &lv_font_montserrat_16, C_DIM, "charger");
    lv_obj_align(s_shore_sub, LV_ALIGN_BOTTOM_LEFT, 0, 4);

    lv_obj_t *batt = ui_card(p, 362, 160, 300, 178, LV_SYMBOL_BATTERY_FULL, "Battery");
    lv_obj_set_style_bg_color(batt, lv_color_hex(C_CARD_HI), 0);
    s_batt_val = ui_label(batt, &lv_font_montserrat_48, C_TEXT, "--");
    lv_obj_align(s_batt_val, LV_ALIGN_TOP_LEFT, 0, 30);
    s_batt_state = ui_label(batt, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_set_style_text_align(s_batt_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_batt_state, LV_ALIGN_TOP_RIGHT, 0, 44);
    s_batt_row = ui_label(batt, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_align(s_batt_row, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Power card: loads, battery charge/discharge rate, and net */
    lv_obj_t *load = ui_card(p, 708, 8, 300, 330, LV_SYMBOL_HOME, "Power");
    lv_obj_t *l;
    l = ui_label(load, &lv_font_montserrat_16, C_DIM, "Loads");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 34);
    s_load_val = ui_label(load, &lv_font_montserrat_36, C_TEXT, "--");
    lv_obj_align(s_load_val, LV_ALIGN_TOP_LEFT, 0, 52);
    s_load_sub = ui_label(load, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_set_style_text_align(s_load_sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_load_sub, LV_ALIGN_TOP_RIGHT, 0, 62);

    l = ui_label(load, &lv_font_montserrat_16, C_DIM, "Battery");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 106);
    s_flow_val = ui_label(load, &lv_font_montserrat_48, C_TEXT, "--");
    lv_obj_align(s_flow_val, LV_ALIGN_TOP_LEFT, 0, 124);
    s_flow_sub = ui_label(load, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_set_style_text_align(s_flow_sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_flow_sub, LV_ALIGN_TOP_RIGHT, 0, 128);

    l = ui_label(load, &lv_font_montserrat_16, C_DIM, "Net");
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 196);
    s_net_val = ui_label(load, &lv_font_montserrat_28, C_TEXT, "--");
    lv_obj_align(s_net_val, LV_ALIGN_TOP_LEFT, 0, 214);
    s_net_sub = ui_label(load, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_align(s_net_sub, LV_ALIGN_TOP_LEFT, 0, 262);

    for (int i = 0; i < 2; i++) {   /* dividers between the sections */
        lv_obj_t *div = lv_obj_create(load);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, LV_PCT(100), 1);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(div, lv_color_hex(C_LINE_OFF), 0);
        lv_obj_align(div, LV_ALIGN_TOP_LEFT, 0, i ? 188 : 98);
    }

    s_c_solar = ui_connector(p, 316, 249, 362, 249);
    s_c_dcdc = ui_connector(p, 434, 128, 434, 160);
    s_c_shore = ui_connector(p, 590, 128, 590, 160);
    s_c_load = ui_connector(p, 662, 249, 708, 249);

#ifndef CONFIG_PM335_ENABLE
    /* ---- bottom row: temperatures (name top, big value) ---- */
    const int gap = 12, tw = (992 - (TEMP_TILES - 1) * gap) / TEMP_TILES, ty = 354, th = 190;
    for (int i = 0; i < TEMP_TILES; i++) {
        lv_obj_t *c = ui_card(p, 16 + i * (tw + gap), ty, tw, th, NULL, "");
        lv_obj_clean(c);
        s_temp_name[i] = ui_label(c, &lv_font_montserrat_20, C_TEXT, "--");
        lv_obj_align(s_temp_name[i], LV_ALIGN_TOP_LEFT, 0, 0);
        s_temp_val[i] = ui_label(c, &lv_font_montserrat_48, C_TEXT, "--");
        lv_obj_align(s_temp_val[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
#else
    /* ---- temperatures: one compact row (name left, value right) ---- */
    const int gap = 12, tw = (992 - (TEMP_TILES - 1) * gap) / TEMP_TILES, ty = 354, th = 78;
    for (int i = 0; i < TEMP_TILES; i++) {
        lv_obj_t *c = ui_card(p, 16 + i * (tw + gap), ty, tw, th, NULL, "");
        lv_obj_clean(c);
        s_temp_name[i] = ui_label(c, &lv_font_montserrat_16, C_TEXT, "--");
        lv_obj_align(s_temp_name[i], LV_ALIGN_LEFT_MID, 0, 0);
        s_temp_val[i] = ui_label(c, &lv_font_montserrat_28, C_TEXT, "--");
        lv_obj_align(s_temp_val[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    /* ---- Projecta PM335: water tanks + hot water / pump switches ---- */
    const int ry = ty + th + 12, rh = 544 - ry;
    lv_obj_t *tanks = ui_card(p, 16, ry, 496, rh, NULL, "");
    lv_obj_clean(tanks);
    for (int t = 0; t < 3; t++) {
        lv_obj_t *col = lv_obj_create(tanks);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 148, LV_PCT(100));
        lv_obj_set_pos(col, t * 160, 0);
        lv_obj_set_scrollable(col, false);
        char name[8];
        snprintf(name, sizeof(name), "Tank %d", t + 1);
        lv_obj_t *n = ui_label(col, &lv_font_montserrat_16, C_DIM, name);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        s_tank_val[t] = ui_label(col, &lv_font_montserrat_28, C_TEXT, "--");
        lv_obj_align(s_tank_val[t], LV_ALIGN_TOP_LEFT, 0, 18);
        s_tank_bar[t] = lv_bar_create(col);
        lv_obj_set_size(s_tank_bar[t], 140, 8);
        lv_obj_align(s_tank_bar[t], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_bar_set_range(s_tank_bar[t], 0, 100);
        lv_obj_set_style_bg_color(s_tank_bar[t], lv_color_hex(C_LINE_OFF), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_tank_bar[t], lv_color_hex(C_BLUE), LV_PART_INDICATOR);
    }

    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, 524 + i * 248, ry);
        lv_obj_set_size(b, 236, rh);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_pad_all(b, 14, 0);
        lv_obj_add_event_cb(b, switch_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_sw_btn[i] = b;
        s_sw_name[i] = ui_label(b, &lv_font_montserrat_20, C_TEXT, i == SW_HOT_WATER ? "Hot water" : "Water pump");
        lv_obj_align(s_sw_name[i], LV_ALIGN_TOP_LEFT, 0, 0);
        s_sw_state[i] = ui_label(b, &lv_font_montserrat_32, C_DIM, "--");
        lv_obj_align(s_sw_state[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        s_sw_pending_until[i] = 0;
        s_sw_shown[i] = s_sw_last[i] = -1;
    }
    s_van_gen = 0;
#endif
}

#ifdef CONFIG_PM335_ENABLE
/* Switch buttons: tap toggles and shows the new state straight away; the PM335's
 * own status confirms it in the background, and only if it hasn't changed after
 * SW_PENDING_US does the button fall back to what the PM reports */
static int sw_effective(int i)
{
    return s_sw_pending_until[i] ? s_sw_want[i] : s_sw_shown[i];
}

static void request(int i, bool want)
{
    if (pm335_switch(i == SW_HOT_WATER ? PM335_SW_HOT_WATER : PM335_SW_PUMP, want)) {
        s_sw_want[i] = want;
        s_sw_req_us[i] = esp_timer_get_time();
        s_sw_pending_until[i] = s_sw_req_us[i] + SW_PENDING_US;
    }
}

static void switch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    pm335_van_t v;
    pm335_get_van(&v);
    int cur = sw_effective(i);
    if (!v.online || cur < 0) return;
    bool want = !cur;
    /* The hot water system needs the pump: pump on before hot water on, and
     * hot water off with the pump (queued in order) */
    if (i == SW_HOT_WATER && want && sw_effective(SW_PUMP) != 1) request(SW_PUMP, true);
    if (i == SW_PUMP && !want && sw_effective(SW_HOT_WATER) == 1) request(SW_HOT_WATER, false);
    request(i, want);
    update_pm335();   /* show it now, not on the next refresh */
}

static void update_pm335(void)
{
    pm335_van_t v;
    pm335_get_van(&v);
    for (int t = 0; t < 3; t++) {
        if (v.valid && v.tank[t] >= 0) {
            ui_text_fmt(s_tank_val[t], "%d" UNIT("%%"), v.tank[t]);
            lv_bar_set_value(s_tank_bar[t], v.tank[t] > 100 ? 100 : v.tank[t], LV_ANIM_OFF);
        } else {
            ui_text(s_tank_val[t], "--");
            lv_bar_set_value(s_tank_bar[t], 0, LV_ANIM_OFF);
        }
    }
    int64_t now = esp_timer_get_time();
    bool fresh = v.valid && v.gen != s_van_gen;
    if (fresh) s_van_gen = v.gen;
    for (int i = 0; i < 2; i++) {
        int raw = v.valid ? (i == SW_HOT_WATER ? v.hot_water : v.pump) : -1;
        if (s_sw_pending_until[i]) {
            /* Waiting on our own request: only a fresh, matching reading after
             * the hold-off confirms it; otherwise give up at the deadline */
            if (fresh && raw == s_sw_want[i] && now - s_sw_req_us[i] >= SW_MIN_HOLD_US) {
                s_sw_pending_until[i] = 0;
                s_sw_shown[i] = raw;
            } else if (now > s_sw_pending_until[i]) {
                s_sw_pending_until[i] = 0;
                s_sw_shown[i] = raw;
            }
            if (fresh) s_sw_last[i] = raw;
        } else if (raw < 0) {
            s_sw_shown[i] = s_sw_last[i] = -1;
        } else if (fresh) {
            /* Changes made elsewhere (Projecta buttons, phone app): first
             * reading, or two replies in a row that agree */
            if (s_sw_shown[i] < 0 || raw == s_sw_last[i]) s_sw_shown[i] = raw;
            s_sw_last[i] = raw;
        }
        int st = s_sw_pending_until[i] ? s_sw_want[i] : s_sw_shown[i];
        bool on = st == 1;
        lv_obj_set_style_bg_color(s_sw_btn[i], lv_color_hex(on ? C_GREEN : C_CARD), 0);
        lv_obj_set_style_border_color(s_sw_btn[i], lv_color_hex(on ? C_GREEN : C_BORDER), 0);
        uint32_t fg = on ? 0x0B0B0C : C_TEXT;
        lv_obj_set_style_text_color(s_sw_name[i], lv_color_hex(fg), 0);
        if (st < 0) {
            ui_text(s_sw_state[i], v.online ? "--" : pm335_connecting() ? "connecting..." : "offline");
            lv_obj_set_style_text_color(s_sw_state[i], lv_color_hex(C_DIM), 0);
        } else {
            ui_text(s_sw_state[i], on ? LV_SYMBOL_POWER "  ON" : "OFF");
            lv_obj_set_style_text_color(s_sw_state[i], lv_color_hex(on ? 0x0B0B0C : C_DIM), 0);
        }
    }
}

#endif

/* Temperature tiles, left to right, by the sensor's name on the Cerbo.
 * Sensors not listed follow, by Cerbo instance number. */
/* Use the names your sensors have on the Cerbo (Settings > Devices > the sensor > Custom name) */
static const char *const s_temp_order[] = {"Outside", "Fridge", "Inside", "Bedroom"};

static int temp_rank(const cerbo_temp_t *t)
{
    for (size_t i = 0; i < sizeof(s_temp_order) / sizeof(s_temp_order[0]); i++) {
        if (strcasecmp(t->dev.name, s_temp_order[i]) == 0) {
            return (int)i;
        }
    }
    return 100;
}

static int temp_order_cmp(const void *pa, const void *pb)
{
    const cerbo_temp_t *a = *(const cerbo_temp_t *const *)pa;
    const cerbo_temp_t *b = *(const cerbo_temp_t *const *)pb;
    int ra = temp_rank(a), rb = temp_rank(b);
    return ra != rb ? ra - rb : a->dev.instance - b->dev.instance;
}

static uint32_t temp_colour(const cerbo_temp_t *t)
{
    if (isnan(t->temp_c)) return C_DIM;
    if (t->type == 1) {  /* fridge */
        return t->temp_c > 8 ? C_ORANGE : C_BLUE;
    }
    if (t->temp_c >= 35) return C_RED;
    if (t->temp_c >= 30) return C_ORANGE;
    if (t->temp_c <= 5) return C_BLUE;
    return C_TEXT;
}

void page_overview_update(const cerbo_data_t *d)
{
    char a[16], b[16], c[16];

    /* Solar */
    ui_text_fmt(s_solar_val, "%s" UNIT("W"), ui_num(a, sizeof(a), "%.0f", d->pv_total_w));
    char rows[160] = "";
    int n = 0;
    float today = 0;
    bool have_today = false;
    for (int i = 0; i < CERBO_MAX_SOLAR; i++) {
        const cerbo_solar_t *s = &d->solar[i];
        if (s->dev.instance < 0) continue;
        const char *name = strncmp(s->dev.name, "Solar - ", 8) == 0 ? s->dev.name + 8 : s->dev.name;
        n += snprintf(rows + n, sizeof(rows) - n, "%s  %s" UNIT("W") "\n", name[0] ? name : "MPPT",
                      ui_num(a, sizeof(a), "%.0f", s->pv_w));
        if (!isnan(s->yield_today_kwh)) {
            today += s->yield_today_kwh;
            have_today = true;
        }
    }
    ui_text(s_solar_rows, rows);
    ui_text_fmt(s_solar_today, "Today %s kWh", ui_num(a, sizeof(a), "%.2f", have_today ? today : NAN));
    ui_connector_set_active(s_c_solar, d->pv_total_w > 1);

    /* DC-DC (Orion, charging from the vehicle) */
    bool dcdc_on = d->dcdc_state > 0 && d->dcdc_out_w > 1;
    if (dcdc_on) {
        ui_text_fmt(s_dcdc_val, "%s" UNIT("W"), ui_num(a, sizeof(a), "%.0f", d->dcdc_out_w));
    } else {
        ui_text(s_dcdc_val, isnan(d->dcdc_out_w) ? "--" : "Off");
    }
    ui_text_fmt(s_dcdc_sub, "In %s V", ui_num(a, sizeof(a), "%.1f", d->dcdc_in_v));
    ui_connector_set_active(s_c_dcdc, dcdc_on);

    /* 240 V charger, inferred (see top of file) */
    bool shore_on = !isnan(d->dc_system_w) && d->dc_system_w < -SHORE_MIN_W;
    if (shore_on) {
        ui_text_fmt(s_shore_val, "~%.0f" UNIT("W"), -d->dc_system_w);
        ui_text_fmt(s_shore_sub, "~%s A", ui_num(a, sizeof(a), "%.1f", -d->dc_system_a));
    } else {
        ui_text(s_shore_val, "Off");
        ui_text(s_shore_sub, "charger");
    }
    ui_connector_set_active(s_c_shore, shore_on);

    /* Battery */
    ui_text_fmt(s_batt_val, "%s" UNIT("%%"), ui_num(a, sizeof(a), "%.0f", d->soc));
    char ttg[16];
    ui_duration(ttg, sizeof(ttg), d->time_to_go_s);
    const char *st = d->batt_state == 1 ? "Charging" : d->batt_state == 2 ? "Discharging" : d->batt_state == 0 ? "Idle" : "";
    ui_text_fmt(s_batt_state, "%s\n%s", st, d->batt_state == 2 ? ttg : "");
    ui_text_fmt(s_batt_row, "%s" UNIT("V") "    %s" UNIT("A") "    %s" UNIT("W"),
                          ui_num(a, sizeof(a), "%.2f", d->batt_v), ui_num(b, sizeof(b), "%.1f", d->batt_a),
                          ui_num(c, sizeof(c), "%.0f", d->batt_w));
    uint32_t soc_col = isnan(d->soc) ? C_TEXT : d->soc < 20 ? C_RED : d->soc < 50 ? C_ORANGE : C_TEXT;
    lv_obj_set_style_text_color(s_batt_val, lv_color_hex(soc_col), 0);

    /* DC loads: unknown while the invisible 240 V charger is running */
    if (shore_on) {
        ui_text(s_load_val, "--" UNIT("A"));
        ui_text(s_load_sub, "unknown on\n240V");
    } else {
        ui_text_fmt(s_load_val, "%s" UNIT("A"), ui_num(a, sizeof(a), "%.1f", d->dc_system_a));
        ui_text(s_load_sub, "");   /* amps only */
    }
    ui_connector_set_active(s_c_load, !shore_on && d->dc_system_w > 1);

    /* Battery flow, signed and coloured: what's going in (or out) right now */
    if (isnan(d->batt_a)) {
        ui_text(s_flow_val, "--");
        ui_text(s_flow_sub, "");
    } else {
        bool charging = d->batt_a > 0.05f;
        ui_text_fmt(s_flow_val, "%s%.1f" UNIT("A"), charging ? "+" : "", d->batt_a);
        lv_obj_set_style_text_color(s_flow_val, lv_color_hex(charging ? C_GREEN : d->batt_a < -0.05f ? C_ORANGE : C_TEXT), 0);
        ui_text(s_flow_sub, charging ? "Charging" : d->batt_a < -0.05f ? "Discharging" : "Idle");
    }

    /* Net: is the van producing more than it's using? The battery current is
     * the difference, so In (all charging sources) = loads + battery. */
    if (isnan(d->batt_a)) {
        ui_text(s_net_val, "--");
        ui_text(s_net_sub, "");
    } else {
        const float net = d->batt_a;
        bool pos = net > 0.1f, neg = net < -0.1f;
        ui_text_fmt(s_net_val, "%s %s", pos ? LV_SYMBOL_UP : neg ? LV_SYMBOL_DOWN : LV_SYMBOL_MINUS,
                    pos ? "Positive" : neg ? "Negative" : "Balanced");   /* under the small "Net" title */
        lv_obj_set_style_text_color(s_net_val, lv_color_hex(pos ? C_GREEN : neg ? C_RED : C_TEXT), 0);
        if (shore_on || isnan(d->dc_system_a)) {
            ui_text(s_net_sub, "In / out unknown on 240V");
        } else {
            float out = d->dc_system_a, in = out + net;
            ui_text_fmt(s_net_sub, "In %s A  " LV_SYMBOL_BULLET "  Out %s A", ui_num(a, sizeof(a), "%.1f", in < 0 ? 0 : in),
                        ui_num(b, sizeof(b), "%.1f", out));
        }
    }

    /* Temperatures in a fixed order (cerbo.c fills slots in MQTT arrival
     * order, which varies between connections) */
    const cerbo_temp_t *sorted[CERBO_MAX_TEMPS];
    int count = 0;
    for (int i = 0; i < CERBO_MAX_TEMPS; i++) {
        if (d->temps[i].dev.instance >= 0) {
            sorted[count++] = &d->temps[i];
        }
    }
    qsort(sorted, count, sizeof(sorted[0]), temp_order_cmp);

#ifdef CONFIG_PM335_ENABLE
    update_pm335();
#endif

    for (int shown = 0; shown < count && shown < TEMP_TILES; shown++) {
        const cerbo_temp_t *t = sorted[shown];
        ui_text(s_temp_name[shown], t->dev.name[0] ? t->dev.name : "Temp");
        ui_text_fmt(s_temp_val[shown], "%s" UNIT("°C"), ui_num(a, sizeof(a), "%.1f", t->temp_c));
        lv_obj_set_style_text_color(s_temp_val[shown], lv_color_hex(temp_colour(t)), 0);
    }
}
