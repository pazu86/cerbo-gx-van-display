#pragma once
/*
 * Live data from the Victron Cerbo GX over its built-in MQTT broker.
 *
 * Connects to the Cerbo for whichever network net.c is on, discovers the
 * portal ID, subscribes to the values the display needs and keeps the
 * broker publishing with periodic keepalives. Devices are found by service
 * type with wildcards, so they are identified by their names (e.g. "Roof",
 * "Fridge") rather than by instance numbers, which can change.
 *
 * Unknown values are NAN. Snapshot the whole lot with cerbo_get().
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CERBO_MAX_SOLAR  4
#define CERBO_MAX_TEMPS  8
#define CERBO_MAX_INPUTS 4
#define CERBO_MAX_CUSTOM 8
#define CERBO_MAX_TANKS  4
#define CERBO_SOLAR_DAYS 7
#define CERBO_MAX_RECENT 6
#define CERBO_MAX_MONTHS 6
#define CERBO_NAME_LEN   24

typedef enum {
    CERBO_DISCONNECTED,   /* no network or broker unreachable */
    CERBO_CONNECTING,
    CERBO_CONNECTED,      /* broker up, waiting for data */
    CERBO_LIVE,           /* data arriving */
    CERBO_STALE,          /* connected but nothing new for a while */
} cerbo_state_t;

typedef struct {
    int instance;                 /* -1 = unused slot */
    char name[CERBO_NAME_LEN];
} cerbo_dev_t;

typedef struct {
    cerbo_dev_t dev;
    float pv_w, pv_v;
    float out_a;                  /* charge current into the battery */
    float yield_today_kwh, max_w_today;
    float yield_total_kwh;        /* user-resettable lifetime yield */
    float daily_kwh[CERBO_SOLAR_DAYS];   /* [0] = today, [1] = yesterday ... */
    int state;                    /* VE.Direct charger state, -1 unknown */
    int error;                    /* 0 = none, -1 unknown */
} cerbo_solar_t;

typedef struct {
    cerbo_dev_t dev;
    float level_pct;
    float remaining, capacity;    /* m3 (Venus units); remaining is NAN if unknown */
    int fluid;                    /* 0 fuel 1 fresh 2 waste 3 live well 4 oil 5 black 6 gasoline 7 diesel 8 LPG ... */
    int status;                   /* 0 ok, other = sensor problem, -1 unknown */
} cerbo_tank_t;

/* From Node-RED's travel logger (MQTT VanDisplay/travel) */
typedef struct {
    char name[48];
    float km;
    int nights;
    char end[11];                 /* YYYY-MM-DD */
    char dur[16];
    float fuel_l;
} cerbo_trip_t;

typedef struct {
    bool valid;
    int64_t rx_us;
    float odo_km;
    float service_km_left;
    int service_pct;
    int service_days_left;        /* INT32_MIN unknown */
    char service_next[11];
    bool service_due, service_warn;
    bool at_home;
    double home_lat, home_lon;
    bool trip_active;
    float trip_km, trip_fuel_l, trip_max_kmh, trip_max_from_home_km;
    char trip_start[26];
    cerbo_trip_t last;
    cerbo_trip_t recent[CERBO_MAX_RECENT];
    int recent_count;
    int stats_trips, stats_nights;
    float stats_km, stats_longest_km;
    struct { char m[8]; float km; } monthly[CERBO_MAX_MONTHS];
    int monthly_count;
} cerbo_travel_t;

typedef struct {
    cerbo_dev_t dev;
    float temp_c, humidity;
    int type;                     /* 0 battery 1 fridge 2 generic 3 room 4 outdoor ... */
} cerbo_temp_t;

typedef struct {
    cerbo_dev_t dev;
    int state;                    /* digital input state enum, -1 unknown */
    int alarm;                    /* 0 ok, 2 alarm, -1 unknown */
} cerbo_input_t;

typedef struct {
    char topic[40];               /* after the "VanDisplay/" prefix */
    char value[32];
} cerbo_custom_t;

typedef struct {
    cerbo_state_t state;
    char portal[16];
    char host[40];
    int64_t last_rx_us;           /* esp_timer time of last message, 0 = never */
    uint32_t rx_count;

    /* Battery (system/0 Dc/Battery - follows whichever battery monitor is active) */
    float soc, batt_v, batt_a, batt_w, time_to_go_s, consumed_ah;
    int batt_state;               /* 0 idle 1 charging 2 discharging, -1 unknown */

    /* System totals */
    float pv_total_w, dc_system_w, dc_system_a;

    cerbo_solar_t solar[CERBO_MAX_SOLAR];

    /* Orion DC-DC (alternator) */
    cerbo_dev_t dcdc;
    float dcdc_in_v, dcdc_out_w;
    int dcdc_state;

    cerbo_temp_t temps[CERBO_MAX_TEMPS];
    cerbo_input_t inputs[CERBO_MAX_INPUTS];

    cerbo_tank_t tanks[CERBO_MAX_TANKS];

    /* GPS */
    int gps_fix;
    double lat, lon;
    float speed_kmh, course_deg;

    cerbo_travel_t travel;

    /* Anything Node-RED publishes under VanDisplay/... on the Cerbo's broker */
    cerbo_custom_t custom[CERBO_MAX_CUSTOM];
} cerbo_data_t;

/* Start the background task. Requires net_start() to have been called. */
void cerbo_start(void);

void cerbo_get(cerbo_data_t *out);

const char *cerbo_state_name(cerbo_state_t state);

/* Publish (QoS 0) to the Cerbo's broker from any task; false if not connected. */
bool cerbo_publish(const char *topic, const char *data, int len, bool retain);

/* Victron charger state name (Off, Bulk, Absorption, Float...) */
const char *cerbo_charger_state_name(int state);

#ifdef __cplusplus
}
#endif
