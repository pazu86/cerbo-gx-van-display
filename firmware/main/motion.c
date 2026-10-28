/*
 * Travel safety: water pump and hot water off once the van is on the move.
 *
 * Every CHECK_US the Cerbo's GPS fix is compared with an anchor position:
 * - The anchor is wherever the van has sat still (within SETTLE_M) for
 *   SETTLE_US, so it follows the van from site to site and a GPS that wanders
 *   or jumps and stays there just re-anchors instead of tripping.
 * - "Moving" = every good fix for TRIP_DWELL_US is more than TRIP_M from the
 *   anchor. A jump that comes back resets the dwell. SETTLE_US < TRIP_DWELL_US
 *   so a parked van always re-anchors before a jump could count.
 * - On moving: for ENFORCE_US, any pump / hot water the PM335 reports on is
 *   switched off (hot water first). Then the anchor moves to where the van is,
 *   so every further TRIP_M of driving checks again, while switching the pump
 *   on at a stop is left alone.
 * The Settings "Travel safety" switch only stops the switching off; detection
 * keeps running so /api/health still shows what it would have done.
 */
#include "motion.h"

#include <math.h>

#include "cerbo.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "pm335.h"
#include "settings.h"

static const char *TAG = "motion";

#define CHECK_US       (2 * 1000 * 1000LL)
#define TRIP_M         350.0
#define TRIP_DWELL_US  (30 * 1000 * 1000LL)
#define SETTLE_M       50.0
#define SETTLE_US      (20 * 1000 * 1000LL)
#define ENFORCE_US     (90 * 1000 * 1000LL)
#define RESEND_US      (15 * 1000 * 1000LL)   /* the PM's status lags the relays ~10 s */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static motion_status_t s_st = {.from_anchor_m = -1, .last_trip_s = -1};
static int64_t s_last_trip_us;

static bool s_anchored;
static double s_anchor_lat, s_anchor_lon;
static double s_settle_lat, s_settle_lon;
static int64_t s_settle_since;     /* 0 = no settle reference */
static int64_t s_far_since;        /* 0 = not beyond TRIP_M */
static int64_t s_enforce_until;
static int64_t s_last_send;

static double dist_m(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371000.0, k = M_PI / 180.0;
    double dlat = (lat2 - lat1) * k, dlon = (lon2 - lon1) * k;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * k) * cos(lat2 * k) * sin(dlon / 2) * sin(dlon / 2);
    return 2 * r * asin(sqrt(a));
}

static void enforce_off(int64_t now)
{
    pm335_van_t v;
    pm335_get_van(&v);
    if (!v.online || !v.valid || (v.pump != 1 && v.hot_water != 1)) return;
    if (s_last_send && now - s_last_send < RESEND_US) return;
    int sent = 0;
    if (v.hot_water == 1 && pm335_switch(PM335_SW_HOT_WATER, false)) sent++;
    if (v.pump == 1 && pm335_switch(PM335_SW_PUMP, false)) sent++;
    if (sent) {
        s_last_send = now;
        ESP_LOGW(TAG, "on the move: %s%s off", v.hot_water == 1 ? "hot water + " : "",
                 v.pump == 1 ? "pump" : "");
        portENTER_CRITICAL(&s_mux);
        s_st.offs_sent += sent;
        portEXIT_CRITICAL(&s_mux);
    }
}

static void check_cb(void *arg)
{
    EXT_RAM_BSS_ATTR static cerbo_data_t d;   /* big; only this timer uses it */
    cerbo_get(&d);
    int64_t now = esp_timer_get_time();

    bool good = d.state == CERBO_LIVE && d.gps_fix == 1 && isfinite(d.lat) && isfinite(d.lon) &&
                !(d.lat == 0 && d.lon == 0);
    float from_anchor = -1;
    if (!good) {
        s_far_since = 0;   /* the dwell needs an unbroken run of good fixes */
    } else {
        /* Settled: stayed within SETTLE_M of one spot for SETTLE_US -> anchor there */
        if (!s_settle_since || dist_m(s_settle_lat, s_settle_lon, d.lat, d.lon) > SETTLE_M) {
            s_settle_lat = d.lat;
            s_settle_lon = d.lon;
            s_settle_since = now;
        }
        if (!s_anchored || now - s_settle_since >= SETTLE_US) {
            if (!s_anchored) ESP_LOGI(TAG, "anchored at %.5f,%.5f", d.lat, d.lon);
            s_anchored = true;
            s_anchor_lat = s_settle_lat;
            s_anchor_lon = s_settle_lon;
        }

        double m = dist_m(s_anchor_lat, s_anchor_lon, d.lat, d.lon);
        from_anchor = (float)m;
        if (m <= TRIP_M) {
            s_far_since = 0;
        } else if (!s_far_since) {
            s_far_since = now;
        } else if (now - s_far_since >= TRIP_DWELL_US) {
            ESP_LOGW(TAG, "moved %.0f m (%.0f km/h)", m, d.speed_kmh);
            s_enforce_until = now + ENFORCE_US;
            s_last_send = 0;
            s_anchor_lat = s_settle_lat = d.lat;
            s_anchor_lon = s_settle_lon = d.lon;
            s_settle_since = now;
            s_far_since = 0;
            from_anchor = 0;
            s_last_trip_us = now;
            portENTER_CRITICAL(&s_mux);
            s_st.trips++;
            portEXIT_CRITICAL(&s_mux);
        }
    }

    settings_t set;
    settings_get(&set);
    bool enabled = !set.travel_safety_off;
    if (enabled && s_enforce_until && now < s_enforce_until) enforce_off(now);

    portENTER_CRITICAL(&s_mux);
    s_st.enabled = enabled;
    s_st.anchored = s_anchored;
    s_st.from_anchor_m = from_anchor;
    s_st.far_s = s_far_since ? (int)((now - s_far_since) / 1000000) : 0;
    s_st.last_trip_s = s_last_trip_us ? (int)((now - s_last_trip_us) / 1000000) : -1;
    portEXIT_CRITICAL(&s_mux);
}

void motion_start(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = check_cb, .name = "motion"};
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_periodic(t, CHECK_US);
}

void motion_get(motion_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}
